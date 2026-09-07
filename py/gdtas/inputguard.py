# -*- coding: utf-8 -*-
"""REFUSE TO REPORT A RUN WHOSE INPUTS MOVED UNDERNEATH IT.

An anchored instrument (fixcensus, quick_regress, deathref) reads a fixed set
of files -- the level dumps, gdref, the plans and their `.groups*.txt`, the
solver executable -- and replays a few thousand sections against them over
several minutes. NOTHING IN THAT ARRANGEMENT NOTICES A SECOND PROCESS WRITING
THE SAME FILES. It notices even less that some sections were replayed against
the old bytes and the rest against the new ones, because a census of families
looks exactly the same either way: a count, a table, a comparison against the
baseline, all of them plausible.

That is not hypothetical. On 2026-09-06 two sessions ran census-family
instruments against the shared lab tree at once. `fixcensus` and
`quick_regress` BOTH REGENERATE `gdref/lv*.bandtrack.txt`, opening them "w",
and the A/B taken across the overlap reported "one family removed" that could
not be told apart from an artefact and had to be voided. The headline moved
with it, by a single row, and NOTHING ELSE IN THE OUTPUT DIFFERED.

The absolute totals from that day are deliberately not quoted here. They are a
function of the leveldp build and of the state of the lab tree, both of which
move; quoting them turns an incident into a fixture, and the next reader
compares against a number that was never reproducible for them. What is durable
is the shape: one row of difference, and no way to tell it from a real one.

So the guard is bracket-shaped and content-based:

    watch(path)          record the file's identity when the run commits to it
    claim_written(path)  record it AFTER we deliberately wrote it -- a file the
                         run writes on purpose is not contamination
    check()              re-read every identity and name what moved

THE VERDICT IS DECIDED BY CONTENT, NOT BY mtime. mtime alone is both too weak
and too strong: too weak because a rewrite inside one filesystem timestamp
tick, or a writer that restores timestamps, changes the bytes without moving
it; too strong because a bare `touch` moves it while the replays still read
exactly the same file. A sha256 answers the question that actually matters --
"did the second half of the run read the same bytes as the first half".

Size and mtime are still recorded, and a file whose bytes match but whose
(size, mtime_ns) moved is ALSO a refusal, under its own name. That case is not
harmless: it means some other writer truncated and rewrote the file during the
window, and a leveldp that opened it mid-write saw a short file. The bandtrack
collision above is exactly this shape -- both writers produce the same bytes.

Cost, measured on this corpus (2026-09-07, warm cache): the whole candidate
input set is 351 files / 532 MB and hashes in 1.11 s, 478 MB/s; two passes cost
2.2 s against a 275 s fixcensus run, under 1%. A `stat` of the same set is
0.0035 s. The 28 MB `.groups` files are therefore hashed like everything else
-- the saving from a size+mtime shortcut is a rounding error on the run, and
what it would buy back is the one failure mode the guard exists for.

WHAT THE BRACKET CANNOT SEE is a file that changed and changed back inside the
window. Nothing short of holding the files open for the duration would, and
that is not available across processes on Windows without also blocking the
legitimate writer.

The singleton is here because the writing site (`quick_regress.band_track_args`)
is reached through helpers the caller does not construct, the same reason
`quick_regress.NO_SPENTPAD` is module-level: one guard, every tool, one meaning.
An instrument that never installs one gets `None` and pays nothing.
"""
from __future__ import annotations

import hashlib
import threading
from pathlib import Path

# The guard the current run installed, or None. `band_track_args` and anything
# else that writes into a watched tree claims its own writes through this.
GUARD: "InputGuard | None" = None


def _digest(p: Path) -> tuple:
    """(exists, size, mtime_ns, sha256) for one path. Missing files are recorded
    as missing rather than raising: a file that was absent when the run decided
    what to pass and present at the end changed the run's arguments just as
    surely as one whose bytes changed."""
    try:
        st = p.stat()
    except OSError:
        return (False, -1, -1, "")
    h = hashlib.sha256()
    try:
        with p.open("rb") as f:
            for blk in iter(lambda: f.read(1 << 20), b""):
                h.update(blk)
    except OSError:
        return (False, -1, -1, "")
    return (True, st.st_size, st.st_mtime_ns, h.hexdigest())


