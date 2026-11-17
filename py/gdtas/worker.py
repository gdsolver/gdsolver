"""Lifecycle management of a single GD worker, and supplying it with plans.

A worker is "a renamed GD + a dedicated Geode root + a dedicated save + a dedicated
data root". GD has no single-instance lock, so any number of them run independently.
Always read the results from the worker's data_root (not from the repository's data dir).

The traps collected here (each one cost real debugging time, once):
  - Without Steam, GD WEDGES SILENTLY INSTEAD OF FAILING (kill does not work either).
    Check for it first.
  - steam_appid is fixed at 480 (Valve's public test appid). Grabbing 322170 makes
    the user's Steam copy of GD unable to launch.
  - Two GDs on one data root mix up result.txt and return PLAUSIBLE NUMBERS FROM A
    DIFFERENT RUN. Always confirm the worker is free before launching.
  - taskkill is asynchronous. Launching the next one without waiting for the old one
    to disappear produces the piggyback above.
  - A BOM on plan_in.txt makes the first input silently disappear (see plan.py).
  - Trust only the lines after the `serve: loaded` marker. Before the marker it is
    still the previous plan.
  - A SESSION THAT RAN TO COMPLETION NEVER READS cmd.txt AGAIN (the MOD's cmd polling
    sits inside `g_started && !g_sessionOver`). No later rerun ever lands, so the only
    way out is to reopen.
  - A hand-configured GD profile costs +400MB of private bytes. Clone the minimal one.
  - A Geode crash leaves the game's process ALIVE behind a window ("Geode Crash Handler"), and result.txt just stops:
    a launch that only watches the file learns of it when the stall timer runs out. CrashWatch notices the new
    geode/crashlogs/*.log or the window on the next poll (SessionResult.crash).
  - A crash Geode never sees (a fail-fast, abort() or std::terminate after an exception nobody caught: exit status
    0xC0000409 in ucrtbase) skips Geode's handler: no crash log and no Geode window. Windows' own error reporting takes
    the process instead -- a WerFault.exe started with `-p <pid>` keeps it alive behind its dialog -- and result.txt
    just stops the same way. CrashWatch notices a WerFault.exe reporting on one of the worker's processes, and a game
    that exits with an NTSTATUS error status is a crash too.
"""

from __future__ import annotations

import hashlib
import shutil
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path

from . import plan as P
from . import results as R
from . import gdsave
from .gdsave import MINIMAL_SAVE_XML as _MINIMAL_SAVE_XML
from .gdsave import decode_save as _decode_save
from .gdsave import encode_save as _encode_save
from .gdbase import check_geode, check_worker, geode_drift_allowed, geode_identity, mod_presses_player2
from .paths import (BUILD_MOD, MOD_CACHE, MOD_ID, WORKERS_ROOT, gd_save_root,
                    worker_manifest)

# Default cfg for serving. attempts is large because a resident session supplies
# 1 attempt = 1 plan over and over.
BASE_CFG = {
    "enabled": "1",
    "attempts": "100000",
    "quitwhendone": "0",
    "blockinput": "1",
    "cbs": "0",
    "cos": "1",
    "fastdt": "0.0166667",
    "fastloops": "1800",
    "skiprender": "1",
    "solve": "0",
    "music": "mute",
    "coins": "0",
    "servemode": "1",
    # The per-attempt research trail (dp_band_itN/dp_groups_itN, dp_fixin_*/dp_attempt_itN) is
    # off by default in the mod (a player's Solve should not accumulate it); this worker serves
    # the research/cold loop, so it always asks for it explicitly.
    "capture": "1",
    # The mod leaves the window alone unless told (a player's game is theirs); a worker's window is
    # held at its launch size, which is part of what a run measured (window_guard.hpp).
    "windowguard": "1",
    # The replay panel is on by default for a person watching a solve (the mod's cfg default). Nobody watches a
    # worker, and the panel would read and parse the level table in a thread beside every level: off here, once, for
    # every launch that goes through this module (cold runs, collects, the MCP's gd_* tools). A launch for a person
    # (observe.launch_visible) asks for it.
    "replaypanel": "0",
}

# The keys every launch through run_session carries, unless the caller set them.
WORKER_DEFAULT_KEYS = ("windowguard=1", "replaypanel=0")


def with_worker_keys(cfg_lines: list[str]) -> list[str]:
    """cfg_lines plus the keys every worker launch carries (windowguard=1, replaypanel=0), unless the caller set them."""
    keys = {ln.split("=", 1)[0].strip() for ln in cfg_lines if "=" in ln}
    return list(cfg_lines) + [kv for kv in WORKER_DEFAULT_KEYS if kv.split("=", 1)[0] not in keys]

def plan_masks(cfg_lines: list[str]) -> list[int]:
    """The masks of the `input=<tick>,<mask>` lines in cfg_lines (a line that is not one is skipped)."""
    out = []
    for ln in cfg_lines:
        k, _, v = ln.strip().partition("=")
        if k.strip() == "input":
            try:
                out.append(int(v.split(",", 1)[1]))
            except (IndexError, ValueError):
                pass
    return out


def check_plan_buttons(cfg_lines: list[str], mod_file: Path) -> None:
    """Refuse the launch of a plan that carries player 2's button on a package that does not press it.

    An older mod reads `input=T,2` (and 3) from the autorun cfg as a RELEASE -- bit 1 is not looked at and bit 0 of a 3 goes
    with it -- so the run would go on, with the plan's buttons silently gone. A plan file given as `planfile=` is refused by
    the mod itself; a cfg line is not, so the check is here, where the line is about to be handed over."""
    masks = plan_masks(cfg_lines)
    if any(m & 2 for m in masks) and not mod_presses_player2(Path(mod_file)):
        raise WorkerError(f"the plan carries player 2's button (input mask with bit 1), and {Path(mod_file).name} does not press it "
                          f"(an older package reads such a mask as a release); give it a package built with the plan-bit-1 input path")
    if any(m & ~3 for m in masks):
        raise WorkerError("the plan holds buttons no version of the mod drives (an input mask above 3)")


SW_SHOWMINNOACTIVE = 7
CREATE_NO_WINDOW = 0x08000000


class WorkerError(RuntimeError):
    pass


class _Reopen(Exception):
    """A situation reopening fixes (CTD / a finished session). Never shown to callers."""


def _log(msg: str) -> None:
    print(msg, flush=True)


def wall_event(kind: str, detail: str = "") -> None:
    """Name, in one line, THE FACT THAT A WALL-CLOCK THRESHOLD CHANGED THE PATH.

    With the same build, the same level and the same cold run, the iteration count
    wobbles between 31 / 57 / failure (2026-08-11). DP produces output identical down
    to the MD5 for the same `--start` even when the thread count changes, so what
    wobbles is GD or this loop. Whether we came through here is the only direct
    evidence for "load changed the path", so always say it out loud.
    """
    _log(f"  [wall] {kind}" + (f": {detail}" if detail else ""))


