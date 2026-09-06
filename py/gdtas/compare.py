# -*- coding: utf-8 -*-
"""Comparisons that carry their own conditions.

Two numbers that look comparable often are not, and nothing in a float says
so. Over one day nine conclusions in this project were wrong for that reason
alone -- a sign read backwards, a p2 value held against a p1 value, a filter
carried over from a site with a different vsize, a rounded table read in place
of the raw column, a truncated pipe read as the corpus, a hand-built table
missing a tick, a mover level replayed without --groups. This module makes the
common ones refuse rather than answer.

What it enforces

    direction     Every difference names which way it is subtracted. There is
                  no unnamed delta: `Difference.direction` is one of
                  GD_MINUS_MODEL / MODEL_MINUS_GD and `flip()` renames as it
                  negates. (fixcensus.py's `edy` is GD - model;
                  drift_origin.py's `d` is model - GD. Both are right; neither
                  file said so.)
    half          Every series records the body it came from. `difference()`
                  refuses two series whose halves differ, so a p2 quantity can
                  never be held against a p1 one.
    conditions    mode / vsize / speed / gravity are read FROM THE DUMP for
                  the ticks in question. A caller may state what it believes
                  they are; if the data disagrees that is a refusal, not a
                  warning, and the message prints both.
    completeness  A window is covered or it is not. A missing tick, a series
                  that stops before the requested window, a repeated tick --
                  each is a refusal naming what is missing. Silently
                  intersecting two tick sets is how a 4.6% replay passes for a
                  whole level.
    provenance    A model trace can carry the argv it was produced with, and
                  `require_replay_flags` refuses one that is missing the flags
                  a level needs. An unrecorded provenance is a refusal too:
                  not knowing is not the same as knowing it was fine.

    raw           Nothing here rounds. `Difference` holds the floats as read.
                  Rendering is `format_difference`, a separate call, and its
                  output is for eyes only -- never the input to a decision.

What it does NOT cover

    Two of the nine were judgement, not mechanism, and no signature can catch
    them:

      * A filter derived from the hypothesis under test, which discards the
        rows that would characterise the answer. Every condition here was
        satisfied; the question was wrong.
      * Counting reach from whole-run traces past each level's first
        divergence. Both series are complete, same half, same conditions, and
        the comparison is still meaningless because after the first divergence
        the model is on a different world-line. `first_divergence()` will tell
        you where that is if you ask; it cannot know you should have.

    Do not read a green result from this module as either of those being safe.
"""
from __future__ import annotations

import csv
import math
import os
from dataclasses import dataclass
from enum import Enum
from pathlib import Path

from gdtas.paths import REPO

# ---------------------------------------------------------------- refusals

class Refused(Exception):
    """Base for every refusal. Catching this catches all of them."""


class HalfMismatch(Refused):
    """Two series from different bodies were about to be subtracted."""


class ConditionMismatch(Refused):
    """The caller's stated conditions are not what the data says for those ticks."""


class ConditionVaries(Refused):
    """A condition is not constant across the window, so it cannot be stated as one value."""


class Incomplete(Refused):
    """Missing ticks, a repeated tick, or a series that stops short of the window."""


class ProvenanceMissing(Refused):
    """The run's arguments are unknown, or lack a flag the level needs."""


# ---------------------------------------------------------------- vocabulary

class Half(str, Enum):
    """Which body a row came from. Dual levels have two; single levels have P1."""

    P1 = "p1"
    P2 = "p2"


class Direction(str, Enum):
    """Which way a difference is subtracted. Both are legitimate; the name is
    the whole point -- `edy` and drift_origin's `d` are opposite conventions
    living in the same repository."""

    GD_MINUS_MODEL = "gd-model"
    MODEL_MINUS_GD = "model-gd"

    def flipped(self) -> "Direction":
        return (Direction.MODEL_MINUS_GD if self is Direction.GD_MINUS_MODEL
                else Direction.GD_MINUS_MODEL)


GD_MINUS_MODEL = Direction.GD_MINUS_MODEL
MODEL_MINUS_GD = Direction.MODEL_MINUS_GD

GD = "gd"          # a dump written by the game (fid_lv*.dump.csv, gdref/lv*.csv)
MODEL = "model"    # a trace written by leveldp (*.trace.csv)

