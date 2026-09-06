# -*- coding: utf-8 -*-
"""Comparisons that carry their own conditions.

Two numbers that look comparable often are not, and nothing in a float says
so. Over one day nine conclusions in this project were wrong for that reason
alone -- a sign read backwards, a p2 value held against a p1 value, a filter
carried over from a site with a different vsize, a rounded table read in place
of the raw column, a truncated pipe read as the corpus, a hand-built table
missing a tick, a mover level replayed without --groups. This module makes the
common ones refuse rather than answer.

A TENTH, on 2026-09-06, is a family of its own, and it is why `Observation`,
`contrast` and the `offset` arguments below exist. A comment block in
dp/src/dp/slopes.hpp contrasted GD's player y 590.506836 with the model's
585.000 as though the two were one moment:

    GD's   590.506836  is GD's player y at t=9,241, the tick GD grounds.
    model's 585.000    is the model's player y at t=9,243, the tick the MODEL
                       grounds.

Both numbers are real, both are the player's y, both were transcribed
correctly, and both sides are lv16's p1 under the same conditions -- so every
guard listed below passes and the comparison is still wrong. It compares two
different moments. Nothing about the way it was written obliged anyone to say
WHEN either number was taken, so nobody noticed that the two answers were to
two different questions. (6a3e5f8 documents it at the site; the numbers are in
build/fidelity's fid_lv16.dump.csv and fid_lv16.trace.csv, and test_compare's
case 10 reads them back out.)

The fix is not to forbid a cross-tick comparison. "The player rect is one tick
ahead" is a real relationship here, and so is "GD grounds two ticks before the
model" -- that second one is the actual finding at this site. The fix is that
the offset must be STATED, with the relationship it stands for, instead of
being implied by two numbers sitting next to each other.

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
    time          A comparison of two single readings is a `contrast` of two
                  `Observation`s, and an Observation cannot be built without
                  its tick -- there is no default. `observe()` fetches the
                  value BY the tick, so the tick is the key the number came
                  out of and not a label written beside a number from
                  somewhere else. Two ticks that differ is a refusal
                  (`TickMismatch`) unless the caller declares the offset and
                  names the relationship: `offset=+2, because="GD grounds two
                  ticks before the model here"`. A declared offset travels in
                  everything the result prints, and an offset the observations
                  do not actually have is refused as well, so the declaration
                  cannot be a rubber stamp. Series comparisons take the same
                  two arguments. `verdict()` tells "compared at the same tick
                  and agreed" apart from `None` -- never compared -- the way
                  `Provenance.argv is None` is unknown rather than guilty.
    conditions    mode / vsize / speed / gravity are read FROM THE DUMP for
                  the ticks in question. A caller may state what it believes
                  they are; if the data disagrees that is a refusal, not a
                  warning, and the message prints both.
    completeness  A window is covered or it is not. A missing tick, a series
                  that stops before the requested window, a repeated tick --
                  each is a refusal naming what is missing. Silently
                  intersecting two tick sets is how a 4.6% replay passes for a
                  whole level.
    provenance    A model trace carries the argv it was produced with, the
                  binary that produced it and the identity of every input file
                  that binary read (`write_provenance`, called by the
                  producer). `require_replay_flags` refuses one that is missing
                  the flags a level needs. An unrecorded provenance is a
                  refusal too, with a different message: not knowing is not the
                  same as knowing it was fine, and a trace written before this
                  existed must not read as guilty.

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

    A declared offset is not evidence either. `offset=+2` is checked against
    the ticks the observations actually carry, and `because=` is checked for
    being non-empty -- nothing here can check that the mechanism named is the
    mechanism at work. What the declaration buys is that a cross-tick reading
    is visible as one, in the call and in the output, to whoever reads it next.
"""
from __future__ import annotations

import csv
import hashlib
import json
import math
import os
import time
from dataclasses import dataclass, field
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


