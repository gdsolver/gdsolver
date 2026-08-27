# -*- coding: utf-8 -*-
u"""Replay a solution plan in GD as it is and collect `result.txt`, for when the
MCP server is not available.

    python py/gdprobe.py <lv> <out.txt> [--cfg k=v ...] [--worker-id 98]

For example:
    python py/gdprobe.py 20 C:\\tmp\\r20.txt --cfg hitboxtrace=1 hbfrom=21440 hbto=21470

This does what `gd_run` does, with a plain Worker and nothing else. The cfg keys
are the mod's own autorun.cfg ones (hitboxtrace, standtrace, dump, clearance,
grouptrace and so on).
"""
from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas import plan as P                                     # noqa: E402
from gdtas.paths import DATA, WORKERS_ROOT                      # noqa: E402
from gdtas.worker import Worker, WorkerError                    # noqa: E402


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("level", type=int)
    ap.add_argument("out")
    ap.add_argument("--plan", default="")
    ap.add_argument("--cfg", nargs="*", default=[])
    ap.add_argument("--worker-id", type=int, default=98)
    ap.add_argument("--timeout", type=float, default=300.0)
    a = ap.parse_args(argv)

    plan = Path(a.plan) if a.plan else DATA / f"solution_lv{a.level}_dp.txt"
    inputs = P.read_inputs(plan)
    if not inputs:
        print(f"the plan is empty: {plan}")
        return 2
    cfg = {}
    for kv in a.cfg:
        k, _, v = kv.partition("=")
        cfg[k] = v
    w = Worker(a.worker_id, WORKERS_ROOT)
    try:
        w.open(a.level, cfg)
        r = w.run(inputs, timeout_s=a.timeout)
        print(f"outcome={getattr(r, 'outcome', '?')} tick={getattr(r, 'tick', '?')} "
              f"x={getattr(r, 'x', '?')}")
        src = w.data / "result.txt"
        shutil.copy2(src, a.out)
        print(f"result.txt -> {a.out} ({Path(a.out).stat().st_size} bytes)")
    except (WorkerError, OSError) as e:
        print(f"error: {e}")
        return 1
    finally:
        try:
            w.close()
        except Exception:
            pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