# Columns, per source kind and half. A quantity absent for a half is absent
# here rather than guessed: `column()` raises, and the caller finds out at the
# door instead of comparing whatever the fallback happened to be.
COLUMNS: dict[tuple[str, Half], dict[str, str]] = {
    (GD, Half.P1): {"y": "y", "vy": "yvel", "x": "x", "mode": "mode",
                    "vsize": "vsize", "speed": "speed", "gravity": "upsideDown",
                    "ground": "onGround", "dual": "dual"},
    (GD, Half.P2): {"y": "p2y", "vy": "p2vy", "x": "p2x", "mode": "p2mode",
                    "vsize": "p2vsize", "speed": "speed", "gravity": "p2up",
                    "ground": "p2ground", "dual": "dual"},
    (MODEL, Half.P1): {"y": "y", "vy": "vy", "x": "x", "mode": "mode",
                       "ground": "grounded", "act": "act", "dual": "dual"},
    (MODEL, Half.P2): {"y": "y2", "vy": "vy2", "mode": "mode2",
                       "ground": "grounded2", "dual": "dual"},
}

# The conditions a comparison is taken under. Read from the GD dump, which is
# the only side that reports all of them.
CONDITIONS = ("mode", "vsize", "speed", "gravity")

# The flags a replay of a level with moving geometry needs. Dropping one does
# not error: the run dies early and the trace looks byte-identical as far as it
# got (lv20 reached 4.6% and went into a commit message as a match).
MOVING_GEOMETRY_FLAGS = ("--groups", "--triggers", "--objgroups", "--obb")
# Whole-run only, and in practice lv22 only -- State::rotSpent accumulates, so
# it is wrong at an anchor. See quick_regress.whole_run_args.
ROTATION_FLAGS = ("--rotqueue",)


def fidelity_dir() -> Path:
    """Where fidelity_diff leaves its dumps.

    <repo>/build/fidelity, except that from a git worktree there is no build
    tree -- the dumps belong to the main checkout. Walk up for the first parent
    that has one, the same shape as gdtas.paths._machine_local.
    """
    env = os.environ.get("GDSOLVER_FIDELITY")
    if env:
        return Path(env)
    here = REPO / "build" / "fidelity"
    if here.exists():
        return here
    for p in REPO.parents:
        cand = p / "build" / "fidelity"
        if cand.exists():
            return cand
    return here


# ---------------------------------------------------------------- provenance

@dataclass(frozen=True)
class Provenance:
    """How a table came to exist: the file, what wrote it, and with which argv.

    `argv` is None when nobody recorded it. That is not the same as "no flags
    were needed": `require_replay_flags` refuses an unrecorded provenance,
    because the whole failure being guarded against is invisible in the data.
    """

    path: Path
    kind: str
    argv: tuple[str, ...] | None = None

    def has_flag(self, flag: str) -> bool:
        return self.argv is not None and flag in self.argv


def read_argv_sidecar(path: Path | str) -> tuple[str, ...] | None:
    """Read `<path>.argv.txt` (one argument per line) if a producer wrote one.

    Nothing in the repository writes this file yet. It exists so that a
    producer can start recording its argv without every reader changing, and
    so that `require_replay_flags` has somewhere to look before it refuses.
    """
    p = Path(str(path) + ".argv.txt")
    if not p.exists():
        return None
    return tuple(l for l in p.read_text(encoding="utf-8").splitlines() if l)


def require_replay_flags(table: "Table", flags=MOVING_GEOMETRY_FLAGS) -> None:
    """Refuse a model trace produced without the flags this comparison needs."""
    prov = table.provenance
    if prov.argv is None:
        raise ProvenanceMissing(
            f"{prov.path.name}: the argv it was replayed with was not recorded, "
            f"so it cannot be shown to carry {', '.join(flags)}. A run missing "
            f"one of those dies early and looks byte-identical as far as it got")
    missing = [f for f in flags if f not in prov.argv]
    if missing:
        raise ProvenanceMissing(
            f"{prov.path.name}: replayed without {', '.join(missing)} "
            f"(argv: {' '.join(prov.argv)})")


# ---------------------------------------------------------------- tables

@dataclass(frozen=True)
class Table:
    """One CSV of per-tick rows, keyed by tick, with its provenance."""

    rows: dict[int, dict]
    columns: tuple[str, ...]
    provenance: Provenance

    @property
    def kind(self) -> str:
        return self.provenance.kind

    @property
    def path(self) -> Path:
        return self.provenance.path

    def row(self, tick: int) -> dict:
        return self.rows[tick]

    def span(self) -> tuple[int, int]:
        return min(self.rows), max(self.rows)