class TickMismatch(Refused):
    """Two readings from different ticks, without a declaration saying so.

    Also raised when a declaration is made and is not true of the readings:
    the wrong offset, or an offset with no relationship named. The refusal
    exists because the two halves of a contrast can both be correct and still
    answer different questions -- see the module docstring's tenth case.
    """


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
#
# PHASE. This table pairs a GD column with a model column; it does not say the
# two are written at the same point in the tick, and until 2026-09-06 nothing
# did. Two columns had been caught late -- the dump's `onSlope` is phase-blind
# because checkCollisions zeroes it at the top of every tick, and `onGround`
# was read as phase-offset from `grounded` on the strength of a per-mode
# breakdown. That second one was wrong, and how it was wrong is the reusable
# part:
#
#   `A is already 1 while B goes 0->1` reads just as naturally as "A fires
#   early" as it does as "A is looser", and ONE SHIFT SWEEP DECIDES between
#   them. Disagreement concentrated in particular modes is NOT evidence for
#   the offset reading -- a mode-dependent predicate WIDTH produces exactly
#   the same shape, and here it was the width.
#
# What was measured on 2026-09-06, over 379,480 ticks inside zero-divergence
# windows (|dx|,|dy|,|dvy| <= 0.001 across +/-10 ticks, so the two sides are on
# one trajectory and a residual column difference can only be a write position
# or a difference of meaning), leveldp 19:41 f726579f against gdref and
# build/fidelity. "Aligned" below means the shift scan put d=0 at a strict
# minimum; where a residual is named it survives every shift and is therefore
# about what the columns MEAN:
#
#   y / vy / x        aligned, 0 disagreeing ticks at d=0 in all 16 regimes
#                     (this is the window's own definition -- it is the proof
#                     that the check CAN return "aligned", not an independent
#                     result)
#   mode              aligned, 0 at d=0; all 111 transitions at delta 0
#   gravity           aligned, 0 at d=0; all 649 transitions at delta 0. NOTE
#                     the gframe=3 inversion has to be applied first (the same
#                     statement `gdUpOf` makes) or every frame-3 tick reads as
#                     a mismatch
#   vsize             aligned in 12 regimes, no power in 4 (the column never
#                     moves there); all 49 transitions at delta 0
#   dual              aligned in 5, no power in 11
#   ground            aligned, and the residual is a DIFFERENT PREDICATE. d=0
#                     is the minimum in every regime and no shift buys back
#                     what is left. The census that first reported offsets in
#                     ship/ufo/wave/spider re-ran the question as a shift sweep
#                     and retracted it: of its own disagreements, 682 sit at
#                     d=0, none at any other shift, and in the remaining 48
#                     GD's flag is already 1 and stays 1. GD calls the player
#                     grounded in states the model does not -- see
#                     gdtas.solveutil.grounded_of, which is the conversion.
#   rot               aligned for cube/ship/wave/robot (d=0 a dramatic
#                     minimum). The huge residual in ball/ufo/swing is the
#                     known unimplemented player rotation, not a phase fact
#   speed             UNVERIFIED: 8 of 16 regimes have no power and the rest
#                     carry a residual the shift cannot touch. `speed` and the
#                     model's `dx` are related by a mapping that is not pinned,
#                     so phase and mapping cannot be told apart here
#
# The blind spot of all of the above: a zero-divergence window only exists
# where the model is already right. lv20 contributes 37% of its ticks, lv22
# 51%, lv19 51%. A phase defect that is itself a cause of divergence is
# precisely what this cannot witness. Details and the per-regime tables are in
# the lab note measure-dump-column-phases-2026-09-06.
COLUMNS: dict[tuple[str, Half], dict[str, str]] = {
    (GD, Half.P1): {"y": "y", "vy": "yvel", "x": "x", "mode": "mode",
                    "vsize": "vsize", "speed": "speed", "gravity": "upsideDown",
                    "ground": "onGround", "dual": "dual", "rot": "rot"},
    # No `rot` for p2 on either side: neither the dump nor the trace carries
    # the partner's sprite angle. Absent here means `column()` raises and the
    # caller finds out at the door, which is the point of the table.
    # THREE OF THESE ARE NOT IN gdref. quick_regress.REF_COLS trims the dump to
    # 24 columns on the way into data/gdref, and `p2mode`, `p2vsize` and `p2x`
    # are not among them -- so a comparison whose GD side is a gdref row cannot
    # reach them at all, whatever the mod emits. Only build/fidelity's
    # fid_lv*.dump.csv (the raw 37-column dump) carries them. Measured
    # 2026-09-06: eligible=0 for all three over 1,116 anchored sections, and
    # 2,331 ticks each on the whole-run dumps, where p2mode and p2vsize never
    # move (so they are unverified, not verified) and p2x is aligned at d=0
    # with all 2,331 ticks disagreeing at every other shift.
    (GD, Half.P2): {"y": "p2y", "vy": "p2vy", "x": "p2x", "mode": "p2mode",
                    "vsize": "p2vsize", "speed": "speed", "gravity": "p2up",
                    "ground": "p2ground", "dual": "dual"},
    # `rot` is raw and the two sides use different branches of the circle (GD's
    # lv20 column runs to -418 where the model's runs to +135), so subtracting
    # them is meaningless without reducing mod 90 first -- see gdtas.padgate.
    (MODEL, Half.P1): {"y": "y", "vy": "vy", "x": "x", "mode": "mode",
                       "ground": "grounded", "act": "act", "dual": "dual",
                       "rot": "rot", "rotneg": "rotneg"},
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
# it is wrong at an anchor. See fidelity_diff.whole_run_args.
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

# The sidecar a producer writes beside its output: `<output>.prov.json`.
#
# The name of a plain-text `<output>.argv.txt` was written down here first and
# never written by anything. It is still read (LEGACY_SUFFIX below), but a line
# per argument cannot carry the other two thirds of "how was this made" -- which
# binary, and which input files -- so the recorded form is JSON. The repository
# already keeps its structured records that way (data/cold_baseline.json,
# gdref/cut.json, worker.json) and already names companion files
# `<base>.<suffix>` (.trace.csv, .groups.txt, .fixups.txt), so this invents
# neither half of the convention.
PROV_SUFFIX = ".prov.json"
LEGACY_SUFFIX = ".argv.txt"
PROV_SCHEMA = "gdsolver.provenance/1"

_DIGEST_CACHE: dict[tuple[str, int, int], str] = {}


@dataclass(frozen=True)
class Provenance:
    """How a table came to exist: the file, what wrote it, and with which argv.

    `argv` is None when nobody recorded it. That is not the same as "no flags
    were needed": `require_replay_flags` refuses an unrecorded provenance,
    because the whole failure being guarded against is invisible in the data.
    `record` is the whole sidecar when there was one -- the binary, the inputs,
    and `stale` when it describes a file that has since been rewritten.
    """

    path: Path
    kind: str
    argv: tuple[str, ...] | None = None
    record: dict | None = field(default=None, compare=False)

    def has_flag(self, flag: str) -> bool:
        return self.argv is not None and flag in self.argv

    @property
    def binary(self) -> dict | None:
        return (self.record or {}).get("binary")

    @property
    def inputs(self) -> tuple[dict, ...]:
        return tuple((self.record or {}).get("inputs") or ())

    @property
    def stale(self) -> str | None:
        return (self.record or {}).get("stale")


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _cached_sha256(path: Path) -> str:
    """Digest keyed on (path, size, mtime_ns). The binary is hashed once a build."""
    st = path.stat()
    key = (str(path), st.st_size, st.st_mtime_ns)
    if key not in _DIGEST_CACHE:
        _DIGEST_CACHE[key] = _sha256(path)
    return _DIGEST_CACHE[key]


def _is_file(s: str) -> bool:
    """Is this argv element the name of a file? Never raises.

    An argv carries values as well as paths (`--start` is 26 comma-separated
    fields, `--startband` a pair). Asking the filesystem about those must not
    take the run down, so every way a path can be malformed answers "no".
    """
    try:
        return Path(s).is_file()
    except (OSError, ValueError):
        return False


def file_identity(path: Path | str, *, digest: bool = False) -> dict:
    """path / size / mtime of one file, and its sha256 when asked.

    Size and mtime identify an input well enough to notice a stale dump; the
    binary gets the digest as well, because "which exe was measured" has been
    wrong here three times and mtime alone does not survive a rebuild that
    produces the same bytes.
    """
    p = Path(path)
    if not _is_file(str(p)):
        # named but not there. Recorded as such: "the exe this ran with is
        # gone" is a fact about the run, and better than no line at all.
        return {"path": str(p), "size": None, "mtime": None, "missing": True}
    st = p.stat()
    out = {"path": str(p), "size": st.st_size,
           "mtime": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(st.st_mtime)),
           "mtime_epoch": round(st.st_mtime, 3)}
    if digest:
        out["sha256"] = _cached_sha256(p)
    return out


