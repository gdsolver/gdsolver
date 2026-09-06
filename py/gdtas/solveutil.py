"""Facts about a run that both the solver driver and the measuring tools need.

These used to live in `dp_solve_level.py`, which made the driver a dependency of
the regression that judges it: `quick_regress` (tier 1, 81 s, no worker),
`fixcensus`, `fidelity_diff` and `regen_groups` all imported it. They are not
driver policy -- they are how a dump, an objrects table and a plan file are
read -- so they belong on the library side, and the driver imports them from
here like everyone else.

The bodies are the driver's, moved unchanged: this is a lift, not a rewrite, and
every consumer is judged by byte-identical output.

Nothing here decides anything. No solving, no worker, no argv.
"""

from __future__ import annotations

import re
import shutil
import time
from pathlib import Path

from gdtas.worker import wall_event

# Where a diagnostic line goes. The driver owns a real log; a tool that has
# none leaves this alone and the line is dropped (the wall_event above is the
# part that must not be lost).
log = None


def _say(msg: str) -> None:
    if log is not None:
        log(msg)


# GD's mode name -> the ordering leveldp uses (`--start`'s 5th field).
#
# swing (mode 7) reached leveldp on 2026-08-12 and was left out of this table:
# for every re-anchor GD reported as swing, the driver passed mode=0. Measured
# on lv22 (2026-08-13): at t=4,879 GD is swing / upsideDown=1 / vy=8 while the
# solve was told cube, and the death point oscillated between x=6,051 and 6,247
# because of it. lv1-21 have no swing section, so nothing else ever saw it.
MODE_ID = {"ship": 1, "ball": 2, "ufo": 3, "wave": 4, "robot": 5, "spider": 6,
           "swing": 7}
# The flying modes keep onGround set while airborne (it is sticky), so a
# grounded test there needs onGround2 and vy ~ 0 as well. The cube family's
# flag can be believed as it stands.
#
# [2026-09-07] THIS SET IS INCOMPLETE BY ITS OWN DESCRIPTION: swing is a flying
# mode and is measured sticky, and it is not here. Share of grounded ticks with
# the body plainly moving (|yvel| > 1.0), from GD's dumps alone:
#
#     wave 87.0 / 91.5   ufo 74.7   ship 53.6 / 33.4   SWING 31.7   <- in
#     ---- the break, about 8x ----                                    FLYING?
#     cube 3.9 / 0.0     ball 3.0 / 0.0   robot 0.0   spider 0.1 / 0.1   no
#
# so spider does NOT belong here (0.1% is the landing tick itself, which every
# non-sticky mode shows -- that is why the floor is not 0), and swing DOES.
#
# Deliberately not added yet. Adding a mode TIGHTENS grounded_of, so it moves
# anchor seeds and is visible to quick_regress, the fixcensus sections and
# deathref -- it needs an A/B arm, not an edit. The reach is at most 2 seeds in
# the corpus: anchors land in swing 8 times of 1,138 and 6 of those are swing
# FLIPPED, which never grounds in these levels (0 of 1,197 ticks) and so seeds
# grounded=0 either way. Two caveats on that number: swing occurs only on lv22,
# so both figures rest on one level; and whether flipped swing CANNOT ground or
# merely did not here is unsettled -- it has 21 ticks at |vy| < 0.01 with the
# flag never set, but a swing's arc apex also reads as |vy| ~ 0, so that does
# not separate "structurally never" from "not in this corpus". If a level
# grounds a flipped swing, the reach-2 number expires.
# See notes/measure-onground-stickiness-per-mode-2026-09-07.
FLYING = (1, 3, 4)