def read_table(path: Path | str, kind: str, argv: tuple[str, ...] | None = None
               ) -> Table:
    """Read a GD dump or a model trace.

    A repeated tick is a refusal. Keeping the last row silently is how one
    file holding several attempts passes for one run.
    """
    p = Path(path)
    if kind not in (GD, MODEL):
        raise ValueError(f"kind must be {GD!r} or {MODEL!r}, not {kind!r}")
    if not p.exists():
        raise Incomplete(f"{p} does not exist")
    rows: dict[int, dict] = {}
    dupes: list[int] = []
    with p.open(newline="", encoding="utf-8-sig", errors="replace") as f:
        rd = csv.DictReader(f)
        cols = tuple(rd.fieldnames or ())
        for r in rd:
            try:
                t = int(r["tick"])
            except (KeyError, TypeError, ValueError):
                continue
            if t in rows:
                dupes.append(t)
            rows[t] = r
    if not rows:
        raise Incomplete(f"{p}: no rows with a tick column (columns: {cols})")
    if dupes:
        raise Incomplete(
            f"{p}: {len(dupes)} repeated ticks (first {dupes[0]}). More than "
            f"one attempt in one file -- split them before comparing")
    if argv is None:
        argv = read_argv_sidecar(p)
    return Table(rows=rows, columns=cols, provenance=Provenance(p, kind, argv))


def column(kind: str, half: Half, quantity: str) -> str:
    """The CSV column for a quantity, per source and half. Raises rather than guessing."""
    try:
        return COLUMNS[(kind, half)][quantity]
    except KeyError:
        known = sorted(COLUMNS.get((kind, half), {}))
        raise Refused(f"{kind} has no {quantity!r} column for {half.value} "
                      f"(it has: {', '.join(known) or 'nothing'})") from None


# ---------------------------------------------------------------- windows

@dataclass(frozen=True)
class Window:
    """A closed tick range [t0, t1]."""

    t0: int
    t1: int

    def __post_init__(self):
        if self.t1 < self.t0:
            raise ValueError(f"empty window {self.t0}..{self.t1}")

    def ticks(self) -> range:
        return range(self.t0, self.t1 + 1)

    def __len__(self) -> int:
        return self.t1 - self.t0 + 1

    def __str__(self) -> str:
        return f"{self.t0}..{self.t1}"


def overlap(*tables: Table) -> Window:
    """The window every table covers. Refuses if they do not overlap."""
    lo = max(t.span()[0] for t in tables)
    hi = min(t.span()[1] for t in tables)
    if hi < lo:
        spans = ", ".join(f"{t.path.name} {t.span()[0]}..{t.span()[1]}"
                          for t in tables)
        raise Incomplete(f"no overlapping ticks ({spans})")
    return Window(lo, hi)


def check_coverage(table: Table, window: Window) -> None:
    """Refuse a table that does not cover the whole window.

    Truncation and holes are reported apart, because they come from different
    mistakes: a truncation is a producer that stopped (a dead replay, a pipe
    closed by `Select-Object -First n`), a hole is a row that was dropped.
    """
    lo, hi = table.span()
    if lo > window.t0 or hi < window.t1:
        n = sum(1 for t in window.ticks() if t in table.rows)
        raise Incomplete(
            f"{table.path.name} covers {lo}..{hi}, the window is {window} "
            f"-- {n}/{len(window)} ticks present. The series stops before the "
            f"window it is being read over")
    missing = [t for t in window.ticks() if t not in table.rows]
    if missing:
        raise Incomplete(
            f"{table.path.name}: {len(missing)} of {len(window)} ticks missing "
            f"in {window} (first {missing[0]}, last {missing[-1]})")


# ---------------------------------------------------------------- conditions

@dataclass(frozen=True)
class Conditions:
    """What the data says the ticks were taken under. Read, never assumed."""

    values: dict[str, tuple[str, ...]]
    window: Window
    half: Half
    source: Path

    def single(self, name: str) -> str:
        v = self.values[name]
        if len(v) != 1:
            raise ConditionVaries(
                f"{name} is not constant over {self.window} ({self.half.value}): "
                f"{', '.join(v)}")
        return v[0]

    @property
    def varying(self) -> tuple[str, ...]:
        return tuple(k for k, v in self.values.items() if len(v) > 1)

    def require(self, **expected) -> "Conditions":
        """Refuse unless the data agrees with what the caller believes.

        This is the guard against a filter carried over from another site: the
        caller writes vsize=1.0/speed=1.1 out of habit and the ticks in front
        of it are vsize 0.6 / speed 0.9. The message prints both sides.
        """
        bad = []
        for name, want in expected.items():
            if name not in self.values:
                bad.append(f"{name}: not reported by {self.source.name}")
                continue
            got = self.values[name]
            if not any(_same(g, want) for g in got):
                bad.append(f"{name}: data says {', '.join(got)}, caller said {want!r}")
            elif len(got) > 1:
                bad.append(f"{name}: not constant over {self.window} "
                           f"({', '.join(got)}), so {want!r} describes only part of it")
        if bad:
            raise ConditionMismatch(
                f"{self.source.name} {self.window} ({self.half.value}): "
                + "; ".join(bad))
        return self

    def __str__(self) -> str:
        return " ".join(f"{k}={'/'.join(v)}" for k, v in sorted(self.values.items()))


