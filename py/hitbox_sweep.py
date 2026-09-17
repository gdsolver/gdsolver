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
import csv
import shutil
import sys
import time
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
        self.inputs = P.read_inputs(a.plan) if a.plan else []
        self.w = Worker(a.worker)
        # A RIG, not an official level. The corpus cannot isolate some objects at
        # all -- a spiked ramp sits on a plain twin on 116 of lv18's 154 -- so the
        # outline has to be taken on a level built for it. The .lvl is copied into
        # the worker's own data root and named by `levelfile`, exactly as
        # py/run_calib.py does it; level 9001 is the id the mod reserves for one.
        extra: dict[str, str] = {}
        # --cfg key=value, repeatable. The one that pays for itself is
        # `gatetrace=-2,-1`, which arms destroyPlayer's `killer:` line while
        # recording nothing (no object falls in that x window): each death in the
        # sweep then says whether GD passed an object, and a sweep that cannot
        # say that has to borrow the answer from some other death -- which is how
        # a leading-edge death's `(no object)` got read as the injected ones'.
        for kv in getattr(a, "cfg", None) or []:
            k, _, v = kv.partition("=")
            extra[k] = v
        if getattr(a, "levelfile", None):
            src = Path(a.levelfile)
            dst = self.w.data / src.name
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
            extra["levelfile"] = str(dst)
            print(f"rig {src.name} -> {dst}", flush=True)
            # ...AND MAKE GD WRITE THE GEOMETRY DOWN. Without this the worker's
            # objrects.txt is whatever the last session left there -- ten days
            # stale, from an official level, when this was noticed -- so a sweep
            # reported a boundary in absolute y and nothing in its own output said
            # where the slope line was. "the lethal offset is 4.08" and "the line
            # sits 1.28 lower than assumed" are then the SAME measurement, and
            # they disagree about whether a kill rule over-kills. dpselftest is
            # the cheapest key that runs buildPois (config.hpp: "writes the
            # dpselftest: lines to result.txt and changes nothing else"); --cfg is
            # applied first and still wins, so it can be turned off deliberately.
            extra.setdefault("dpselftest", "1")
        self.w.open(a.level, extra)
        print(f"worker {a.worker} open on lv{a.level}, {len(self.inputs)} inputs",
              flush=True)
        self.opened_at = time.time()

    def _read_geometry(self, deadline: float = 40.0) -> list[dict]:
        """The rows GD wrote for THIS level, so a sweep can name what it hit.

        The file has to be newer than the session, and waiting for it is the
        whole point: buildPois runs when the PlayLayer inits, which is after
        w.open() returns, so reading it straight away gets the LAST session's
        dump. That is not a stale-data nuisance, it is the same silence this
        reporting exists to remove -- the first version of it printed nothing for
        a rig whose dump was ten days old, and "no slope near this column" and
        "you are looking at another level" read identically.
        """
        f = self.w.data / "objrects.txt"
        end = time.time() + deadline
        while time.time() < end:
            if f.exists() and f.stat().st_mtime >= self.opened_at - 1.0:
                with f.open(newline="", encoding="utf-8", errors="replace") as fh:
                    return list(csv.DictReader(fh))
            time.sleep(0.5)
        age = ("absent" if not f.exists()
               else f"{self.opened_at - f.stat().st_mtime:.0f}s older than this session")
        print(f"  (objrects.txt {age}: this sweep's geometry is UNRECORDED -- "
              f"do not convert its boundary into a distance from the line)",
              flush=True)
        return []

    def report_geometry(self, xs: list[float], pad: float = 40.0) -> None:
        """Print the slope rows near the probed columns, GD's numbers verbatim."""
        rows = self._read_geometry()
        if not rows:
            return
        shown = 0
        for r in rows:
            try:
                cx, w = float(r["cx"]), float(r["w"])
                sy0, sy1 = float(r["sy0"]), float(r["sy1"])
            except (KeyError, ValueError):
                continue
            if sy0 == sy1:          # not a slope: sy0/sy1 are 0,0 on a flat solid
                continue
            if not any(abs(cx - x) <= w / 2.0 + pad for x in xs):
                continue
            print(f"  geom id={r['id']} uid={r.get('uid')} cx={r['cx']} cy={r['cy']}"
                  f" w={r['w']} h={r['h']} sy0={r['sy0']} sy1={r['sy1']}"
                  f" shz={r.get('shz')} sdir={r.get('sdir')} sup={r.get('sup')}",
                  flush=True)
            shown += 1
        if not shown:
            print(f"  (the dump is this level's, and it holds no slope within "
                  f"{pad:g}px of {', '.join(f'{x:g}' for x in xs)})", flush=True)

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
        xs = parse_range(a.x)
        p.report_geometry(xs)
        for x in xs:
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
        p.report_geometry([a.x_one])
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
        s.add_argument("--level", type=int, default=9001,
                       help="an official level id, or 9001 with --levelfile")
        s.add_argument("--levelfile",
                       help="a rig .lvl to drive instead of an official level. "
                            "Copied into the worker's data root")
        s.add_argument("--plan", default="",
                       help="the input列 to replay before the injection. A rig "
                            "usually needs none (the injection makes the state)")
        s.add_argument("--tick", type=int, required=True)
        s.add_argument("--dx", type=float, default=1.29825,
                       help="how far x advances in one tick (differs per speed)")
        s.add_argument("--dy", type=float, default=-2.5965,
                       help="the y step from tick-1 to tick, signed")
        s.add_argument("--stop-off", type=int, default=0,
                       help="how many ticks to run on from there (0 = that tick only)")
        s.add_argument("--worker", type=int, default=99)
        s.add_argument("--cfg", action="append", metavar="KEY=VALUE",
                       help="extra autorun.cfg lines for the session "
                            "(repeatable). `--cfg gatetrace=-2,-1` arms the "
                            "killer: line so each death names its object or "
                            "says (no object)")
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
