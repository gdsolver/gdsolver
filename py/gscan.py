# -*- coding: utf-8 -*-
"""Count the free-fall vy change per tick, per mode and per speed, from the GD reference.

Only ticks where nothing is held, the player is not grounded, and vy is not at
terminal velocity are considered.
"""
import csv
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdtas.paths import LEVEL_DATA  # noqa: E402

want = sys.argv[1] if len(sys.argv) > 1 else None
hist = defaultdict(Counter)

for lv in range(1, 23):
    try:
        f = open(LEVEL_DATA / "gdref" / f"lv{lv}.csv", newline="",
                 encoding="utf-8-sig")
    except OSError:
        continue
    rows = []
    with f:
        for r in csv.DictReader(f):
            try:
                rows.append((int(r["tick"]), float(r["yvel"]), r["onGround"],
                             r["mode"], float(r["speed"] or 0),
                             float(r["vsize"] or 1), r["upsideDown"]))
            except (ValueError, KeyError):
                continue
    for i in range(2, len(rows)):
        t0, v0, g0, m0, sp0, vs0, u0 = rows[i - 1]
        t1, v1, g1, m1, sp1, vs1, u1 = rows[i]
        tp, vp, gp, mp, spp, vsp, up = rows[i - 2]
        if t1 != t0 + 1 or t0 != tp + 1:
            continue
        if g0 == "1" or g1 == "1" or gp == "1":
            continue
        if m0 != m1 or m1 != mp or sp0 != sp1 or vs0 != vs1 or u0 != u1:
            continue
        if want and m1 != want:
            continue
        d = round(v1 - v0, 4)
        # Drop ticks that were capped at terminal velocity
        if abs(d) < 1e-9 or abs(d) > 1.0:
            continue
        hist[(m1, sp1, round(vs1, 2), u1)][d] += 1

for k in sorted(hist, key=lambda k: (k[0], k[1], k[2], k[3])):
    top = hist[k].most_common(3)
    n = sum(hist[k].values())
    print(f"{k[0]:<7} speed={k[1]:<4} vsize={k[2]:<4} up={k[3]}  n={n:<6} "
          + "  ".join(f"{d:+.4f} x{c}" for d, c in top))
