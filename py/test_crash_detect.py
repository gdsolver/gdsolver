"""Noticing a Geode crash the poll after it happens, checked without GD.

What is checked:
  * crash_logs lists the `.log` files of geode/crashlogs and not the `last-crashed` marker; a missing folder is no logs;
  * crash_summary names the exception, the module, the mod blamed and the top stack frames of a crash log;
  * CrashWatch ignores the logs that existed before the launch, reports a new one, and reports the crash window (title
    "Geode Crash Handler" on a window of the worker's processes) at most once a second, and no other window;
  * run_session ends a launch whose crash window stays open (the process stays alive) on the next poll instead of
    waiting for the stall timer, says so in SessionResult.crash, and does not call a quiet launch a crash;
  * Worker._collect turns a crash into a reopen that names it;
  * a crash Geode never sees (fail-fast, abort(): no crash log, no Geode window) shows as a WerFault.exe reporting on one of
    the worker's processes: wer_pids reads `-p` / `-ip` out of its command line, CrashWatch reports a worker process in
    that set (once a second, and not a process of someone else's), run_session ends the launch on it, and an exit status
    of 0xC0000409 / 0xC0000005 is a crash while 0, 1 and a signal-less stop are not.

usage: python py/test_crash_detect.py   (exit 1 on the first failure)
"""
import json
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdtas import worker as W       # noqa: E402

LOG = """2026-10-16T02:25:19.9445419+0000
Whoopsies! An unhandled exception has occurred.

== Geode Information ==
Loader Version: v5.11.2

== Exception Information ==
Faulty Module: D:\\workers\\worker-7\\GeometryDash-worker-7.exe
Faulty Mod: <Unknown>
Exception Code: c0000005 (EXCEPTION_ACCESS_VIOLATION)
Crashed thread: Main

== Stack Trace (the most important part) ==
- GeometryDash-worker-7.exe + 0x6289d (sub_62830 + 0x0)
- mod.dll + 0x1067a6 (audio::sync + 0x146)
- mod.dll + 0x10f6cb (hook::update + 0x25bb)
- 0x2cb23c03059 (hook handler)

== Register States ==
RAX: 0x0
"""


def check(cond: bool, what: str) -> None:
    if not cond:
        print("FAIL:", what)
        sys.exit(1)
    print("ok:  ", what)


def make_worker(root: Path, worker_id: int = 7) -> None:
    (root / "geode").mkdir(parents=True, exist_ok=True)
    (root / "session" / "data").mkdir(parents=True, exist_ok=True)
    (root / "worker.json").write_text(json.dumps({
        "worker_id": str(worker_id), "executable": str(root / f"GeometryDash-worker-{worker_id}.exe"),
        "data_root": str(root / "session" / "data"), "geode_root": str(root / "geode"), "gd_save_root": str(root / "save"),
    }), encoding="ascii")


