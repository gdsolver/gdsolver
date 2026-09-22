"""A turned hazard has two tests, and this fixture separates them (no game needed).

The game's hazard loop is the player's rect against the object's RECT, and then, only
when the object is oriented, the two turned boxes (the disassembly is quoted at
hazardHit in dp/src/dp/object.hpp). The model had the turned box in the FIRST test as
well, which is strictly tighter: a position inside the rect but outside the box is a
kill the game performs and the model does not (lv19 t=7,131 cost a cold iteration).

The fixture puts a cube exactly there -- one 30x30 hazard turned 45 degrees, the player
at dx = dy = 27, where

    the rect   (the 42.4 bound)   overlaps   |27| <= 21.2 + 15
    the box    (15/15 at 45 deg)  does not   38.2 >  15 + 21.2   (the object's axis)

so the three arms have to disagree in one direction only:

    no flag                  the box decides stage 1  -> survives
    --hazaabb --no-obb-all   stage 1 is the rect      -> dies
    --hazaabb                ...and stage 2 is the box -> survives again

(--obb-all, the second stage, has been on by default since 2026-09-21, so the
rect-only arm is the one that has to switch it off.)

The third arm is here because the second alone was measured once as if it were the
pair (the flag's first cold run had no stage 2 for any mode but the wave, which reads
as "the pair is wrong" when it was never tried).

WHICH ARM IS THE GAME'S IS NOT WHAT THIS FIXTURE SAYS. The rig
(data/rigs/calib_hazrot_wave.lvl, swept 2026-09-18 at rot 0/8/15/45, three columns on
the 45) puts the game's boundary on the recorded box against the player's box
UNTURNED, which is the first test's own shape -- so the second test, which turns the
player, is the one that differs from the game. This fixture pins the three arms apart
so that a change to any of them is visible; it does not vote on them.
"""

from __future__ import annotations

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
              "force,free,touch,spawn,chan,axis,exstat")
OBB_HEADER = "uid,id,type,cx,cy,rot,x0,y0,x1,y1,x2,y2,x3,y3,oob"

HAZ_UID = 7001
HAZ_CX, HAZ_CY = 1200.0, 300.0
# the turned box: a 30x30 at 45 degrees, so half-extents 15/15 on axes (c, s) = (.7071, .7071)
HALF = 15.0
BOUND = 21.2132      # 30 / sqrt(2) -- what the dump records as w/h for the turned object
PLAYER_X, PLAYER_Y = HAZ_CX + 27.0, HAZ_CY + 27.0


def obj_rows() -> list[str]:
    # a floor far below, so nothing else is in reach, and the hazard itself
    rows = []
    for i, x in enumerate(range(1100, 1400, 30)):
        rows.append(",".join(str(c) for c in
                             [1, 0, float(x), 15.0, 30, 30, 0, 100 + i] + [0] * 7
                             + [30, 30] + [0] * 31))
    haz = [8, 2, HAZ_CX, HAZ_CY, BOUND * 2, BOUND * 2, 0, HAZ_UID, 0, 45] + [0] * 5 \
        + [30, 30] + [0] * 31
    rows.append(",".join(str(c) for c in haz))
    return rows


def obb_row() -> str:
    c = s = 0.70710678
    # the four corners of the 30x30 turned 45 degrees, in the order the dump writes
    pts = []
    for sx, sy in ((-HALF, -HALF), (HALF, -HALF), (HALF, HALF), (-HALF, HALF)):
        pts += [HAZ_CX + sx * c - sy * s, HAZ_CY + sx * s + sy * c]
    return ",".join(str(v) for v in
                    [HAZ_UID, 8, 2, HAZ_CX, HAZ_CY, 45] + [round(p, 3) for p in pts] + [1])


def write_fixture(d: Path) -> None:
    (d / "objrects.txt").write_text(OBJ_HEADER + "\n" + "\n".join(obj_rows()) + "\n")
    (d / "obb.txt").write_text(OBB_HEADER + "\n" + obb_row() + "\n")
    (d / "plan.txt").write_text("input=1,0\n")


def start_fields() -> str:
    # tick, x, y, vy, mode(0 = cube), ... everything after the sixth field is zero
    return f"600,{PLAYER_X},{PLAYER_Y},0,0,0,0,0,0,0,0,0,0,0,0.899999976,0,0,0,0,0,0,0,0,0,0,-1,-1"


@unittest.skipUnless(Path(LEVELDP_EXE).exists(), "no built leveldp")
class TestTurnedHazardHasTwoStages(unittest.TestCase):
    def arm(self, flags: list[str]) -> bool:
        """True when the replay dies on the turned hazard."""
        with tempfile.TemporaryDirectory() as t:
            d = Path(t)
            write_fixture(d)
            r = subprocess.run(
                [str(LEVELDP_EXE), str(d / "objrects.txt"), "--replay", str(d / "plan.txt"),
                 "--start", start_fields(), "--out", str(d / "out"),
                 "--obb", str(d / "obb.txt"), "--cap", "50"] + flags,
                capture_output=True, text=True, timeout=120)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            return "DIED" in (r.stdout + r.stderr)

    def test_the_three_arms_disagree_in_one_direction(self):
        # stage 1 with the box: the position is outside it, so nothing happens
        self.assertFalse(self.arm([]), "the box in stage 1 should let this position live")
        # stage 1 with the rect: the game's first test, and this position is inside it
        self.assertTrue(self.arm(["--hazaabb", "--no-obb-all"]),
                        "the rect in stage 1 should kill here")
        # ...and the game's second test (on by default) puts the box back, which
        # saves it again
        self.assertFalse(self.arm(["--hazaabb"]), "stage 2 should re-apply the box")
