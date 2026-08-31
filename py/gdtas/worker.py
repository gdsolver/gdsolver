"""Lifecycle management of a single GD worker, and supplying it with plans.

A worker is "a renamed GD + a dedicated Geode root + a dedicated save + a dedicated
data root". GD has no single-instance lock, so any number of them run independently.
Always read the results from the worker's data_root (not from D:\\GD\\data).

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
}

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
        snap, digest = snapshot_mod(mod_file)
        mods = self.root / "geode" / "mods"
        mods.mkdir(parents=True, exist_ok=True)
        # A worker's mods folder holds ours and nothing else, so anything else
        # in it is a package under a name we no longer build. Geode would load
        # it alongside this one -- two solvers hooking the same game.
        for stale in mods.glob("*.geode"):
            if stale.name != BUILD_MOD.name:
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


def run_session(worker_id: int, cfg_lines: list[str], timeout_s: float = 360.0,
                workers_root: Path = WORKERS_ROOT, mod_file: Path = BUILD_MOD,
                minimized: bool = True, stall_s: float = 90.0,
                progress=None, done_marker: str = R.SESSION_END) -> SessionResult:
    """Run exactly one autorun launch and read result.txt.

    For a plain replay that does not use servemode (attempts=1 quitwhendone=1).
    `input=t,d` lines may be mixed into cfg_lines as they are (the MOD picks them up
    from autorun.cfg).

    `done_marker` is the line that means the launch is finished. It defaults to
    `session_end`, which is right for the one-level-per-process arrangement this
    was written for. A `levels=` suite writes one of those per LEVEL, so a reader
    left on the default would take the first level's log for the whole run and
    kill the game with twenty-one levels still to go: it passes `suite: done`.
    """
    w = _WorkerBase(worker_id, workers_root)
    w._wait_free()   # confirm it is free before overwriting .geode (fails if held)
    w._prepare(mod_file)
    result = w.data / "result.txt"
    result.unlink(missing_ok=True)
    # autorun.cfg carries no BOM. A BOM breaks the test on the very first key.
    (w.data / "autorun.cfg").write_bytes(("\n".join(cfg_lines) + "\n").encode("ascii"))

    proc = _spawn(w.exe, w.root, w.data, minimized)
    deadline = time.time() + timeout_s
    last_len, last_change = -1, time.time()
    timed_out, stalled = True, False
    while time.time() < deadline:
        time.sleep(1.0)
        if proc.poll() is not None:
            timed_out = False
            break
        try:
            txt = result.read_text(encoding="utf-8-sig", errors="replace")
        except OSError:
            txt = ""
        if done_marker in txt:
            timed_out = False
            break
        if len(txt) != last_len:
            last_len, last_change = len(txt), time.time()
            # The section solver RUNS ENTIRELY INSIDE A SINGLE GD FRAME, so during it
            # neither rendering nor the HUD refresh runs (the overlay looks frozen for
            # 20-30 minutes). This is the only place the caller can report progress.
            if progress is not None:
                try:
                    progress(txt)
                except Exception:
                    pass
        # The MOD emits a heartbeat every ~10s. A long silence is not "quiet", it is
        # "stuck"
        if last_len > 0 and time.time() - last_change > stall_s:
            stalled = True
            wall_event("session-stall",
                       f"worker={worker_id} no result.txt update for {stall_s:.0f}s"
                       f" - aborting")
            break

    if timed_out and not stalled and proc.poll() is None:
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
    return SessionResult(w.data, R.read_lines(result), code, timed_out)


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
        """
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

        self.proc = _spawn(self.exe, self.root, self.data)
        deadline = time.time() + timeout_s
        while time.time() < deadline:
            time.sleep(0.25)
            if not self.is_alive():
                raise WorkerError(f"worker-{self.worker_id} died right after launch "
                                  f"(exit {self.proc.returncode})")
            if any(R.SESSION_START in l for l in self._read_result()):
                return self.info()
        self.close()
        wall_event("open-timeout",
                   f"worker={self.worker_id} no session_start in {timeout_s:.0f}s")
        raise WorkerError(f"no session_start within {timeout_s}s")

    def close(self) -> None:
        stop_worker_processes(self.root, self.image)
        self.proc = None

    def info(self) -> dict:
        return {
            "worker_id": self.worker_id, "level": self.level,
            "alive": self.is_alive(), "mod_sha1": self.mod_digest,
            "data_root": str(self.data),
            "cfg": {k: v for k, v in self.cfg.items() if k not in BASE_CFG},
        }

    # ---------- supply ----------

    def _read_result(self) -> list[str]:
        return R.read_lines(self.data / "result.txt")

    def _reopen(self) -> None:
        extra = {k: v for k, v in self.cfg.items()
                 if k not in BASE_CFG and k != "level"}
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
