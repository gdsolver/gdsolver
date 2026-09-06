# -*- coding: utf-8 -*-
"""The rotated-pad gate, asked as two questions instead of one. (no GD; seconds)

    python py/test_leaf_padgate.py          # all
    python py/test_leaf_padgate.py -v       # with names
    python py/test_leaf_padgate.py --show   # print the table, run nothing

    Q1  Given GD's OWN inputs, does the predicate return GD's answer?
    Q2  Given the same situation, do the MODEL's inputs equal GD's?

Every other instrument in this repository asks one question -- "does the whole
replay change" -- and a leaf that is RIGHT BUT STARVED reads exactly like a
leaf that is WRONG. That is not hypothetical. On 2026-09-06 the shape rule for
a rotated pad (59ba018) was written against a base without 351f9de, where the
angle it was fed was 6.9 degrees wrong; at that error the turned square cleared
the board anyway. The rule changed nothing, in any arm, on all 22 levels, and
its acceptance table said so. Rebased onto a base that HAS 351f9de the same
rule moves lv20's firing 7,296 -> 7,298. The pair was order-dependent, and
neither commit's acceptance table could show it.

Below, Q1 is green and Q2 is red, on the same six numbers.

WHAT THIS CANNOT PROVE. gdtas.padgate is a TRANSCRIPTION of the C++; nothing
here executes dp/. So Q1 is a statement about the RULE (this reading of GD's
predicate reproduces GD's answers), not about the shipped implementation, and
it would stay green if the C++ were edited to disagree with it. The guard
against that is `check_transcription`, which fingerprints the copied code and
refuses when it moves -- a stale-transcription alarm, not a proof.

Q2 needs no transcription at all: it is two angle series against each other.

SOURCES, all on disk, no game:
    board geometry     GD-lab/data/objrects_lv20.txt, rows uid 6963/6964/
                       7025/7026/7027/7030 (id 35, type 8, rot 29). These six
                       are the whole rotated-pad population of the 22-level
                       corpus; the other 233 type-8 pads are within 0.5 deg of
                       a quarter turn and `oriented` is not even set for them.
    GD's per-tick x, y, rot, yvel, mode, vsize, upsideDown
                       build/fidelity/fid_lv20.dump.csv (2026-09-06 08:49),
                       agreeing digit for digit with GD-lab/data/gdref/lv20.csv
                       on every pinned tick.
    GD's activation tick
                       the same dump's `yvel` column: -8.648 at 7,298 and
                       +16.000 at 7,299, the only |yvel| == 16 anywhere in
                       7,280..7,320 (checked over that whole span whenever the
                       dump is present). Taken from the deciding column, not
                       from a comment that names the tick.
    the model's angle  build/fidelity/{fid,abnew}_lv20.trace.csv, `rot` and
                       `rotneg` of row t-1 (see padgate.sat_angle on the
                       phase). The two files differ at t=7,295 by exactly one
                       spin step's SIGN -- 128.1656 stepping to +129.8963 in
                       one and to -126.4348 in the other -- which is the single
                       expression 351f9de changed. The arms are named here by
                       that sign and NOT by the commit: the attribution rests
                       on the two files' timestamps straddling it, and neither
                       trace records the argv it was produced with.

CROSS-CHECK. The pinned `flat` and `gd` margin rows are the table already
written into dp/src/dp/constants.hpp above `g_noPadPlayerRot`, and the `pre`
row is the one in GD-lab/notes/measure-padplayerrot-2026-09-06.md section 5.
They were recomputed here from the raw columns and agree to 1e-3; the table in
constants.hpp is rounded to three decimals, which is why that is the tolerance.
"""
from __future__ import annotations

import csv
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas import padgate
from gdtas.compare import (GD, MODEL, Difference, Half, MODEL_MINUS_GD,
                           Provenance, Series, Window, check_coverage,
                           conditions_of, difference, fidelity_dir, read_table,
                           series)
