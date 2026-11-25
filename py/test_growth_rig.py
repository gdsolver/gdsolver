"""A mini body that grows to full size on a block row, against GD's own numbers (no game needed).

Three rules in speed.hpp decide what a grounded body does on the tick a size portal makes it
bigger, and all were measured on calibration rigs built from data/rigs/calib_growaway.lvl:

    Multiple grow supports
                the grown body is lifted onto its solids on the grow tick only when its pre-grow
                box stands on two or more of them (across a seam, or on solids that overlap --
                GD counts solids, not edges); on one solid it is lifted by the next tick's
                support -- whatever the mode
    Grow-away   a body moving away from its floor by then is not lifted at all
    Size-press  a press on the grow tick jumps or taps with the full-size value; the cube is
                lifted first only where two or more supports lift it on the grow tick

The rig: a mode portal and a mini portal on the ground, then a row of 30-wide blocks with their
tops at y 150 and a normal-size portal across it at x 900 (the blocks meet at 885 and 915). The
player is placed on the row ten ticks before the grow tick G (y 159 = 150 + the mini half 9,
vy 0) and either rests, or presses on one of the ticks before G and holds. Where the anchor's x
puts the pre-grow box decides the seam: from x 863.343506 its right edge is at 885.33 on G, over
the 885 seam; from 862.536 it is at 884.52, on one block. The ball unit sits 1,200 further on
(G 1,600, seam at 2,085): from 2,062.90576 its edge is at 2,084.89, one block; from 2,063.40576
at 2,085.39, over the seam. The WIDE variant replaces the blocks around the portal with one
block 120 wide (840..960), so there is no seam at all.

GD's y and vy for the rows G-1..G+2 of every cell are below, read from the game's per-tick dump;
the model replays each cell from the same anchor and has to reproduce every row. The objects are
the rig's objrects as the mod exported them (with the clearance boxes, id 38). The cells that
tell the rules apart: the cube and the ship on one block at rest (165 only on G+1) against the
seam (165 on G); the ball over the seam (165 on G, where the old mode rule said G+1); the ship
on one block pressing a tick before G (never lifted -- a custom level, t=5,974); the wide block
with a press (the cube jumps from 159, not lifted first).

GDSOLVER_LEVELDP points this at another build.
"""

from __future__ import annotations

import csv
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas.paths import LEVELDP_EXE

OBJ_HEADER = ("id,type,cx,cy,w,h,groups,uid,radius,rot,sy0,sy1,shz,sdir,sup,w0,h0,"
              "tpy,tpg,tpix,tpiy,tw,zoom,zdur,zease,zrate,mvdir,gnddir,optp1,optp2,"
              "flipx,flipy,nofx,notouch,tpex,tpey,dis,editvel,vmodx,vmody,ovrvel,"
              "force,free,touch,spawn,chan,axis,exstat,rev,nocol")

# (id, type, cx, cy, w, h, uid, w0, h0); every other column is 0
PORTALS = [
    (12, 6, 300, 105, 34, 86, 12, 34, 86), (38, 7, 300, 105, 25.5, 77.5, 13, 25.5, 77.5),
    (12, 6, 300, 195, 34, 86, 14, 34, 86), (38, 7, 300, 195, 25.5, 77.5, 15, 25.5, 77.5),
    (101, 18, 390, 105, 31, 90, 16, 31, 90), (38, 7, 390, 105, 26, 75.5, 17, 26, 75.5),
    (99, 17, 900, 105, 31, 90, 35, 31, 90), (38, 7, 900, 105, 25.75, 75.5, 36, 25.75, 75.5),
    (99, 17, 900, 195, 31, 90, 37, 31, 90), (38, 7, 900, 195, 25.75, 75.5, 38, 25.75, 75.5),
    (47, 16, 1500, 105, 34, 86, 39, 34, 86), (38, 7, 1500, 105, 25.5, 77.5, 40, 25.5, 77.5),
    (47, 16, 1500, 195, 34, 86, 41, 34, 86), (38, 7, 1500, 195, 25.5, 77.5, 42, 25.5, 77.5),
    (101, 18, 1590, 105, 31, 90, 43, 31, 90), (38, 7, 1590, 105, 26, 75.5, 44, 26, 75.5),
    (99, 17, 2100, 105, 31, 90, 62, 31, 90), (38, 7, 2100, 105, 25.75, 75.5, 63, 25.75, 75.5),
    (99, 17, 2100, 195, 31, 90, 64, 31, 90), (38, 7, 2100, 195, 25.75, 75.5, 65, 25.75, 75.5),
    (1, 0, 3600, 105, 30, 30, 66, 30, 30), (0, 7, 3940, 225, 30, 30, 67, 30, 30),
]
ROW = [(1, 0, 600 + 30 * i, 135, 30, 30, 18 + i, 30, 30) for i in range(17)] \
    + [(1, 0, 1800 + 30 * i, 135, 30, 30, 45 + i, 30, 30) for i in range(17)]