# The two definitions of "GD was grounded" that `cause_of` can sign a family
# with. RAW is the dump's `onGround` column as it stands, and is the default
# because it is what every census key in data/gdref/fixcensus.json was minted
# from; CORROBORATED puts the same row through `grounded_of` first, which is
# what every anchor built from the same recording already uses.
# THESE ARE DIFFERENT PREDICATES, NOT THE SAME ONE AT DIFFERENT TIMES -- the
# measurement that settled that is in `grounded_of`'s docstring. The switch
# exists so the question "is this family signed by physics or by the raw
# column's looseness?" can be asked without changing what the baseline means.
GD_GROUND_RAW = "raw"
GD_GROUND_CORROBORATED = "corroborated"


def _ground_letter(raw: str, mode: int, gd_row: dict | None,
                   gd_ground: str) -> str:
    """The digit a `gdg` / `gdgo` component carries, under one definition.

    RAW hands back the column untouched, so the default path through `cause_of`
    is the identity and every existing key keeps its spelling. CORROBORATED
    re-derives it with `grounded_of` from the SAME dump row, which needs
    `onGround2` and `yvel` as well -- hence the row rather than the one column.

    A row that cannot be translated mints `?`, deliberately: falling back to the
    raw digit would make an untranslatable row indistinguishable from a
    genuinely raw-agreeing one, and the census would report a zero difference
    that came from the instrument rather than from the world. (Measured
    2026-09-07 over fixcensus's 20 divergences: 40 rows, 0 untranslatable, so
    the `?` branch is unexercised on today's corpus rather than untested policy.)
    """
    if gd_ground == GD_GROUND_RAW:
        return raw
    if gd_row is None:
        return "?"
    try:
        return str(grounded_of(mode, gd_row.get("onGround", "0"),
                               gd_row.get("onGround2", "0"), gd_row["yvel"]))
    except (KeyError, ValueError, TypeError):
        return "?"

# The types the model actually collides with or reads. If any of them carries a
# group id, the level has geometry that moves.
_COLLIDER_TYPES = {"0", "2", "47", "25", "3", "4", "5", "6", "16", "17", "18",
                   "19", "21", "23", "24", "26", "27", "33", "41",
                   "8", "9", "10", "34", "11", "12", "13", "29", "32", "35"}


def read_lines(path) -> list[str]:
    """Get-Content's equivalent. utf-8-sig: existing artefacts carry a BOM."""
    try:
        return Path(path).read_text(encoding="utf-8-sig",
                                    errors="replace").splitlines()
    except OSError:
        return []


def copy_held_file(src, dst) -> bool:
    """Copy a file the worker still holds open. NEVER raises.

    A failed snapshot is not a reason to kill a level -- the caller degrades and
    carries on -- but it must not be silent either, because a missing snapshot
    reads exactly like a physics wall.
    """
    for attempt in range(3):
        try:
            with open(src, "rb") as fin, open(dst, "wb") as fout:
                shutil.copyfileobj(fin, fout)
            # A retry that succeeded is a wall-clock branch too (the heavier the
            # load, the likelier it is). Succeeding silently would let the run
            # be recorded as "the same conditions".
            if attempt:
                wall_event("snapshot-retry", f"{Path(src).name} ok on try {attempt + 1}")
            return True
        except OSError as e:
            if attempt == 2:
                wall_event("snapshot-failed", f"{Path(src).name}: {e}")
                _say(f"  [snapshot] cannot copy {src}: {e}")
                return False
            time.sleep(0.2)
    return False


def has_grouped_colliders(path) -> bool:
    """Does any collider carry a group id? (objrects' `groups` column.)"""
    p = Path(path)
    if not p.exists():
        return False
    with p.open("r", encoding="utf-8-sig", errors="replace") as f:
        header = f.readline()
        if not header:
            return False
        cols = header.strip().split(",")
        if "type" not in cols or "groups" not in cols:
            return False
        i_type, i_grp = cols.index("type"), cols.index("groups")
        for line in f:
            fl = line.rstrip("\n").split(",")
            if len(fl) <= i_grp or fl[i_grp] == "0":
                continue
            if fl[i_type] in _COLLIDER_TYPES:
                return True
    return False


