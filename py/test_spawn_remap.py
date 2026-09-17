# -*- coding: utf-8 -*-
"""A Spawn's group remap is a property of the BRANCH, not of the trigger. (no GD; seconds)

    python py/test_spawn_remap.py
    python py/test_spawn_remap.py -v

lv22 builds its sinking block rows from one template: group 493 holds a Move
that names group 100, and each touch box spawns 493 under its own remap --
uid17771 sends 100 to 508, uid17772 sends it to 510 (property 442, read from
the level string). A walk that rewrote the shared trigger row, or cached what
a group reaches by group id alone, would let whichever box it met first fix
100 for the others.

The fixture is that shape and nothing else, three roots over one template:

    root A  spawn 493, remap 100 -> 508     reaches group 508 (2 objects)
    root B  spawn 493, remap 100 -> 510     reaches group 510 (3 objects)
    root C  spawn 493, no remap             reaches group 100 (5 objects)

The group sizes differ so a count names the group. Every root must reach its
own target and only that, with the rows in either order; with
--no-spawnremap all three reach group 100, which is the pre-remap behaviour.
Read off leveldp's own `triggers: box ... moves N objects` lines, so what is
tested is the shipped walk, not a transcription of it.
"""

from __future__ import annotations

import re
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
TRIG_HEADER = ("uid,id,cx,cy,w,h,target,center,touch,spawn,dur,ox,oy,ease,erate,"
               "lockx,locky,grav,gravmod,deg,ord,chan,sord,sordd,t360,lockrot,"
               "sdelay,mvtgt,mvaxis,tmodctr,dirsnap,dirdist,dynmode,silent,togon,remap")

GROUPS = {508: [2001, 2002], 510: [3001, 3002, 3003], 100: [4001, 4002, 4003, 4004, 4005]}
ROOTS = {1001: (300, "100:100:508:0;101:101:509:0"),
         1002: (400, "100:100:510:0"),
         1003: (500, "-")}
EXPECT = {1001: 2, 1002: 3, 1003: 5}
TEMPLATE_MOVE = 900   # in group 493, target 100


def trig_row(uid, tid, cx, target, touch, spawn, oy, remap):
    cols = [uid, tid, cx, 300, 30, 30, target, 0, touch, spawn, 0.5, 0, oy, 0, 2,
            0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, -1, remap]
    return ",".join(str(c) for c in cols)


def obj_row(uid, cx):
    cols = [1, 0, cx, 105, 30, 30, 1, uid] + [0] * 7 + [30, 30] + [0] * 31
    return ",".join(str(c) for c in cols)


def write_fixture(d: Path, reverse: bool) -> None:
    objs = [obj_row(u, 1000 + 30 * i)
            for i, u in enumerate(u for g in GROUPS.values() for u in g)]
    (d / "objrects.txt").write_text(OBJ_HEADER + "\n" + "\n".join(objs) + "\n")
    trigs = [trig_row(TEMPLATE_MOVE, 901, 50, 100, 0, 1, -12, "-")]
    trigs += [trig_row(u, 1268, cx, 493, 1, 0, 0, rm) for u, (cx, rm) in ROOTS.items()]
    if reverse:
        trigs.reverse()
    (d / "triggers.txt").write_text(TRIG_HEADER + "\n" + "\n".join(trigs) + "\n")
    grp = [f"{TEMPLATE_MOVE} 493"] + [f"{u} {g}" for g, us in GROUPS.items() for u in us]
    if reverse:
        grp.reverse()
    (d / "objgroups.txt").write_text("uid,groups\n" + "\n".join(grp) + "\n")


BOX = re.compile(r"^triggers: box \d+ uid (\d+) \([^)]*\) \S+ moves (\d+) objects")


def boxes(d: Path, flags: list[str]) -> dict[int, int]:
    exe = Path(LEVELDP_EXE)
    r = subprocess.run([str(exe), str(d / "objrects.txt"), "--horizon", "2",
                        "--out", str(d / "out.txt"),
                        "--triggers", str(d / "triggers.txt"),
                        "--objgroups", str(d / "objgroups.txt"), *flags],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       text=True, timeout=120)
    out = {}
    for line in r.stdout.splitlines():
        m = BOX.match(line)
        if m:
            out[int(m.group(1))] = int(m.group(2))
    return out


@unittest.skipUnless(Path(LEVELDP_EXE).exists(), "no built leveldp")
class TestSpawnRemapIsPerBranch(unittest.TestCase):
    def run_arm(self, reverse: bool, flags: list[str]) -> dict[int, int]:
        with tempfile.TemporaryDirectory() as t:
            d = Path(t)
            write_fixture(d, reverse)
            got = boxes(d, flags)
        # The fixture is only a test if all three boxes came out; an empty
        # parse would otherwise pass every comparison below vacuously.
        self.assertEqual(sorted(got), sorted(ROOTS), f"boxes printed: {got}")
        return got

    def test_each_root_reaches_only_its_own_target(self):
        for reverse in (False, True):
            with self.subTest(reverse=reverse):
                self.assertEqual(self.run_arm(reverse, []), EXPECT)
                self.assertEqual(self.run_arm(reverse, ["--spawnremap"]), EXPECT)

    def test_without_the_flag_every_root_reaches_the_template_group(self):
        # the remap is on by default since v0.1.4; --no-spawnremap is the off arm
        for reverse in (False, True):
            with self.subTest(reverse=reverse):
                self.assertEqual(self.run_arm(reverse, ["--no-spawnremap"]),
                                 {u: len(GROUPS[100]) for u in ROOTS})


if __name__ == "__main__":
    unittest.main()
