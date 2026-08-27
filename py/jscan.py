# -*- coding: utf-8 -*-
"""Count the vy at the tick the player leaves the ground (= initial jump velocity),
per mode, from the GD reference."""
import csv
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdtas.paths import LEVEL_DATA  # noqa: E402

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
    for i in range(1, len(rows)):
        t0, v0, g0, m0, sp0, vs0, u0 = rows[i - 1]
        t1, v1, g1, m1, sp1, vs1, u1 = rows[i]
        if t1 != t0 + 1 or g0 != "1" or g1 == "1" or m0 != m1 or u0 != u1:
            continue
        if abs(v1) < 0.5:      # just walked off an edge, not a jump
            continue
        hist[(m1, sp1, round(vs1, 2), u1)][round(abs(v1), 4)] += 1

print(f"{'mode':<7} {'speed':<6} {'vsize':<6} {'up':<3} {'n':<5} most common")
for k in sorted(hist, key=lambda k: (k[0], k[1], k[2], k[3])):
    top = hist[k].most_common(4)
    n = sum(hist[k].values())
    print(f"{k[0]:<7} {k[1]:<6.2f} {k[2]:<6} {k[3]:<3} {n:<5} "
          + "  ".join(f"{d:.4f} x{c}" for d, c in top))
