"""What each kA39 station should measure, and the sweep that measures it.

brief-013 asks whether lv22's kA39 flag is what picks GD's circle-hazard
branch. The rigs are generated in pairs (mklevel sawcal39_*, once with --ka39
and once without) and this prints, per station and mode, the boundary branch A
and branch B each predict -- plus the ready-to-run hitbox_sweep command.

The point of computing rather than tabulating: the two formulas live in
dp/src/dp/object.hpp and are written out here once, so the expected numbers
cannot drift away from the model they are testing.

    python py/sawcal39_plan.py                       # every station, cube
    python py/sawcal39_plan.py --modes cube spider   # the two brief-013 asks for
    python py/sawcal39_plan.py --commands            # emit the sweep lines too

NO WORKER IS TOUCHED. This is arithmetic and text; whoever holds a worker runs
what it prints.
"""
from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

_PY = Path(__file__).resolve().parent
sys.path.insert(0, str(_PY))

from mklevel import KA39_STATIONS                            # noqa: E402

# The dumped m_objectRadius of each station's id, and its raw sprite size.
# Radius: object.hpp's own list (id1734 32 / id1735 17.51 / id88 32.3 /
# id918 24) plus lv21's blades (id1582/1583 r=4, prep-013 section 1.1).
# Size: only id 1583 is known here (31x23, from lv22 uid710's dump) -- the rest
# are left None because THE RIG IS WHAT MEASURES THEM. A guessed w0 would walk
# straight into the scale-recovery bug this rig exists to expose.
RADIUS = {1583: 4.0, 1582: 4.0, 918: 24.0, 1734: 32.0, 1735: 17.51, 88: 32.3}
RAW_WH = {1583: (31.0, 23.0)}

# sawRectHalfB (object.hpp): the player's HAZARD half, per mode, mini x0.6.
def rect_half(mode: str, mini: bool) -> float:
    s = 0.6 if mini else 1.0
    if mode == "wave":
        return 5.0 * s
    if mode == "spider":
        return 13.5 * s
    return 15.0 * s


def branch_a(h: float, R: float) -> tuple[float, float]:
    """Circle against circle: one radius, the same in every direction."""
    return h + R, h + R


def branch_b(h: float, R: float) -> tuple[float, float]:
    """The player's rect plus corner discs (axis, diagonal).

    Axis: dy=0 leaves ey=-h, so the corner test needs (|dx|-h)^2 + h^2 < R^2 --
    which has no solution past the rect at all when R <= h, and the boundary is
    then the flat edge. That collapse is the whole reason the r=4 stations
    separate the branches in kind rather than in degree.
    """
    axis = h + math.sqrt(R * R - h * h) if R > h else h
    return axis, h * math.sqrt(2.0) + R


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--modes", nargs="+",
                    default=["cube", "spider", "wave", "ball"])
    ap.add_argument("--mini", action="store_true")
    ap.add_argument("--commands", action="store_true",
                    help="also emit the hitbox_sweep command for each station")
    ap.add_argument("--level", type=int, default=1,
                    help="the level id the rig is loaded as (for the commands)")
    ap.add_argument("--tick", type=int, default=600)
    a = ap.parse_args()

    print("boundary = distance from the saw's centre to the player's centre at "
          "which the kill starts.\nA = circle vs circle, B = rect + corner "
          "discs. Both carry the same margin, so it cancels in A-B.\n")
    for mode in a.modes:
        h = rect_half(mode, a.mini)
        print(f"=== {mode}{' mini' if a.mini else ''}   player hazard half "
              f"h = {h}  ===")
        print(f"{'x':>6} {'id':>5} {'rot':>5} {'R':>6} | {'A axis':>7} "
              f"{'B axis':>7} {'A-B':>7} | {'A diag':>7} {'B diag':>7} "
              f"{'A-B':>7}")
        for oid, x, scale, rot, _why in KA39_STATIONS:
            R = RADIUS[oid] * (scale if scale else 1.0)
            aa, ad = branch_a(h, R)
            ba, bd = branch_b(h, R)
            print(f"{x:6d} {oid:5d} {rot:5.0f} {R:6.2f} | {aa:7.3f} {ba:7.3f} "
                  f"{aa - ba:+7.3f} | {ad:7.3f} {bd:7.3f} {ad - bd:+7.3f}")
        # the model's own radius for the swapped station, which is the second
        # question this rig answers
        if 1583 in RAW_WH:
            w0, h0 = RAW_WH[1583]
            swapped = RADIUS[1583] * (h0 / w0)
            print(f"\n  the 90-degree station (id 1583, {w0:.0f}x{h0:.0f}): the "
                  f"model recovers the scale as w/w0 and gets R = "
                  f"{swapped:.3f},\n  where undoing the swap gives R = "
                  f"{RADIUS[1583]:.3f}. On the axis that is "
                  f"{h + RADIUS[1583] - (h + swapped):.3f} px apart under "
                  f"branch A\n  ({h + swapped:.3f} against "
                  f"{h + RADIUS[1583]:.3f}) -- well over the 0.02 px the "
                  f"bisection resolves.")
        if a.commands:
            print("\n  sweeps (run from a checkout that holds a worker):")
            for oid, x, scale, rot, _why in KA39_STATIONS:
                R = RADIUS[oid] * (scale if scale else 1.0)
                aa, _ = branch_a(h, R)
                ba, _ = branch_b(h, R)
                lo = 300.0 + min(aa, ba) - 3.0
                hi = 300.0 + max(aa, ba) + 3.0
                print(f"    # x={x} id={oid}: top boundary lands at "
                      f"y={300 + ba:.2f} for B, y={300 + aa:.2f} for A")
                print(f"    python py/hitbox_sweep.py column --level {a.level} "
                      f"--plan <probe plan> --tick {a.tick} \\\n"
                      f"        --x {x} --y {lo:.1f}:{hi:.1f}:0.5")
                print(f"    python py/hitbox_sweep.py edge --level {a.level} "
                      f"--plan <probe plan> --tick {a.tick} \\\n"
                      f"        --y-lo {lo:.1f} --y-hi {hi:.1f} "
                      f"--x {x - 40}:{x + 40}:2")
        print()

    print("READING THE RESULT")
    print("  * the kA39=0 arm is the CONTROL. It has to land on B, reproducing "
          "what is\n    already on record. If it does not, stop -- something "
          "other than the flag moved")
    print("  * the r=4 stations are the decisive ones: B cannot put a boundary "
          "past h at all\n    there, so 'axis boundary > h' is by itself "
          "branch A")
    print("  * sweep the DIAGONAL too. A is wider on the axis and NARROWER on "
          "the diagonal,\n    and a rig that only ever looks along one axis "
          "reads the second half backwards")
    print("  * print the saw's own position next to every boundary. On the "
          "spun rig it is\n    moving, and a boundary quoted against the "
          "wrong centre is a fitted number")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