# ---------- processes ----------

def _tasklist_has(image: str) -> bool:
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {image}", "/NH"],
                         capture_output=True, text=True, errors="replace",
                         creationflags=CREATE_NO_WINDOW).stdout
    return image.lower() in out.lower()


def taskkill_image(image: str) -> None:
    # The exe name is unique per worker (GeometryDash-worker-98.exe), so firing at it
    # with /IM cannot hit someone else's worker or the user's own GD
    subprocess.run(["taskkill", "/F", "/IM", image], capture_output=True,
                   text=True, creationflags=CREATE_NO_WINDOW)


def _wait_gone(image: str, timeout_s: float = 10.0) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        if not _tasklist_has(image):
            return True
        time.sleep(0.25)
    return not _tasklist_has(image)


def processes_under(directory: Path) -> list[int]:
    """PIDs of the live processes launched from under that worker directory."""
    import psutil
    root = str(directory).lower()
    pids = []
    for p in psutil.process_iter(["pid", "exe"]):
        exe = p.info.get("exe")
        if exe and exe.lower().startswith(root):
            try:
                if p.is_running() and p.status() != psutil.STATUS_ZOMBIE:
                    pids.append(p.info["pid"])
            except psutil.Error:
                pass
    return pids


def _argless(procs: list[tuple[int, list[str]]]) -> list[int]:
    """The PIDs among (pid, command line) pairs that were started without our data-root argument."""
    key = f"--geode:{MOD_ID}.data-root="
    return [pid for pid, cmd in procs if not any(a.startswith(key) for a in cmd)]


def argless_processes(worker_dir: Path) -> list[int]:
    """PIDs of this worker's GDs that carry no --geode:<mod>.data-root argument.

    That is what GeodeUpdater leaves behind when it applies a staged Geode update: it restarts the game itself,
    without our arguments, and that game waits in the menu with no data root."""
    import psutil
    procs = []
    for pid in processes_under(worker_dir):
        try:
            procs.append((pid, psutil.Process(pid).cmdline()))
        except psutil.Error:
            pass
    return _argless(procs)


def wait_argless(worker_dir: Path, timeout_s: float) -> list[int]:
    """Wait up to timeout_s for an argless GD of this worker to appear (the updater starts it a few seconds after
    the launching game exits); [] when none did."""
    deadline = time.time() + timeout_s
    while True:
        pids = argless_processes(worker_dir)
        if pids or time.time() >= deadline:
            return pids
        time.sleep(1.0)


def stop_worker_processes(worker_dir: Path, image: str | None = None) -> None:
    """Reliably remove this worker's GD.

    "quit" in cmd.txt is not enough: a finished session no longer runs
    pollCommandFile, so it squats there and blocks the next launch.
    """
    if image:
        taskkill_image(image)
    for pid in processes_under(worker_dir):
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True,
                       text=True, creationflags=CREATE_NO_WINDOW)
    if image:
        _wait_gone(image)


# ---------- crashes ----------
#
# Geode's crash handler writes <worker>/geode/crashlogs/<time>.log (and an empty `last-crashed` marker), then opens a
# window titled CRASH_WINDOW_TITLE that keeps the game's process alive until someone closes it. A launch that watches
# only result.txt learns of the crash when the file has been quiet for stall_s; these notice it on the next poll.

CRASH_WINDOW_TITLE = "Geode Crash Handler"

# The mod's exe guard (src/mod/exe_guard.cpp) writes a line with this phrase to the Geode log when the game is not the executable the
# package was built for, and leaves every hook off. Nothing else is wrong then: the game comes up, no session ever starts, and a
# launch that watches result.txt waits out its stall timer (40 minutes in cold_regress). The harness cannot tell the two builds
# of one game version apart from the package alone (both are gd.win 2.209), so the mod's own line is what it reads.
EXE_GUARD_PHRASE = "is not the one this mod was built for"
_GUARD_HEAD_BYTES = 256 * 1024
_GUARD_WATCH_S = 180.0


def geode_logs(worker_dir: Path) -> set[str]:
    """Names of the logs under this worker's geode/logs."""
    try:
        return {p.name for p in (Path(worker_dir) / "geode" / "logs").iterdir() if p.is_file() and p.suffix == ".log"}
    except OSError:
        return set()


def crash_logs(worker_dir: Path) -> set[str]:
    """Names of the crash logs under this worker's geode/crashlogs (the `last-crashed` marker is not one)."""
    try:
        return {p.name for p in (Path(worker_dir) / "geode" / "crashlogs").iterdir()
                if p.is_file() and p.suffix == ".log"}
    except OSError:
        return set()


def crash_summary(log: Path, frames: int = 3) -> str:
    """One line out of a Geode crash log: the exception, the module it was in, the mod blamed and the top stack frames."""
    try:
        text = Path(log).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return "(the crash log could not be read)"
    def field(label: str) -> str:
        for ln in text.splitlines():
            if ln.startswith(label):
                return ln[len(label):].strip()
        return "?"
    top: list[str] = []
    lines = text.splitlines()
    for i, ln in enumerate(lines):
        if ln.startswith("== Stack Trace"):
            top = [x[2:].strip() for x in lines[i + 1:i + 1 + 12] if x.startswith("- ")][:frames]
            break
    return (f"exception {field('Exception Code:')} in {Path(field('Faulty Module:')).name}, mod {field('Faulty Mod:')}"
            + (f"; top frames: {' <- '.join(top)}" if top else ""))


def _window_titles(pids: list[int]) -> list[str]:
    """Titles of the visible top-level windows owned by these processes."""
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    want = set(pids)
    titles: list[str] = []
    enum_proc = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)

    def visit(hwnd, _):
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value in want and user32.IsWindowVisible(hwnd):
            n = user32.GetWindowTextLengthW(hwnd)
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            titles.append(buf.value)
        return True

    user32.EnumWindows(enum_proc(visit), 0)
    return titles


def wer_pids(argvs) -> set[int]:
    """The process ids that WerFault.exe command lines (each an argv list) report on: the value after `-p`, and after `-ip`
    (the initiating process). The forms Windows is known to use for a crash: `-u -p 7 -s 912`, `-pss -s 580 -p 7 -ip 7`
    (written from the invocations seen in Windows logs; a live WerFault.exe of this machine has not been read yet)."""
    out: set[int] = set()
    for argv in argvs:
        argv = list(argv)
        for i, a in enumerate(argv[:-1]):
            if a.lower() in ("-p", "-ip") and argv[i + 1].isdigit():
                out.add(int(argv[i + 1]))
    return out