def _same(got: str, want) -> bool:
    """Compare a CSV cell against a caller's value: numbers loosely, text exactly.

    GD stores float32 as `0.600000024` and `1.10000002`; a caller writes 0.6
    and 1.1. Those are the same condition. `cube` and `ship` are not numbers
    and are compared as written.
    """
    if isinstance(want, str):
        try:
            wf = float(want)
        except ValueError:
            return got.strip() == want
    else:
        wf = float(want)
    try:
        gf = float(got)
    except ValueError:
        return False
    return math.isclose(gf, wf, rel_tol=1e-6, abs_tol=1e-9)


def conditions_of(gd: Table, window: Window, half: Half = Half.P1,
                  names=CONDITIONS) -> Conditions:
    """Read the conditions off the GD dump for exactly these ticks."""
    if gd.kind != GD:
        raise Refused(f"conditions come from the GD dump, not from "
                      f"{gd.path.name} ({gd.kind})")
    check_coverage(gd, window)
    out: dict[str, tuple[str, ...]] = {}
    for name in names:
        try:
            col = column(GD, half, name)
        except Refused:
            continue
        if col not in gd.columns:
            continue
        seen: list[str] = []
        for t in window.ticks():
            v = gd.rows[t].get(col, "")
            if v not in seen:
                seen.append(v)
        out[name] = tuple(seen)
    return Conditions(values=out, window=window, half=half, source=gd.path)


# ---------------------------------------------------------------- series

@dataclass(frozen=True)
class Series:
    """One quantity, one half, one window, raw values, with its provenance."""

    quantity: str
    half: Half
    window: Window
    values: dict[int, float]
    provenance: Provenance

    @property
    def kind(self) -> str:
        return self.provenance.kind

    def deltas(self) -> "Series":
        """One-tick differences, over the window shortened by its first tick.

        This is what `dy` and `dvy` mean: the transition of a tick, not the
        state at it. Keeping it as a Series means the half and the provenance
        travel with it.
        """
        w = Window(self.window.t0 + 1, self.window.t1)
        vals = {t: self.values[t] - self.values[t - 1] for t in w.ticks()}
        return Series(quantity=f"d{self.quantity}", half=self.half, window=w,
                      values=vals, provenance=self.provenance)


def series(table: Table, quantity: str, half: Half = Half.P1,
           window: Window | None = None) -> Series:
    """Pull one quantity for one half over a window, refusing an uncovered window."""
    if window is None:
        window = Window(*table.span())
    check_coverage(table, window)
    col = column(table.kind, half, quantity)
    if col not in table.columns:
        raise Refused(f"{table.path.name} has no {col!r} column "
                      f"({quantity} for {half.value})")
    vals: dict[int, float] = {}
    for t in window.ticks():
        raw = table.rows[t].get(col, "")
        try:
            vals[t] = float(raw)
        except (TypeError, ValueError):
            raise Incomplete(f"{table.path.name} t={t}: {col}={raw!r} is not a "
                             f"number") from None
    return Series(quantity=quantity, half=half, window=window, values=vals,
                  provenance=table.provenance)


# ---------------------------------------------------------------- differences