def write_provenance(output: Path | str, argv, *, produced_by: str,
                     binary: Path | str | None = None, inputs=None,
                     extra: dict | None = None) -> Path:
    """Record how `output` was made, in `<output>.prov.json`. Returns the sidecar.

    argv        the command line as it was actually run, argv[0] first. Not
                "the flags this producer usually passes" -- the list handed to
                subprocess, so that a flag added by a condition is recorded by
                the condition having been true.
    binary      defaults to argv[0]; recorded with its digest.
    inputs      defaults to every later argv element that names an existing
                file. Derived rather than listed, so a producer that grows a
                new input records it without this call being edited.
    extra       whatever the producer knows and argv does not (exit code, the
                level, the tick a replay died at).

    Raises OSError if the sidecar cannot be written; a producer that would
    rather lose the attribution than the run catches it and says so.
    """
    out = Path(output)
    argv = [str(a) for a in argv]
    if binary is None and argv and _is_file(argv[0]):
        binary = argv[0]
    if inputs is None:
        seen, inputs = set(), []
        for a in argv[1:]:
            if a in seen or a.startswith("-"):
                continue
            if _is_file(a):
                seen.add(a)
                inputs.append(Path(a))
    rec = {
        "schema": PROV_SCHEMA,
        "produced_by": produced_by,
        "produced_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "argv": argv,
        "binary": file_identity(binary, digest=True) if binary else None,
        "inputs": [file_identity(p) for p in inputs],
        "output": file_identity(out, digest=True) if out.exists() else None,
    }
    if extra:
        rec["extra"] = extra
    sidecar = Path(str(out) + PROV_SUFFIX)
    sidecar.write_text(json.dumps(rec, indent=1), encoding="utf-8")
    return sidecar