def _wer_handled_pids() -> set[int]:
    """The processes a WerFault.exe that is running right now reports on. Cheap while there is none: only the image name is read."""
    import psutil
    argvs = []
    for p in psutil.process_iter(["name"]):
        try:
            if (p.info.get("name") or "").lower() == "werfault.exe":
                argvs.append(p.cmdline())
        except psutil.Error:
            pass
    return wer_pids(argvs)


_CRASH_STATUS = {0xC0000005: "access violation", 0xC0000409: "fail-fast (abort, std::terminate, stack buffer overrun)",
                 0xC0000374: "heap corruption", 0xC00000FD: "stack overflow", 0xC0000142: "a DLL failed to initialise"}


def crash_exit_status(code) -> str:
    """Why an exit code is a crash, or "" when it is not: an NTSTATUS error (0xC0000000 and up). A normal exit, and the exit
    status 1 that a stopped or quit game leaves, are not."""
    if code is None:
        return ""
    u = int(code) & 0xFFFFFFFF
    if u < 0xC0000000:
        return ""
    return f"the game exited with status 0x{u:08X}" + (f" ({_CRASH_STATUS[u]})" if u in _CRASH_STATUS else "")


class CrashWatch:
    """Notices a crash in a worker the poll after it happens: a Geode crash (a new crash log, or Geode's crash window) and a
    crash Geode never sees (Windows Error Reporting taking one of the worker's processes). Make it before the launch: the
    crash logs that already exist are not this launch's. check() returns "" or why the launch has crashed."""

    def __init__(self, worker_dir: Path, window_every_s: float = 1.0, titles=None, clock=time.time, wer=None):
        self.dir = Path(worker_dir)
        self.before = crash_logs(self.dir)
        self.logs_before = geode_logs(self.dir)
        self._t0 = clock()
        self.window_every_s = window_every_s
        self._titles = titles or _window_titles
        self._wer = wer or _wer_handled_pids
        self._clock = clock
        self._next_window = 0.0
        self.reason = ""

    def check(self) -> str:
        new = sorted(crash_logs(self.dir) - self.before)
        if new:
            self.reason = f"Geode wrote a crash log ({new[-1]}): " + crash_summary(self.dir / "geode" / "crashlogs" / new[-1])
            return self.reason
        now = self._clock()
        if now - self._t0 <= _GUARD_WATCH_S:
            refused = self._exe_guard_refusal()
            if refused:
                self.reason = refused
                return refused
        if now >= self._next_window:
            self._next_window = now + self.window_every_s
            pids = processes_under(self.dir)
            if pids and any(CRASH_WINDOW_TITLE in t for t in self._titles(pids)):
                self.reason = f"the '{CRASH_WINDOW_TITLE}' window is open"
                return self.reason
            hit = sorted(set(pids) & self._wer()) if pids else []
            if hit:
                self.reason = (f"Windows Error Reporting is handling a crash of the worker (WerFault.exe -p {hit[0]}): "
                               "Geode's handler never saw it")
                return self.reason
        return ""

    def _exe_guard_refusal(self) -> str:
        """The mod's refusal of this executable, read from the head of this launch's own Geode log (the first line of it,
        the line the mod wrote), or ""."""
        for name in sorted(geode_logs(self.dir) - self.logs_before)[-1:]:
            try:
                with open(self.dir / "geode" / "logs" / name, "rb") as f:
                    head = f.read(_GUARD_HEAD_BYTES).decode("utf-8", errors="replace")
            except OSError:
                continue
            for line in head.splitlines():
                if EXE_GUARD_PHRASE in line:
                    return ("the mod refused this game's executable and left its hooks off (exe guard): "
                            + line.split("]:", 1)[-1].strip()
                            + " -- the package and the worker must be the same build of the game "
                              "(a worker of another build refuses or silently ignores the package)")
        return ""


def ensure_steam() -> None:
    """Without Steam, GD wedges silently instead of failing. Name it and fail early."""
    if _tasklist_has("steam.exe"):
        return
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
            steam_exe = winreg.QueryValueEx(k, "SteamExe")[0].replace("/", "\\")
        subprocess.Popen([steam_exe, "-silent"])
        for _ in range(60):
            time.sleep(1)
            if _tasklist_has("steam.exe"):
                time.sleep(5)  # wait for the client to finish starting up
                return
    except Exception:
        pass
    raise WorkerError(
        "Steam is not running. Without it a GD worker does not fail, it wedges -- "
        "and cannot even be killed. Start Steam and try again.")


# ---------- saves ----------

def save_readable(path: Path) -> bool:
    """Whether the GD save is readable. Killing GD mid-save truncates it."""
    if not path.exists():
        return False
    try:
        _decode_save(path.read_bytes())
        return True
    except Exception:
        return False


def repair_save(worker_id: int) -> None:
    """Repair a corrupt save. These are disposable profiles, so be rough with them.

    Without the repair, GD crashes at launch and takes the whole batch down with it.
    """
    root = gd_save_root(worker_id)
    primary = root / "CCGameManager.dat"
    backup = root / "CCGameManager2.dat"
    if primary.exists() and not save_readable(primary):
        if save_readable(backup):
            shutil.copyfile(backup, primary)
            _log(f"worker-{worker_id}: corrupt save repaired from CCGameManager2.dat")
        else:
            primary.unlink()
            _log(f"worker-{worker_id}: corrupt save discarded (regenerating minimal)")
    if not primary.exists():
        # Build a minimal save. Fullscreen has been measured at +454MiB of private
        # bytes, so never let it fall back to GD's own default profile.
        root.mkdir(parents=True, exist_ok=True)
        blob = _encode_save(_MINIMAL_SAVE_XML)
        primary.write_bytes(blob)
        backup.write_bytes(blob)
    elif primary.stat().st_size > 1000:
        # Do not rewrite the profile behind the user's back (some workers are
        # hand-configured; the ones for human demos are 16:9). But do not stay silent
        # either: look here before you start suspecting memMB=.
        _log(f"worker-{worker_id}: hand-configured GD profile "
             f"({primary.stat().st_size} B vs ~150 B minimal, measured +400MB private)")
    # The resolution key alone is re-inserted even into existing disposable saves.
    # Without it GD comes up at 4:3, and the different screen width shifts the firing
    # tick of camera-driven triggers (read the note on MINIMAL_SAVE_XML). This was the
    # cause of the same plan being "clear on one worker and DEAD on another".
    if primary.stat().st_size <= 1000 and not gdsave.has_resolution(worker_id):
        gdsave.set_resolution_index(worker_id, 25)
        _log(f"worker-{worker_id}: no resolution set, wrote 25 (16:9) -- at 4:3 "
             f"the moving-geometry checks cannot be trusted")