# calib_growwide: the blocks at 840..960 are one block 120 wide (as the mod exported it)
WIDE = [r for r in ROW if not 825 < r[2] < 975] + [(1, 0, 900, 135, 120, 30, 70, 30, 30)]
# Is the seam rule about seams or about the number of solids? Two variants of the wide block
# tell them apart, and GD lifts on the grow tick in both, so it counts solids:
#   dup      the same 120-wide block written twice (two solids, no edge anywhere near the box).
#            GD measured it on a cube cell at x0 1,500 (G 1,600, x 2,075.89 on G, 0.44 behind
#            the wide cell); it is placed here at the wide cell's position, where the box
#            (867..885 on G) is as far from any edge.
#   partial  the wide block plus a second 120-wide one whose left edge is under the body's
#            centre on G (876.326..996.326), so the box straddles an edge that block A spans.
DUP = WIDE + [(1, 0, 900, 135, 120, 30, 71, 30, 30)]
PARTIAL = WIDE + [(1, 0, 936.326, 135, 120, 30, 71, 30, 30)]
LEVELS = {"row": PORTALS + ROW, "wide": PORTALS + WIDE, "dup": PORTALS + DUP,
          "partial": PORTALS + PARTIAL}

MODE = {"cube": 0, "ship": 1, "ball": 2}
# name -> (level, unit mode, grow tick G in GD's dump, GD's x at G - 10)
UNITS = {
    "cube seam": ("row", "cube", 676, 863.343506),
    "cube one": ("row", "cube", 676, 862.536),
    "ship seam": ("row", "ship", 676, 863.343506),
    "ship one": ("row", "ship", 676, 862.536),
    "ball one": ("row", "ball", 1600, 2062.90576),
    "ball seam": ("row", "ball", 1600, 2063.40576),
    "cube wide": ("wide", "cube", 676, 863.343506),
    "cube dup": ("dup", "cube", 676, 863.343506),
    "cube partial": ("partial", "cube", 676, 863.343506),
}

