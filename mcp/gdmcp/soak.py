"""How much memory does a resident GD gain per run, and per level entered?

    python -m gdmcp.soak --level 1 --plan data/solution_lv1_dp.txt --worker 100 --mode reenter --n 300 --csv out.csv

One resident session (the MCP tools, as a client uses them) repeats the same short run n times and
the GD process's private bytes and working set are written to a CSV every 5 s AND after every run
(column `n` = runs done). Modes:

    reenter    gd_run's default: the level is entered afresh before each run (the exact mode)
    inplace    gd_run(in_place=True): the level is reset where it stands
    alternate  --level and --alt-level in turn, each run on a different level than the last
               (a level switch between every two runs)

The run is the first --inputs inputs of the plan, which dies near the start: what is measured is
the cost of entering / resetting a level and of the bookkeeping around a run, not of a long flight.
`--stop-private-mb` ends the soak when the process passes it (a floor for the machine's memory).
"""

from __future__ import annotations

import argparse
import csv
import sys
import threading
import time
from pathlib import Path

import psutil

from . import tools as T


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="gdmcp.soak", description=__doc__.split("\n")[0])
    ap.add_argument("--level", type=int, required=True)
    ap.add_argument("--alt-level", type=int, default=None)
    ap.add_argument("--plan", required=True)
    ap.add_argument("--inputs", type=int, default=3)
    ap.add_argument("--worker", type=int, default=100)
    ap.add_argument("--mode", choices=["reenter", "inplace", "alternate"], required=True)
    ap.add_argument("--n", type=int, required=True)
    ap.add_argument("--mod", default=None)
    ap.add_argument("--cfg", action="append", default=[], metavar="K=V")
    ap.add_argument("--leveldir", default=None)
    ap.add_argument("--csv", type=Path, required=True)
    ap.add_argument("--stop-private-mb", type=float, default=0.0)
    a = ap.parse_args(argv)
    if a.mode == "alternate" and a.alt_level is None:
        ap.error("alternate needs --alt-level")
    cfg = dict(kv.split("=", 1) for kv in a.cfg)
    if a.leveldir:
        cfg["leveldir"] = a.leveldir.replace("\\", "/")
    plan = T.parse_plan_file(a.plan)[:a.inputs] or [(0, 0)]
    plan = [list(p) for p in plan]
    T.gd_session_open(a.level, a.worker, cfg, a.mod)
    pid = T._session.proc.pid
    proc = psutil.Process(pid)
    state = {"n": 0, "level": a.level}
    t0 = time.time()
    f = a.csv.open("w", newline="", encoding="utf-8")
    w = csv.writer(f)
    w.writerow(["t_s", "n", "level", "private_mb", "ws_mb", "free_gb"])
    lock = threading.Lock()
    stop = threading.Event()

    def sample() -> None:
        try:
            mi = proc.memory_info()
        except psutil.Error:
            return
        with lock:
            w.writerow([round(time.time() - t0, 1), state["n"], state["level"],
                        round(mi.private / 2**20, 1), round(mi.wset / 2**20, 1),
                        round(psutil.virtual_memory().available / 2**30, 2)])
            f.flush()

    def ticker() -> None:
        while not stop.wait(5.0):
            sample()

    th = threading.Thread(target=ticker, daemon=True)
    th.start()
    sample()
    try:
        for i in range(a.n):
            lvl = a.level
            if a.mode == "alternate":
                lvl = a.alt_level if i % 2 else a.level
                if lvl != state["level"]:
                    T.gd_session_open(lvl, a.worker, cfg, a.mod)
                    state["level"] = lvl
            s = T.gd_run(plan=plan, in_place=(a.mode == "inplace"), timeout_s=120)
            state["n"] = i + 1
            sample()
            if s["outcome"] == "timeout":
                print(f"run {i + 1}: timeout", flush=True)
                break
            if a.stop_private_mb and proc.memory_info().private / 2**20 > a.stop_private_mb:
                print(f"run {i + 1}: private passed {a.stop_private_mb:.0f} MB, stopping", flush=True)
                break
            if (i + 1) % 25 == 0:
                print(f"run {i + 1}/{a.n}: {s.get('entered')} private "
                      f"{proc.memory_info().private / 2**20:.0f} MB", flush=True)
    finally:
        stop.set()
        T.gd_session_close()
        f.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