def ensure_appid_480(worker_dir: Path, worker_id: int) -> None:
    """Grabbing 322170 makes the user's Steam GD unable to launch (launch-time only).

    480 is Valve's public test appid (Spacewar). SteamAPI_Init still passes and the
    behaviour is the same. The original value is kept alongside, so this is reversible.
    """
    f = worker_dir / "steam_appid.txt"
    if not f.exists():
        f.write_text("480", encoding="ascii", newline="")
        return
    cur = f.read_text(encoding="utf-8-sig").strip()
    if cur == "480":
        return
    bak = worker_dir / "steam_appid.txt.bak322170"
    if not bak.exists():
        shutil.copyfile(f, bak)
    f.write_text("480", encoding="ascii", newline="")
    _log(f"worker-{worker_id} steam_appid {cur} -> 480")


# ---------- MOD ----------

def snapshot_mod(mod_file: Path) -> tuple[Path, str]:
    """Pin the .geode used for a measurement, keyed by a content hash.

    The main line can rebuild at any time. If the binary is swapped mid-session, the
    "observations obtained within one session" turn out to be from a different build.
    """
    mod_file = Path(mod_file)
    if not mod_file.exists():
        raise WorkerError(f"mod not found: {mod_file} (build it first)")
    data = mod_file.read_bytes()
    digest = hashlib.sha1(data).hexdigest()[:12]
    MOD_CACHE.mkdir(parents=True, exist_ok=True)
    snap = MOD_CACHE / f"gdsolver-{digest}.geode"
    if not snap.exists():
        snap.write_bytes(data)
    return snap, digest


def _spawn(exe: Path, worker_dir: Path, data_root: Path,
           minimized: bool = True) -> subprocess.Popen:
    """Launch minimised, so as not to steal the foreground.

    DO NOT TAKE THE ROUTE OF FIXING THIS WITH SetForegroundWindow: it has been measured
    to race with window creation and freeze the worker before it advances a single
    frame. The price of minimising is that PrintWindow comes out pure white (pass
    minimized=False when you want screenshots).
    """
    si = subprocess.STARTUPINFO()
    if minimized:
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = SW_SHOWMINNOACTIVE
    # Geode's setting override is --geode:<mod-id>.<key>=<value>, so the mod id
    # belongs to mod.json and not to this line. It was spelled out here, and when
    # the id changed the override started naming a mod that does not exist: the
    # worker launched, the mod loaded, and then sat at the menu forever because it
    # never learned where its data root was. Nothing reported an error -- the
    # setting was simply addressed to nobody.
    arg = f"--geode:{MOD_ID}.data-root=" + str(data_root).replace("\\", "/")
    return subprocess.Popen([str(exe), arg], cwd=str(worker_dir), startupinfo=si)


# A worker can keep packages of its own in geode/mods: <worker>/keep_mods.txt, one file name a line (blank lines and lines starting with # are
# ignored). _prepare leaves those in place when it empties the folder of every package but the solver's. File names only: an entry with a path
# separator or "..", or one that is not a .geode, is refused. No file, no list: everything else is removed, as it always was.
KEEP_MODS_FILE = "keep_mods.txt"


def read_keep_mods(worker_root: Path) -> tuple[set[str], list[str]]:
    """The package file names a worker asks to keep, and the entries refused (not a bare *.geode name)."""
    path = Path(worker_root) / KEEP_MODS_FILE
    if not path.exists():
        return set(), []
    keep: set[str] = set()
    refused: list[str] = []
    for raw in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        name = raw.strip()
        if not name or name.startswith("#"):
            continue
        if "/" in name or "\\" in name or ".." in name or ":" in name or not name.lower().endswith(".geode") or Path(name).name != name:
            refused.append(name)
            continue
        keep.add(name)
    return keep, refused


class _WorkerBase:
    def __init__(self, worker_id: int, workers_root: Path = WORKERS_ROOT,
                 min_worker_id: int = 0):
        if worker_id < min_worker_id:
            raise WorkerError(
                f"worker-{worker_id} is off limits here "
                f"(use {min_worker_id} or above)")
        self.worker_id = worker_id
        self.root = Path(workers_root) / f"worker-{worker_id}"
        if not self.root.exists():
            raise WorkerError(
                f"{self.root} does not exist. Run "
                f"`python -m gdtas.provision --worker-id {worker_id}` first.")
        man = worker_manifest(worker_id, workers_root)
        self.exe = Path(man["executable"])
        self.image = self.exe.name
        self.data = Path(man["data_root"])
        self.save_root = Path(man.get("gd_save_root") or gd_save_root(worker_id))

    def _prepare(self, mod_file: Path) -> str:
        """Shared work done just before launch. Returns the hash of the .geode used."""
        check_worker(self.root, self.exe, mod_file=mod_file)   # the frozen build the package is for, or no launch at all
        check_geode(self.root)              # ...and the pinned Geode loader, so no launch applies an update
        snap, digest = snapshot_mod(mod_file)
        mods = self.root / "geode" / "mods"
        mods.mkdir(parents=True, exist_ok=True)
        # A worker's mods folder holds ours and nothing else, so anything else
        # in it is a package under a name we no longer build. Geode would load
        # it alongside this one -- two solvers hooking the same game. The
        # worker's own keep_mods.txt names the packages that stay.
        keep, refused = read_keep_mods(self.root)
        for bad in refused:
            _log(f"  keep_mods.txt of worker-{self.worker_id}: refused {bad!r} (a bare *.geode file name is expected)")
        for name in sorted(keep):
            if name != BUILD_MOD.name and not (mods / name).exists():
                _log(f"  keep_mods.txt of worker-{self.worker_id} names {name}, which is not in geode/mods")
        for stale in mods.glob("*.geode"):
            if stale.name != BUILD_MOD.name and stale.name not in keep:
                stale.unlink()
        shutil.copyfile(snap, mods / BUILD_MOD.name)
        ensure_appid_480(self.root, self.worker_id)
        ensure_steam()
        repair_save(self.worker_id)
        self.data.mkdir(parents=True, exist_ok=True)
        return digest

    def _wait_free(self, timeout_s: float = 15.0) -> None:
        """Wait for this worker to become free; piggybacking is never allowed.

        Two GDs on one data root corrupt result.txt. Give dying processes some grace:
        the previous session returns at session_end, but GD's own cleanup is still
        going on. A genuinely busy worker is still busy 15 seconds later.
        """
        deadline = time.time() + timeout_s
        while True:
            pids = processes_under(self.root)
            if not pids:
                return
            if time.time() >= deadline:
                wall_event("wait-free-timeout",
                           f"worker={self.worker_id} {timeout_s:.0f}s pids={pids}")
                raise WorkerError(
                    f"worker-{self.worker_id} is already running (PID {pids}). "
                    "Two GDs on one data root corrupt result.txt.")
            time.sleep(1.0)


