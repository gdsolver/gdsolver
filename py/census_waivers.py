# -*- coding: utf-8 -*-
"""EXPLICIT LIST OF KNOWN OUTLIERS (census waivers).

The only things allowed in here are one-off outliers that are "mechanically
correct, and where every attempt at turning them into a rule was ruled out by
measurement". IF A RULE CAN BE WRITTEN, WRITE THE RULE -- this is not a place
to hide "not fixed yet".

Waivers are MATCHED STRICTLY BY KEY (lv / tick / cause / in / edy / edvy).
If any one of them moves, the waiver comes off and that divergence shows up
red as usual. In other words, we waive "this tick diverges the way it always
has", but we do not miss "another tick started diverging" or "the way it
diverges changed".

    python py/fixcensus.py --no-waivers    # ignore waivers, see the raw numbers
"""
from __future__ import annotations

# tol: if edy / edvy agree within this width, treat it as "the same outlier"
TOL = 0.005

WAIVERS: list[dict] = [
    {
        "lv": 22,
        "t": 6129,
        "cause": "m2/mini0/g1/gdg0/sp1.1/clamp:fly/land/air",
        "in": "0",
        "edy": 0.0,
        "edvy": -0.129,
        "since": "2026-08-22",
        "why": (
            "In the rotated section GD alone drops onGround for one tick, takes "
            "a single step of gravity (-0.129), and is back at 0 the tick after. "
            "y agrees with the model on every tick (edy=0.000). The same blip "
            "occurs 48 times in the corpus and py/blipscan.py reproduces 47 of "
            "the 48, so this is a lone outlier rather than a hole in a "
            "mechanism. All four candidate rules were refuted by measurement: "
            "(1) r103, narrowing the contact tolerance, over-fires within 4 "
            "ticks either side; (2) r105, narrowing it by one tick of fall, "
            "does the same; (3) 'the support set shrinks but survives' was "
            "judged by py/shrinkscan.py to mis-fire on 97.9% of all 1,756 cases "
            "in the corpus; (4) deriving the geometry from the definitions "
            "rather than the recording has nothing to switch to, since group "
            "201 has no Move or Rotate trigger at all. The long version is in "
            "the development notes."
        ),
    },
]


def _match(d: dict, w: dict) -> bool:
    return (d["lv"] == w["lv"] and d["t"] == w["t"]
            and d["cause"] == w["cause"] and str(d["in"]) == str(w["in"])
            and abs(d["edy"] - w["edy"]) <= TOL
            and abs(d["edvy"] - w["edvy"]) <= TOL)


def split(found: list[dict]) -> tuple[list[dict], list[tuple[dict, dict]]]:
    """Split into (divergences that were not waived, [(divergence, waiver entry)])."""
    kept: list[dict] = []
    waived: list[tuple[dict, dict]] = []
    for d in found:
        hit = next((w for w in WAIVERS if _match(d, w)), None)
        if hit is None:
            kept.append(d)
        else:
            waived.append((d, hit))
    return kept, waived
