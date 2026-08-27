"""Measure the outline of a lethal region on the real game.
WHEN YOU DO NOT KNOW THE SHAPE, USE THIS TO GET THE WHOLE OUTLINE.

    # sweep x and bisect y (the upper outline)
    python py/hitbox_sweep.py edge --level 20 --plan data/solution_lv20_ca.txt.best \\
        --tick 816 --y-lo 168 --y-hi 182 --x 1053:1061:0.25

    # look at life/death across a whole column of one x (checks bisection is valid)
    python py/hitbox_sweep.py column --level 20 --plan data/solution_lv20_ca.txt.best \\
        --tick 816 --x 1057.19 --y 160:194:2

The reason for the outline rather than "is this one point lethal" is that THE
SHAPE OF A HITBOX DOES NOT FOLLOW FROM A SINGLE BOUNDARY. Measured (2026-08-09,
lv20's spike id667 rot -63):

    x <= 1054.0        slope +1.969
    1054.25..1057.00   flat (width 3.00)
    1057.25..1060.25   slope -0.314
    x >= 1060.5        slope -0.503

Four straight lines = not a rotated rectangle. Looking at a single point would
have been misread as "the OBB matches" (and it was misread exactly that way
once; docs/findings.md 2026-08-09).

Practice:

  - X IS PINNED BY THE TICK. Unless the injection overwrites x too, you can
    only measure one point
  - LOOK AT MONOTONICITY WITH column BEFORE BISECTING. If there is another
    lethal band above, the bisection converges on some other object's boundary
    and still returns a plausible-looking number
  - Run `--stop-off` at both 0 and 1 and confirm the boundary does not move. If
    it does, what you are measuring is the sum of several ticks, not one tick
  - The worker is 99 (separate from MCP's 98). It can run in parallel while MCP
    is held
  - ~9 runs per point = 6 seconds. 33 points at a 0.25 step is about 3 minutes
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

_PY = Path(__file__).resolve().parent
sys.path.insert(0, str(_PY))
sys.path.insert(0, str(_PY.parent / "mcp"))

from gdmcp.worker import Worker
from gdtas import plan as P


def parse_range(s: str) -> list[float]:
    """Either `a:b:step`, or the values themselves separated by commas."""
    if ":" in s:
        a, b, step = (float(v) for v in s.split(":"))
        out, v = [], a
        while v <= b + 1e-9:
            out.append(round(v, 6))
            v += step
        return out
    return [float(v) for v in s.split(",")]


class Probe:
    """Build an (x, y) by injection, advance one tick, and report life or death.

    To build the state at tick T, inject at T-1 (an injection takes effect
    AFTER the physics update). That is why one tick's worth of advance is
    subtracted from x/y.
    """

    def __init__(self, a):
        self.T = a.tick
        self.dx = a.dx
        self.dy = a.dy
        self.stop_off = a.stop_off
        self.inputs = P.read_inputs(a.plan)
        self.w = Worker(a.worker)
        self.w.open(a.level, {})
        print(f"worker {a.worker} open on lv{a.level}, {len(self.inputs)} inputs",
              flush=True)

    def alive(self, x: float, y: float) -> bool:
        r = self.w.run(self.inputs,
                       injects=[{"tick": self.T - 1,
                                 "x": x - self.dx, "y": y - self.dy}],
                       stop_at=self.T + self.stop_off)
        return r.outcome != "death"

    def close(self) -> None:
        self.w.close()


def cmd_edge(a) -> int:
    p = Probe(a)
    try:
        for x in parse_range(a.x):
            lo, hi = a.y_lo, a.y_hi
            if p.alive(x, lo):
                print(f"x={x:9.3f}  already alive at the low end {lo} - widen the range")
                continue
            if not p.alive(x, hi):
                print(f"x={x:9.3f}  already dead at the high end {hi} - widen the range")
                continue
            while hi - lo > a.tol:
                mid = (lo + hi) / 2
                if p.alive(x, mid):
                    hi = mid
                else:
                    lo = mid
            print(f"x={x:9.3f}  boundary ({lo:.3f}, {hi:.3f}]", flush=True)
    finally:
        p.close()
    return 0


def cmd_column(a) -> int:
    p = Probe(a)
    try:
        cells = []
        for y in parse_range(a.y):
            dead = not p.alive(a.x_one, y)
            cells.append((y, dead))
            print(f"  y={y:8.3f}  {'DEAD' if dead else 'alive'}", flush=True)
        print(f"\nx={a.x_one:.3f}  " + "".join("D" if d else "." for _, d in cells))
        runs = sum(1 for i in range(1, len(cells))
                   if cells[i][1] != cells[i - 1][1])
        print(f"{runs} alive/dead transitions "
              f"({'bisection is valid' if runs <= 1 else '**bisection is NOT valid**'})")
    finally:
        p.close()
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("edge", "column"):
        s = sub.add_parser(name)
        s.add_argument("--level", type=int, required=True)
        s.add_argument("--plan", required=True)
        s.add_argument("--tick", type=int, required=True)
        s.add_argument("--dx", type=float, default=1.29825,
                       help="how far x advances in one tick (differs per speed)")
        s.add_argument("--dy", type=float, default=-2.5965,
                       help="the y step from tick-1 to tick, signed")
        s.add_argument("--stop-off", type=int, default=0,
                       help="how many ticks to run on from there (0 = that tick only)")
        s.add_argument("--worker", type=int, default=99)
        if name == "edge":
            s.add_argument("--x", required=True, help="a:b:step or a,b,c")
            s.add_argument("--y-lo", type=float, required=True,
                           help="a y that always dies")
            s.add_argument("--y-hi", type=float, required=True,
                           help="a y that always survives")
            s.add_argument("--tol", type=float, default=0.02)
        else:
            s.add_argument("--x", dest="x_one", type=float, required=True)
            s.add_argument("--y", required=True, help="a:b:step or a,b,c")
    a = ap.parse_args(argv)
    return cmd_edge(a) if a.cmd == "edge" else cmd_column(a)


if __name__ == "__main__":
    sys.exit(main())