# Trace columns, for cause_of:
#   0 tick 1 x 2 y 3 vy 4 mode 5 grounded 6 dual 7 y2 8 vy2 9 flip2 10 act
#   11 onslope 12 slopem 13 slopet 14 bandf 15 bandc 16 mini 17 held 18 dx
#
# THESE ARE THE MODEL'S COLUMNS. In particular column 11 is leveldp's
# `s.onSlope`, not GD's: the dump has no onSlope field (the mod's 37-column
# header does not carry one, and GD's flag surfaces only in the `slp:` log
# lines of hooks_player.cpp). So the known fact that GD's onSlope is
# phase-blind -- checkCollisions zeroes it at the top of every tick -- has no
# consumer in any GD-vs-model comparison; a 2026-09-06 sweep of py/ and mcp/
# found `onslope` read here and nowhere else. Worth knowing before treating it
# as a live hazard: it is a hazard for anyone reading the log line, and for
# nothing in this tree.
def cause_of(row: list, nxt: list | None = None, gd_grounded: str = "?",
             gd_mode: int = -1, gd_grounded_out: str | None = None,
             gd_mode_out: int | None = None, *,
             gd_ground: str = GD_GROUND_RAW, gd_row: dict | None = None,
             gd_row_out: dict | None = None) -> str:
    """One signature for what the model was DOING across this transition.

    **The key that groups by cause rather than by point.** If many records share
    a signature, what needs fixing is one rule, not n local overrides. A small
    spread in dy means a constant is off (fit it); a large one means the formula
    is wrong (fix the code) -- and point corrections could not tell those apart.

    **BOTH SIDES ARE READ ENTERING THE TICK.** `gd_grounded` / `gd_mode` are
    GD's values on the SAME row the model's come from (t-1); GD's values on the
    way out go in `gd_grounded_out` / `gd_mode_out` and are emitted only when
    they differ, as `gdgo` / `gdmo`.
    Until 2026-09-05 the model's half was read at t-1 and GD's at t, and the
    mismatch misread four separate families in one day -- most sharply lv17
    t=15,730, where `gdm3` said "GD is in another mode" about a tick on which
    BOTH sides switch on time, because the model's mode was taken from the row
    before the switch and GD's from the row after.
    The asymmetry was not useless: `g0/gdg1` happened to mean "the model came in
    airborne and GD left grounded", which is the same-tick portal seat's
    signature and is how that family was found. `gdgo` keeps that -- and states
    it as what it is, a transition of GD's own, rather than as an artefact of
    comparing two different rows.

    **WHICH DEFINITION OF GROUNDED THE `gdg` LETTERS COME FROM IS A SWITCH.**
    `gd_ground=GD_GROUND_RAW` (the default, and today's behaviour exactly) mints
    them from `gd_grounded` / `gd_grounded_out` as passed, i.e. from the dump's
    raw `onGround`. `gd_ground=GD_GROUND_CORROBORATED` re-derives them with
    `grounded_of`, which needs the whole row, so `gd_row` / `gd_row_out` must be
    passed with it. The default is load-bearing: every key in the blessed census
    baseline was minted raw, so changing it would rename every family at once.
    The switch is there to answer whether a family is signed by physics or by
    the raw column's looseness, not to replace one with the other.
    """
    if len(row) < 18:
        return "notrace"          # an old trace (the columns are not there)
    # **GD's grounded goes in too.** `air` is the model's own claim, so without
    # this "integration error in free flight" and "the model missed a contact"
    # share a signature. Measured on lv20: m1/mini1/g0/air was 47 of 59 records
    # with an 11.8 px spread in dy -- the mark of two causes mixed, not one.
    #
    # `gdg` IS THE RAW COLUMN BY DEFAULT, NOT grounded_of(). fixcensus.eval_trace
    # passes `g0.get("onGround")` straight through, while every anchor built from
    # the same recording is translated by grounded_of first, and the two mean
    # different things in ship / ufo / wave / spider -- the per-mode gap is in
    # grounded_of's docstring below. Read here rather than there because this is
    # where the letter is minted: `gdg1` on a flying-mode record means "GD's
    # contact flag was set", not "GD was resting on something", and about a third
    # of ship's records carry it for a body in free flight.
    # THE DEFAULT DOES NOT CHANGE: the key is what the family baseline is named
    # by, and renaming it moves every family at once. `gd_ground` opens the other
    # definition for an A/B, and the two letters below are the whole of it.
    if gd_ground not in (GD_GROUND_RAW, GD_GROUND_CORROBORATED):
        raise ValueError(f"gd_ground must be {GD_GROUND_RAW!r} or "
                         f"{GD_GROUND_CORROBORATED!r}, not {gd_ground!r}")
    g_in = _ground_letter(gd_grounded, gd_mode, gd_row, gd_ground)
    parts = [f"m{row[4]}", f"mini{row[16]}", f"g{row[5]}", f"gdg{g_in}"]
    # ...and GD's own within-tick transition of it, when there is one. This is
    # what carries "GD seated on this tick" (see the docstring). Both ends go
    # through the same definition, so the comparison stays like-for-like -- the
    # out row is translated with the out row's OWN mode, because a tick that
    # crosses a portal has GD in two of them.
    if gd_grounded_out is not None:
        g_out = _ground_letter(
            gd_grounded_out,
            gd_mode_out if gd_mode_out is not None else gd_mode,
            gd_row_out, gd_ground)
        if g_out != g_in:
            parts.append(f"gdgo{g_out}")
    # **A tick where GD's MODE differs is a different cause.** Portal boundaries
    # are a known +/-1 tick class, and there the two are running different
    # physics. Mixed in, it reads as "the mini ship's integration error"
    # (measured: GD alone gains 2.1 of vy in one tick -- 16x the mini ship's
    # maximum acceleration of 0.127, which cannot happen in one mode).
    if gd_mode >= 0 and str(gd_mode) != row[4]:
        parts.append(f"gdm{gd_mode}")
    # ...and GD's own within-tick mode change, by the same rule as gdgo. No
    # corpus instance today (the one level that had a `gdm` lost it when the
    # portal seat landed), so this changes no key now; it is here so the mode
    # cannot repeat the misreading the grounded column just had.
    # `>= 0` on BOTH: -1 is MODE_ID's "not a mode I know", which a row can carry
    # when GD's column is blank, and reading it as a transition put a literal
    # `gdmo-1` on lv16's ride24+ family the first time this was run.
    if (gd_mode_out is not None and gd_mode >= 0 and gd_mode_out >= 0
            and gd_mode_out != gd_mode):
        parts.append(f"gdmo{gd_mode_out}")
    if len(row) >= 19:
        # Velocity arrives in px/tick, so it goes back to the familiar multiplier
        # for the signature (0.9 -> 1.29825). The thresholds are per speed, so
        # without this the families cannot be split.
        try:
            parts.append(f"sp{float(row[18]) / 1.29825 * 0.9:.1f}")
        except ValueError:
            pass
    # **The clamp and the neighbourhood are read from the NEXT row.** Both happen
    # inside that step, so neither appears on the t-1 row. This used to read them
    # off mp0.
    ev = nxt if (nxt is not None and len(nxt) >= 21) else row
    if len(ev) >= 21 and ev[20] not in ("-", ""):
        # **The name of the clamp that zeroed vy, and what it hit.** A wrong
        # clamp shows up as "only the model loses its speed", but which one fired
        # cannot be worked out from outside (measured: it was happening where
        # there was neither a static object nor a flight band).
        parts.append(f"clamp:{ev[20]}")
        if len(ev) >= 22 and ev[21] not in ("-1", ""):
            parts.append(f"uid{ev[21]}")
    if len(ev) >= 20 and ev[19] == "1":
        # A tick with something that can deliver an impulse within reach. The
        # axis that isolates the divergence where GD alone gains about 2 units of
        # velocity (an order of magnitude away from any difference in
        # acceleration).
        parts.append("orbnear")
    if row[11] == "1":
        try:
            parts.append(f"slope{float(row[12]):+.2f}")
        except ValueError:
            parts.append("slope?")
        try:
            st = int(row[13])
            parts.append("ride24+" if st >= 24 else f"ride{st}")
        except ValueError:
            pass
    else:
        parts.append("air")
    try:
        y, bf, bc = float(row[2]), float(row[14]), float(row[15])
        if bc < 1e8:              # only while the flight band is live
            if abs(y - bc) < 20.0:
                parts.append("nearceil")
            elif abs(y - bf) < 20.0:
                parts.append("nearfloor")
    except ValueError:
        pass
    return "/".join(parts)