from gdtas.padgate import (Board, PLAYER_HALF_FULL, aabb_conjunct,
                           angle_error_mod90, gate, mod90, obb_sat_margin,
                           read_objrects, sat_angle)
from gdtas.paths import LEVEL_DATA, REPO

# ------------------------------------------------------------------ fixture
# Pinned 2026-09-06 at 6a3c721. Every value below was read off the file named
# in the module docstring; none was copied from a note or a comment.

LEVEL = 20
WINDOW = Window(7295, 7301)          # the ticks the gate is live for uid 7030
UIDS = (6963, 6964, 7025, 7026, 7027, 7030)
PAD = 7030                            # the one GD actually takes
GD_ACTIVATION_TICK = 7299

# objrects_lv20.txt, verbatim. All six rows are identical but for cx/cy.
OBJRECTS_ROWS = {
    uid: {"id": "35", "type": "8", "cx": cx, "cy": cy,
          "w": "34.5176", "h": "22.6471", "uid": str(uid), "radius": "0",
          "rot": "29", "w0": "25", "h0": "4"}
    for uid, cx, cy in (
        (6963, "10783", "160.5"),
        (6964, "10799", "182.5"),
        (7025, "10835", "161.5"),
        (7026, "10811.9", "161.5"),
        (7027, "10805", "135.5"),
        (7030, "10835.9", "133.5"),
    )
}

# What level_loader.hpp:829-880 makes of that row. k = 1.45 recovered from the
# bound, because objrects' w0/h0 (25 x 4) are the RAW sprite size.
BOARD_OHW = 18.125388483051534
BOARD_OHH = 2.9000621572882457
BOARD_SCALE = 1.4500310786441228

# fid_lv20.dump.csv. `up` (upsideDown) is 1 through 7,294 and 0 from 7,295:
# the tick also teleports (y 353.106 -> 161.000) and flips gravity.
GD_ROWS = {
    7292: dict(x=10826.1152, y=346.356293, rot=-408.395721, yvel=15.0, up=1),
    7293: dict(x=10827.4131, y=349.731293, rot=-410.126495, yvel=15.0, up=1),
    7294: dict(x=10828.7109, y=353.106293, rot=-411.857269, yvel=15.0, up=1),
    7295: dict(x=10830.0088, y=161.0, rot=-413.588043, yvel=-8.0, up=0),
    7296: dict(x=10831.959, y=159.151398, rot=-415.318817, yvel=-8.216, up=0),
    7297: dict(x=10833.9092, y=157.254196, rot=-417.049591, yvel=-8.432, up=0),
    7298: dict(x=10835.8594, y=155.308395, rot=-418.780365, yvel=-8.648, up=0),
    7299: dict(x=10837.8096, y=153.313995, rot=-417.049591, yvel=16.0, up=0),
    7300: dict(x=10839.7598, y=156.865402, rot=-415.318817, yvel=15.784, up=0),
    7301: dict(x=10841.71, y=160.368195, rot=-413.588043, yvel=15.568, up=0),
    7302: dict(x=10843.6602, y=163.822388, rot=-411.857269, yvel=15.352, up=0),
}
GD_MODE, GD_VSIZE = "cube", 1.0       # so pHalf is 15 and the spin step 1.7308

# The angle the model hands the SAT, degrees, per arm. Arms are named by the
# SIGN of the cube's same-tick pad spin at t=7,295, which is the one thing that
# differs between them -- "SPIN_PLUS" is the pre-351f9de behaviour (the gravity
# at the END of the tick), "SPIN_MINUS" is the shipped one (the gravity the
# pad's own call saw). Derived as sat_angle(trace.rot[t-1], trace.rotneg[t-1]).
MODEL_ANGLE = {
    7295: dict(spin_plus=129.8963271, spin_minus=129.8963271),
    7296: dict(spin_plus=131.627101, spin_minus=128.1655608),
    7297: dict(spin_plus=129.8963365, spin_minus=126.43478870000001),
    7298: dict(spin_plus=131.6271104, spin_minus=128.16556260000002),
    7299: dict(spin_plus=133.3578844, spin_minus=129.8963365),
    7300: dict(spin_plus=135.0886583, spin_minus=131.6271104),
    7301: dict(spin_plus=136.8194322, spin_minus=133.3578844),
}