def main() -> int:
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        root = td / "worker-7"
        make_worker(root)
        crashdir = root / "geode" / "crashlogs"

        # ---- crash_logs / crash_summary
        check(W.crash_logs(root) == set(), "no crashlogs folder: no logs")
        crashdir.mkdir(parents=True)
        (crashdir / "last-crashed").write_bytes(b"")
        check(W.crash_logs(root) == set(), "the last-crashed marker is not a log")
        (crashdir / "2026-10-16_02-25-19.9445782.log").write_text(LOG, encoding="utf-8")
        check(W.crash_logs(root) == {"2026-10-16_02-25-19.9445782.log"}, "a .log file is a crash log")
        s = W.crash_summary(crashdir / "2026-10-16_02-25-19.9445782.log")
        check("c0000005 (EXCEPTION_ACCESS_VIOLATION)" in s and "GeometryDash-worker-7.exe" in s and "<Unknown>" in s, "the summary names exception, module and mod: " + s)
        check("audio::sync" in s and "hook handler" not in s, "the summary carries the top three frames only")
        check(W.crash_summary(crashdir / "gone.log").startswith("(the crash log"), "an unreadable log is said to be unreadable")

        # ---- CrashWatch
        titles = {"now": []}
        clock = {"t": 100.0}
        pids = {"p": []}
        W_processes = W.processes_under
        W.processes_under = lambda d: list(pids["p"])
        try:
            w = W.CrashWatch(root, titles=lambda p: list(titles["now"]), clock=lambda: clock["t"])
            check(w.check() == "", "a crash log that existed before the launch is not this launch's")
            pids["p"] = [1234]
            titles["now"] = ["Geometry Dash", "Some other window"]
            check(w.check() == "", "other windows are no crash")
            clock["t"] += 0.5
            titles["now"] = ["Geode Crash Handler"]
            check(w.check() == "", "the window is looked at once a second, not every poll")
            clock["t"] += 1.0
            why = w.check()
            check("Geode Crash Handler" in why, "the crash window is a crash: " + why)
            w2 = W.CrashWatch(root, titles=lambda p: [], clock=lambda: clock["t"])
            (crashdir / "2026-10-16_03-00-00.0000000.log").write_text(LOG, encoding="utf-8")
            why = w2.check()
            check("2026-10-16_03-00-00.0000000.log" in why and "c0000005" in why, "a new crash log is a crash and is summarised: " + why)
            w3 = W.CrashWatch(root, titles=lambda p: ["Geode Crash Handler"], clock=lambda: clock["t"])
            pids["p"] = []
            check(w3.check() == "", "a crash window title with no process of the worker is not looked for")

            # ---- Windows Error Reporting: a crash Geode never sees
            check(W.wer_pids([["C:\\WINDOWS\\system32\\WerFault.exe", "-u", "-p", "39960", "-s", "1252"]]) == {39960},
                  "wer_pids reads the process id after -p")
            check(W.wer_pids([["WerFault.exe", "-pss", "-s", "580", "-p", "7", "-ip", "8"]]) == {7, 8},
                  "wer_pids reads -p and -ip of the snapshot form")
            check(W.wer_pids([["WerFault.exe"], ["WerFault.exe", "-p", "x"], ["WerFault.exe", "-p"]]) == set(),
                  "no id, a non-number and a trailing -p name no process")
            check(isinstance(W._wer_handled_pids(), set), "the live read of WerFault.exe processes returns a set (none is fine)")
            check(W.crash_exit_status(3221226505).startswith("the game exited with status 0xC0000409") and "fail-fast" in W.crash_exit_status(3221226505),
                  "exit status 0xC0000409 is a crash and is named")
            check("access violation" in W.crash_exit_status(0xC0000005), "exit status 0xC0000005 is a crash and is named")
            check("0xC0000999" in W.crash_exit_status(0xC0000999) and "(" not in W.crash_exit_status(0xC0000999), "an unnamed NTSTATUS error is still a crash")
            check(W.crash_exit_status(0) == "" and W.crash_exit_status(1) == "" and W.crash_exit_status(None) == "",
                  "exit status 0, 1 and none (still running) are not a crash")
            wer = {"now": set()}
            pids["p"] = [1234]
            titles["now"] = []
            w4 = W.CrashWatch(root, titles=lambda p: list(titles["now"]), clock=lambda: clock["t"], wer=lambda: set(wer["now"]))
            clock["t"] += 2.0
            check(w4.check() == "", "a WerFault.exe reporting on nobody is no crash")
            wer["now"] = {4321}
            clock["t"] += 2.0
            check(w4.check() == "", "a WerFault.exe reporting on someone else's process is no crash of this worker")
            wer["now"] = {4321, 1234}
            clock["t"] += 0.5
            check(w4.check() == "", "the WerFault.exe read happens once a second, with the window check")
            clock["t"] += 1.0
            why = w4.check()
            check("Windows Error Reporting" in why and "WerFault.exe -p 1234" in why and "never saw it" in why,
                  "a WerFault.exe reporting on a process of the worker is a crash: " + why)
            check(w4.reason == why, "and the reason is kept for the end of the launch")
            pids["p"] = []
            w5 = W.CrashWatch(root, titles=lambda p: [], clock=lambda: clock["t"], wer=lambda: {1234})
            check(w5.check() == "", "a WerFault.exe is not looked for when the worker has no process")

            # ---- the mod's exe guard: a launch whose mod refused the executable is reported at once
            logdir = root / "geode" / "logs"
            logdir.mkdir(parents=True)
            guard_line = ("07:26:43 WARN  [Main] [GD Solver]: gdsolver: this Geometry Dash build (GD v1792203143, libcocos2d 6ad2d987, "
                          "libExtensions 6ad2d987) is not the one this mod was built for (GD 2.209, first executable); the mod is not loaded")
            (logdir / "Geode 2026-10-20 07.00.00.log").write_text("07:00:00 INFO [Main] [Geode]: an older launch\n" + guard_line + "\n", encoding="utf-8")
            pids["p"] = []
            w6 = W.CrashWatch(root, titles=lambda p: [], clock=lambda: clock["t"])
            check(w6.check() == "", "a Geode log that existed before the launch is not this launch's (its guard line is not read)")
            (logdir / "Geode 2026-10-20 08.00.00.log").write_text("08:00:00 INFO  [Main] [Geode]: Loading mod gdsolver.solver\n", encoding="utf-8")
            check(w6.check() == "", "a new log without the guard line is no refusal")
            (logdir / "Geode 2026-10-20 08.00.00.log").write_text(
                "08:00:00 INFO  [Main] [Geode]: Loading mod gdsolver.solver\n" + guard_line + "\n", encoding="utf-8")
            why = w6.check()
            check("exe guard" in why and "GD v1792203143" in why and "same build of the game" in why,
                  "the guard line in this launch's log is a refusal, with the mod's own words: " + why)
            check(w6.reason == why, "and the reason is kept for the end of the launch")
            clock["t"] += 1000.0
            w7 = W.CrashWatch(root, titles=lambda p: [], clock=lambda: clock["t"])
            (logdir / "Geode 2026-10-20 09.00.00.log").write_text(guard_line + "\n", encoding="utf-8")
            clock["t"] += 200.0
            check(w7.check() == "", "the log is read only for the first 180 seconds of a launch")
        finally:
            W.processes_under = W_processes

        # ---- run_session: a crash window that stays open ends the launch at the next poll
        class FakeProc:
            def __init__(self):
                self.returncode = None
                self.pid = 4242
                self.killed = False

            def poll(self):
                return self.returncode

            def wait(self, t=None):
                return self.returncode

        saved = {k: getattr(W, k) for k in ("_spawn", "stop_worker_processes", "_wait_gone", "CrashWatch", "processes_under")}
        saved_cls = {k: getattr(W._WorkerBase, k) for k in ("_prepare", "_wait_free")}
        sleeps = []
        real_sleep = time.sleep
        try:
            proc = FakeProc()
            W._spawn = lambda exe, d, data, minimized=True: proc
            W._wait_gone = lambda image, t=10.0: True
            def fake_stop(d, image=None):
                proc.returncode = 1
            W.stop_worker_processes = fake_stop
            W._WorkerBase._prepare = lambda self, mod: "x"
            W._WorkerBase._wait_free = lambda self, t=15.0: None
            (root / "geode" / "resources" / "geode.loader").mkdir(parents=True, exist_ok=True)

            # a quiet launch that times out: not a crash, and no waiting for the stall timer to be skipped by mistake
            W.processes_under = lambda d: []
            real_watch = W.CrashWatch
            W.CrashWatch = lambda d: real_watch(d, titles=lambda p: [])
            t0 = time.time()
            def fast_sleep(s):
                sleeps.append(s)
                real_sleep(0.01)
            time.sleep = fast_sleep
            r = W.run_session(7, ["enabled=1"], timeout_s=0.2, workers_root=td, mod_file=Path("x.geode"), stall_s=1000.0)
            check(r.crash == "" and r.timed_out, "a quiet launch that spends its budget is a timeout, not a crash")

            # the crash window opens while the process stays alive
            proc.returncode = None
            n = {"polls": 0}
            def titles_open(p):
                n["polls"] += 1
                return ["Geode Crash Handler"] if n["polls"] >= 3 else []
            W.CrashWatch = lambda d: real_watch(d, window_every_s=0.0, titles=titles_open)
            W.processes_under = lambda d: [4242]
            r = W.run_session(7, ["enabled=1"], timeout_s=60.0, workers_root=td, mod_file=Path("x.geode"), stall_s=1000.0)
            check("Geode Crash Handler" in r.crash, "run_session reports the open crash window in SessionResult.crash: " + r.crash)
            check(not r.timed_out and n["polls"] == 3, "the launch ended on the poll that saw it, not at the timeout")
            check(proc.returncode == 1, "and the game was stopped")

            # the same for a crash Geode never saw: WerFault.exe reports on the game's process, which stays alive
            proc.returncode = None
            n["polls"] = 0
            W.CrashWatch = lambda d: real_watch(d, window_every_s=0.0, titles=lambda p: [], wer=lambda: {4242})
            r = W.run_session(7, ["enabled=1"], timeout_s=60.0, workers_root=td, mod_file=Path("x.geode"), stall_s=1000.0)
            check("Windows Error Reporting" in r.crash and not r.timed_out, "run_session reports the WerFault.exe in SessionResult.crash: " + r.crash)
            check(proc.returncode == 1, "and the game was stopped")

            # a game that already died with a fail-fast status, before anything looked at it, is a crash from its status
            proc.returncode = 3221226505
            W.CrashWatch = lambda d: real_watch(d, titles=lambda p: [], wer=lambda: set())
            W.processes_under = lambda d: []
            r = W.run_session(7, ["enabled=1"], timeout_s=0.2, workers_root=td, mod_file=Path("x.geode"), stall_s=1000.0)
            check("0xC0000409" in r.crash, "a launch whose game exited 0xC0000409 is a crash: " + r.crash)
            proc.returncode = 1
            r = W.run_session(7, ["enabled=1"], timeout_s=0.2, workers_root=td, mod_file=Path("x.geode"), stall_s=1000.0)
            check(r.crash == "", "a launch whose game exited 1 is not")
        finally:
            time.sleep = real_sleep
            for k, v in saved.items():
                setattr(W, k, v)
            for k, v in saved_cls.items():
                setattr(W._WorkerBase, k, v)

        # ---- Worker._collect and Worker.open
        saved = {k: getattr(W, k) for k in ("stop_worker_processes", "processes_under")}
        try:
            W.stop_worker_processes = lambda d, image=None: None
            W.processes_under = lambda d: []
            wk = W.Worker(7, td)
            wk.proc = FakeProc()
            wk._watch = W.CrashWatch(root, titles=lambda p: [])
            (crashdir / "2026-10-16_04-00-00.0000000.log").write_text(LOG, encoding="utf-8")
            try:
                wk._collect(0, 5.0)
                check(False, "_collect should raise on a crash")
            except W._Reopen as e:
                check("crashed mid-run" in str(e) and "c0000005" in str(e), "Worker._collect turns a crash into a reopen: " + str(e))
        finally:
            for k, v in saved.items():
                setattr(W, k, v)
    print("all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
