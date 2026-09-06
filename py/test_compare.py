# -*- coding: utf-8 -*-
"""Pin the mis-comparisons of 2026-09-05/06 as tests. (no GD, no solve; seconds)

    python py/test_compare.py            # all
    python py/test_compare.py -v         # with names

Seven of the nine mis-comparisons listed in the campaign notes are mechanical,
and each is pinned here in BOTH directions: under the mistaken conditions
gdtas.compare refuses (or returns something whose sign, half or coverage makes
the mistake visible), and under the correct conditions it reproduces the
number that was measured at the time.

    case 1  sign          lv19 t=21,402   GD-model is -0.006470, not +0.006470
    case 2  half          lv16 t=7,100    p2-vs-p2 is -0.0484, p2-vs-p1 is +92.9
    case 3  conditions    lv19 21,396..   vsize 0.6 / speed 0.9, not 1.0 / 1.1
    case 4  raw values    lv19 21,402..   seven raw steps, one rounded string
    case 5  truncation    a 250-tick window delivered as 29 rows
    case 6  a dropped tick in the middle of a hand-built table
    case 9  a mover level whose replay stops at 1,157 of 23,672 ticks
    case 9b the sidecar the producer now writes, and the three verdicts it
            makes possible: flags present / flag missing by name / not recorded
    case 10 two ticks     lv16 9,241 vs 9,243   GD's 590.506836 against the
            model's 585.000, which are two ticks apart and were contrasted as
            though they were one moment (6a3e5f8, dp/src/dp/slopes.hpp)

Cases 7 and 8 are judgement and are not pinned -- see gdtas.compare's docstring.

The data is the real thing: the fidelity dumps in build/fidelity (GD's side)
and the model traces beside them. They are working files, not tracked, so on a
checkout without them every data-backed test SKIPS and says so. The refusal
tests that need no dump run everywhere.
"""
from __future__ import annotations

import csv
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas.compare import (GD, MODEL, NEVER_COMPARED, ConditionMismatch,
                           Contrast, Difference, Direction, GD_MINUS_MODEL,
                           Half, HalfMismatch, Incomplete, MODEL_MINUS_GD,
                           MOVING_GEOMETRY_FLAGS, Observation, PROV_SCHEMA,
                           PROV_SUFFIX, ProvenanceMissing, Provenance, Table,
                           TickMismatch, Window, compare, conditions_of,
                           contrast, difference, fidelity_dir,
                           format_contrast, format_difference, observe,
                           read_table, require_replay_flags, series, verdict,
                           write_provenance)

FID = fidelity_dir()


def dumps(lv: int, model: str | None = None):
    """(GD dump, model trace) for a level, or None when they are not on disk."""
    g = FID / f"fid_lv{lv}.dump.csv"
    m = FID / (model or f"fid_lv{lv}.trace.csv")
    if not g.exists() or not m.exists():
        return None
    return read_table(g, GD), read_table(m, MODEL)


def need(lv: int, model: str | None = None):
    got = dumps(lv, model)
    if got is None:
        raise unittest.SkipTest(f"no fidelity dumps for lv{lv} in {FID}")
    return got


def rows_of(table: Table, ticks) -> Table:
    """A table holding only these ticks -- how a hand-built table is made."""
    return Table(rows={t: table.rows[t] for t in ticks if t in table.rows},
                 columns=table.columns, provenance=table.provenance)


class Case1Sign(unittest.TestCase):
    """`edy` is GD - model (fixcensus.py:75). Read the other way a correct
    reading was retracted."""

    def test_direction_is_named_and_the_two_are_opposite(self):
        gd, model = need(19)
        w = Window(21401, 21402)
        d = compare(gd, model, "y", window=w, deltas=True,
                    direction=GD_MINUS_MODEL)
        # the number the census prints: GD fell 0.250595, the model 0.244125,
        # so GD - model is NEGATIVE. The model fell LESS than GD.
        self.assertEqual(d.direction, GD_MINUS_MODEL)
        self.assertAlmostEqual(d.at(21402), -0.0064696, places=7)
        self.assertLess(d.at(21402), 0.0)

        flipped = d.flip()
        self.assertEqual(flipped.direction, MODEL_MINUS_GD)
        self.assertAlmostEqual(flipped.at(21402), +0.0064696, places=7)
        # the mistake was reading one as the other. They cannot be confused
        # here because the sign and the name move together.
        self.assertNotEqual(d.direction, flipped.direction)
        self.assertEqual(d.at(21402), -flipped.at(21402))

    def test_asking_for_a_direction_gives_that_direction(self):
        gd, model = need(19)
        w = Window(21401, 21402)
        a = compare(gd, model, "y", window=w, deltas=True,
                    direction=MODEL_MINUS_GD)
        b = compare(gd, model, "y", window=w, deltas=True,
                    direction=GD_MINUS_MODEL).as_direction(MODEL_MINUS_GD)
        self.assertEqual(a.values, b.values)
        self.assertGreater(a.at(21402), 0.0)


