#!/usr/bin/env python
"""The cold regression: every level solved from nothing by the game itself.

    python py/cold_regress.py                    # all 22, pool 90-93
    python py/cold_regress.py --levels 16 22     # just those two
    python py/cold_regress.py --bless            # adopt this run as the baseline

One GD session per level, `dpsolve=1`, no plan, no solution file, no seed: the
mod builds the level out of PlayLayer and runs the whole repair loop itself.
This is the successor to dp_solve_batch.py, which drove the same loop from
Python; the levels are what is being measured, not the driver, so what changed
is only who holds the loop.

Read the result as three numbers per level, in this order of authority:

  CLEARED     only `dpsolve: solution saved` proves it. A `complete:` line with
              pct=100 is ALSO what the nodeath recording pass writes -- it runs
              the level with dying switched off and the mod refuses it as "not a
              clear" -- and reading the percentage alone once reported lv22 as
              CLEARED on a run that gave up at 41%.
  iters       the iteration count. It is deterministic: two cold lv22 runs on
              the same build both took exactly 60, lv16 both took 102, lv18 both
              took 11. That is what makes this a regression rather than a demo,
              and it is the number the baseline compares.
  record      must read `none`. The safety gate (config.hpp botDriving) blocks
              the level's own record while the bot drives; a run that shows
              anything else here is a bug in the gate, not a solved level.

Wall-clock is reported but NEVER compared: with four workers each running its
own DP threads it inflates by roughly 2x against a solo run (measured, lv22:
465 s alone, 928 s in a full sweep) while the iteration count is unchanged.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from gdtas.paths import DATA
from gdtas.worker import run_session

BASELINE = DATA / "cold_baseline.json"

# Relative cost, so the long levels start first and the pool drains evenly. It
# only has to get the ORDER roughly right; anything missing is treated as cheap.
# Measured on the 2026-08-27 full sweep (four workers).
COST = {16: 12.0, 21: 16.0, 22: 11.0, 20: 8.0, 19: 1.6, 18: 1.0, 17: 0.8,
        14: 0.7, 10: 0.6, 11: 0.5, 9: 0.4, 7: 0.5, 8: 0.4, 15: 0.4,
        13: 0.4, 12: 0.3, 1: 0.3, 2: 0.3, 3: 0.3, 4: 0.3, 5: 0.3, 6: 0.3}

# How long a level is allowed to be wrong for. A uniform cap is paid for by the
# FAILING side: with everything at the same budget one broken level burns the
# whole run's wall clock. Per level it is the baseline's own iteration count
# with room for the model moving underneath it.
ITER_MARGIN, ITER_FLOOR, ITER_UNKNOWN = 3, 30, 60

CFG = ["enabled=1", "attempts=1000000", "quitwhendone=1", "blockinput=1",
       "cbs=0", "cos=1", "fastdt=0.0166667", "fastloops=1800", "skiprender=1",
       "music=mute", "servemode=0", "dpsolve=1"]

# A from-the-start anchored solve on lv22 legitimately burns 20+ minutes of pure
# DP with no result.txt growth (measured 2026-08-26, five cores busy), and 1200
# reads that as a stall and kills a healthy run.
STALL_S = 2400.0


def load_baseline() -> dict:
    try:
        return json.loads(BASELINE.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def iter_cap(lv: int, base: dict) -> int:
    b = base.get(str(lv), {}).get("iters")
    return max(ITER_FLOOR, ITER_MARGIN * (b if b else ITER_UNKNOWN))


def assign(levels: list[int], pool: list[int]) -> dict[int, list[int]]:
    """Longest-first onto the least loaded worker."""
    load = {w: 0.0 for w in pool}
    out: dict[int, list[int]] = {w: [] for w in pool}
    for lv in sorted(levels, key=lambda l: -COST.get(l, 1.0)):
        w = min(pool, key=lambda x: (load[x], pool.index(x)))
        out[w].append(lv)
        load[w] += COST.get(lv, 1.0)
    return out


def wipe(worker_id: int) -> None:
    """Everything the loop learns lives in the worker's data dir, so a cold run
    starts by deleting it. Leaving one file behind is a seed, and a seeded run
    is not a cold one (CLAUDE.md's first rule)."""
    from gdtas.paths import WORKERS_ROOT
    d = WORKERS_ROOT / f"worker-{worker_id}" / "session" / "data"
    for name in ("dp_seed.txt", "dp_plan.txt", "dp_plan.txt.trace.csv",
                 "dp_tail.txt", "dp_tail.txt.trace.csv", "dp_fixup.trace.csv",
                 "dp_fixups.txt", "dp_fixups_noop.txt", "dp_groups.txt",
                 "dp_groups_deep.txt", "dp_band.txt", "grouptrace.txt",
                 "grouptrace_last.txt", "trace.csv", "dump.csv", "result.txt",
                 "plan_in.txt"):
        try:
            (d / name).unlink(missing_ok=True)
        except OSError:
            pass
    for f in d.glob("solution_lv*_dp.txt"):
        try:
            f.unlink()
        except OSError:
            pass


def one(level: int, wid: int, budget: float, extra: list[str],
        out_dir: Path) -> dict:
    t0 = time.time()
    wipe(wid)
    try:
        r = run_session(wid, CFG + extra + [f"level={level}"],
                        timeout_s=budget, stall_s=STALL_S)
    except Exception as e:                      # noqa: BLE001  a worker that will not start
        return {"lv": level, "cleared": False, "why": f"ERROR {e}",
                "iters": 0, "deepest_t": -1, "deepest_x": -1.0, "fx": 0,
                "record": "?", "wall": time.time() - t0, "fp": ""}
    txt = "\n".join(r.lines)
    # The whole session log, next to the results. A stuck level has to be read
    # out of the ladder's own lines, and there is nowhere else they survive.
    try:
        (out_dir / f"coldlog_lv{level}.txt").write_text(txt, encoding="utf-8",
                                                       errors="replace")
    except OSError:
        pass
    out = read_result(txt, getattr(r, "timed_out", False))
    out["lv"] = level
    out["wall"] = time.time() - t0
    return out


def read_result(txt: str, timed_out: bool = False) -> dict:
    """Read one session's log into the numbers a cold run is judged by.

    Split out of one() so that the private tree's custom-level runner reads a
    session EXACTLY the same way (docs/NOTES.md; custom levels are not supported
    here yet). The point of that runner is that the same loop is being measured
    and only the level source differs, which is worth nothing if the two
    disagree about what counts as cleared."""
    iters = re.findall(r"^dpsolve: iter (\d+):", txt, re.M)
    deaths = [(int(a), float(b)) for a, b in
              re.findall(r"^death: attempt=\d+ tick=(\d+) x=([\d.]+)", txt, re.M)]
    fps = re.findall(r"^dpsolve:   \[fp\] (.*)$", txt, re.M)
    saved = bool(re.search(r"^dpsolve: solution saved", txt, re.M))
    why = "CLEARED" if saved else "stuck"
    if not saved:
        m = re.search(r"^dpsolve: giving up - (.*)$", txt, re.M)
        if m:
            why = "stuck: " + m.group(1)[:60]
        elif timed_out:
            why = "stuck: out of wall time"
    rec = re.search(r"^level record changed: (.*)$", txt, re.M)
    return {"cleared": saved, "why": why, "iters": len(iters),
            "deepest_t": max((d[0] for d in deaths), default=-1),
            "deepest_x": max((d[1] for d in deaths), default=-1.0),
            "fx": len(re.findall(r"\[fixup\] t=\d+ x=", txt)),
            "record": rec.group(1) if rec else "?",
            # The last [fp] is the state the run ended in -- the cheapest single
            # value to diff two runs by (see logFingerprint in repair.hpp).
            "fp": fps[-1] if fps else ""}


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--levels", nargs="+", type=int,
                    default=list(range(1, 23)))
    ap.add_argument("--pool", nargs="+", type=int, default=[90, 91, 92, 93])
    ap.add_argument("--budget", type=float, default=3600.0,
                    help="wall-clock seconds per level")
    ap.add_argument("--cfg", nargs="*", default=[],
                    help="extra autorun.cfg keys, e.g. dpfingerprint=0")
    ap.add_argument("--bless", action="store_true",
                    help="save this run's iteration counts as the baseline")
    a = ap.parse_args(argv)

    base = load_baseline()
    buckets = assign(a.levels, a.pool)
    for w in a.pool:
        if buckets[w]:
            mins = sum(COST.get(l, 1.0) for l in buckets[w])
            print(f"  worker {w}: {','.join(str(l) for l in buckets[w])} "
                  f"(~{mins:.0f} min)")

    lock = threading.Lock()
    launched = [0]
    results: list[dict] = []

    def run_bucket(w: int) -> list[dict]:
        out = []
        with lock:
            k = launched[0]
            launched[0] += 1
        # Simultaneous GD starts are the documented worker trap; stagger the
        # first wave only.
        if k:
            time.sleep(k * 12.0)
        for lv in buckets[w]:
            cap = iter_cap(lv, base)
            res = one(lv, w, a.budget, list(a.cfg), DATA)
            res["cap"] = cap
            with lock:
                print(f"lv{lv:<3} {res['why']:<40} iters={res['iters']:<4}"
                      f"deepest t={res['deepest_t']:<6} x={res['deepest_x']:<9.0f}"
                      f"fx={res['fx']:<4} {res['wall']:>5.0f}s "
                      f"record={res['record']}", flush=True)
            out.append(res)
        return out

    busy = [w for w in a.pool if buckets[w]]
    with ThreadPoolExecutor(max_workers=max(1, len(busy))) as ex:
        for got in ex.map(run_bucket, busy):
            results.extend(got)

    results.sort(key=lambda r: r["lv"])
    print("\n=== cold regression ===")
    bad: list[str] = []
    for r in results:
        b = base.get(str(r["lv"]), {})
        mark = ""
        if b.get("iters") and r["cleared"]:
            d = r["iters"] - b["iters"]
            if d:
                mark = f"  (baseline {b['iters']}, {d:+d})"
            if r["iters"] > iter_cap(r["lv"], base):
                bad.append(f"lv{r['lv']}: {r['iters']} iterations against a cap of "
                           f"{iter_cap(r['lv'], base)}")
        print(f"lv{r['lv']:<3} {'CLEARED' if r['cleared'] else r['why']:<40}"
              f"iters={r['iters']:<4}{mark}")
        if not r["cleared"]:
            bad.append(f"lv{r['lv']}: {r['why']}")
        if r["record"] != "none" and not r["record"].startswith("none"):
            bad.append(f"lv{r['lv']}: RECORD GATE LEAKED ({r['record']})")

    # Count the result lines. A worker that hung or crashed reports nothing at
    # all, so a short run must never read as a clean one.
    if len(results) != len(a.levels):
        missing = sorted(set(a.levels) - {r["lv"] for r in results})
        print(f"INCOMPLETE: {len(results)} of {len(a.levels)} levels reported; "
              f"missing {missing}. Check free memory first, then shrink --pool.")
        return 1

    if a.bless:
        out = {str(r["lv"]): {"iters": r["iters"], "fp": r["fp"]}
               for r in results if r["cleared"]}
        BASELINE.write_text(json.dumps(out, indent=1, sort_keys=True),
                            encoding="utf-8")
        print(f"blessed {len(out)} levels -> {BASELINE}")

    if bad:
        print("\nFAIL:")
        for b in bad:
            print("  " + b)
        return 1
    print(f"\nPASS ({len(results)}/{len(results)} cleared cold)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
