"""The start watchdog of gdtas.worker.run_session: a launch that never enters a level is stopped and started once more, a second one is an INSTRUMENT failure.
No GD: the process, the worker directory and the clock are stood in for.

usage: python py/test_start_watch.py   (exit 1 on the first failure)
"""
import sys
import tempfile
import types
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdtas import worker as W          # noqa: E402

PREAMBLE = "session_start\ndpconfig: dpsolve=1 package=1/abcd\nsession: research capture off\n"


def check(cond, what):
    if not cond:
        print("FAIL:", what)
        sys.exit(1)
    print("ok  ", what)


class FakeProc:
    returncode = 0

    def __init__(self):
        self.stopped = False

    def poll(self):
        return 0 if self.stopped else None

    def wait(self, t=None):
        return 0


def main():
    check(not W.level_entered(PREAMBLE), "the three preamble lines are not a level entered")
    check(W.level_entered(PREAMBLE + "levelsource: id=1 kind=main\n"), "a levelsource line is")
    check(W.level_entered("session_start\nsuite: level=1 (1/22)\n"), "any other line is (a suite marker)")
    check(not W.level_entered(""), "an empty log is not")
    snap = {"launch": 1, "quiet_s": 181, "start_s": 180, "gd_running": 5, "mem_free_gb": 7.5, "last_line": "session: research capture off"}
    ln = W.start_stall_line(snap, relaunch=True)
    check("launch=1" in ln and "quiet_s=181" in ln and "gd_running=5" in ln and "mem_free_gb=7.5" in ln and "relaunched once" in ln and "last_line='session: research" in ln,
          "the record line carries the launch, the quiet seconds, the game count, the free memory and the last line")
    check("INSTRUMENT" in W.start_stall_line(snap, relaunch=False), "the final record says INSTRUMENT")

    tmp = Path(tempfile.mkdtemp(prefix="startwatch_"))
    data = tmp / "data"
    data.mkdir()
    log = {"spawns": 0, "stops": 0}
    scripts = []     # what each launch writes

    class FakeBase:
        def __init__(self, wid, root):
            self.root, self.data, self.exe, self.image = tmp, data, tmp / "gd.exe", "gd.exe"

        def _wait_free(self):
            pass

        def _prepare(self, mod):
            pass

    def fake_spawn(exe, root, d, minimized):
        (d / "result.txt").write_bytes(scripts[log["spawns"]].encode("utf-8"))
        log["spawns"] += 1
        return FakeProc()

    def fake_stop(root, image):
        log["stops"] += 1

    saved = {k: getattr(W, k) for k in ("_WorkerBase", "_spawn", "stop_worker_processes", "_wait_gone", "geode_identity", "check_plan_buttons", "CrashWatch", "wall_event", "with_worker_keys", "_log")}
    shim = types.SimpleNamespace(time=__import__("time").time, sleep=lambda s: __import__("time").sleep(0.02))
    saved_time = W.time
    W._WorkerBase, W._spawn, W.stop_worker_processes = FakeBase, fake_spawn, fake_stop
    W._wait_gone = lambda *a, **k: None
    W.geode_identity = lambda root: {"version": "v0", "dll_sha256": "0" * 64}
    W.check_plan_buttons = lambda *a, **k: None
    W.CrashWatch = type("CW", (), {"__init__": lambda self, root: None, "check": lambda self: "", "reason": ""})
    W.wall_event = lambda *a, **k: None
    W.with_worker_keys = lambda cfg: list(cfg)
    W._log = lambda *a, **k: None
    W.time = shim
    try:
        # (1) the first launch is stuck, the second enters a level and finishes
        scripts[:] = [PREAMBLE, PREAMBLE + "levelsource: id=1 kind=main\nsession_end: level_complete\n"]
        log.update(spawns=0, stops=0)
        r = W.run_session(1, ["enabled=1"], timeout_s=30, start_s=0.3)
        check(log["spawns"] == 2 and log["stops"] >= 1, "a stuck first launch is stopped and the game is started again")
        check(len(r.start_restarts) == 1 and r.start_restarts[0]["launch"] == 1 and r.start_restarts[0]["kind"] == "start"
              and {"quiet_s", "last_line", "gd_running", "mem_free_gb"} <= set(r.start_restarts[0]),
              "the restart is recorded in the stall dict's shape (plus kind and launch)")
        check(r.lines and r.lines[0].startswith("gdtas: start-stall launch=1") and any(l.startswith("session_end") for l in r.lines),
              "the session's lines open with the start-stall record and then carry the second launch's own log")
        # (2) a healthy launch is not touched
        scripts[:] = [PREAMBLE + "levelsource: id=1\nsession_end: ok\n"]
        log.update(spawns=0, stops=0)
        r = W.run_session(1, ["enabled=1"], timeout_s=30, start_s=0.3)
        check(log["spawns"] == 1 and not r.start_restarts and not r.lines[0].startswith("gdtas:"), "a launch that enters its level at once is launched once")
        # (3) both launches stuck: INSTRUMENT, with both stops
        scripts[:] = [PREAMBLE, PREAMBLE]
        log.update(spawns=0, stops=0)
        try:
            W.run_session(1, ["enabled=1"], timeout_s=30, start_s=0.3)
            check(False, "two stuck launches raise")
        except W.WorkerError as e:
            check(str(e).startswith("INSTRUMENT:") and "2 times" in str(e) and log["spawns"] == 2 and log["stops"] >= 2,
                  "two launches that never enter a level end in WorkerError INSTRUMENT, the game stopped twice")
        # (4) off
        scripts[:] = [PREAMBLE + "session_end: x\n"]
        log.update(spawns=0, stops=0)
        r = W.run_session(1, ["enabled=1"], timeout_s=30, start_s=0)
        check(log["spawns"] == 1 and not r.start_restarts, "start_s=0 turns the watchdog off")
    finally:
        for k, v in saved.items():
            setattr(W, k, v)
        W.time = saved_time
    print("all ok")


if __name__ == "__main__":
    main()