# The tightest SAT axis for uid 7030, px, > 0 == contact. Recomputed here from
# the raw columns; `gd` and `flat` reproduce constants.hpp's pinned table and
# `spin_plus` reproduces the note's section 5, both to 1e-3.
#   gd         GD's own rotation column
#   flat       pRot = 0, the axis-aligned square (a874728 / --no-padplayerrot)
#   spin_plus  the model's angle before 351f9de
#   spin_minus the model's angle after it (shipped at 6a3c721)
MARGIN = {
    7295: dict(gd=-1.4861672460155688, flat=-1.1761857780209724,
               spin_plus=-0.7308194430079773, spin_minus=-0.7308194430079773,
               aabb=False),
    7296: dict(gd=-1.2133050700625923, flat=0.6724162219790273,
               spin_plus=0.29175313265397307, spin_minus=-0.4267213173153763,
               aabb=True),
    7297: dict(gd=-0.9129105616716622, flat=2.5696182219790202,
               spin_plus=0.6543850630175747, spin_minus=-0.09580973613312693,
               aabb=True),
    7298: dict(gd=-0.5846073396290912, flat=4.237133290576107,
               spin_plus=1.76196971166695, spin_minus=1.0434937884318884,
               aabb=True),
    7299: dict(gd=0.6423154402096714, flat=5.035999113089954,
               spin_plus=2.895712567831513, spin_minus=2.2096110648989082,
               aabb=True),
    7300: dict(gd=-2.995830803637922, flat=0.9843928414130936,
               spin_plus=-0.8376696983618572, spin_minus=-1.4907707374960637,
               aabb=True),
    7301: dict(gd=-6.606432470372546, flat=-3.024694667820903,
               spin_plus=-4.545476569476644, spin_minus=-5.1649812130793435,
               aabb=False),
}

# The tick each arm first takes uid 7030, both conjuncts.
FIRST_FIRE = {"gd": 7299, "flat": 7296, "spin_plus": 7296, "spin_minus": 7298}

# Rows where the geometry says contact at GD's own inputs and GD does NOT
# activate. They are not a failure of the shape rule: step.hpp's loop skips a
# pad whose uid is below the tick's teleport (`teleUid`), and 7,295 is the tick
# that teleports. Pinned so that anyone who later grows this leaf into the
# whole gate has to account for them.
GEOMETRIC_ONLY = {7025: tuple(WINDOW.ticks()), 7026: tuple(WINDOW.ticks())}

TOL = 1e-3          # constants.hpp's table is rounded to three decimals


def board(uid: int) -> Board:
    return Board.from_objrects_row(OBJRECTS_ROWS[uid])


def fire_ticks(b: Board, angle_of) -> list[int]:
    return [t for t in WINDOW.ticks()
            if gate(b, GD_ROWS[t]["x"], GD_ROWS[t]["y"], PLAYER_HALF_FULL,
                    angle_of(t))]


ARMS = {
    "gd": lambda t: GD_ROWS[t]["rot"],
    "flat": lambda t: 0.0,
    "spin_plus": lambda t: MODEL_ANGLE[t]["spin_plus"],
    "spin_minus": lambda t: MODEL_ANGLE[t]["spin_minus"],
}


# ------------------------------------------------------------ the transcription

class TestTranscriptionIsCurrent(unittest.TestCase):
    """Does gdtas.padgate still say what dp/ says? A stale-copy alarm."""

    def test_copied_spans_have_not_moved(self):
        if not (REPO / "dp" / "src" / "dp" / "object.hpp").exists():
            self.skipTest("no dp/ in this checkout")
        stale = padgate.check_transcription()
        self.assertEqual(stale, [], "\n  ".join(
            ["the C++ this leaf was copied from has changed. Re-read it, "
             "re-run this file, and re-pin PINNED_FINGERPRINTS:"] + stale))


