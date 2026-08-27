"""Run ONE in-process solve (cfg dpsolve=1) on a worker and dump its result.txt.

    python py/solve_probe.py --level 1474319 --worker 95 --iters 20

Investigation tool: it answers "how far does this level get, and does the loop stall
because the model cannot plan past a point (PARTIAL tails) or because the game refuses
what the model planned (SOLVED tails dying)". Nothing here writes to the repo's data
directory -- each worker keeps its own data root, which is where the artefacts land.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas.worker import BASE_CFG, run_session


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--level", type=int, required=True)
    ap.add_argument("--worker", type=int, required=True)
    ap.add_argument("--iters", type=int, default=20)
    ap.add_argument("--timeout", type=float, default=900.0)
    # While the solver thread works the level is frozen, and a frozen level emits no
    # heartbeat, so result.txt stops growing for as long as one DP call takes. The
    # default 90 s stall guard is tuned for replays and would abort a healthy search.
    ap.add_argument("--stall", type=float, default=420.0)
    ap.add_argument("--horizon", type=int, default=0)
    # A worker's save is a generated minimal one, so it holds no downloaded levels and
    # `level=<id>` alone gives "error: level not found". Hand it the object string
    # instead (py/extract_custom_level.py pulls it out of the player's own save).
    ap.add_argument("--levelfile", default="")
    a = ap.parse_args(argv)

    cfg = dict(BASE_CFG)
    cfg.update({
        "level": str(a.level),
        "dpsolve": "1",
        "dpmaxiters": str(a.iters),
        "dpshow": "0",        # headless: never turn the run into a showing
        "servemode": "0",     # the loop drives; do not stop at attempt heads
        "quitwhendone": "1",
        "notrace": "0",       # we want trace.csv / dump.csv for the diff
    })
    if a.horizon:
        cfg["dphorizon"] = str(a.horizon)
    if a.levelfile:
        cfg["levelfile"] = str(Path(a.levelfile).resolve()).replace("\\", "/")
    lines = [f"{k}={v}" for k, v in cfg.items()]

    t0 = time.time()
    res = run_session(a.worker, lines, timeout_s=a.timeout, stall_s=a.stall)
    print(f"=== level {a.level} on worker {a.worker}: "
          f"{time.time() - t0:.0f}s, timed_out={res.timed_out} ===")
    print(f"data_root: {res.data_root}")
    for line in res.lines:
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