def grounded_of(mode: int, on_ground: str, on_ground2: str, yvel: str) -> int:
    """GD's ground state as the model means it, from a dump row.

    THE FLIGHT CONJUNCTION IS LOAD-BEARING, not belt-and-braces: GD's
    `onGround` and the model's `grounded` DO NOT MEAN THE SAME THING in ship,
    UFO, wave and spider, and this is what converts one into the other.

    They are NOT out of phase -- an earlier version of this comment said they
    were and it was wrong. Measured over model 0->1 landings inside windows
    where the two sides agree on y to 0.001 px across +-10 ticks, asking at what
    shift d GD makes the same 0->1 transition:

        d = 0                      682
        d != 0 (any shift, +-5)      0
        GD never transitions        48

    Not one disagreement is explained by a shift. In all 48 GD's flag is
    ALREADY 1 and stays 1, so GD calls the player grounded in states the model
    does not -- a wider predicate, not a shifted one. A separate audit puts the
    raw-versus-corroborated gap at 48,564 of 455,396 gdref ticks (10.7%),
    concentrated in exactly these modes: ship 31.1%, ufo 49.8%, wave 44.1%,
    spider 6.6%, cube at or under 0.1%.

    Requiring `on_ground2` and a near-zero yvel narrows GD's looser flag to the
    model's meaning. Do not simplify this to `on_ground == "1"` for flight: the
    redundancy IS the conversion, and without it the anchor would be seeded
    grounded for a body GD considers grounded and the model does not.

    `gdtas.solveutil.cause_of` does NOT apply it by default -- it reads the raw
    column -- so family signatures carrying a `gdg`/`gdgo` component in those
    modes can be signed by the definitional gap rather than by physics. Pass
    `gd_ground=GD_GROUND_CORROBORATED` (fixcensus: `--gd-ground corroborated`)
    to sign them with this function instead and see which ones move.

    MEASURED ON fixcensus's OWN POPULATION, 2026-09-07, all 22 levels / 1,116
    sections / 20 divergences, both arms on the same leveldp: the headline does
    NOT move -- 20 divergences and 19 families either way, and the one family
    holding two records holds the same two. Three signatures are RENAMED, all
    three in a flying mode, and no two families merge:

        lv16 t=19,104 mini ship upright  g0/gdg1       -> g0/gdg0
        lv19 t=21,402 mini ship upright  g0/gdg1       -> g0/gdg0
        lv20 t=22,015 full ufo  upright  g0/gdg1       -> g0/gdg0/gdgo1

    The third is the interesting one and was not predicted: GD's raw flag is 1
    on both rows, so raw sees no transition, while corroborated goes 0 -> 1
    because `onGround2` goes 0 -> 1 and yvel lands on exactly 0. Under the
    corroborated definition that tick IS a GD seating event, and the raw column
    hides it. So `g0/gdg0/gdgo1` -- the same-tick portal seat's signature --
    goes from 2 families to 3, while the literal `g0/gdg1` goes from 4 to 1;
    the survivor is lv19 t=22,602, robot, which grounded_of does not narrow.
    Of the 40 GD rows behind those 20 records, 15 are in a flying mode, 25 are
    not, and none of the 25 widened (grounded_of's non-flying branch also
    accepts yvel == 0, which no such row had) -- that zero is the world's, not
    the filter's.
    See notes/measure-onground-phase-offset-2026-09-06.

    Corroborated from the other direction by the column-phase audit
    (notes/measure-dump-column-phases-2026-09-06), which swept d = -3..+3 over
    379,480 ticks in the same kind of window and found d=0 the strict minimum
    in all 16 (mode, flipped) regimes -- for the raw column, for `onGround2`,
    and for this function's output alike. Two instruments, opposite starting
    assumptions, no shift in either.
    """
    if mode in FLYING:
        return 1 if (on_ground == "1" and on_ground2 == "1"
                     and abs(float(yvel)) < 0.01) else 0
    return 1 if (on_ground == "1" or float(yvel) == 0) else 0