def read_argv_sidecar(path: Path | str) -> tuple[str, ...] | None:
    """Read the legacy `<path>.argv.txt` (one argument per line), if one exists.

    Superseded by `<path>.prov.json`; kept because it was the published name
    and a hand-written one should keep working.
    """
    p = Path(str(path) + LEGACY_SUFFIX)
    if not p.exists():
        return None
    return tuple(l for l in p.read_text(encoding="utf-8").splitlines() if l)


def _stale_reason(rec: dict, out: Path) -> str | None:
    """Why this sidecar does not describe the file it sits beside, or None.

    A sidecar outlives its output: the producer is run again with different
    flags, or by hand, and the .json from the run before is still there
    claiming the flags of a run that no longer exists. That is worse than no
    provenance, so it is refused rather than believed.
    """
    o = rec.get("output")
    if not out.exists():
        return None                      # nothing to contradict
    if not isinstance(o, dict):
        # written for a run that produced no output (the exe failed), and a
        # file has since appeared under that name. The argv in it is somebody
        # else's.
        return "the sidecar was written for a run that produced no output"
    size = out.stat().st_size
    if o.get("size") is not None and o["size"] != size:
        return f"the sidecar was written for {o['size']} bytes, the file is {size}"
    want = o.get("sha256")
    if want and want != _sha256(out):
        return "the file has been rewritten since the sidecar was written"
    return None


