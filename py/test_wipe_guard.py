"""cold_regress.wipe leaves the data of a worker that is running alone -- no GD.

What is pinned:
  * a worker with a GD under it keeps result.txt and the plan files, and the call says so (WorkerError) after the
    launch's own grace period;
  * a worker that is idle is wiped as before (the solution files go, a file the wipe does not name stays);
  * a GD that is only finishing the previous level (gone within the grace period) does not stop the wipe;
  * one() reports a busy worker as an ERROR result, with its files untouched, instead of raising.

usage: python py/test_wipe_guard.py   (exit 1 on the first failure)
"""
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import cold_regress as C          # noqa: E402
from gdtas import paths           # noqa: E402
import gdtas.worker as W          # noqa: E402

FAILED = []


def check(name, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + name + ("" if ok else f"  {detail}"))
    if not ok:
        FAILED.append(name)


def main() -> int:
    saved = (paths.WORKERS_ROOT, W.processes_under, C.time.sleep, C.time.time)
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        data = root / "worker-7" / "session" / "data"
        data.mkdir(parents=True)

        def fill():
            for n in ("result.txt", "dp_plan.txt", "dump.csv", "solution_lv1_dp.txt", "keep_me.txt"):
                (data / n).write_text("x", encoding="ascii")

        paths.WORKERS_ROOT = root
        try:
            # a GD runs under the worker: nothing is deleted, the call refuses after the grace period
            fill()
            clock = [1000.0]
            C.time.time = lambda: clock[0]
            C.time.sleep = lambda s: clock.__setitem__(0, clock[0] + s)
            W.processes_under = lambda d: [4242]
            try:
                C.wipe(7)
                check("busy worker: wipe refuses", False, "no error")
            except W.WorkerError as e:
                check("busy worker: wipe refuses", "already running" in str(e) and "4242" in str(e), str(e))
            check("busy worker: result.txt kept", (data / "result.txt").exists())
            check("busy worker: plan and solution kept", (data / "dp_plan.txt").exists() and (data / "solution_lv1_dp.txt").exists())
            check("busy worker: waited out the grace period", clock[0] - 1000.0 >= 15.0, f"{clock[0] - 1000.0}")

            # one() turns the refusal into an ERROR result and does not raise
            r = C.one(1, 7, 60.0, [], root / "out")
            check("one(): ERROR result for a busy worker", (not r["cleared"]) and r["why"].startswith("ERROR ") and "already running" in r["why"], str(r.get("why")))
            check("one(): files still there", (data / "result.txt").exists())

            # an idle worker is wiped; a file the wipe does not name stays
            W.processes_under = lambda d: []
            C.wipe(7)
            check("idle worker: result.txt gone", not (data / "result.txt").exists())
            check("idle worker: plan, dump and solution gone", not any((data / n).exists() for n in ("dp_plan.txt", "dump.csv", "solution_lv1_dp.txt")))
            check("idle worker: an unnamed file stays", (data / "keep_me.txt").exists())

            # a GD that exits within the grace period does not stop the wipe
            fill()
            calls = [0]

            def finishing(d):
                calls[0] += 1
                return [4242] if calls[0] < 4 else []
            W.processes_under = finishing
            C.wipe(7)
            check("finishing GD: wiped after it left", not (data / "result.txt").exists() and calls[0] == 4, f"calls={calls[0]}")
        finally:
            paths.WORKERS_ROOT, W.processes_under, C.time.sleep, C.time.time = saved
    print("all ok" if not FAILED else f"{len(FAILED)} failed: {FAILED}")
    return 1 if FAILED else 0


if __name__ == "__main__":
    sys.exit(main())
