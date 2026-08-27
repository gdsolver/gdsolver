#!/usr/bin/env python
"""Exhaustively check whether a GD run clipped through solid geometry.

    python py\\clip_check.py --dump <workers>\\worker-98\\session\\data\\dump.csv \\
                            --objrects data/objrects_lv20.txt

(<workers> is gdtas.paths.WORKERS_ROOT.)

Looks at STATIC SOLIDS ONLY (groups=0, type=0). Moving objects would need the
grouptrace time axis, and a door legitimately overlaps the moment it opens, so
checking them here yields nothing but false positives.

GD resolves collisions, so in a healthy run the penetration never exceeds one
tick's worth of movement. If an overlap well beyond that persists, suspect a
clip-through (the plan is passing through geometry). If the solver's "best x"
depended on it, that progress is not real.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

# Player half-width. Use the box GD KILLS WITH ON THE SIDE, NOT the visual 30x30.
# Measured 2026-08-11 (lv22 uid1036 (1845,165) 30x30, static injection + bisection):
#   for both y=165 and y=180 the killing boundary is x=1821.61 -> 1830 - 1821.61 = 8.39
# With 15 the corner-clip depth reads 8.7px, but the penetration into the actual
# killing box is 5.0px.
# Vertically it matches the visual size (this is the box for landing / head-bumping).
HALF_X = {"cube": 8.39, "ship": 8.39, "ball": 8.39, "ufo": 8.39,
          "wave": 5.0, "robot": 8.39, "spider": 8.39, "swing": 8.39}
HALF_Y = 15.0
# The normal overlap while standing on a surface. Without this floor, hundreds of
# merely-grounded ticks show up as "clip-through" (measured: 271 of 277 ticks on lv22).
REST_OVERLAP = 1.6


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dump", required=True)
    ap.add_argument("--objrects", required=True)
    # A sustained penetration beyond one tick's movement (~2.1px even at top speed)
    # is abnormal. Since we use the measured box, the default is 2.5 (above the 1.5
    # of resting contact, below one tick's movement).
    ap.add_argument("--tol", type=float, default=2.5)
    # Report only runs longer than this many ticks (a 1-tick graze is mid-resolution)
    ap.add_argument("--min-run", type=int, default=3)
    a = ap.parse_args(argv)

    solids = []
    with open(a.objrects, encoding="utf-8", errors="replace") as f:
        f.readline()
        for ln in f:
            p = ln.split(",")
            if len(p) < 8:
                continue
            # Static solids only; groups!=0 means movable, so drop it
            if p[1] != "0" or p[6] != "0":
                continue
            solids.append((float(p[2]), float(p[3]), float(p[4]) / 2,
                           float(p[5]) / 2, p[7]))
    solids.sort()
    xs = [s[0] for s in solids]
    print(f"static solids: {len(solids)}")

    import bisect
    hdr = None
    hits = []
    with open(a.dump, encoding="utf-8", errors="replace") as f:
        for ln in f:
            p = ln.rstrip("\n").split(",")
            if hdr is None:
                hdr = p
                idx = {n: (hdr.index(n) if n in hdr else -1)
                       for n in ("tick", "x", "y", "mode", "vsize", "dead")}
                continue
            if len(p) < len(hdr):
                continue
            try:
                t = int(p[idx["tick"]])
                px, py = float(p[idx["x"]]), float(p[idx["y"]])
            except ValueError:
                continue
            phx, phy = HALF_X.get(p[idx["mode"]], 8.39), HALF_Y
            if idx["vsize"] >= 0 and p[idx["vsize"]]:
                try:
                    vs = float(p[idx["vsize"]])
                    phx, phy = phx * vs, phy * vs
                except ValueError:
                    pass
            lo = bisect.bisect_left(xs, px - 60)
            hi = bisect.bisect_right(xs, px + 60)
            worst, who = 0.0, ""
            for (cx, cy, hw, hh, uid) in solids[lo:hi]:
                ox = (hw + phx) - abs(px - cx)
                oy = (hh + phy) - abs(py - cy)
                if ox <= 0 or oy <= 0:
                    continue
                d = min(ox, oy)
                # Do not count the overlap of merely standing on a surface
                if d <= REST_OVERLAP:
                    continue
                if d > worst:
                    worst, who = d, uid
            if worst > a.tol:
                hits.append((t, px, py, worst, who))

    if not hits:
        print(f"nothing clipped through (no tick overlaps by more than {a.tol} px)")
        return 0
    # Group into contiguous runs
    runs, cur = [], [hits[0]]
    for h in hits[1:]:
        if h[0] - cur[-1][0] <= 2:
            cur.append(h)
        else:
            runs.append(cur); cur = [h]
    runs.append(cur)
    runs = [r for r in runs if len(r) >= a.min_run]
    if not runs:
        print(f"only isolated grazes ({len(hits)} ticks, none of them a run of "
              f"{a.min_run} or more)")
        return 0
    print(f"suspected clip-through: {len(runs)} stretches")
    for r in runs[:20]:
        deep = max(r, key=lambda h: h[3])
        print(f"  t={r[0][0]}..{r[-1][0]} ({len(r)} tick)  "
              f"x={r[0][1]:.0f}..{r[-1][1]:.0f}  deepest overlap {deep[3]:.1f}px "
              f"@uid{deep[4]} (t={deep[0]} y={deep[2]:.1f})")
    return 1


if __name__ == "__main__":
    sys.exit(main())