def read_provenance(path: Path | str, kind: str,
                    argv: tuple[str, ...] | None = None) -> Provenance:
    """Provenance for one output: the caller's argv, else the sidecar, else none.

    A stale sidecar yields `argv=None` -- unknown, not trusted -- and the
    reason travels in `record["stale"]` so the refusal can say which of the
    two it is.
    """
    p = Path(path)
    if argv is not None:
        return Provenance(p, kind, tuple(argv))
    sidecar = Path(str(p) + PROV_SUFFIX)
    if sidecar.exists():
        try:
            rec = json.loads(sidecar.read_text(encoding="utf-8"))
        except (OSError, ValueError) as e:
            return Provenance(p, kind, None, {"stale": f"unreadable sidecar: {e}"})
        stale = _stale_reason(rec, p)
        if stale:
            return Provenance(p, kind, None, dict(rec, stale=stale))
        got = rec.get("argv")
        return Provenance(p, kind, tuple(got) if got is not None else None, rec)
    return Provenance(p, kind, read_argv_sidecar(p))


def require_replay_flags(table: "Table", flags=MOVING_GEOMETRY_FLAGS) -> None:
    """Refuse a model trace produced without the flags this comparison needs.

    Three outcomes, and the difference between the last two is the whole
    reason the sidecar exists: the flags are there; the argv says one is
    missing (name it); or nobody recorded the argv, which is a refusal with a
    different message, because a trace from before this existed is unknown,
    not guilty.
    """
    prov = table.provenance
    if prov.argv is None:
        why = (f"the sidecar does not describe this file ({prov.stale})"
               if prov.stale else
               "the argv it was replayed with was not recorded")
        raise ProvenanceMissing(
            f"{prov.path.name}: {why}, so it cannot be shown to carry "
            f"{', '.join(flags)}. A run missing one of those dies early and "
            f"looks byte-identical as far as it got")
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
    return Table(rows=rows, columns=cols,
                 provenance=read_provenance(p, kind, argv))


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


# ---------------------------------------------------------------- observations

@dataclass(frozen=True)
class Observation:
    """One number, at ONE tick, from one side, for one half.

    `tick` has no default and no fallback. That is the entire guard: two floats
    can be written next to each other in a sentence, and an Observation cannot
    be written at all without saying when it was taken. Build them with
    `observe()` wherever a table is at hand -- there the tick is the key the
    value was fetched with, so the two cannot come apart.
    """

    kind: str
    quantity: str
    half: Half
    tick: int
    value: float
    provenance: Provenance | None = None

    def __post_init__(self):
        if self.kind not in (GD, MODEL):
            raise ValueError(f"kind must be {GD!r} or {MODEL!r}, not {self.kind!r}")

    @property
    def source(self) -> str:
        """The file this came out of, or a name saying that nobody recorded one."""
        return self.provenance.path.name if self.provenance else "(source unrecorded)"

    def __str__(self) -> str:
        return (f"{self.kind} {self.quantity} {self.half.value} "
                f"t={self.tick} = {self.value!r} ({self.source})")


def observe(table: Table, quantity: str, tick: int,
            half: Half = Half.P1) -> Observation:
    """One quantity, at one tick, read out of a table BY that tick.

    A missing tick is a refusal rather than a neighbouring row: reading "the
    nearest tick we have" is the same defect this module exists for, arrived at
    from the other end.
    """
    if tick not in table.rows:
        lo, hi = table.span()
        raise Incomplete(f"{table.path.name} has no tick {tick} "
                         f"(it covers {lo}..{hi})")
    col = column(table.kind, half, quantity)
    if col not in table.columns:
        raise Refused(f"{table.path.name} has no {col!r} column "
                      f"({quantity} for {half.value})")
    raw = table.rows[tick].get(col, "")
    try:
        value = float(raw)
    except (TypeError, ValueError):
        raise Incomplete(f"{table.path.name} t={tick}: {col}={raw!r} is not a "
                         f"number") from None
    return Observation(kind=table.kind, quantity=quantity, half=half, tick=tick,
                       value=value, provenance=table.provenance)