class TestBoardFromObjrects(unittest.TestCase):
    """The object side rebuilds from one objrects row -- level_loader.hpp."""

    def test_scale_is_recovered_from_the_bound(self):
        b = board(PAD)
        self.assertTrue(b.oriented)
        self.assertAlmostEqual(b.scale, BOARD_SCALE, places=9)
        self.assertAlmostEqual(b.ohw, BOARD_OHW, places=9)
        self.assertAlmostEqual(b.ohh, BOARD_OHH, places=9)

    def test_the_obb_reproduces_the_recorded_bound(self):
        """ohw|cos| + ohh|sin| has to come back to the dump's own w,h. If it
        does not, k was recovered wrong and every margin below is fiction."""
        import math
        b = board(PAD)
        c, s = abs(math.cos(math.radians(29.0))), abs(math.sin(math.radians(29.0)))
        self.assertAlmostEqual(b.ohw * c + b.ohh * s, b.hw, places=3)
        self.assertAlmostEqual(b.ohw * s + b.ohh * c, b.hh, places=3)


# ------------------------------------------------------------------------ Q1

class TestQ1RuleAtGDsOwnInputs(unittest.TestCase):
    """Given GD's own x, y and rotation, does the predicate return GD's answer?"""

    def test_gd_activation_tick_comes_from_the_yvel_column(self):
        """Not from a comment. A yellow pad sets vy = +16; nothing else in the
        window does, and the tick before is still falling at -8.648."""
        jumps = [t for t, r in GD_ROWS.items() if abs(abs(r["yvel"]) - 16.0) < 1e-9]
        self.assertEqual(jumps, [GD_ACTIVATION_TICK])
        self.assertLess(GD_ROWS[GD_ACTIVATION_TICK - 1]["yvel"], 0.0)

    def test_the_turned_square_fires_on_exactly_that_tick(self):
        """Q1, GREEN. One free parameter would be one too many; there are none."""
        self.assertEqual(fire_ticks(board(PAD), ARMS["gd"]), [GD_ACTIVATION_TICK])

    def test_margins_match_the_table_pinned_in_constants_hpp(self):
        b = board(PAD)
        for t in WINDOW.ticks():
            r = GD_ROWS[t]
            self.assertAlmostEqual(
                obb_sat_margin(b, r["x"], r["y"], PLAYER_HALF_FULL, r["rot"]),
                MARGIN[t]["gd"], delta=TOL, msg=f"t={t}")

    def test_the_aabb_conjunct_rejects_the_two_end_ticks(self):
        """The pinned table spans two ticks the gate never reaches -- both are
        negative anyway, but a reader must not take the row as a verdict."""
        b = board(PAD)
        for t in WINDOW.ticks():
            got = aabb_conjunct(b, GD_ROWS[t]["x"], GD_ROWS[t]["y"],
                                PLAYER_HALF_FULL)
            self.assertEqual(got, MARGIN[t]["aabb"], f"t={t}")

    def test_geometry_alone_is_not_the_whole_gate(self):
        """Two other boards are in contact at GD's own inputs from 7,295 and GD
        activates neither. The missing conjunct is the teleport uid ordering,
        not the shape -- see padgate.gate's docstring."""
        for uid, ticks in GEOMETRIC_ONLY.items():
            b = board(uid)
            hit = tuple(t for t in ticks
                        if gate(b, GD_ROWS[t]["x"], GD_ROWS[t]["y"],
                                PLAYER_HALF_FULL, GD_ROWS[t]["rot"]))
            self.assertEqual(hit, ticks, f"uid {uid}")
        self.assertEqual([t for t, r in GD_ROWS.items()
                          if abs(abs(r["yvel"]) - 16.0) < 1e-9],
                         [GD_ACTIVATION_TICK])


