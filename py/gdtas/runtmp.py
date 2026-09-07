# -*- coding: utf-8 -*-
"""REFUSE TO REPORT A RUN WHOSE OWN TRACES WERE WRITTEN BY SOMEBODY ELSE.

`gdtas.inputguard` closed one contamination channel: the files an anchored
instrument READS. THIS IS THE OTHER ONE, and it was left wide open. An
instrument like `fixcensus` launches a thousand solver replays, each writing a
trace into `--tmp` under a name built from the level and the anchor tick --
`fc_lv13_5800.trace.csv` -- AND NOTHING IN THAT NAME SAYS WHICH RUN WROTE IT.
`--tmp` defaulted to a shared directory, so two runs on one machine wrote the
same thousand paths and read back whichever bytes landed last. The input guard
watches none of it: a trace is the run's own output, not one of its declared
inputs, so the run's fingerprints all verified and the census was still a
mixture of two worlds.

The damage is the same shape as the bandtrack collision the input guard exists
for: A SINGLE ROW MOVES AND NOTHING ELSE IN THE OUTPUT SAYS SO. A census that
lost one section to a clobbered trace prints one fewer divergence, one fewer
family, and a plausible table.

Two mechanisms, because one of them is not enough:

1.  THE DEFAULT IS PRIVATE. With no `--tmp`, a run works in a directory named
    after itself (tool, UTC stamp, pid, and six random hex digits) and removes
    it when it is done. Two runs cannot name the same directory, so they cannot
    collide -- which is a stronger promise than any detector, because it holds
    for the runs that never look at their exit code. Orphans from crashed runs
    are pruned by age on the next run's way in.

2.  THE TRACES ARE FINGERPRINTED ANYWAY. `--tmp` pointing somewhere deliberate
    is how isolation is done today and stays exactly as it was -- the same
    stable file names in the directory you named -- so two runs CAN still be
    aimed at one directory by hand. Each trace is digested right after our
    solver wrote it and re-read when the run is over; anything that moved
    withholds the whole result and exits 2. Same stance as the input guard and
    as `quick_regress --whole`'s "NO BASELINE -- THIS IS NOT A PASS": refuse
    where it cannot be mistaken for a number.

A STALE TRACE IS DELETED, NOT DETECTED. `reserve()` unlinks whatever is already
at the path before the solver runs. That covers three failure modes a bare pid
or timestamp does not: an orphan left by a crashed run, yesterday's trace under
today's identical name, and the same name written by a job with different
`--seg-len` (so the file is real, current, and the wrong length). Without the
unlink, a solver that dies without writing leaves the reader looking at an
older run's file and calling it this run's physics. Pre-existence is NOT itself
a refusal: re-running into the same explicit `--tmp` is ordinary, and a guard
that cries on the ordinary case gets turned off.

WHAT THIS CANNOT SEE, exactly like the input guard: a trace clobbered between
our solver's exit and our digest of it, or clobbered and restored inside the
window. The private default is what covers those; the fingerprints are what
prove the default is doing its job, and what catches a deliberate `--tmp` being
shared.

Cost, measured on this corpus (2026-09-07): fixcensus writes 1,116 traces /
1.5 GB, digested once inline on eight threads and once serially at the end.
"""
from __future__ import annotations

import calendar
import os
import re
import secrets
import shutil
import threading
import time
from pathlib import Path

from .inputguard import digest

# ONE IDENTITY PER PROCESS. The stamp is UTC and sorts; the pid separates two
# processes started in the same second; the random tail separates two runs that
# a recycled pid would otherwise collide (Windows recycles pids freely, and a
# run directory outlives the run that made it when the run crashes).
RUN_ID = "{}-{}-{}".format(time.strftime("%Y%m%dT%H%M%S", time.gmtime()),
                           os.getpid(), secrets.token_hex(3))

# Only directories minted by this module are ever pruned. Anything a human put
# in the scratch root is left alone.
_RUN_DIR = re.compile(r"^[a-z_]+-(\d{8}T\d{6})-\d+-[0-9a-f]{6}$")


def prune(root: Path, keep_hours: float = 24.0) -> int:
    """Delete run directories under `root` older than `keep_hours`.

    Only names this module mints are considered, and only ones whose stamp is
    old enough that no live run can own them (the instruments here take
    minutes). Failure to delete is ignored: a directory another process still
    holds open is not worth failing a run over.
    """
    if not root.is_dir():
        return 0
    cutoff = time.time() - keep_hours * 3600.0
    n = 0
    for d in root.iterdir():
        m = _RUN_DIR.match(d.name)
        if not m or not d.is_dir():
            continue
        try:
            # RUN_ID's stamp is UTC, so it is read back as UTC. Reading it as
            # local time would make the cutoff wrong by the offset -- harmless
            # in one direction, and in the other it deletes a directory that is
            # hours younger than it looks.
            stamp = calendar.timegm(time.strptime(m.group(1), "%Y%m%dT%H%M%S"))
        except ValueError:
            continue
        if stamp >= cutoff:
            continue
        try:
            shutil.rmtree(d)
            n += 1
        except OSError:
            pass
    return n