@dataclass(frozen=True)
class Contrast:
    """Two observations held against each other, carrying BOTH of their ticks.

    `offset` is model tick minus GD tick, and it is a property of when the two
    readings were taken -- not of which way they are subtracted -- so `flip()`
    negates the value and leaves the offset and its reason alone.
    """

    direction: Direction
    quantity: str
    half: Half
    gd: Observation
    model: Observation
    offset: int
    because: str
    conditions: Conditions | None = None

    @property
    def value(self) -> float:
        """The difference, raw, the way `direction` names it."""
        return (self.gd.value - self.model.value
                if self.direction is GD_MINUS_MODEL
                else self.model.value - self.gd.value)

    @property
    def same_tick(self) -> bool:
        return self.offset == 0

    @property
    def alignment(self) -> str:
        """How the two readings line up in time, in words, always printed."""
        if self.offset == 0:
            return f"same tick t={self.gd.tick}"
        n = abs(self.offset)
        return (f"gd t={self.gd.tick} vs model t={self.model.tick} -- "
                f"offset {self.offset:+d} ticks (model {n} tick"
                f"{'' if n == 1 else 's'} "
                f"{'after' if self.offset > 0 else 'before'} gd), declared")

    def agrees(self, tol: float) -> bool:
        return abs(self.value) <= tol

    def flip(self) -> "Contrast":
        return Contrast(direction=self.direction.flipped(),
                        quantity=self.quantity, half=self.half, gd=self.gd,
                        model=self.model, offset=self.offset,
                        because=self.because, conditions=self.conditions)

    def as_direction(self, direction: Direction) -> "Contrast":
        return self if direction is self.direction else self.flip()

    def __str__(self) -> str:
        return (f"{self.quantity} {self.direction.value} {self.half.value} "
                f"{self.alignment}")


def contrast(a: Observation, b: Observation, *,
             direction: Direction = GD_MINUS_MODEL,
             offset: int | None = None,
             because: str = "",
             conditions: Conditions | None = None) -> Contrast:
    """Hold one GD reading against one model reading, ticks and all.

    The same-tick call is the short one and stays short:

        contrast(observe(gd, "y", 9243), observe(model, "y", 9243))

    Different ticks refuse unless the caller says so, and says what for:

        contrast(observe(gd, "y", 9241), observe(model, "y", 9243),
                 offset=+2, because="GD grounds two ticks before the model")

    offset   model tick minus GD tick. Refused if it is not the offset the two
             observations actually have -- a declaration that cannot be wrong
             is not a declaration.
    because  required when the offset is not zero. An offset with no mechanism
             behind it is a number picked so that two readings would meet,
             which is the defect this guards against wearing a hat.
    """
    if a.half is not b.half:
        raise HalfMismatch(
            f"{a.source}:{a.quantity} is {a.half.value} and "
            f"{b.source}:{b.quantity} is {b.half.value}. A comparison across "
            f"halves is not a divergence, it is two bodies")
    if {a.kind, b.kind} != {GD, MODEL}:
        raise Refused(f"a contrast is one GD reading against one model reading, "
                      f"not {a.kind} against {b.kind}")
    if a.quantity != b.quantity:
        raise Refused(f"different quantities: {a.quantity} vs {b.quantity}")
    gd, model = (a, b) if a.kind == GD else (b, a)
    actual = model.tick - gd.tick
    if offset is None:
        if actual != 0:
            raise TickMismatch(
                f"{gd.quantity}: gd t={gd.tick} ({gd.source}) against model "
                f"t={model.tick} ({model.source}) -- {abs(actual)} ticks apart. "
                f"Both readings can be right and still answer different "
                f"questions. If the offset is the point, say so: "
                f"offset={actual:+d}, because=\"...\"")
        offset = 0
    elif offset != actual:
        raise TickMismatch(
            f"{gd.quantity}: declared offset {offset:+d}, but gd t={gd.tick} "
            f"and model t={model.tick} are {actual:+d} apart")
    if offset != 0 and not because.strip():
        raise TickMismatch(
            f"{gd.quantity}: offset {offset:+d} between gd t={gd.tick} and "
            f"model t={model.tick} needs `because=` naming the relationship "
            f"(\"the player rect is one tick ahead\", \"GD grounds two ticks "
            f"before the model\"). An offset with nothing behind it is a "
            f"number chosen to make two readings meet")
    return Contrast(direction=direction, quantity=gd.quantity, half=gd.half,
                    gd=gd, model=model, offset=offset, because=because.strip(),
                    conditions=conditions)