# ------------------------------------------------------------------------ Q2

class TestQ2InputsAgainstGDs(unittest.TestCase):
    """Given the same situation, do the model's inputs equal GD's?"""

    def test_the_shipped_angle_is_wrong_by_two_spin_steps(self):
        """Q2, RED. At 7,296 the model hands the SAT 38.166 deg where GD's own
        column reads 34.681: +3.484, which is 2 x 1.7308 (the one-step advance
        pointing the opposite way to the column's own step) plus 0.023 of
        column drift. The rule is right; its input is not."""
        err = angle_error_mod90(MODEL_ANGLE[7296]["spin_minus"],
                                GD_ROWS[7296]["rot"])
        self.assertAlmostEqual(err, 3.484, delta=TOL)
        self.assertAlmostEqual(err, 2 * padgate.CUBE_SPIN_STEP, delta=0.03)

    def test_before_351f9de_the_error_was_twice_that(self):
        err = angle_error_mod90(MODEL_ANGLE[7296]["spin_plus"],
                                GD_ROWS[7296]["rot"])
        self.assertAlmostEqual(err, 6.946, delta=TOL)

    def test_the_starved_rule_is_indistinguishable_from_no_rule(self):
        """THE FAILURE THIS FILE EXISTS FOR.

        At the pre-351f9de angle the turned square and the FLAT square take the
        pad on the SAME tick, 7,296. The shape rule was therefore inert, and no
        whole-replay instrument could have said whether it was right, wrong or
        absent.

        The two arms do part company at 7,300 (+0.984 flat against -0.838
        starved), and that difference is worth nothing: the pad has been taken
        three ticks earlier, `usedPad` holds it, and both runs are on a
        world-line the other never enters. It is pinned here as an example of
        exactly the comparison gdtas.compare's docstring says it cannot catch
        for you."""
        b = board(PAD)
        flat = fire_ticks(b, ARMS["flat"])
        starved = fire_ticks(b, ARMS["spin_plus"])
        self.assertEqual(flat[0], starved[0])
        self.assertEqual(flat[0], 7296)
        self.assertNotEqual(flat[0], GD_ACTIVATION_TICK)
        self.assertEqual(flat, [7296, 7297, 7298, 7299, 7300])
        self.assertEqual(starved, [7296, 7297, 7298, 7299])

    def test_fixing_the_input_is_what_makes_the_rule_bite(self):
        """...and with the input fixed the same rule moves the firing 7,296 ->
        7,298, which is the number step.hpp claims at the site. One tick of
        error is left, and it is the residual angle above, not the shape."""
        b = board(PAD)
        fed = fire_ticks(b, ARMS["spin_minus"])
        self.assertEqual(fed[0], 7298)
        self.assertNotEqual(fed, fire_ticks(b, ARMS["flat"]))
        self.assertLess(fed[0], GD_ACTIVATION_TICK)

    def test_first_fire_tick_per_arm(self):
        b = board(PAD)
        for name, fn in ARMS.items():
            self.assertEqual(fire_ticks(b, fn)[0], FIRST_FIRE[name], name)

    def test_margins_per_arm(self):
        b = board(PAD)
        for t in WINDOW.ticks():
            r = GD_ROWS[t]
            for name in ("flat", "spin_plus", "spin_minus"):
                self.assertAlmostEqual(
                    obb_sat_margin(b, r["x"], r["y"], PLAYER_HALF_FULL,
                                   ARMS[name](t)),
                    MARGIN[t][name], delta=TOL, msg=f"{name} t={t}")


# --------------------------------------------------- the fixture against disk

def _dump_path() -> Path:
    return fidelity_dir() / f"fid_lv{LEVEL}.dump.csv"


def _objrects_path() -> Path:
    return LEVEL_DATA / f"objrects_lv{LEVEL}.txt"


# The two model traces the arms were read from. Working files, untracked.
TRACE = {"spin_plus": "fid_lv20.trace.csv", "spin_minus": "abnew_lv20.trace.csv"}