class Case2Half(unittest.TestCase):
    """A p2 quantity was held against GD's p1 y. lv16 is dual from t=7,049."""

    def test_cross_half_is_refused(self):
        gd, model = need(16)
        w = Window(7100, 7110)
        p1_gd = series(gd, "y", Half.P1, w)
        p2_model = series(model, "y", Half.P2, w)
        with self.assertRaises(HalfMismatch) as cm:
            difference(p1_gd, p2_model)
        self.assertIn("p1", str(cm.exception))
        self.assertIn("p2", str(cm.exception))

    def test_cross_half_would_have_looked_like_a_huge_divergence(self):
        """What the refusal is worth: the forbidden subtraction is +92.9 px and
        growing -- it reads as a catastrophic divergence and is two bodies."""
        gd, model = need(16)
        raw = float(model.row(7100)["y2"]) - float(gd.row(7100)["y"])
        self.assertGreater(raw, 90.0)

    def test_same_half_reproduces_the_measured_offset(self):
        gd, model = need(16)
        w = Window(7100, 7110)
        p2 = compare(gd, model, "y", window=w, half=Half.P2,
                     direction=MODEL_MINUS_GD)
        self.assertEqual(p2.half, Half.P2)
        for t in w.ticks():
            self.assertAlmostEqual(p2.at(t), -0.04840, places=4)
        # and p1 over the same ticks agrees to float storage error
        p1 = compare(gd, model, "y", window=w, half=Half.P1,
                     direction=MODEL_MINUS_GD)
        for t in w.ticks():
            self.assertLess(abs(p1.at(t)), 1e-5)


class Case3Conditions(unittest.TestCase):
    """A filter carried over from the lv16 site (vsize 1.0 / speed 1.1) was used
    to measure lv19 21,396.., which is vsize 0.6 / speed 0.9."""

    WIN = Window(21396, 21417)

    def test_the_carried_over_filter_is_refused(self):
        gd, model = need(19)
        with self.assertRaises(ConditionMismatch) as cm:
            compare(gd, model, "y", window=self.WIN,
                    expect={"vsize": 1.0, "speed": 1.1})
        msg = str(cm.exception)
        self.assertIn("0.600000024", msg)     # what the data says
        self.assertIn("1.0", msg)             # what the caller said
        self.assertIn("speed", msg)

    def test_the_right_filter_passes_and_the_conditions_are_read_not_stated(self):
        gd, model = need(19)
        d = compare(gd, model, "y", window=self.WIN,
                    expect={"vsize": 0.6, "speed": 0.9, "mode": "ship"})
        self.assertEqual(d.conditions.single("vsize"), "0.600000024")
        self.assertEqual(d.conditions.single("speed"), "0.899999976")
        self.assertEqual(d.conditions.single("mode"), "ship")

    def test_the_same_filter_is_right_at_the_site_it_came_from(self):
        """It was not a wrong filter, it was the right filter somewhere else."""
        gd, model = need(16)
        c = conditions_of(gd, Window(7100, 7110), Half.P1)
        c.require(vsize=1.0, speed=1.1, mode="cube")
        self.assertEqual(c.single("vsize"), "1")

    def test_a_condition_that_varies_cannot_be_stated_as_one_value(self):
        gd, _ = need(19)
        c = conditions_of(gd, Window(10800, 21417), Half.P1)
        self.assertIn("mode", c.varying)
        with self.assertRaises(ConditionMismatch):
            c.require(mode="ship")