NEVER_COMPARED = "never compared"


def verdict(c: Contrast | None, tol: float) -> str:
    """One line saying what is actually known about two numbers.

    `None` is not "they agree" and not "they differ": it is that nothing was
    ever put through this. That is the state the slopes.hpp block was in --
    two numbers side by side and no comparison behind them -- and it has to be
    sayable, the way `Provenance.argv is None` says unknown rather than guilty.

    Rounded, therefore for eyes. Decide on `Contrast.value`.
    """
    if c is None:
        return NEVER_COMPARED
    verb = "agree" if c.agrees(tol) else "DIFFER"
    line = (f"{c.quantity} {c.half.value} {c.alignment}: {verb} "
            f"({c.direction.value} {c.value:+.6f}, tol {tol:g})")
    if c.because:
        line += f" -- {c.because}"
    return line


def format_contrast(c: Contrast, decimals: int = 6) -> list[str]:
    """Render for eyes; both ticks and any offset are always in the output.

    The rounding happens HERE, as it does in `format_difference`, and the
    values above it stay raw.
    """
    out = [f"{c.quantity} ({c.direction.value}, {c.half.value})  {c.alignment}"]
    if c.because:
        out.append(f"  because: {c.because}")
    out.append(f"  gd     t={c.gd.tick:<8}{c.gd.value:.{decimals}f}   {c.gd.source}")
    out.append(f"  model  t={c.model.tick:<8}{c.model.value:.{decimals}f}   "
               f"{c.model.source}")
    out.append(f"  {c.direction.value} = {c.value:+.{decimals}f}")
    if c.conditions is not None:
        out.append(f"  conditions: {c.conditions}")
    return out


# ---------------------------------------------------------------- differences

@dataclass(frozen=True)
class Difference:
    """A per-tick difference that knows its direction, its half and its terms.

    `values` are raw. Round them in `format_difference` and nowhere else.

    `window` is always the GD side's ticks. `offset` (model tick minus GD tick)
    is 0 for every ordinary comparison and says, when it is not, which model
    tick each GD tick was read against -- `values[t]` is then GD at t against
    the model at t + offset.
    """

    direction: Direction
    quantity: str
    half: Half
    window: Window
    values: dict[int, float]
    gd: Provenance
    model: Provenance
    conditions: Conditions | None = None
    offset: int = 0
    because: str = ""

    def at(self, tick: int) -> float:
        return self.values[tick]

    @property
    def same_tick(self) -> bool:
        return self.offset == 0

    def flip(self) -> "Difference":
        """The same difference the other way round, renamed as it is negated.

        The offset is when the readings were taken, not which way they are
        subtracted, so it survives the flip unchanged.
        """
        return Difference(direction=self.direction.flipped(),
                          quantity=self.quantity, half=self.half,
                          window=self.window,
                          values={t: -v for t, v in self.values.items()},
                          gd=self.gd, model=self.model,
                          conditions=self.conditions, offset=self.offset,
                          because=self.because)

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
        tail = "" if self.offset == 0 else f" offset {self.offset:+d}"
        return (f"{self.quantity} {self.direction.value} {self.half.value} "
                f"{self.window} n={len(self.values)}{tail}")