class TestFixtureStillMatchesTheFiles(unittest.TestCase):
    """Re-derive the fixture from the dumps when they are here.

    These are working files (build/fidelity, GD-lab) and are not tracked, so
    on a clean checkout every test in this class skips and says why. When they
    ARE present, a difference means the fixture above is stale -- the fixture
    is not a second source of truth, it is a cache of these files.
    """

    def setUp(self):
        if not _dump_path().exists():
            self.skipTest(f"no {_dump_path()} (fidelity_diff has not run here)")

    def test_conditions_are_read_from_the_dump_not_assumed(self):
        """gdtas.compare's job: mode / vsize / gravity for exactly these ticks.
        pHalf = 15 and the 1.7308 spin step are only right for a full cube."""
        gd = read_table(_dump_path(), GD)
        check_coverage(gd, WINDOW)
        cond = conditions_of(gd, WINDOW, Half.P1)
        cond.require(mode=GD_MODE, vsize=GD_VSIZE, gravity=0)
        self.assertEqual(cond.single("mode"), GD_MODE)

    def test_no_other_pad_launch_in_the_neighbourhood(self):
        """The fixture's window is 11 ticks wide; the claim that 7,299 is GD's
        only activation is made over 7,280..7,320, so check it there."""
        gd = read_table(_dump_path(), GD)
        wide = Window(7280, 7320)
        check_coverage(gd, wide)
        vy = series(gd, "vy", Half.P1, wide)
        jumps = [t for t, v in vy.values.items() if abs(abs(v) - 16.0) < 1e-9]
        self.assertEqual(jumps, [GD_ACTIVATION_TICK])

    def test_gd_rows_match_the_dump(self):
        gd = read_table(_dump_path(), GD)
        check_coverage(gd, Window(min(GD_ROWS), max(GD_ROWS)))
        for q in ("x", "y", "rot", "vy"):
            s = series(gd, q, Half.P1, Window(min(GD_ROWS), max(GD_ROWS)))
            key = "yvel" if q == "vy" else q
            for t, v in s.values.items():
                self.assertAlmostEqual(v, GD_ROWS[t][key], places=6,
                                       msg=f"{q} t={t}")

    def test_boards_match_objrects(self):
        if not _objrects_path().exists():
            self.skipTest(f"no {_objrects_path()}")
        live = read_objrects(_objrects_path(), UIDS)
        self.assertEqual(sorted(live), sorted(UIDS))
        for uid in UIDS:
            self.assertEqual(live[uid], board(uid), f"uid {uid}")

    def test_model_angles_match_the_traces(self):
        """...and the difference is built with gdtas.compare, so it carries its
        direction, its half and the conditions it was taken under."""
        gd = read_table(_dump_path(), GD)
        for arm, name in TRACE.items():
            p = fidelity_dir() / name
            if not p.exists():
                self.skipTest(f"no {p}")
            model = read_table(p, MODEL)
            # The window starts one tick early: the gate at t reads row t-1.
            check_coverage(model, Window(WINDOW.t0 - 1, WINDOW.t1))
            # A mover level replayed without --groups dies at t=1,157 and looks
            # byte-identical as far as it got. Neither trace records its argv,
            # so the provenance guard cannot be used; what CAN be said is that
            # this one reached the window and agrees with GD's x and y up to
            # the model's own divergence at 7,296.
            for t in range(WINDOW.t0 - 5, 7297):
                self.assertAlmostEqual(float(model.row(t)["x"]),
                                       float(gd.row(t)["x"]), delta=2e-3,
                                       msg=f"{name} x t={t}")
                self.assertAlmostEqual(float(model.row(t)["y"]),
                                       float(gd.row(t)["y"]), delta=2e-3,
                                       msg=f"{name} y t={t}")
            derived = {}
            for t in WINDOW.ticks():
                prev = model.row(t - 1)
                derived[t] = sat_angle(float(prev["rot"]), int(prev["rotneg"]))
                self.assertAlmostEqual(derived[t], MODEL_ANGLE[t][arm],
                                       places=6, msg=f"{arm} t={t}")
            gd_ang = series(gd, "rot", Half.P1, WINDOW)
            diff = difference(
                Series(quantity="pad_sat_angle_mod90", half=Half.P1,
                       window=WINDOW,
                       values={t: mod90(v) for t, v in gd_ang.values.items()},
                       provenance=gd.provenance),
                Series(quantity="pad_sat_angle_mod90", half=Half.P1,
                       window=WINDOW,
                       values={t: mod90(v) for t, v in derived.items()},
                       provenance=model.provenance),
                direction=MODEL_MINUS_GD,
                conditions=conditions_of(gd, WINDOW, Half.P1))
            self.assertIs(diff.direction, MODEL_MINUS_GD)
            self.assertAlmostEqual(
                diff.at(7296), angle_error_mod90(MODEL_ANGLE[7296][arm],
                                                 GD_ROWS[7296]["rot"]),
                delta=TOL, msg=arm)

    def test_gdref_agrees_with_the_fidelity_dump(self):
        """Two GD-side files, one answer. If they disagree the fixture's
        provenance line is wrong and the rest of this is unanchored."""
        ref = LEVEL_DATA / "gdref" / f"lv{LEVEL}.csv"
        if not ref.exists():
            self.skipTest(f"no {ref}")
        a, b = read_table(_dump_path(), GD), read_table(ref, GD)
        w = Window(min(GD_ROWS), max(GD_ROWS))
        for q in ("x", "y", "rot", "vy"):
            sa, sb = series(a, q, Half.P1, w), series(b, q, Half.P1, w)
            for t in w.ticks():
                self.assertAlmostEqual(sa.values[t], sb.values[t], places=6,
                                       msg=f"{q} t={t}")


