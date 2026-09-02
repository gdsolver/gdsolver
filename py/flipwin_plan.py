"""What each flip-window rung should show, and how to tell the arms apart.

brief-004b has to put back two arms of the bonk gate at once -- the 1859 arming
arm and the flip-grace arm -- and the reason 005 came apart was that a bonk
seen in the wild cannot say which arm produced it. The rigs (mklevel
flipwin_cube[_mini][_armed]) are built as a 2x2: delay across the window,
against the presence of an 1859.

This prints the predictions each hypothesis makes for that grid, so the run can
be read without holding three notes open at once. It computes from the rig's
own unit table, so it cannot drift from the level that gets loaded.

    python py/flipwin_plan.py
    python py/flipwin_plan.py --rig data/rigs/calib_flipwin_cube_armed.lvl

NO WORKER IS TOUCHED.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path

# The window brief-004b is testing, in ticks. 0.1 s at 240 Hz is 24; the
# conversion GD actually performs may round the other way, and which of 24 or
# 25 it is is one of the things the ladder is for -- so both are marked.
WINDOW = (24, 25)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--rig", default="data/rigs/calib_flipwin_cube.lvl")
    a = ap.parse_args()
    units = json.loads(Path(a.rig).with_suffix(".units.json").read_text())
    rungs = [u for u in units if u.get("rung", -1) >= 0]
    blocks = [u for u in units if u.get("rung", 0) < 0]
    armed = bool(rungs and rungs[0].get("armed"))

    print(f"rig: {a.rig}   1859s under the ceilings: {armed}")
    print(f"the window under test: {WINDOW[0]}-{WINDOW[1]} ticks "
          f"(0.1 s at 240 Hz)\n")
    print(f"{'rung':>5} {'delay':>6} {'x_blue':>9} {'x_yellow':>9} "
          f"{'ceiling':>9} | {'in window?':>11}  expected if the arm is real")
    for u in rungs:
        k = u["delay_ticks"]
        inw = k < WINDOW[0]
        edge = WINDOW[0] <= k <= WINDOW[1]
        state = "yes" if inw else ("BOUNDARY" if edge else "no")
        if armed:
            exp = "survive (armed, so the 1859 arm covers it either way)"
        else:
            exp = ("survive: grace converts the head hit" if inw
                   else "DIE: grace expired, nothing else covers a full cube")
        print(f"{u['rung']:5d} {k:6d} {u['x_blue']:9.0f} {u['x_yellow']:9.1f} "
              f"{u['ceil_bottom']:9.1f} | {state:>11}  {exp}")

    print("\nWHAT EACH ANSWER MEANS")
    print("  unarmed rig dies at the first rung past the window, armed rig "
          "survives all\n     -> both arms are real and separable. This is "
          "what 004b assumes")
    print("  both rigs survive every rung\n     -> the 1859 arm alone accounts "
          "for it and the grace is not what saves the player;\n        the "
          "unarmed rig is then the one that matters and its boundary is the "
          "answer")
    print("  both rigs die on every rung, including short delays\n     -> the "
          "grace does not apply to a head hit at all, and 004b's arm 3 is "
          "mis-stated")
    print("  the armed rig dies where the unarmed one survives\n     -> the "
          "1859 is doing something other than arming a bonk; stop and measure "
          "that first")

    print("\nHOW TO READ THE RUN (one natural pass, no injection)")
    print("  * the rungs ascend in delay, so the pass walks the ladder and "
          "stops at the first\n    lethal one. THE DEATH x IS THE ANSWER -- "
          "no bisection")
    print("  * take the delay from the DUMP, not from this table: the ticks "
          "here assume the\n    cube's 0.216, and the ladder only has to "
          "BRACKET the boundary, not land on it")
    print("  * the two ticks to read per rung are the tick the yellow portal "
          "fires (upsideDown\n    goes 1 -> 0) and the tick the head reaches "
          "the ceiling. Their difference is the\n    delay this rig is "
          "measuring")
    print("  * print the ceiling's own y next to the head's y on the contact "
          "tick. A boundary\n    quoted without the surface it was measured "
          "against is a fitted number")
    print("  * IF THE PLAYER NEVER COMES BACK DOWN, the yellow portal was "
          "missed -- the flip is\n    also a launch and the portal sits on a "
          "computed parabola. That failure is loud\n    by design; regenerate "
          "with the measured gravity rather than reading the run")

    if blocks:
        print("\nTHE THIRD BEHAVIOUR (the stations with no portals)")
        for b in blocks:
            print(f"  block row at x={b['x_block']:.0f} cy={b['block_cy']:.0f}"
                  f"  (top face {b['block_cy'] + 15:.0f})")
        print("  An inverted player driven head-first into a block's TOP face "
              "was measured\n  sinking 7 px in and carrying on down -- neither "
              "a bonk nor a kill, and in\n  neither the model nor the "
              "disassembly. Inject INVERTED, above the row, moving\n  DOWN, "
              "and sweep the entry speed: the question is whether the "
              "pass-through is\n  unconditional or whether it has a speed or "
              "a depth at which it stops.")
        print("  Sweep the outline, not one point ([[gd-sweep-the-outline]]), "
              "and check with\n  `column` that there is a single alive/dead "
              "transition before trusting a bisection.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
