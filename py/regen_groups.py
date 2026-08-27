# -*- coding: utf-8 -*-
u"""Re-take the moving-geometry recording (`solution_lvNN_dp.txt.groups.txt`) from GD.

FOR RECOVERY. When the solution itself (`solution_lvNN_dp.txt`) is still there
but only the `.groups*.txt` beside it is lost, `verify.py` /
`quick_regress.py` SKIP that level (`no … .groups*.txt`), so the acceptance
gate silently becomes full of holes. That is exactly what happened on
2026-08-21 when D: died and everything moved to C:
(lv19/20/21/22 were SKIPped, and the families looked like they went 21 -> 9).

The body is the bootstrap the loop runs for itself: pass no plan,
run once to completion with `grouptrace=1 / nodeath=1`, and just copy
`grouptrace_last.txt`. IT IS NOT A RE-SOLVE, so the solution does not change at
all.

    python py/regen_groups.py 19 20 21 22          # the base (.groups.txt)
    python py/regen_groups.py 19 20 22 --live      # real replay recording (.groups.live.txt)
    python py/regen_groups.py --all                # every level with a solution in data/

`--live` IS THE IMPORTANT ONE. `--groups` is last-one-wins, and the base (the
nodeath bootstrap) runs A DIFFERENT WORLDLINE FROM THE PLAN (measured, in the
note on the deep recording overlay: with the same plan and the same
worker, merely adding nodeath=1 makes onGround disagree at t=539, and at
t=5,600 it runs a frame 1,300px away). Restoring only the base without
restoring live clearly drops the fidelity of levels with moving geometry --
measured 2026-08-21: tracking of lv22 19,227 -> 16,371 / diverging sections
7 -> 16.

live is RECORDED BY REPLAYING THE PLAN IN GD AS IT IS. Two replays without
`nodeath` are bit-identical even on different workers (same note), so as long
as the plan is intact THE SAME RECORDING AS BACK THEN COMES BACK -- no re-solve
needed.

DO NOT FALL BACK to an in-progress `grouptrace.txt` (its content is decided by
the instant you read it, so adopting it makes a run with the same input stop
reproducing) -- the same promise as upstream.
"""
from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas import plan as P                                     # noqa: E402
from gdtas import results as R                                  # noqa: E402
from gdtas.paths import LEVEL_DATA, WORKERS_ROOT                      # noqa: E402
from gdtas.worker import Worker, WorkerError                    # noqa: E402
from gdtas.solveutil import (copy_held_file, has_grouped_colliders,  # noqa: E402
                             read_lines)


def wait_complete(w: Worker, timeout_s: float) -> bool:
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        time.sleep(2.0)
        if any(l.startswith(R.COMPLETE) for l in R.read_lines(w.data / "result.txt")):
            return True
        if not w.is_alive():
            return False
    return False


def regen_live(level: int, data: Path, worker_id: int, timeout_s: float) -> str:
    u"""Replay the plan in GD as it is and re-record `.groups.live.txt`."""
    plan = data / f"solution_lv{level}_dp.txt"
    if not plan.exists():
        return f"lv{level}: no solution ({plan})"
    objrects = data / f"objrects_lv{level}.txt"
    if not has_grouped_colliders(objrects):
        return f"lv{level}: no moving geometry (no live recording needed)"
    inputs = P.read_inputs(plan)
    if not inputs:
        return f"lv{level}: the plan is empty"
    dst = data / f"solution_lv{level}_dp.txt.groups.live.txt"
    w = Worker(worker_id, WORKERS_ROOT)
    try:
        w.open(level, {"grouptrace": "1"})
        r = w.run(inputs, timeout_s=timeout_s)
        gt = R.gt_last(R.read_lines(w.data / "result.txt"), None)
        if not gt or gt["rows"] <= 0:
            return (f"lv{level}: grouptrace produced no settled recording "
                    f"(outcome={getattr(r,'outcome','?')})")
        src = w.data / "grouptrace_last.txt"
        if not src.exists():
            return f"lv{level}: no grouptrace_last.txt"
        if not copy_held_file(src, dst):
            return f"lv{level}: the copy failed"
        rows = read_lines(dst)
        return (f"lv{level}: live {len(rows) - 1} samples "
                f"(outcome={getattr(r, 'outcome', '?')} t={getattr(r, 'tick', '?')}) "
                f"-> {dst.name}")
    except (WorkerError, OSError) as e:
        return f"lv{level}: {e}"
    finally:
        try:
            w.close()
        except Exception:
            pass


def regen(level: int, data: Path, worker_id: int, timeout_s: float) -> str:
    objrects = data / f"objrects_lv{level}.txt"
    if not objrects.exists():
        return f"lv{level}: no objrects ({objrects})"
    if not has_grouped_colliders(objrects):
        return f"lv{level}: no moving geometry (this level needs no .groups.txt)"
    dst = data / f"solution_lv{level}_dp.txt.groups.txt"
    w = Worker(worker_id, WORKERS_ROOT)
    try:
        w.open(level, {"grouptrace": "1", "nodeath": "1"})
        done = wait_complete(w, timeout_s)
        if not done:
            return f"lv{level}: did not run to the end (timeout {timeout_s}s)"
        gt = R.gt_last(R.read_lines(w.data / "result.txt"), None)
        if not gt or gt["rows"] <= 0:
            return f"lv{level}: grouptrace produced no settled recording"
        src = w.data / "grouptrace_last.txt"
        if not src.exists():
            return f"lv{level}: no grouptrace_last.txt"
        if not copy_held_file(src, dst):
            return f"lv{level}: the copy failed"
        rows = read_lines(dst)
        return f"lv{level}: {len(rows) - 1} samples -> {dst.name}"
    except (WorkerError, OSError) as e:
        return f"lv{level}: {e}"
    finally:
        try:
            w.close()
        except Exception:
            pass


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("levels", type=int, nargs="*")
    ap.add_argument("--all", action="store_true",
                    help="every level with a solution in data/")
    ap.add_argument("--data-dir", default=str(LEVEL_DATA))
    ap.add_argument("--worker-id", type=int, default=98)
    ap.add_argument("--timeout", type=float, default=300.0)
    ap.add_argument("--live", action="store_true",
                    help="re-record the live replay (.groups.live.txt) rather "
                         "than the bootstrap")
    a = ap.parse_args(argv)

    data = Path(a.data_dir)
    levels = list(a.levels)
    if a.all:
        levels = sorted(int(p.name.split("_lv")[1].split("_")[0])
                        for p in data.glob("solution_lv*_dp.txt")
                        if p.name.split("_lv")[1].split("_")[0].isdigit())
    if not levels:
        ap.error("name some levels or pass --all")

    fn = regen_live if a.live else regen
    for lv in levels:
        print(fn(lv, data, a.worker_id, a.timeout), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