# The same identity, for the guard that watches the OTHER channel: `runtmp`
# fingerprints the traces a run writes, and the two verdicts have to be taken
# the same way or one of them would be arguing from mtime while the other
# argues from bytes.
digest = _digest


class InputGuard:
    """Record the identity of every file a run depends on, and re-check it."""

    def __init__(self) -> None:
        self._seen: dict[Path, tuple] = {}
        self._kind: dict[Path, str] = {}
        self._lock = threading.Lock()

    def _record(self, path, kind: str) -> None:
        p = Path(path).resolve()
        with self._lock:
            # FIRST OBSERVATION WINS for an input: that is the identity the run
            # committed to. A write we claim replaces it, because the bytes we
            # just put there are what the rest of the run will read.
            if kind == "input" and p in self._seen:
                return
            self._seen[p] = _digest(p)
            self._kind[p] = kind

    def watch(self, path) -> None:
        """Record a file the run READS. Absent files are watched too."""
        self._record(path, "input")

    def watch_many(self, paths) -> None:
        for p in paths:
            self.watch(p)

    def claim_written(self, path) -> None:
        """Record a file the run WROTE, as it stands after our write.

        This is the distinction the guard turns on. `gdref/lv*.bandtrack.txt`
        is regenerated by the run itself every time, so its state before the
        run says nothing; what has to hold is that OUR bytes are still the ones
        there when the last section finishes."""
        self._record(path, "output")

    def check(self) -> list[str]:
        """Re-read every identity. Returns one line per file that moved, empty
        if nothing did."""
        bad: list[str] = []
        with self._lock:
            items = sorted(self._seen.items())
            kinds = dict(self._kind)
        for p, before in items:
            now = _digest(p)
            if now == before:
                continue
            what = "written by this run" if kinds.get(p) == "output" else "input"
            if before[0] and not now[0]:
                bad.append(f"{p}: DISAPPEARED ({what})")
            elif not before[0] and now[0]:
                bad.append(f"{p}: APPEARED during the run ({what}) -- the "
                           f"arguments were built while it was absent")
            elif before[3] != now[3]:
                bad.append(f"{p}: CONTENT CHANGED ({what}) "
                           f"sha256 {before[3][:12]} -> {now[3][:12]}, "
                           f"size {before[1]} -> {now[1]}")
            else:
                # bytes equal, stamp moved: someone truncated and rewrote it
                # while the replays were reading it. Same content at the ends
                # says nothing about the middle.
                bad.append(f"{p}: REWRITTEN during the run ({what}, same "
                           f"bytes) size {before[1]} -> {now[1]}, "
                           f"mtime_ns {before[2]} -> {now[2]}")
        return bad

    def count(self) -> int:
        with self._lock:
            return len(self._seen)


def banner(bad: list[str], tool: str) -> str:
    """The refusal text. Shaped after quick_regress --whole's `NO BASELINE --
    THIS IS NOT A PASS`: a run whose inputs moved must not be able to print
    numbers that read as a result."""
    out = ["",
           "=" * 72,
           f"INPUTS CHANGED UNDER THIS RUN -- {tool.upper()} RESULT WITHHELD",
           "=" * 72,
           f"{len(bad)} file(s) moved between the first section and the last, so "
           "the sections",
           "were not all measured against the same world. The counts this run "
           "would have",
           "printed are not comparable with any baseline and are NOT A RESULT.",
           ""]
    out += [f"  {b}" for b in bad]
    out += ["",
            "Re-run in a data root nothing else is writing (GDSOLVER_LAB, and a "
            "private",
            "--tmp), one instrument at a time.",
            "=" * 72]
    return "\n".join(out)


def install() -> InputGuard:
    """Create the run's guard and publish it as the singleton."""
    global GUARD
    GUARD = InputGuard()
    return GUARD


def claim(path) -> None:
    """Claim a write against whatever guard is installed, if any.

    A tool with no guard installed pays nothing, which is what keeps this
    callable from the shared helpers without every caller having to opt in.
    """
    g = GUARD
    if g is not None:
        g.claim_written(path)


def watch(path) -> None:
    g = GUARD
    if g is not None:
        g.watch(path)