# ------------------------------------------------------------------- display

def show() -> None:
    b = board(PAD)
    print(f"lv{LEVEL} uid {PAD} (id 35, type 8, rot 29) "
          f"ohw={b.ohw:.4f} ohh={b.ohh:.4f} k={b.scale:.5f}")
    print(f"GD activates at t={GD_ACTIVATION_TICK} (yvel {GD_ROWS[7298]['yvel']} "
          f"-> {GD_ROWS[GD_ACTIVATION_TICK]['yvel']})\n")
    print("  t     aabb |   gd      flat   spin+   spin-  | angle mod 90: "
          "gd     spin+   spin-")
    for t in WINDOW.ticks():
        m, r = MARGIN[t], GD_ROWS[t]
        print(f"  {t} {str(m['aabb']):>5} | {m['gd']:+7.3f} {m['flat']:+7.3f} "
              f"{m['spin_plus']:+7.3f} {m['spin_minus']:+7.3f} | "
              f"{mod90(r['rot']):9.3f} {mod90(MODEL_ANGLE[t]['spin_plus']):7.3f} "
              f"{mod90(MODEL_ANGLE[t]['spin_minus']):7.3f}")
    print("\n  first fire:", ", ".join(f"{k}={v}" for k, v in FIRST_FIRE.items()))
    print("  Q1 the rule at GD's inputs: fires on GD's own tick, 7,299")
    print("  Q2 the model's input, mod 90, at t=7,296:")
    print("       +6.946 deg (before 351f9de) -> fires 7,296, the same tick as "
          "the FLAT square:")
    print("                                      the shape rule is inert and "
          "unmeasurable")
    print("       +3.484 deg (after it)       -> fires 7,298: the shape rule "
          "bites, one tick of")
    print("                                      angle error left "
          "(2 x 1.7308 + 0.023 of column drift)")


if __name__ == "__main__":
    if "--show" in sys.argv:
        show()
        raise SystemExit(0)
    unittest.main(verbosity=2 if "-v" in sys.argv else 1,
                  argv=[a for a in sys.argv if a != "-v"])
