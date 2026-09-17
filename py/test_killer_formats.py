# -*- coding: utf-8 -*-
u"""Pin the two `killer:` line formats destroyPlayer prints, and who reads which.
(no GD, no file I/O; seconds)

    python py/test_killer_formats.py            # all
    python py/test_killer_formats.py -v         # with names

`destroyPlayer` (src/mod/hooks_playlayer.cpp) prints `killer:` in two shapes:

    #1  killer: t=<T> who=p1|p2|? py=... pvy=... px=... obj=yes|NULL uid=<U>
        id=<I> type=<Y> ox=... oy=...
        gate: (hitboxTrace || dpSolve) && g_started && !g_sessionOver
        reader: py/itermap_from_log.py's RE_KILLER (anchored on `^killer: t=`)

    #2  killer: tick=<T> who=p1|p2|? uid=<U> id=<I> type=<Y> ox=... oy=...
        or  killer: tick=<T> who=p1|p2|? (no object) px=... py=...
        gate: g_gateTrace, only once destroyPlayer's original call has set
        m_isDead
        reader: py/deathref.py's record_one (through deathref.select_killer)

Both start with the literal `killer:`, so `record_one`'s old loop -- which
selected on that prefix alone -- accepted lines of either shape as candidates,
and `R.parse_killer`'s key=value reading pulls `uid`/`ox`/`oy` out of a #1 line
just as readily as out of a #2 one. #1 carries no `tick=` key (it has `t=`
instead), so tick-matching against it always misses, and an unlucky #1 line
could be adopted as the "last killer: line" fallback.

`deathref.select_killer` fixes this by discarding any `killer:` line without a
`tick` key before it is even a candidate. This file pins that against
hand-built log lines -- no GD, no recorded reference -- and separately pins
that `itermap_from_log.RE_KILLER` only ever matches the #1 shape, so the two
readers cannot be pointed at each other's lines by construction.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from deathref import select_killer                                # noqa: E402
from itermap_from_log import RE_KILLER as ITERMAP_RE_KILLER       # noqa: E402


def fmt1(t, who="p1", uid=100, id_=35, type_=8, ox=10.0, oy=20.0):
    """Build a #1-format line (hitboxtrace/dpsolve), `t=`, no `tick=`."""
    return (f"killer: t={t} who={who} py=123.456 pvy=-1.234 px=234.567 "
            f"obj=yes uid={uid} id={id_} type={type_} ox={ox:.3f} oy={oy:.3f}")


def fmt2(t, who="p1", uid=200, id_=36, type_=9, ox=30.0, oy=40.0):
    """Build a #2-format line (gatetrace), `tick=`, with an object."""
    return (f"killer: tick={t} who={who} uid={uid} id={id_} type={type_} "
            f"ox={ox:.1f} oy={oy:.1f} rect=1.00,2.00,3.00,4.00 on=1 "
            f"px=345.6 py=456.7")


def fmt2_no_object(t, who="p1"):
    """Build a #2-format line with no object."""
    return f"killer: tick={t} who={who} (no object) px=345.6 py=456.7"


class TestSelectKiller(unittest.TestCase):
    """deathref.select_killer picks only #2-format lines."""

    def test_tick_match_among_several_format2_lines(self):
        lines = [fmt2(100, uid=1), fmt2(200, uid=2), fmt2(300, uid=3)]
        killer, other = select_killer(lines, 200)
        self.assertIn("uid2", killer)
        self.assertNotIn("(unmatched)", killer)
        self.assertEqual(other, 0)

    def test_no_tick_match_falls_back_to_last_format2_unmatched(self):
        lines = [fmt2(100, uid=1), fmt2(200, uid=2)]
        killer, other = select_killer(lines, 999)
        self.assertIn("uid2", killer)
        self.assertIn("(unmatched)", killer)
        self.assertEqual(other, 0)

    def test_format1_only_yields_no_candidate(self):
        """Candidate count is zero: not even "(unmatched)"."""
        lines = [fmt1(100, uid=1), fmt1(200, uid=2)]
        killer, other = select_killer(lines, 200)
        self.assertEqual(killer, "")
        self.assertEqual(other, 2)

    def test_format1_and_format2_mixed_format1_ignored(self):
        lines = [fmt1(200, uid=999), fmt2(100, uid=1), fmt2(200, uid=2),
                 fmt1(300, uid=888)]
        killer, other = select_killer(lines, 200)
        self.assertIn("uid2", killer)
        self.assertNotIn("uid999", killer)
        self.assertNotIn("(unmatched)", killer)
        self.assertEqual(other, 2)

    def test_who_p2_format2_line_is_selected(self):
        """Selection is not p1-only."""
        lines = [fmt2(200, who="p2", uid=7)]
        killer, other = select_killer(lines, 200)
        self.assertIn("uid7", killer)
        self.assertEqual(other, 0)

    def test_no_object_format2_line_is_a_candidate(self):
        lines = [fmt2_no_object(200, who="p2")]
        killer, other = select_killer(lines, 200)
        self.assertIn("no object", killer)
        self.assertEqual(other, 0)


class TestItermapRegexOnlyMatchesFormat1(unittest.TestCase):
    """itermap_from_log.RE_KILLER must not be fooled by #2 lines."""

    def test_matches_format1(self):
        self.assertIsNotNone(ITERMAP_RE_KILLER.match(fmt1(100)))

    def test_does_not_match_format2(self):
        self.assertIsNone(ITERMAP_RE_KILLER.match(fmt2(100)))

    def test_does_not_match_format2_no_object(self):
        self.assertIsNone(ITERMAP_RE_KILLER.match(fmt2_no_object(100)))


if __name__ == "__main__":
    unittest.main(verbosity=2 if "-v" in sys.argv else 1,
                  argv=[a for a in sys.argv if a != "-v"])