# ---------- disposable session (1 run = 1 process) ----------

@dataclass
class SessionResult:
    data_root: Path
    lines: list[str]
    exit_code: int | None = None
    timed_out: bool = False
    # Why the caller's guard ended the launch ("" when it did not): a level past its
    # deadline, the machine below its memory floor. timed_out is set as well, so the
    # evidence-keeping path for a cut-off level runs unchanged.
    stopped_why: str = ""
    geode: dict | None = None      # the loader this launch ran on: {"version", "dll_sha256"}
    crash: str = ""                # why the launch counts as crashed (a new Geode crash log, or the crash window); "" if not
    # Set when the launch was ended because result.txt stopped growing (stall_s): {"quiet_s", "last_line", "gd_running", "mem_free_gb"} -- what the log last said, how long it
    # had been quiet, how many game processes ran on the machine and how much physical memory was free at that moment. None for any other end.
    stall: dict | None = None
    # The launches ended for never entering a level (run_session's start watchdog) and started again: one stall-shaped dict each ("kind": "start", "launch": 1, plus the
    # stall fields). Empty when the first launch entered its level. A second launch that does not enter either raises WorkerError (INSTRUMENT).
    start_restarts: list = field(default_factory=list)


class _ResultTail:
    """Follow a growing result.txt by what was APPENDED, not by re-reading it.

    run_session polls once a second, and a one-session cold run's log reaches tens of
    MB, so reading the whole file each time grew the reader's memory and I/O with the
    log -- times four arms in one window, which is how a cold window died of a
    MemoryError (an empty exception text) the evening the machine ran low on commit.
    This reads from the last offset, hands on only COMPLETE lines (an unfinished
    last line waits for its newline, so a marker is never seen in halves), and keeps
    nothing but that unfinished line between polls.
    """

    def __init__(self, path: Path):
        self.path = path
        self.pos = 0          # bytes consumed
        self.carry = b""      # an unfinished last line

    def poll(self) -> tuple[str, str]:
        """(the complete new lines, the same plus the unfinished line) since the last
        poll. Both are empty when nothing was appended."""
        try:
            size = self.path.stat().st_size
            if size < self.pos:            # replaced by a shorter file: start over
                self.pos, self.carry = 0, b""
            if size == self.pos:
                return "", ""
            with open(self.path, "rb") as f:
                f.seek(self.pos)
                new = f.read(size - self.pos)
        except OSError:
            return "", ""
        first = self.pos == 0
        self.pos += len(new)
        if first and new.startswith(b"\xef\xbb\xbf"):
            new = new[3:]                  # the stream's BOM, as utf-8-sig drops it
        buf = self.carry + new
        cut = buf.rfind(b"\n") + 1
        self.carry = buf[cut:]
        # ...and CRLF read as "\n", the way read_text's universal newlines did: the MOD
        # writes result.txt with CRLF, and a line-anchored pattern (cold_regress's
        # `^suite: level=... \)$`) does not match a line that still ends in "\r".
        return (buf[:cut].decode("utf-8", errors="replace").replace("\r\n", "\n"),
                buf.decode("utf-8", errors="replace").replace("\r\n", "\n"))


def _stall_snapshot(quiet_s: float, last_line: str) -> dict:
    """What to write down when a launch is ended for a silent result.txt: the quiet time, the last line the log had, how many game processes ran
    on the machine (the load that other sessions put on it) and the free physical memory. A stall is counted from these, not hidden by a relaunch."""
    info = {"quiet_s": round(quiet_s), "last_line": last_line, "gd_running": None, "mem_free_gb": None}
    try:
        import psutil
        info["gd_running"] = sum(1 for p in psutil.process_iter(["name"])
                                 if (p.info.get("name") or "").lower().startswith("geometrydash"))
        info["mem_free_gb"] = round(psutil.virtual_memory().available / 2**30, 1)
    except Exception:       # noqa: BLE001  a snapshot is evidence, never a reason for the launch to fail
        pass
    return info


# A launch that wrote session_start and then nothing for this long, with no level loaded, is a game stuck in the menu (MenuLayer::init hook fired, the level never entered):
# seen three times on 2026-10-22. The stall timer would have kept it for stall_s (cold_regress: 2400 s) and a level deadline only counts from a level's own marker.
START_WATCH_S = 180.0
# What a launch writes before it has done anything with a level: the session's start and its config. A game stuck in the menu leaves exactly these lines (the three of 2026-10-22); a healthy
# launch writes a fourth kind of line (levelsource:, suite: level=, window: level entry ...) within seconds. Judged by what is NOT here, so a package that words its level lines differently still counts.
STARTUP_PREAMBLE = ("session_start", "dpconfig:", "session:")


def level_entered(text: str) -> bool:
    """Whether the result.txt text holds a line beyond the startup preamble (STARTUP_PREAMBLE): the launch did something with its level."""
    return any(ln.strip() and not ln.startswith(STARTUP_PREAMBLE) for ln in text.split("\n"))

def start_stall_line(snap: dict, relaunch: bool) -> str:
    """The line that records a launch ended for never entering a level, written into the session's lines (so the coldlog and the suite output carry it) and to the log.
    It names the launch, the quiet seconds, the last line the log had, the game processes on the machine and the free memory -- the 81e0155c stall dict's fields."""
    return (f"gdtas: start-stall launch={snap.get('launch')} kind=start quiet_s={snap.get('quiet_s')} start_s={snap.get('start_s')} "
            f"gd_running={snap.get('gd_running')} mem_free_gb={snap.get('mem_free_gb')} last_line={snap.get('last_line')!r} "
            f"-> {'relaunched once' if relaunch else 'INSTRUMENT: ended'}")