class Case4Raw(unittest.TestCase):
    """A self-made three-decimal table was read in place of the raw column."""

    def test_the_raw_steps_are_seven_values_and_the_rendering_is_one(self):
        gd, _ = need(19)
        w = Window(21401, 21408)
        steps = series(gd, "y", Half.P1, w).deltas()
        raw = [steps.values[t] for t in steps.window.ticks()]
        self.assertEqual(len(raw), 7)
        # raw: not one value
        self.assertGreater(max(raw) - min(raw), 2e-5)
        self.assertLess(max(raw) - min(raw), 5e-5)
        self.assertNotEqual(len(set(raw)), 1)
        # rendered at three decimals: one value, seven times
        self.assertEqual({f"{v:.3f}" for v in raw}, {"-0.251"})

    def test_the_comparison_returns_raw_and_the_display_is_a_separate_call(self):
        gd, model = need(19)
        w = Window(21401, 21402)
        d = compare(gd, model, "y", window=w, deltas=True)
        self.assertNotEqual(d.at(21402), round(d.at(21402), 3))
        lines = format_difference(d, decimals=3)
        self.assertTrue(any("-0.006" in l for l in lines))
        # and rounding is nowhere in the values themselves
        self.assertAlmostEqual(d.at(21402), -0.0064696, places=7)

    def test_a_threshold_taken_off_the_rounded_table_is_a_different_threshold(self):
        """|vy| * 0.225 > step. The raw step and its three-decimal rendering
        put the crossing 1.8e-3 apart in vy, and disagree in between."""
        gd, _ = need(19)
        step = abs(series(gd, "y", Half.P1, Window(21402, 21403)).deltas()
                   .values[21403])
        rounded = abs(float(f"{-step:.3f}"))
        raw_thr, rounded_thr = step / 0.225, rounded / 0.225
        self.assertAlmostEqual(raw_thr, 1.113689, places=5)
        self.assertAlmostEqual(rounded_thr - raw_thr, 0.0018667, places=6)
        between = (raw_thr + rounded_thr) / 2
        self.assertTrue(between * 0.225 > step)          # raw says yes
        self.assertFalse(between * 0.225 > rounded)      # the table says no


class Case5Truncation(unittest.TestCase):
    """`Select-Object -First 34` closed the pipe; 29 rows were read as the corpus."""

    def test_a_truncated_series_is_refused_and_the_refusal_counts_the_rows(self):
        gd, _ = need(19)
        want = Window(21200, 21449)                    # 250 ticks
        cut = rows_of(gd, range(21200, 21229))         # 29 rows arrived
        with self.assertRaises(Incomplete) as cm:
            series(cut, "y", Half.P1, want)
        msg = str(cm.exception)
        self.assertIn("29/250", msg)
        self.assertIn("stops before", msg)

    def test_the_full_window_is_accepted(self):
        gd, _ = need(19)
        want = Window(21200, 21449)
        s = series(gd, "y", Half.P1, want)
        self.assertEqual(len(s.values), 250)


class Case6DroppedTick(unittest.TestCase):
    """A hand-built table dropped one tick, so a second structure was missed."""

    def test_a_hole_is_refused_and_named(self):
        gd, _ = need(19)
        want = Window(21396, 21417)
        holed = rows_of(gd, [t for t in want.ticks() if t != 21409])
        with self.assertRaises(Incomplete) as cm:
            series(holed, "y", Half.P1, want)
        self.assertIn("21409", str(cm.exception))
        self.assertIn("missing", str(cm.exception))

    def test_a_hole_is_reported_as_a_hole_and_a_short_series_as_a_truncation(self):
        gd, _ = need(19)
        want = Window(21396, 21417)
        short = rows_of(gd, range(21396, 21410))
        with self.assertRaises(Incomplete) as cm:
            series(short, "y", Half.P1, want)
        self.assertIn("stops before", str(cm.exception))

    def test_the_whole_window_is_accepted(self):
        gd, _ = need(19)
        want = Window(21396, 21417)
        self.assertEqual(len(series(gd, "y", Half.P1, want).values), 22)

    def test_a_repeated_tick_is_refused_at_the_door(self):
        """Two attempts in one file: keeping the last row silently is the same
        family of mistake."""
        p = Path(self.tmp.name) / "twice.csv"
        with p.open("w", newline="", encoding="utf-8") as f:
            w = csv.writer(f)
            w.writerow(["tick", "y", "vy"])
            for t, y in ((1, 10.0), (2, 11.0), (2, 99.0), (3, 12.0)):
                w.writerow([t, y, 0])
        with self.assertRaises(Incomplete) as cm:
            read_table(p, MODEL)
        self.assertIn("repeated", str(cm.exception))

    def setUp(self):
        import tempfile
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)


