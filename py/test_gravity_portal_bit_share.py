"""Sharing a gravity-portal latch bit past kGravPortalBits, step by step (no game needed).

State::portalLatch has 128 bits. A level with more gravity portals used to be refused; now,
on a level with no reversal, the portal 128 later in x order takes the bit of the one before
it, and the step clears the bit where x crosses the point between the two
(dp/src/dp/prelude.hpp, g_gpHandoff). Two real levels clearing proves none of the steps
below on its own, so each is pinned here.

The fixture is 130 normal-gravity portals (id 10, type 4) every 60 px along the ground and a
cube that walks through all of them without a press. A normal portal leaves an upright
cube's gravity alone, but GD latches a gravity portal on its first overlap whether or not
it flips anything, so every portal sets its bit and nothing else changes. Portal 0 and
portal 128 share bit 0, portal 1 and portal 129 bit 1.

    1. portal 0 is entered unspent and sets bit 0
    2. bit 0 is still set before the hand-over point and clear after it
    3. portal 128 is entered unspent -- without the hand-over it would read as spent
    4. an anchor past the hand-over that names portal 0 as spent does not set bit 0
    5. a reversal, or a pair closer than the sharing gap, is refused as before; 128 portals
       share nothing

Read from --slopedbg: the loader's `gpbit` lines (uid -> bit) and the portal pass's
`gplatch` lines (the bit, whether it was spent coming in, and the incoming mask).
The second body's mask (portalLatch2) is cleared by the same statement and is not
exercised: none of the levels that need sharing has a dual.
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
              "force,free,touch,spawn,chan,axis,exstat,rev,nocol")

X0, STEP = 300.0, 60.0
UID0 = 5000


def portal_row(i: int, cx: float, rev: int = 0) -> str:
    # a 25 x 75 portal centred at y 120, so the cube's box (90..120 on the ground) is inside it
    cols = [10, 4, cx, 120.0, 25, 75, 0, UID0 + i, 0, 0] + [0] * 5 + [25, 75] + [0] * 31 \
        + [rev, 0]
    return ",".join(str(c) for c in cols)


def run(rows: list[str], extra: list[str]) -> subprocess.CompletedProcess:
    with tempfile.TemporaryDirectory() as t:
        d = Path(t)
        (d / "objrects.txt").write_text(OBJ_HEADER + "\n" + "\n".join(rows) + "\n")
        (d / "plan.txt").write_text("input=1,0\n")
        return subprocess.run(
            [str(LEVELDP_EXE), str(d / "objrects.txt"), "--replay", str(d / "plan.txt"),
             "--out", str(d / "out"), "--cap", "50", "--slopedbg", "--shadowstep=all"] + extra,
            capture_output=True, text=True, timeout=300)


def spread(n: int, step: float = STEP, rev_at: int = -1) -> list[str]:
    return [portal_row(i, X0 + i * step, 1 if i == rev_at else 0) for i in range(n)]


GPBIT = re.compile(r"^gpbit bit=(\d+) uid=(\d+)", re.M)
GPLATCH = re.compile(r"^gplatch half=0 t=(\d+) uid=(\d+) bit=(\d+) spent=(\d) mask=(\S+)", re.M)


def first_latch(out: str) -> dict[int, tuple[int, int, int]]:
    """uid -> (tick, spent, mask) of the first gplatch line naming it"""
    seen: dict[int, tuple[int, int, int]] = {}
    for t, uid, _bit, spent, mask in GPLATCH.findall(out):
        if int(uid) not in seen:
            seen[int(uid)] = (int(t), int(spent), int(mask, 16))
    return seen


def handoff_x() -> float:
    a, b = X0, X0 + 128 * STEP
    return ((a + 12.5) + (b - 12.5)) / 2.0


@unittest.skipUnless(Path(LEVELDP_EXE).exists(), "no built leveldp")
class TestGravityPortalBitShare(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        r = run(spread(130), [])
        cls.out = r.stdout + r.stderr
        cls.rc = r.returncode

    def test_0_the_level_is_accepted_and_two_bits_are_shared(self):
        self.assertEqual(self.rc, 0, self.out[-2000:])
        self.assertIn("gravity portals: 2 bits shared past 128", self.out)
        bits = {int(u): int(b) for b, u in GPBIT.findall(self.out)}
        self.assertEqual(bits[UID0 + 0], 0)
        self.assertEqual(bits[UID0 + 128], 0)
        self.assertEqual(bits[UID0 + 129], 1)

    def test_1_portal_0_enters_unspent(self):
        first = first_latch(self.out)
        # every portal was reached and printed, so the reads below are not of an empty table
        self.assertEqual(sorted(first), [UID0 + i for i in range(130)])
        self.assertEqual(first[UID0 + 0][1], 0)

    def test_2_bit_0_clears_at_the_hand_over(self):
        first = first_latch(self.out)
        hx = handoff_x()
        before = max(i for i in range(128) if X0 + i * STEP + 12.5 < hx - 20)
        after = min(i for i in range(128) if X0 + i * STEP - 12.5 > hx + 20)
        self.assertTrue(first[UID0 + before][2] & 1, "bit 0 should still be set before it")
        self.assertFalse(first[UID0 + after][2] & 1, "bit 0 should be clear after it")

    def test_3_portal_128_enters_unspent(self):
        first = first_latch(self.out)
        self.assertEqual(first[UID0 + 128][1], 0, "the hand-over should have freed bit 0")

    def anchored(self, x: float) -> str:
        t = int(x / 1.2982504)
        start = f"{t},{x},105,0,0,1,0,0,0,0,0,0,0,0,0.899999976,0,0,0,0,0,0,0,0,0,0,-1,-1"
        r = run(spread(130), ["--start", start,
                              "--anchor-state", f"owns=portal;portal={UID0};portal2="])
        out = r.stdout + r.stderr
        self.assertEqual(r.returncode, 0, out[-2000:])
        return out

    def test_4_an_anchor_past_the_hand_over_does_not_seed_portal_0(self):
        # control: before the hand-over the same payload does set the bit
        self.assertIn("seed payload: 1 gravity portals spent", self.anchored(handoff_x() - 200.0))
        out = self.anchored(handoff_x() + 200.0)
        self.assertIn("seed payload: 0 gravity portals spent", out)
        self.assertEqual(first_latch(out)[UID0 + 128][1], 0)

    def test_5_refusals_and_the_width_itself(self):
        r = run(spread(130, rev_at=5), [])
        self.assertIn("unsupported: gravity portals: 130 (limit 128, reversal)", r.stdout)
        r = run(spread(130, step=0.5), [])
        self.assertIn("too close to share bits", r.stdout)
        r = run(spread(128), [])
        out = r.stdout + r.stderr
        self.assertEqual(r.returncode, 0, out[-2000:])
        self.assertNotIn("bits shared", out)


if __name__ == "__main__":
    unittest.main()