def run_session(worker_id: int, cfg_lines: list[str], timeout_s: float = 360.0,
                workers_root: Path = WORKERS_ROOT, mod_file: Path = BUILD_MOD,
                minimized: bool = True, stall_s: float = 90.0,
                progress=None, done_marker: str = R.SESSION_END,
                guard=None, start_s: float = START_WATCH_S) -> SessionResult:
    """Run exactly one autorun launch and read result.txt.

    For a plain replay that does not use servemode (attempts=1 quitwhendone=1).
    `input=t,d` lines may be mixed into cfg_lines as they are (the MOD picks them up
    from autorun.cfg).

    `done_marker` is the line that means the launch is finished: a line that BEGINS with it
    (results.marker_line_seen). It defaults to
    `session_end`, which is right for the one-level-per-process arrangement this
    was written for. A `levels=` suite writes one of those per LEVEL, so a reader
    left on the default would take the first level's log for the whole run and
    kill the game with twenty-one levels still to go: it passes `suite: done`.

    `guard`, when given, is called once a poll with this worker's root directory and
    returns "" to go on or a reason to stop. It is the caller's own limit inside the
    launch -- a level's deadline, a memory floor -- and only ever ends THIS launch's
    processes (the ones started from under this worker's directory).

    start_s (0 = off): a launch whose result.txt shows no level entered (level_entered) that many seconds after it started is stopped and started once more;
    each such launch is recorded in SessionResult.start_restarts and as a gdtas: start-stall line at the head of the session's lines. A second launch that does not
    enter a level either raises WorkerError ("INSTRUMENT: ..."): the game, not the plan or the level, is what failed.
    """
    check_plan_buttons(cfg_lines, mod_file)   # before anything is touched: a plan the package would misread does not launch
    w = _WorkerBase(worker_id, workers_root)
    w._wait_free()   # confirm it is free before overwriting .geode (fails if held)
    w._prepare(mod_file)
    geode = geode_identity(w.root)
    _log(f"worker-{worker_id}: Geode {geode['version']} (Geode.dll {str(geode['dll_sha256'])[:12]})")
    result = w.data / "result.txt"
    result.unlink(missing_ok=True)
    # autorun.cfg carries no BOM. A BOM breaks the test on the very first key.
    (w.data / "autorun.cfg").write_bytes(("\n".join(with_worker_keys(cfg_lines)) + "\n").encode("ascii"))

    watch = CrashWatch(w.root)       # before the launch: the crash logs that exist now are not this launch's
    proc = _spawn(w.exe, w.root, w.data, minimized)
    deadline = time.time() + timeout_s
    last_change = time.time()
    tail = _ResultTail(result)
    timed_out, stalled = True, False
    launch_no, t_launch, entered = 1, time.time(), False      # the start watchdog (start_s)
    start_restarts: list[dict] = []
    prelude: list[str] = []
    stall_info: dict | None = None
    last_line = ""
    stopped_why = ""
    exited_early = False
    while time.time() < deadline:
        time.sleep(1.0)
        if proc.poll() is not None:
            timed_out = False
            # The launching game left on its own. Before the done marker it did not finish: either it
            # died, or GeodeUpdater is applying a staged Geode update (see below).
            exited_early = not R.marker_line_seen(done_marker, tail.poll()[0])
            break
        crashed = watch.check()
        if crashed:
            # Geode's crash window keeps the process alive, so the game would sit here until the stall timer ran out.
            wall_event("geode-crash", f"worker={worker_id} {crashed}")
            timed_out = False
            break
        lines_new, seen_new = tail.poll()
        if R.marker_line_seen(done_marker, lines_new):
            timed_out = False
            break
        if not entered and seen_new and level_entered(seen_new):
            entered = True
        if start_s > 0 and not entered and time.time() - t_launch > start_s:
            # The game is up and has not entered a level: stuck in the menu. Stop it and start it once more; a second launch that does the same is the instrument's failure.
            snap = _stall_snapshot(time.time() - last_change, last_line)
            snap.update({"kind": "start", "launch": launch_no, "start_s": round(start_s), "since_launch_s": round(time.time() - t_launch)})
            line = start_stall_line(snap, relaunch=launch_no < 2)
            _log(line)
            wall_event("session-start-stall", f"worker={worker_id} launch {launch_no}: no level entered in {start_s:.0f}s - " + ("relaunching" if launch_no < 2 else "giving up"))
            stop_worker_processes(w.root, w.image)
            try:
                proc.wait(20)
            except subprocess.TimeoutExpired:
                pass
            _wait_gone(w.image, 20)
            start_restarts.append(snap)
            prelude.append(line)
            if launch_no >= 2:
                raise WorkerError(f"INSTRUMENT: worker-{worker_id}: the game started {launch_no} times and never entered a level within {start_s:.0f}s "
                                  f"(last line {snap['last_line']!r}; {snap['gd_running']} game processes, {snap['mem_free_gb']} GB free); "
                                  f"{len(start_restarts)} start-stall record(s) in the log above")
            try:
                result.unlink(missing_ok=True)
            except OSError:
                pass
            elapsed = time.time() - t_launch
            watch = CrashWatch(w.root)
            proc = _spawn(w.exe, w.root, w.data, minimized)
            deadline += elapsed
            launch_no, t_launch, entered = launch_no + 1, time.time(), False
            last_change, last_line = time.time(), ""
            tail = _ResultTail(result)
            continue
        if guard is not None:
            # The lines first, so a guard that keeps "the last iteration / death" sees
            # the poll it is deciding on.
            if progress is not None and lines_new:
                try:
                    progress(lines_new)
                except Exception:
                    pass
                lines_new = ""
            try:
                stopped_why = guard(w.root) or ""
            except Exception:
                stopped_why = ""
            if stopped_why:
                wall_event("session-guard", f"worker={worker_id} {stopped_why}")
                break
        if seen_new:
            last_change = time.time()
            _tail_lines = [ln for ln in seen_new.split("\n") if ln.strip()]
            if _tail_lines:
                last_line = _tail_lines[-1][:200]
            # The section solver RUNS ENTIRELY INSIDE A SINGLE GD FRAME, so during it
            # neither rendering nor the HUD refresh runs (the overlay looks frozen for
            # 20-30 minutes). This is the only place the caller can report progress.
            # `progress` gets the complete lines appended since the last call, not the
            # whole log (see _ResultTail).
            if progress is not None and lines_new:
                try:
                    progress(lines_new)
                except Exception:
                    pass
        # The MOD emits a heartbeat every ~10s. A long silence is not "quiet", it is
        # "stuck"
        if tail.pos > 0 and time.time() - last_change > stall_s:
            stalled = True
            stall_info = _stall_snapshot(time.time() - last_change, last_line)
            wall_event("session-stall",
                       f"worker={worker_id} no result.txt update for {stall_s:.0f}s"
                       f" - aborting")
            break

    if exited_early and not geode_drift_allowed():
        # A launch that applies a staged Geode update exits at once and GeodeUpdater restarts the game a few
        # seconds later WITHOUT our --geode:<mod>.data-root argument: nothing ran, and that game would sit in the
        # menu holding the worker. Say so, and stop it, instead of reporting an empty session.
        stray = wait_argless(w.root, 12.0)
        if stray:
            stop_worker_processes(w.root, w.image)
            _wait_gone(w.image, 20)
            wall_event("geode-update-relaunch", f"worker={worker_id} argless pid(s) {stray} stopped")
            raise WorkerError(
                f"worker-{worker_id}: the launch exited and GeodeUpdater restarted the game without our arguments "
                f"(a staged Geode update was applied); that game was stopped and nothing ran. "
                f"The worker is on Geode {geode_identity(w.root)['version']} now: relaunch, and align the workers "
                f"with the pinned Geode first (gdtas/gdbase.py)")
    if timed_out and not stalled and not stopped_why and proc.poll() is None:
        wall_event("session-timeout",
                   f"worker={worker_id} budget {timeout_s:.0f}s spent")
    if proc.poll() is None:
        stop_worker_processes(w.root, w.image)
        try:
            proc.wait(20)
        except subprocess.TimeoutExpired:
            pass
    code = proc.returncode
    # The exit code is the last clue to "why did it die". The __fastfail family
    # (0xC0000409 = STACK_BUFFER_OVERRUN / 0xC0000374 = HEAP_CORRUPTION) does not pass
    # through the unhandled-exception filter, so this is the only place it gets a name.
    if code:
        _log(f"worker-{worker_id} exit code: 0x{code & 0xFFFFFFFF:08X} ({code})")
    # Stop-Process is asynchronous. Unless we wait for it to really vanish, the next
    # session gets rejected.
    _wait_gone(w.image, 20)
    crash = watch.reason or watch.check()      # a crash after the done marker (the exit path) counts too
    crash = crash or crash_exit_status(code)   # a crash that was answered before anything looked: the status says it
    if crash and not watch.reason:
        wall_event("geode-crash", f"worker={worker_id} {crash}")
    return SessionResult(w.data, prelude + R.read_lines(result), code, timed_out, stopped_why, geode, crash, stall_info,
                         start_restarts=start_restarts)