def difference(a: Series, b: Series, direction: Direction = GD_MINUS_MODEL,
               conditions: Conditions | None = None, *,
               offset: int | None = None, because: str = "") -> Difference:
    """Subtract two series, refusing every pairing that is not comparable.

    Refused: different halves, two series from the same side, different
    quantities, and windows that do not line up.

    Two windows that are the same length but shifted are the series form of the
    tenth case in the module docstring, and are refused unless the shift is
    declared -- `offset` (model tick minus GD tick) plus `because` naming the
    relationship. The result is keyed on the GD side's ticks: `values[t]` is
    GD at t against the model at t + offset.
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
    gd, model = (a, b) if a.kind == GD else (b, a)
    shift = model.window.t0 - gd.window.t0
    aligned = len(gd.window) == len(model.window)
    if offset is None:
        if a.window != b.window:
            if aligned and shift:
                raise TickMismatch(
                    f"gd {gd.window} against model {model.window}: the same "
                    f"{len(gd.window)} ticks shifted by {shift:+d}. If that "
                    f"shift is the point, declare it: offset={shift:+d}, "
                    f"because=\"...\"")
            raise TickMismatch(f"different windows: {a.window} vs {b.window}")
        offset = 0
    elif not aligned or shift != offset:
        raise TickMismatch(
            f"declared offset {offset:+d}, but gd {gd.window} and model "
            f"{model.window} are {shift:+d} apart over "
            f"{len(gd.window)}/{len(model.window)} ticks")
    if offset != 0 and not because.strip():
        raise TickMismatch(
            f"offset {offset:+d} between gd {gd.window} and model "
            f"{model.window} needs `because=` naming the relationship. An "
            f"offset with nothing behind it is a number chosen to make two "
            f"series meet")
    if direction is GD_MINUS_MODEL:
        vals = {t: gd.values[t] - model.values[t + offset]
                for t in gd.window.ticks()}
    else:
        vals = {t: model.values[t + offset] - gd.values[t]
                for t in gd.window.ticks()}
    return Difference(direction=direction, quantity=a.quantity, half=a.half,
                      window=gd.window, values=vals, gd=gd.provenance,
                      model=model.provenance, conditions=conditions,
                      offset=offset, because=because.strip())


def compare(gd: Table, model: Table, quantity: str, *,
            window: Window | None = None, half: Half = Half.P1,
            direction: Direction = GD_MINUS_MODEL,
            expect: dict | None = None,
            deltas: bool = False,
            require_flags: tuple[str, ...] = (),
            offset: int = 0, because: str = "") -> Difference:
    """The whole guard in one call.

    window   None means "the ticks both sides cover", and that overlap is
             reported on the result. Anything else is a request, and a side
             that stops short of it is a refusal -- which is the difference
             between noticing a dead replay and averaging over it. With an
             offset it names the GD ticks; the model side is read `offset`
             ticks along from each of them.
    expect   what the caller believes the conditions are, checked against the
             dump for exactly these ticks.
    deltas   compare the one-tick transitions instead of the states.
    offset   model tick minus GD tick, 0 for an ordinary comparison. Non-zero
             needs `because` naming the relationship it stands for; the
             conditions are still read off the GD side's own ticks.
    """
    if gd.kind != GD or model.kind != MODEL:
        raise Refused(f"compare(gd, model, ...) got kinds "
                      f"{gd.kind!r} and {model.kind!r}")
    if require_flags:
        require_replay_flags(model, require_flags)
    if offset:
        if window is not None:
            win = window
        else:
            lo = max(gd.span()[0], model.span()[0] - offset)
            hi = min(gd.span()[1], model.span()[1] - offset)
            if hi < lo:
                raise Incomplete(
                    f"no ticks left once the model is shifted by {offset:+d} "
                    f"({gd.path.name} {gd.span()[0]}..{gd.span()[1]}, "
                    f"{model.path.name} {model.span()[0]}..{model.span()[1]})")
            win = Window(lo, hi)
        mwin = Window(win.t0 + offset, win.t1 + offset)
    else:
        win = window if window is not None else overlap(gd, model)
        mwin = win
    a, b = series(gd, quantity, half, win), series(model, quantity, half, mwin)
    if deltas:
        a, b = a.deltas(), b.deltas()
    cond = conditions_of(gd, a.window, half)
    if expect:
        cond.require(**expect)
    return difference(a, b, direction=direction, conditions=cond,
                      offset=offset or None, because=because)


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
    if diff.offset:
        head += (f"\n  offset {diff.offset:+d} ticks (model tick - gd tick): "
                 f"gd {diff.window} against model "
                 f"{Window(diff.window.t0 + diff.offset, diff.window.t1 + diff.offset)}"
                 + (f" -- {diff.because}" if diff.because else ""))
    if diff.conditions is not None:
        head += f"\n  conditions: {diff.conditions}"
    out = [head]
    for t in ts:
        out.append(f"  t={t:<8}{diff.values[t]:+.{decimals}f}")
    return out