def held_before(plan_path, tick: int) -> int:
    """Whether the last input before `tick` was down.

    The ship's acceleration branches on it, so assuming 0 accumulates
    trajectory error from the anchor onwards.
    """
    held = 0
    for l in read_lines(plan_path):
        m = re.match(r"^input=(\d+),(\d)$", l)
        if m and int(m.group(1)) < tick:
            held = int(m.group(2))
    return held


def rot2900s(objrects_path) -> list[tuple[int, float, float]]:
    """The id 2900 (gameplay rotation) objects as (uid, cx, cy).

    lv1-21 have none, so the spentrot / trigraw machinery sleeps entirely there.
    """
    out: list[tuple[int, float, float]] = []
    try:
        for i, ln in enumerate(read_lines(objrects_path)):
            if i == 0:
                continue
            c = ln.split(",")
            if len(c) < 10 or c[0] != "2900":
                continue
            out.append((int(c[7]), float(c[2]), float(c[3])))
    except (OSError, ValueError):
        out = []
    return out


def spent2900_from_dump(path, attempt: int, t0: int,
                        trigs: list[tuple[int, float, float]]) -> list[int]:
    """Which 2900s had already fired before t0, from the dump's gframe changes.

    A 2900's one-shot (firedT) cannot travel through `--start`. Re-anchor behind
    the maze without it and the model re-fires a spent trigger at the next
    crossing, entering a rotated frame and breaking the world (lv22 t=14,321,
    uid6337: GD consumed it at t=12,795 and passes straight through; a fixup
    cannot carry a frame, so this cost 30 iterations of treading water).

    The culprit for a transition is the nearest 2900 to the player at that tick.
    Measured over lv22's 13 transitions the worst is 27 px, and the 2900s sit
    300+ px apart, so the cut is at 100.
    """
    if not trigs:
        return []
    p = Path(path)
    if not p.exists():
        return []
    spent: list[int] = []
    try:
        f = p.open("r", encoding="utf-8-sig", errors="replace")
    except OSError:
        return []
    with f:
        header = f.readline()
        if not header:
            return []
        cols = header.strip().split(",")
        if "gframe" not in cols:
            return []
        i_att, i_tick = cols.index("attempt"), cols.index("tick")
        i_x, i_y, i_gf = cols.index("x"), cols.index("y"), cols.index("gframe")
        prev = None
        for line in f:
            fl = line.rstrip("\n").split(",")
            if len(fl) <= max(i_gf, i_x, i_y):
                continue
            if attempt >= 0 and fl[i_att] != str(attempt):
                continue
            try:
                t = int(fl[i_tick])
                gf = int(float(fl[i_gf]))
            except ValueError:
                continue
            if t > t0:
                break
            if prev is not None and gf != prev:
                try:
                    x, y = float(fl[i_x]), float(fl[i_y])
                except ValueError:
                    prev = gf
                    continue
                best, bd = -1, 100.0 ** 2
                for uid, cx, cy in trigs:
                    d = (cx - x) ** 2 + (cy - y) ** 2
                    if d < bd:
                        bd, best = d, uid
                if best >= 0 and best not in spent:
                    spent.append(best)
            prev = gf
    return spent