class Case9MoverReplay(unittest.TestCase):
    """A mover level replayed without --groups dies at 4.6% and the trace it
    leaves looks byte-identical as far as it got."""

    def test_a_replay_that_stopped_early_is_refused_over_the_real_window(self):
        got = dumps(20, "ab_lv20.trace.csv")
        if got is None:
            self.skipTest("no ab_lv20.trace.csv")
        gd, short = got
        self.assertEqual(short.span(), (1, 1157))
        self.assertEqual(gd.span()[1], 23672)
        with self.assertRaises(Incomplete) as cm:
            compare(gd, short, "y", window=Window(1, 15000))
        self.assertIn("1..1157", str(cm.exception))
        self.assertIn("stops before", str(cm.exception))

    def test_the_full_replay_covers_the_window(self):
        got = dumps(20, "abold_lv20.trace.csv")
        if got is None:
            self.skipTest("no abold_lv20.trace.csv")
        gd, full = got
        self.assertEqual(full.span(), (1, 15125))
        d = compare(gd, full, "y", window=Window(1, 15000))
        self.assertEqual(len(d.values), 15000)

    def test_an_unrecorded_argv_is_refused_rather_than_assumed_fine(self):
        got = dumps(20, "ab_lv20.trace.csv")
        if got is None:
            self.skipTest("no ab_lv20.trace.csv")
        _, short = got
        self.assertIsNone(short.provenance.argv)
        with self.assertRaises(ProvenanceMissing) as cm:
            require_replay_flags(short)
        self.assertIn("not recorded", str(cm.exception))

    def test_provenance_names_the_missing_flag(self):
        without = Table(rows={1: {"tick": "1"}}, columns=("tick",),
                        provenance=Provenance(Path("x.trace.csv"), MODEL,
                                              ("leveldp", "--replay", "p.txt",
                                               "--triggers", "t", "--objgroups",
                                               "g", "--obb", "o")))
        with self.assertRaises(ProvenanceMissing) as cm:
            require_replay_flags(without)
        self.assertIn("--groups", str(cm.exception))

        with_all = Table(rows=without.rows, columns=without.columns,
                         provenance=Provenance(
                             Path("x.trace.csv"), MODEL,
                             without.provenance.argv + ("--groups", "g.txt")))
        require_replay_flags(with_all)          # no raise
        for f in MOVING_GEOMETRY_FLAGS:
            self.assertTrue(with_all.provenance.has_flag(f))