class RunTmp:
    """The working directory of one run, and the ledger of what it wrote.

    `explicit` is the `--tmp` the caller asked for, or None. With None the run
    gets a private directory under `root` and removes it on `release()`; with a
    path, that path is used as it always was -- same file names, files kept --
    and the ledger is what makes sharing it detectable.
    """

    def __init__(self, tool: str, explicit=None, root=None,
                 keep_hours: float = 24.0) -> None:
        self.tool = tool
        self.private = explicit is None
        if self.private:
            base = Path(root)
            base.mkdir(parents=True, exist_ok=True)
            prune(base, keep_hours)
            name = f"{tool}-{RUN_ID}"
            # THE MINTER AND THE MATCHER ARE TWO STATEMENTS OF ONE FORMAT, so
            # make the mint check itself against _RUN_DIR rather than trusting
            # them to stay in step. `prune` is the ONLY thing that removes an
            # orphan left by a crashed run, and it only considers names that
            # match; a tool named with a digit or a capital (densefit2,
            # reachCheck) would mint a directory prune can never see and leak
            # 1.5 GB per crash, silently, forever. Failing here costs the first
            # run of a new tool; not failing here costs a disk.
            assert _RUN_DIR.match(name), (
                f"run directory {name!r} does not match _RUN_DIR, so prune() "
                f"would never reclaim it -- tool names must be lowercase "
                f"letters and underscores")
            self.dir = base / name
            # exist_ok=False on purpose: this name cannot legitimately be taken,
            # so if it is, something is wrong enough to stop for.
            self.dir.mkdir(parents=False, exist_ok=False)
        else:
            self.dir = Path(explicit)
            self.dir.mkdir(parents=True, exist_ok=True)
        self._seen: dict[Path, tuple] = {}
        self._lock = threading.Lock()

    # -- writing -----------------------------------------------------------
    def reserve(self, name: str, *suffixes: str) -> Path:
        """Clear the way for one output and return its base path.

        `name` is the stem the solver is given as `--out`; the suffixes are the
        files it will produce from it (".trace.csv" and so on). Whatever sits at
        those paths now belongs to some earlier run and is removed, so that a
        solver which fails to write cannot leave the reader holding it."""
        base = self.dir / name
        for s in ("",) + suffixes:
            p = Path(str(base) + s)
            try:
                p.unlink()
            except OSError:
                pass
        return base

    def claim(self, path) -> None:
        """Fingerprint one file WE JUST WROTE. Absent is recorded as absent: a
        trace the solver never produced must still not be allowed to appear
        later and be read by nothing."""
        p = Path(path)
        d = digest(p)
        with self._lock:
            self._seen[p] = d

    # -- reading the verdict -----------------------------------------------
    def check(self) -> list[str]:
        """Re-read every fingerprint. One line per file that moved."""
        with self._lock:
            items = sorted(self._seen.items())
        bad: list[str] = []
        for p, before in items:
            now = digest(p)
            if now == before:
                continue
            if before[0] and not now[0]:
                bad.append(f"{p}: DISAPPEARED before the run finished")
            elif not before[0] and now[0]:
                bad.append(f"{p}: APPEARED after our solver had already "
                           f"finished with it -- another writer owns this path")
            elif before[3] != now[3]:
                bad.append(f"{p}: OVERWRITTEN by another writer, sha256 "
                           f"{before[3][:12]} -> {now[3][:12]}, size "
                           f"{before[1]} -> {now[1]}")
            else:
                bad.append(f"{p}: REWRITTEN by another writer (same bytes) "
                           f"size {before[1]} -> {now[1]}, mtime_ns "
                           f"{before[2]} -> {now[2]}")
        return bad

    def count(self) -> int:
        with self._lock:
            return len(self._seen)

    def release(self) -> None:
        """Drop a private directory. A directory the caller named is kept: it
        was named so somebody could look in it."""
        if self.private:
            try:
                shutil.rmtree(self.dir)
            except OSError:
                pass


def banner(bad: list[str], tool: str, where: Path) -> str:
    """The refusal text, shaped after `inputguard.banner`."""
    out = ["",
           "=" * 72,
           f"TRACES CLOBBERED UNDER THIS RUN -- {tool.upper()} RESULT WITHHELD",
           "=" * 72,
           f"{len(bad)} trace file(s) this run wrote were changed by another "
           "writer before the",
           "run finished, so some sections were evaluated against a trace this "
           "run did not",
           "produce. The counts this run would have printed are NOT A RESULT.",
           ""]
    out += [f"  {b}" for b in bad[:20]]
    if len(bad) > 20:
        out.append(f"  ... and {len(bad) - 20} more")
    out += ["",
            f"--tmp was {where}, and something else is writing there. Drop "
            "--tmp (each run then",
            "gets a private directory of its own) or give this run a --tmp "
            "nothing else uses,",
            "and run one instrument at a time.",
            "=" * 72]
    return "\n".join(out)