# ---------- resident session (servemode) ----------

@dataclass
class RunResult:
    outcome: str                 # "death" | "complete" | "stopped" | "timeout"
    tick: int = -1
    x: float = -1.0
    pct: float = 0.0
    goal_x: float = 0.0
    attempt: int = -1
    wall_ms: int = -1
    state: dict = field(default_factory=dict)          # terminal state when stopped
    injected: list[dict] = field(default_factory=list)  # injections that actually fired
    lines: list[str] = field(default_factory=list)      # raw result.txt lines of the run

    def as_dict(self) -> dict:
        d = {
            "outcome": self.outcome, "tick": self.tick, "x": round(self.x, 2),
            "pct": round(self.pct, 3), "goal_x": self.goal_x,
            "attempt": self.attempt, "wall_ms": self.wall_ms,
        }
        if self.state:
            d["state"] = self.state
        # ALWAYS RETURN THE INJECTIONS THAT FIRED. An injection discarded because the
        # run stepped past the given tick vanishes silently, so this prevents
        # "I thought I injected it, but it never took effect"
        if self.injected:
            d["injected"] = self.injected
        return d


class Worker(_WorkerBase):
    """Hold one GD worker open, supply it with plans and receive the results.

    Launching GD takes ~20s, supplying a plan ~3s. Both the search and the observation
    only work because of that difference.
    """

    def __init__(self, worker_id: int, workers_root: Path = WORKERS_ROOT,
                 min_worker_id: int = 0):
        super().__init__(worker_id, workers_root, min_worker_id)
        self.proc: subprocess.Popen | None = None
        self._watch: CrashWatch | None = None
        self.level = 0
        self.cfg: dict[str, str] = {}
        self.mod_digest = ""
        self._initial_plan = ""
        # Running to completion kills the session (the MOD's cmd polling is inside
        # sessionOver). No later rerun ever lands, so this is the signal to reopen.
        self.finished = False

    # ---------- lifecycle ----------

    def is_alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def open(self, level: int, extra_cfg: dict | None = None,
             mod_file: Path = BUILD_MOD, timeout_s: float = 120.0,
             initial_plan: str | Path | None = None) -> dict:
        """Launch the worker and wait until session_start.

        initial_plan is THE PLAN PUT ON THE ATTEMPT AT STARTUP. It is mandatory for a
        session that runs to completion without dying (nodeath): once the startup
        attempt runs to completion, cmd.txt is never read again, so a plan sent later
        never runs at all. A string is the plan body, a Path is a plan file.

        A GD already running under this worker that this object did not start is not
        stopped: open refuses instead. close used to stop whatever ran under the root, so
        a probe opened on a worker another session was using killed that session's run.
        reclaim() is the deliberate way to take a worker back. A GD this object started that
        has exited no longer makes the root its own: what runs there now is someone else's.
        """
        if not self.is_alive():
            pids = processes_under(self.root)
            if pids:
                raise WorkerError(
                    f"worker-{self.worker_id} is running a GD this Worker did not start "
                    f"(PID {pids}); not stopping it. Use another worker, or reclaim() it.")
        self.close()
        self.mod_digest = self._prepare(mod_file)
        self._wait_free()

        # Delete grouptrace* too: if they survive, the reader of the new session adopts
        # THE RECORDING LEFT BEHIND BY THE PREVIOUS RUN as its own (measured: the
        # bootstrap of the 2nd lv19 run was byte-identical to the 1st run's harvest).
        for name in ("result.txt", "cmd.txt", "plan_in.txt", "dump.csv", "trace.csv",
                     "grouptrace.txt", "grouptrace_last.txt"):
            (self.data / name).unlink(missing_ok=True)

        self.level = level
        self.finished = False
        self.cfg = dict(BASE_CFG, level=str(level), **(extra_cfg or {}))
        (self.data / "autorun.cfg").write_bytes(
            "\n".join(f"{k}={v}" for k, v in self.cfg.items()).encode("ascii"))

        if initial_plan is None:
            text = ""
        elif isinstance(initial_plan, Path):
            text = P.format_plan(P.read_inputs(initial_plan))
        else:
            text = str(initial_plan)
        self._initial_plan = text
        # If empty the first attempt is a dummy (instant death) awaiting the first
        # real supply
        P.write_plan(self.data / "plan_in.txt", text)

        self._watch = CrashWatch(self.root)
        self.proc = _spawn(self.exe, self.root, self.data)
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            time.sleep(0.25)
            if not self.is_alive():
                raise WorkerError(f"worker-{self.worker_id} died right after launch "
                                  f"(exit {self.proc.returncode})")
            crashed = self._watch.check()
            if crashed:
                wall_event("geode-crash", f"worker={self.worker_id} {crashed}")
                self.close()
                raise WorkerError(f"worker-{self.worker_id} crashed right after launch: {crashed}")
            if any(R.SESSION_START in l for l in self._read_result()):
                return self.info()
        self.close()
        wall_event("open-timeout",
                   f"worker={self.worker_id} no session_start in {timeout_s:.0f}s")
        raise WorkerError(f"no session_start within {timeout_s}s")

    def close(self) -> None:
        """Stop the GD this object started while it still runs (and what runs under its root
        with it); a Worker whose GD is not running stops nothing (see open)."""
        if self.is_alive():
            stop_worker_processes(self.root, self.image)
        self.proc = None

    def reclaim(self) -> None:
        """Stop every GD under this worker's root, whoever started it."""
        stop_worker_processes(self.root, self.image)
        self.proc = None

    def info(self) -> dict:
        return {
            "worker_id": self.worker_id, "level": self.level,
            "alive": self.is_alive(), "mod_sha1": self.mod_digest,
            "data_root": str(self.data),
            # ...and an override of a base key is part of "what this session is",
            # for the same reason _reopen has to carry it.
            "cfg": {k: v for k, v in self.cfg.items() if BASE_CFG.get(k) != v},
        }

    # ---------- supply ----------

    def _read_result(self) -> list[str]:
        return R.read_lines(self.data / "result.txt")

    def _reopen(self) -> None:
        # EVERY key the caller set, not only the ones BASE_CFG has no name for.
        # An override that shares a name with a base key was dropped here, so the
        # reopened session came back running the DEFAULT and said nothing about
        # it: measured 2026-09-20 on a coin rig, where `coins=1` reverted to the
        # base's 0 and the whole run produced no pickup lines at all. A reopen is
        # invisible from outside (it happens inside run()), so this reads as "the
        # feature did not work" rather than as "the session was restarted".
        extra = {k: v for k, v in self.cfg.items()
                 if k != "level" and BASE_CFG.get(k) != v}
        self.open(self.level, extra, initial_plan=self._initial_plan or None)

    def wait_initial(self, timeout_s: float = 180.0) -> RunResult:
        """Wait for the result of the initial_plan run (with no rerun in between)."""
        return self._collect(0, timeout_s, load_marker=R.SERVE_INITIAL)

    def _collect(self, before: int, timeout_s: float,
                 load_marker: str = R.SERVE_LOADED,
                 load_timeout_s: float | None = None) -> RunResult:
        deadline = time.time() + timeout_s
        load_deadline = (time.time() + load_timeout_s) if load_timeout_s else None
        while time.time() < deadline:
            time.sleep(0.12)
            if not self.is_alive():
                raise _Reopen("died mid-run "
                              f"(exit {self.proc.returncode if self.proc else '?'})")
            crashed = self._watch.check() if self._watch else ""
            if crashed:
                # the crash window keeps the process alive: without this the run waits out its whole budget
                wall_event("geode-crash", f"worker={self.worker_id} {crashed}")
                self.close()
                raise _Reopen(f"crashed mid-run ({crashed})")
            new = self._read_result()[before:]
            for l in new:
                if l.startswith(R.SERVE_FAILED):
                    raise WorkerError("the mod could not read plan_in.txt (empty plan?)")
            # TRUST ONLY WHAT COMES AFTER THE LOAD MARKER. Until the replacement lands,
            # attempts keep running on the previous plan, and their death gets mistaken
            # for this run's result
            mark = R.last_index(new, load_marker)
            if mark < 0:
                # No marker = the MOD is not reading cmd.txt. After a run to completion
                # or after session_end we always land here. Waiting out the whole
                # budget just throws the timeout away.
                if load_deadline and time.time() > load_deadline:
                    wall_event("load-deadline",
                               f"worker={self.worker_id} no `{load_marker}` in "
                               f"{load_timeout_s:.0f}s")
                    raise _Reopen("the rerun never landed (session already over?)")
                continue
            after = new[mark + 1:]
            fired = [R.parse_inject(l) for l in after if l.startswith(R.INJECT)]
            for l in after:
                # LOOK AT stop: BEFORE death:. The cut-off is implemented with
                # destroyPlayer, so a death: line follows as well. The order is what
                # tells them apart
                if l.startswith(R.STOP):
                    d = R.parse_stop(l)
                    return RunResult("stopped", d["tick"], d["x"],
                                     attempt=d["attempt"], state=d["state"],
                                     injected=fired, lines=after)
                if l.startswith(R.DEATH):
                    d = R.parse_death(l)
                    return RunResult("death", d["tick"], d["x"],
                                     attempt=d["attempt"], wall_ms=d["wall_ms"],
                                     injected=fired, lines=after)
                if l.startswith(R.COMPLETE):
                    d = R.parse_complete(l)
                    self.finished = True   # this session accepts no more commands
                    return RunResult("complete", d["tick"], d["x"],
                                     pct=d["pct"] or 0.0, goal_x=d["goal_x"],
                                     attempt=d["attempt"], lines=after)
        wall_event("run-timeout",
                   f"worker={self.worker_id} no verdict in {timeout_s:.0f}s")
        return RunResult("timeout", lines=self._read_result()[before:])

    def _supply(self, text: str, timeout_s: float,
                load_timeout_s: float) -> RunResult:
        before = len(self._read_result())
        P.write_plan(self.data / "plan_in.txt", text)
        (self.data / "cmd.txt").write_text("rerun", encoding="ascii")
        return self._collect(before, timeout_s, R.SERVE_LOADED, load_timeout_s)

    def run(self, plan: list[tuple[int, int]], timeout_s: float = 120.0,
            load_timeout_s: float = 20.0, relaunch: bool = True,
            injects: list[dict] | None = None,
            stop_at: int | None = None) -> RunResult:
        """Run one plan and wait until it dies / runs to completion / hits the cut-off.

        A CTD during the run and "a finished session that accepts no commands" are both
        fixed by reopening. A reopen is ~2s measured, so retry silently exactly once.
        """
        text = P.format_plan(plan, injects, stop_at)
        for attempt in range(2):
            if self.finished or not self.is_alive():
                if attempt or not relaunch:
                    raise WorkerError("no session is open (call open first)")
                self._reopen()
            try:
                return self._supply(text, timeout_s, load_timeout_s)
            except _Reopen as e:
                if attempt or not relaunch:
                    raise WorkerError(f"worker-{self.worker_id}: {e}")
                # Reopening makes GD reload the level = the next run is THE FIRST
                # ATTEMPT OF THAT SESSION. The flight band (pmin/pmax) and the attempt
                # number are reset here, so this marker lets you track whether the
                # path changed.
                wall_event("relaunch", f"worker={self.worker_id} {e}")
                self._reopen()
        raise WorkerError("unreachable")

    def command(self, cmd: str, timeout_s: float = 10.0) -> list[str]:
        """Send one cmd.txt command and return the lines it added to result.txt.

        The escape hatch for pause / resume / step N / status / diag. The MOD only
        reads at ~5Hz, so a round trip costs about 0.2s.
        """
        if not self.is_alive():
            raise WorkerError("no session is open")
        before = len(self._read_result())
        (self.data / "cmd.txt").write_text(cmd, encoding="ascii")
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            time.sleep(0.1)
            lines = self._read_result()
            if len(lines) > before:
                return lines[before:]
        return []