class Case9bProvenanceSidecar(unittest.TestCase):
    """The producer's half of case 9: a trace that says how it was made.

    No GD and no leveldp here -- the sidecar is written by
    `gdtas.compare.write_provenance`, so a fake argv over real files on disk
    exercises the same code the replay uses. The demonstration that the flag
    it names is the flag that killed the run is in the commit message; these
    tests pin the mechanism.
    """

    def setUp(self):
        import tempfile
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.d = Path(self.tmp.name)
        self.exe = self.d / "leveldp.exe"
        self.exe.write_bytes(b"MZ not really an exe")
        self.objrects = self.d / "objrects_lv20.txt"
        self.objrects.write_text("1,0,0\n", encoding="utf-8")
        self.groups = self.d / "plan.txt.groups.txt"
        self.groups.write_text("g\n", encoding="utf-8")
        self.trace = self.d / "t_lv20.trace.csv"
        self.trace.write_text("tick,y,vy\n1,10,0\n2,11,0\n", encoding="utf-8")

    def argv(self, with_groups: bool) -> list[str]:
        a = [str(self.exe), str(self.objrects), "--replay", str(self.d / "plan.txt"),
             "--out", str(self.d / "t_lv20")]
        if with_groups:
            a += ["--groups", str(self.groups)]
        return a

    def table(self) -> Table:
        return read_table(self.trace, MODEL)

    def test_the_sidecar_records_argv_the_binary_and_every_input(self):
        import json
        sc = write_provenance(self.trace, self.argv(True), produced_by="a test")
        self.assertEqual(sc, Path(str(self.trace) + PROV_SUFFIX))
        rec = json.loads(sc.read_text(encoding="utf-8"))
        self.assertEqual(rec["schema"], PROV_SCHEMA)
        self.assertEqual(rec["argv"], self.argv(True))
        # the binary: path, size, mtime, and a digest that survives a rebuild
        self.assertEqual(rec["binary"]["path"], str(self.exe))
        self.assertEqual(rec["binary"]["size"], self.exe.stat().st_size)
        self.assertTrue(rec["binary"]["mtime"])
        self.assertEqual(len(rec["binary"]["sha256"]), 64)
        # the inputs: every argv element that names a file, and not the exe
        names = [Path(i["path"]).name for i in rec["inputs"]]
        self.assertEqual(names, ["objrects_lv20.txt", "plan.txt.groups.txt"])
        for i in rec["inputs"]:
            self.assertEqual(i["size"], Path(i["path"]).stat().st_size)
            self.assertTrue(i["mtime"])
        self.assertNotIn("leveldp.exe", names)
        # --out names a base, not a file, and --replay names a plan that does
        # not exist in this fixture: neither is recorded as an input that was read
        self.assertNotIn("plan.txt", names)

    def test_a_value_that_looks_like_a_path_is_not_recorded_as_a_file(self):
        """--start is 26 comma-separated floats. Asking the filesystem about
        that must answer 'not a file' rather than take the run down."""
        import json
        argv = self.argv(True) + ["--start", "1,2.5,3,cube,0" * 4,
                                  "--startband", "10.5,90.25"]
        rec = json.loads(write_provenance(self.trace, argv, produced_by="a test")
                         .read_text(encoding="utf-8"))
        self.assertEqual([Path(i["path"]).name for i in rec["inputs"]],
                         ["objrects_lv20.txt", "plan.txt.groups.txt"])

    def test_the_flag_that_is_there_is_accepted(self):
        write_provenance(self.trace, self.argv(True), produced_by="a test")
        t = self.table()
        self.assertEqual(t.provenance.argv, tuple(self.argv(True)))
        require_replay_flags(t, ("--groups",))                 # no raise
        self.assertTrue(t.provenance.has_flag("--groups"))
        self.assertEqual(len(t.provenance.inputs), 2)

    def test_the_flag_that_is_missing_is_named(self):
        write_provenance(self.trace, self.argv(False), produced_by="a test")
        with self.assertRaises(ProvenanceMissing) as cm:
            require_replay_flags(self.table(), ("--groups",))
        msg = str(cm.exception)
        self.assertIn("replayed without --groups", msg)
        # and it is NOT the refusal an unrecorded trace gets: the two are
        # different findings and the message has to say which one this is
        self.assertNotIn("was not recorded", msg)

    def test_no_sidecar_is_unknown_and_not_the_same_refusal(self):
        """Every trace on disk today predates this. They must read as UNKNOWN --
        conflating that with 'the flag was absent' makes them all look guilty."""
        t = self.table()
        self.assertIsNone(t.provenance.argv)
        self.assertIsNone(t.provenance.binary)
        self.assertEqual(t.provenance.inputs, ())
        with self.assertRaises(ProvenanceMissing) as cm:
            require_replay_flags(t, ("--groups",))
        msg = str(cm.exception)
        self.assertIn("was not recorded", msg)
        self.assertNotIn("replayed without", msg)

    def test_a_sidecar_left_behind_by_an_earlier_run_is_refused_not_believed(self):
        """The producer is run again by hand with different flags; the .json
        from the run before is still there claiming the flags of a run that no
        longer exists. Worse than nothing, so it does not count as recorded."""
        write_provenance(self.trace, self.argv(True), produced_by="a test")
        require_replay_flags(self.table(), ("--groups",))       # fresh: accepted
        self.trace.write_text("tick,y,vy\n1,10,0\n2,99,0\n", encoding="utf-8")
        t = self.table()
        self.assertIsNone(t.provenance.argv)
        self.assertIn("rewritten", t.provenance.stale)
        with self.assertRaises(ProvenanceMissing) as cm:
            require_replay_flags(t, ("--groups",))
        self.assertIn("does not describe this file", str(cm.exception))

    def test_a_sidecar_for_a_file_of_another_size_is_refused_by_size(self):
        write_provenance(self.trace, self.argv(True), produced_by="a test")
        self.trace.write_text("tick,y,vy\n1,10,0\n", encoding="utf-8")
        t = self.table()
        self.assertIsNone(t.provenance.argv)
        self.assertIn("bytes", t.provenance.stale)

    def test_a_sidecar_from_a_run_that_produced_nothing_does_not_adopt_a_later_file(self):
        """leveldp fails, the sidecar is written anyway, and something else
        later writes a trace under that name. The argv in the sidecar is
        somebody else's, so it is not evidence about this file."""
        missing = self.d / "gone_lv20.trace.csv"
        write_provenance(missing, self.argv(True), produced_by="a test")
        missing.write_text("tick,y,vy\n1,10,0\n", encoding="utf-8")
        t = read_table(missing, MODEL)
        self.assertIsNone(t.provenance.argv)
        self.assertIn("produced no output", t.provenance.stale)
        with self.assertRaises(ProvenanceMissing):
            require_replay_flags(t, ("--groups",))

    def test_the_legacy_argv_sidecar_still_works(self):
        """`<trace>.argv.txt` was the published name before this. A hand-written
        one keeps being read."""
        Path(str(self.trace) + ".argv.txt").write_text(
            "\n".join(self.argv(True)) + "\n", encoding="utf-8")
        t = self.table()
        self.assertEqual(t.provenance.argv, tuple(self.argv(True)))
        require_replay_flags(t, ("--groups",))                 # no raise

    def test_recording_provenance_does_not_touch_the_trace(self):
        """A sidecar is a second file. The data it describes must not move --
        an instrument's output has to stay comparable across this change."""
        before = self.trace.read_bytes()
        write_provenance(self.trace, self.argv(True), produced_by="a test")
        self.assertEqual(self.trace.read_bytes(), before)

    def test_the_callers_argv_still_wins_over_any_sidecar(self):
        write_provenance(self.trace, self.argv(False), produced_by="a test")
        stated = ("leveldp", "--groups", "g.txt")
        t = read_table(self.trace, MODEL, argv=stated)
        self.assertEqual(t.provenance.argv, stated)
        require_replay_flags(t, ("--groups",))                 # no raise