# (unit, press tick relative to G or None for rest) -> GD's (y, vy) on rows G-1, G, G+1, G+2
GD = {
    ("cube seam", None): [(159, 0), (165, 0), (165, 0), (165, 0)],
    ("cube seam", -1): [(159, 0), (165, 11.18), (167.467, 10.964), (169.885, 10.748)],
    ("cube seam", -2): [(159, 8.944), (160.964, 8.728), (162.879, 8.512), (164.746, 8.296)],
    ("cube seam", -3): [(160.964, 8.728), (162.879, 8.512), (164.746, 8.296), (166.564, 8.08)],
    ("cube seam", -4): [(162.879, 8.512), (164.746, 8.296), (166.564, 8.08), (168.333, 7.864)],
    ("cube one", None): [(159, 0), (159, 0), (165, 0), (165, 0)],
    ("ship seam", None): [(159, 0), (165, 0), (165, 0), (165, 0)],
    ("ship seam", -1): [(159, 0), (165, 0), (165.024, 0.108), (165.073, 0.216)],
    ("ship one", None): [(159, 0), (159, 0), (165, 0), (165, 0)],
    ("ship one", -1): [(159, 0), (159, 0), (159.0243, 0.108), (159.0729, 0.216)],
    ("ball one", None): [(159, 0), (159, 0), (165, 0), (165, 0)],
    ("ball one", -1): [(159, 0), (159, 3.354), (159.784, 3.483), (160.596, 3.612)],
    ("ball one", -2): [(159, 2.6832), (159.633, 2.813), (160.295, 2.942), (160.986, 3.071)],
    ("ball one", -3): [(159.633, 2.813), (160.295, 2.942), (160.986, 3.071), (161.706, 3.2)],
    ("ball one", -4): [(160.295, 2.942), (160.986, 3.071), (161.706, 3.2), (162.455, 3.329)],
    ("ball seam", None): [(159, 0), (165, 0), (165, 0), (165, 0)],
    ("cube wide", None): [(159, 0), (159, 0), (165, 0), (165, 0)],
    ("cube wide", -1): [(159, 0), (159, 11.18), (161.467, 10.964), (163.885, 10.748)],
    ("cube wide", -2): [(159, 8.944), (160.964, 8.728), (162.879, 8.512), (164.746, 8.296)],
    ("cube dup", None): [(159, 0), (165, 0), (165, 0), (165, 0)],
    ("cube dup", -1): [(159, 0), (165, 11.18), (167.467, 10.964), (169.885, 10.748)],
    ("cube dup", -2): [(159, 8.944), (160.964, 8.728), (162.879, 8.512), (164.746, 8.296)],
    ("cube partial", None): [(159, 0), (165, 0), (165, 0), (165, 0)],
    ("cube partial", -1): [(159, 0), (165, 11.18), (167.467, 10.964), (169.885, 10.748)],
    ("cube partial", -2): [(159, 8.944), (160.964, 8.728), (162.879, 8.512), (164.746, 8.296)],
}
TOL = 0.0006          # GD's y is read to three or four decimals


def objrects(level: str) -> str:
    rows = []
    for oid, typ, cx, cy, w, h, uid, w0, h0 in LEVELS[level]:
        cols = [oid, typ, cx, cy, w, h, 0, uid, 0, 0, 0, 0, 0, 0, 0, w0, h0] + [0] * 33
        rows.append(",".join(str(c) for c in cols))
    return OBJ_HEADER + "\n" + "\n".join(rows) + "\n"


def replay(unit: str, press: int | None, extra: list[str]) -> dict[int, tuple[float, float]]:
    level, mode, g, x = UNITS[unit]
    t0 = g - 10
    # mini, upright, on the row, at speed 0.9 -- the anchor GD's rig placed the body at
    start = f"{t0},{x},159,0,{MODE[mode]},1,0,0,1,0,0,0,0,0,0.899999976,0,0,0,-1,0,0,0,0,0,0,-1,-1"
    plan = "input=1,0\n" if press is None else f"input={g + press},1\ninput={g + 6},0\n"
    with tempfile.TemporaryDirectory() as t:
        d = Path(t)
        (d / "objrects.txt").write_text(objrects(level))
        (d / "plan.txt").write_text(plan)
        subprocess.run([str(LEVELDP_EXE), str(d / "objrects.txt"), "--replay", str(d / "plan.txt"),
                        "--start", start, "--out", str(d / "m"), "--cap", "50", "--shadowstep=all"] + extra,
                       capture_output=True, text=True, timeout=120, check=True)
        with open(d / "m.trace.csv", encoding="utf-8-sig") as f:
            return {int(r["tick"]): (float(r["y"]), float(r["vy"])) for r in csv.DictReader(f)}


class GrowthRig(unittest.TestCase):
    def test_every_cell_matches_gd(self):
        extra = sys.argv[1:] if __name__ == "__main__" else []
        for (unit, press), want in GD.items():
            g = UNITS[unit][2]
            got = replay(unit, press, extra)
            for k, (gy, gvy) in enumerate(want):
                t = g - 1 + k
                with self.subTest(unit=unit, press=press, row=f"G{k - 1:+d}"):
                    self.assertIn(t, got)
                    my, mvy = got[t]
                    self.assertAlmostEqual(my, gy, delta=TOL)
                    self.assertAlmostEqual(mvy, gvy, delta=TOL)


if __name__ == "__main__":
    # extra leveldp flags may follow, for running an older build with its flags
    unittest.main(argv=sys.argv[:1])