@dataclass(frozen=True)
class Difference:
    """A per-tick difference that knows its direction, its half and its terms.

    `values` are raw. Round them in `format_difference` and nowhere else.
    """

    direction: Direction
    quantity: str
    half: Half
    window: Window
    values: dict[int, float]
    gd: Provenance
    model: Provenance
    conditions: Conditions | None = None

    def at(self, tick: int) -> float:
        return self.values[tick]

    def flip(self) -> "Difference":
        """The same difference the other way round, renamed as it is negated."""
        return Difference(direction=self.direction.flipped(),
                          quantity=self.quantity, half=self.half,
                          window=self.window,
                          values={t: -v for t, v in self.values.items()},
                          gd=self.gd, model=self.model,
                          conditions=self.conditions)

    def as_direction(self, direction: Direction) -> "Difference":
        return self if direction is self.direction else self.flip()

    def first_beyond(self, tol: float) -> int | None:
        """First tick whose magnitude exceeds tol, or None.

        Past this tick the two runs are on different world-lines and further
        per-tick differences are between two different worlds -- see the
        module docstring on what is NOT covered.
        """
        for t in self.window.ticks():
            if abs(self.values[t]) > tol:
                return t
        return None

    def __str__(self) -> str:
        return (f"{self.quantity} {self.direction.value} {self.half.value} "
                f"{self.window} n={len(self.values)}")


def difference(a: Series, b: Series, direction: Direction = GD_MINUS_MODEL,
               conditions: Conditions | None = None) -> Difference:
    """Subtract two series, refusing every pairing that is not comparable.

    Refused: different halves, two series from the same side, different
    quantities, different windows.
    """
    if a.half is not b.half:
        raise HalfMismatch(
            f"{a.provenance.path.name}:{a.quantity} is {a.half.value} and "
            f"{b.provenance.path.name}:{b.quantity} is {b.half.value}. A "
            f"comparison across halves is not a divergence, it is two bodies")
    if {a.kind, b.kind} != {GD, MODEL}:
        raise Refused(f"a difference is one GD series against one model series, "
                      f"not {a.kind} against {b.kind}")
    if a.quantity != b.quantity:
        raise Refused(f"different quantities: {a.quantity} vs {b.quantity}")
    if a.window != b.window:
        raise Refused(f"different windows: {a.window} vs {b.window}")
    gd, model = (a, b) if a.kind == GD else (b, a)
    if direction is GD_MINUS_MODEL:
        vals = {t: gd.values[t] - model.values[t] for t in a.window.ticks()}
    else:
        vals = {t: model.values[t] - gd.values[t] for t in a.window.ticks()}
    return Difference(direction=direction, quantity=a.quantity, half=a.half,
                      window=a.window, values=vals, gd=gd.provenance,
                      model=model.provenance, conditions=conditions)


def compare(gd: Table, model: Table, quantity: str, *,
            window: Window | None = None, half: Half = Half.P1,
            direction: Direction = GD_MINUS_MODEL,
            expect: dict | None = None,
            deltas: bool = False,
            require_flags: tuple[str, ...] = ()) -> Difference:
    """The whole guard in one call.

    window   None means "the ticks both sides cover", and that overlap is
             reported on the result. Anything else is a request, and a side
             that stops short of it is a refusal -- which is the difference
             between noticing a dead replay and averaging over it.
    expect   what the caller believes the conditions are, checked against the
             dump for exactly these ticks.
    deltas   compare the one-tick transitions instead of the states.
    """
    if gd.kind != GD or model.kind != MODEL:
        raise Refused(f"compare(gd, model, ...) got kinds "
                      f"{gd.kind!r} and {model.kind!r}")
    if require_flags:
        require_replay_flags(model, require_flags)
    win = window if window is not None else overlap(gd, model)
    a, b = series(gd, quantity, half, win), series(model, quantity, half, win)
    if deltas:
        a, b = a.deltas(), b.deltas()
    cond = conditions_of(gd, a.window, half)
    if expect:
        cond.require(**expect)
    return difference(a, b, direction=direction, conditions=cond)


# ---------------------------------------------------------------- display

def format_difference(diff: Difference, ticks=None, decimals: int = 6) -> list[str]:
    """Render for eyes. The rounding happens HERE and nowhere upstream.

    Seven one-tick steps of a moving surface on lv19 span 0.250579..0.250611 and
    every one of them renders as 0.251 at three decimals. A table like that
    read back as data says the steps are identical, and they are not. Read
    `Difference.values` when a number is going to be used; read this when a
    number is going to be looked at.
    """
    ts = list(ticks) if ticks is not None else list(diff.window.ticks())
    head = (f"{diff.quantity} ({diff.direction.value}, {diff.half.value}) "
            f"{diff.window}  gd={diff.gd.path.name} model={diff.model.path.name}")
    if diff.conditions is not None:
        head += f"\n  conditions: {diff.conditions}"
    out = [head]
    for t in ts:
        out.append(f"  t={t:<8}{diff.values[t]:+.{decimals}f}")
    return out