# ------------------------------------------------------- case 10: two ticks
#
# lv16 around t=9,241, transcribed from build/fidelity's fid_lv16.dump.csv and
# fid_lv16.trace.csv (the shipped seat arm). Held here as literals so the
# refusal tests run on a checkout with no working files at all;
# `test_the_fixture_is_what_the_dumps_say` reads the files back when they are
# there, so the literals cannot quietly go stale.
GD16 = {
    9238: {"tick": "9238", "x": "14050.6025", "y": "593.121277", "yvel": "0.541",
           "onGround": "0"},
    9239: {"tick": "9239", "x": "14052.2168", "y": "593.266174", "yvel": "0.644",
           "onGround": "0"},
    9240: {"tick": "9240", "x": "14053.8311", "y": "593.391724", "yvel": "0.558",
           "onGround": "0"},
    9241: {"tick": "9241", "x": "14055.4453", "y": "590.506836", "yvel": "0",
           "onGround": "1"},
    9242: {"tick": "9242", "x": "14057.0596", "y": "589.699707", "yvel": "0",
           "onGround": "1"},
    9243: {"tick": "9243", "x": "14058.6738", "y": "588.892578", "yvel": "0",
           "onGround": "1"},
    9244: {"tick": "9244", "x": "14060.2881", "y": "588.085449", "yvel": "-2",
           "onGround": "1"},
    9245: {"tick": "9245", "x": "14061.9023", "y": "587.27832", "yvel": "0",
           "onGround": "1"},
}
for _r in GD16.values():                       # constant over these eight ticks
    _r.update({"mode": "ship", "vsize": "1", "speed": "1.10000002",
               "upsideDown": "1"})

MODEL16 = {
    9238: {"tick": "9238", "x": "14050.60254", "y": "593.1212769", "vy": "0.5410000086",
           "grounded": "0"},
    9239: {"tick": "9239", "x": "14052.2168", "y": "593.2661743", "vy": "0.6439999938",
           "grounded": "0"},
    9240: {"tick": "9240", "x": "14053.83105", "y": "593.3917236", "vy": "0.5580000281",
           "grounded": "0"},
    9241: {"tick": "9241", "x": "14055.44531", "y": "593.5404663", "vy": "0.6610000134",
           "grounded": "0"},
    9242: {"tick": "9242", "x": "14057.05957", "y": "593.7123413", "vy": "0.7639999986",
           "grounded": "0"},
    9243: {"tick": "9243", "x": "14058.67383", "y": "588.8925781", "vy": "0",
           "grounded": "1"},
    9244: {"tick": "9244", "x": "14060.28809", "y": "588.0854492", "vy": "-2",
           "grounded": "1"},
    9245: {"tick": "9245", "x": "14061.90234", "y": "587.2783203", "vy": "0",
           "grounded": "1"},
}
for _r in MODEL16.values():
    _r.update({"mode": "1"})

# The other seat arm, from 6a3e5f8's measurement (`--no-slopeseat`): y = 585
# exactly at 9,243, grounded. One row, because one row is all the block's
# second number ever was.
SEAT16 = {9243: {"tick": "9243", "y": "585", "vy": "0", "grounded": "1", "mode": "1"}}


def _table(rows: dict, kind: str, name: str) -> Table:
    return Table(rows=rows, columns=tuple(next(iter(rows.values()))),
                 provenance=Provenance(Path(name), kind))


def gd16() -> Table:
    return _table(GD16, GD, "fid_lv16.dump.csv")


def model16() -> Table:
    return _table(MODEL16, MODEL, "fid_lv16.trace.csv")


def seat16() -> Table:
    return _table(SEAT16, MODEL, "noslopeseat_lv16.trace.csv")


class Case10TwoTicks(unittest.TestCase):
    """The contrast in dp/src/dp/slopes.hpp put GD's 590.506836 against the
    model's 585.000 as though they were one moment. They are two ticks apart:
    9,241 is the tick GD grounds, 9,243 the tick the model grounds. Both
    numbers are real, both are the player's y, both are p1, both are under the
    same conditions -- every other guard in this module passes."""

    WHY = "GD grounds two ticks before the model here"

    def gd_at_9241(self) -> Observation:
        return observe(gd16(), "y", 9241)

    def seat_at_9243(self) -> Observation:
        return observe(seat16(), "y", 9243)

    def test_the_two_numbers_are_the_ones_that_were_contrasted(self):
        self.assertEqual(self.gd_at_9241().value, 590.506836)
        self.assertEqual(self.seat_at_9243().value, 585.0)

    def test_the_contrast_as_it_was_written_is_refused(self):
        """No declaration: the comparison cannot be produced at all."""
        with self.assertRaises(TickMismatch) as cm:
            contrast(self.gd_at_9241(), self.seat_at_9243())
        msg = str(cm.exception)
        self.assertIn("9241", msg)
        self.assertIn("9243", msg)
        self.assertIn("2 ticks apart", msg)
        self.assertIn("offset=+2", msg)          # and it says how to say so

    def test_declaring_the_offset_is_allowed_and_carries_it(self):
        c = contrast(self.gd_at_9241(), self.seat_at_9243(),
                     offset=+2, because=self.WHY)
        self.assertEqual(c.offset, 2)
        self.assertFalse(c.same_tick)
        self.assertEqual(c.gd.tick, 9241)
        self.assertEqual(c.model.tick, 9243)
        self.assertEqual(c.direction, GD_MINUS_MODEL)
        self.assertAlmostEqual(c.value, 5.506836, places=6)
        # the flip renames and negates; the offset is WHEN, not which way round
        self.assertAlmostEqual(c.flip().value, -5.506836, places=6)
        self.assertEqual(c.flip().offset, 2)

    def test_the_declared_form_shows_the_two_tick_offset_in_its_output(self):
        c = contrast(self.gd_at_9241(), self.seat_at_9243(),
                     offset=+2, because=self.WHY)
        out = "\n".join(format_contrast(c))
        self.assertIn("offset +2 ticks", out)
        self.assertIn("t=9241", out)
        self.assertIn("t=9243", out)
        self.assertIn("model 2 ticks after gd", out)
        self.assertIn(self.WHY, out)
        self.assertIn("590.506836", out)
        self.assertIn("585.000000", out)
        # and the one-line form says it too
        self.assertIn("offset +2", str(c))
        self.assertIn("offset +2", verdict(c, 1e-6))

    def test_an_offset_that_is_not_the_offset_is_refused(self):
        """A declaration that cannot be wrong is not a declaration."""
        with self.assertRaises(TickMismatch) as cm:
            contrast(self.gd_at_9241(), self.seat_at_9243(),
                     offset=+1, because=self.WHY)
        self.assertIn("+1", str(cm.exception))
        self.assertIn("+2", str(cm.exception))

    def test_an_offset_with_no_relationship_named_is_refused(self):
        with self.assertRaises(TickMismatch) as cm:
            contrast(self.gd_at_9241(), self.seat_at_9243(), offset=+2)
        self.assertIn("because=", str(cm.exception))

    def test_an_observation_cannot_be_built_without_its_tick(self):
        with self.assertRaises(TypeError):
            Observation(kind=GD, quantity="y", half=Half.P1, value=590.506836)
        # and a tick the table does not have is a refusal, not a neighbour
        with self.assertRaises(Incomplete) as cm:
            observe(seat16(), "y", 9241)
        self.assertIn("9241", str(cm.exception))
        self.assertIn("9243..9243", str(cm.exception))

    # ------------------------------------------------ the negative controls

    def test_the_same_two_numbers_at_one_tick_are_not_flagged(self):
        """The control for the refusal above: what is refused is the two
        TICKS, not the two numbers. Had the model's 585.000 been its y at
        9,241, this is a plain 5.5px disagreement and nothing objects."""
        c = contrast(self.gd_at_9241(),
                     Observation(kind=MODEL, quantity="y", half=Half.P1,
                                 tick=9241, value=585.0))
        self.assertTrue(c.same_tick)
        self.assertEqual(c.offset, 0)
        self.assertEqual(c.because, "")
        self.assertAlmostEqual(c.value, 5.506836, places=6)
        self.assertIn("same tick t=9241", verdict(c, 1e-6))

    def test_the_same_two_quantities_at_the_same_tick_compare_cleanly(self):
        """The tick the model does land: GD 588.892578 against the shipped
        arm's 588.8925781, and nothing is flagged."""
        c = contrast(observe(gd16(), "y", 9243), observe(model16(), "y", 9243))
        self.assertTrue(c.same_tick)
        self.assertEqual(c.offset, 0)
        self.assertTrue(c.agrees(1e-6))
        self.assertLess(abs(c.value), 1e-6)
        self.assertNotIn("offset", str(c))
        self.assertIn("same tick t=9243", str(c))

    def test_a_same_tick_disagreement_is_reported_not_refused(self):
        """At 9,241 the two really do differ by 3px, because GD is grounded and
        the model is not. A guard that refused this would be hiding the
        finding."""
        c = contrast(observe(gd16(), "y", 9241), observe(model16(), "y", 9241))
        self.assertTrue(c.same_tick)
        self.assertAlmostEqual(c.value, 590.506836 - 593.5404663, places=6)
        self.assertFalse(c.agrees(1e-6))
        self.assertIn("DIFFER", verdict(c, 1e-6))

    def test_the_guard_does_not_reject_the_whole_window(self):
        """The --eval-padgate lesson: a check that answers nothing looks the
        same as a check that passes. Count what came out."""
        gd, model = gd16(), model16()
        agree, differ = [], []
        for t in range(9238, 9246):
            c = contrast(observe(gd, "y", t), observe(model, "y", t))
            self.assertIsInstance(c, Contrast)
            (agree if c.agrees(1e-6) else differ).append(t)
        self.assertEqual(len(agree) + len(differ), 8)      # nothing refused
        self.assertEqual(differ, [9241, 9242])             # and not everything
        self.assertEqual(len(agree), 6)

    def test_never_compared_is_not_the_same_as_compared_and_agreed(self):
        """The state the comment was in was not disagreement. It was that no
        comparison had been made at all, and that has to be sayable."""
        self.assertEqual(verdict(None, 1e-6), NEVER_COMPARED)
        agreed = verdict(contrast(observe(gd16(), "y", 9243),
                                  observe(model16(), "y", 9243)), 1e-6)
        self.assertIn("agree", agreed)
        self.assertIn("same tick t=9243", agreed)
        self.assertNotEqual(agreed, NEVER_COMPARED)

    # ------------------------------------------------------- the series form

    def test_a_shifted_window_is_refused_and_names_the_shift(self):
        gd, model = gd16(), model16()
        a = series(gd, "y", Half.P1, Window(9238, 9241))
        b = series(model, "y", Half.P1, Window(9240, 9243))
        with self.assertRaises(TickMismatch) as cm:
            difference(a, b)
        msg = str(cm.exception)
        self.assertIn("shifted by +2", msg)
        self.assertIn("offset=+2", msg)

    def test_a_declared_shift_is_keyed_on_gd_and_prints_the_offset(self):
        gd, model = gd16(), model16()
        d = compare(gd, model, "y", window=Window(9239, 9241), offset=+2,
                    because=self.WHY)
        self.assertEqual(d.offset, 2)
        self.assertEqual((d.window.t0, d.window.t1), (9239, 9241))
        # values[t] is GD at t against the model at t+2
        self.assertAlmostEqual(d.at(9241), 590.506836 - 588.8925781, places=6)
        head = format_difference(d)[0]
        self.assertIn("offset +2 ticks", head)
        self.assertIn("9241..9243", head)
        self.assertIn(self.WHY, head)

    def test_the_ordinary_series_comparison_is_untouched(self):
        d = compare(gd16(), model16(), "y", window=Window(9238, 9245))
        self.assertEqual(d.offset, 0)
        self.assertTrue(d.same_tick)
        self.assertEqual(len(d.values), 8)
        self.assertNotIn("offset", str(d))
        self.assertNotIn("offset", format_difference(d)[0])
        self.assertEqual(d.conditions.single("mode"), "ship")

    # ------------------------------------------------------- the fixture itself

    def test_the_fixture_is_what_the_dumps_say(self):
        """Rule 4. The literals above are a transcription; when the files are
        on disk they are the authority, and a drift between the two must show
        up here rather than in a conclusion."""
        got = dumps(16)
        if got is None:
            self.skipTest(f"no fidelity dumps for lv16 in {FID}")
        gd, model = got
        for t in range(9238, 9246):
            self.assertEqual(float(gd.row(t)["y"]), float(GD16[t]["y"]),
                             msg=f"gd y t={t}")
            self.assertEqual(gd.row(t)["onGround"], GD16[t]["onGround"])
            self.assertEqual(float(model.row(t)["y"]), float(MODEL16[t]["y"]),
                             msg=f"model y t={t}")
            self.assertEqual(model.row(t)["grounded"], MODEL16[t]["grounded"])
        # the two ticks the contrast is about, named the way 6a3e5f8 names them
        self.assertEqual(gd.row(9241)["onGround"], "1")     # GD grounds at 9,241
        self.assertEqual(model.row(9241)["grounded"], "0")
        self.assertEqual(model.row(9243)["grounded"], "1")  # the model at 9,243


class NotCovered(unittest.TestCase):
    """Cases 7 and 8 are judgement. This test exists so nobody reads a green
    suite as covering them."""

    def test_a_comparison_past_the_first_divergence_is_still_accepted(self):
        gd, model = need(19)
        d = compare(gd, model, "y", window=Window(21396, 21500))
        self.assertIsNotNone(d.first_beyond(0.3))
        # every guard is satisfied and the tail is still two world-lines.
        self.assertIsInstance(d, Difference)
        self.assertEqual(d.direction, Direction.GD_MINUS_MODEL)


if __name__ == "__main__":
    unittest.main(verbosity=2)
