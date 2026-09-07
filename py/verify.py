# -*- coding: utf-8 -*-
"""Unified acceptance driver: get both fidelity (fixcensus) and regressions
(quick_regress) out of A SINGLE SECTION REPLAY. (proposal A, 2026-08-18)

fixcensus and quick_regress were each launching leveldp ~1,100 times, on the
same plans, the same gdref anchors and the same section split. Sections,
anchors, exe and replay are completely identical, so both evaluations can come
out of a single trace -- they were merged with "the numbers agree with the
separated runs" as the acceptance condition (cross-check 2026-08-18: census
71 divergences / 67 families, baseline diff 0/0, regress PASS on all 22 levels
-- identical to the separated runs).

    python py/verify.py               # both verdicts (~3 min)
    python py/verify.py --bless       # print the verdicts, then update both baselines
    python py/verify.py --levels 19 20   # Target/Guard profile (bless not allowed)

Real examples of the divergences are left in data/gdref/last_census.json every
time. Looking inside a family does not need another 5 minutes of replay:

    python -c "import json;
      [print(d) for d in json.load(open(r'data/gdref/last_census.json'))
       if d['cause']=='...']"

reach_check is not merged in (9 s, and being DP rather than a section replay
there is nothing for it to ride along with).
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))

import fixcensus                                   # noqa: E402
import quick_regress as qr                         # noqa: E402
from gdtas import inputguard                       # noqa: E402  contamination refusal
from gdtas.paths import LEVELDP_EXE, LEVEL_DATA    # noqa: E402


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--levels", type=int, nargs="+", default=list(range(1, 23)))
    ap.add_argument("--plans", default=str(qr.DATA / "solution_lv{}_dp.txt"))
    ap.add_argument("--leveldp", default=str(LEVELDP_EXE))
    # THE SAME `seg_lv{lv}_{t}` NAMES quick_regress WRITES. Sharing one
    # directory with it was not an accident of the default -- it was the
    # default -- so a verify and a quick_regress running at once wrote each
    # other's traces. Unset, the run now gets a directory only it can name.
    ap.add_argument("--tmp", default=None)
    # in the separated era it was two runs, census 8 / regress 6. Now that it is
    # a single run, we take census's 8
    ap.add_argument("--parallel", type=int, default=8)
    ap.add_argument("--tol", type=float, default=0.3,
                    help="tolerance on the regress side (diff_trace)")
    ap.add_argument("--eps", type=float, default=0.05,
                    help="the census side; the same default as the recorder's "
                         "--fixup-eps")
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--seg-step", type=int, default=400)
    ap.add_argument("--seg-len", type=int, default=400)
    ap.add_argument("--seg-start", type=int, default=200)
    ap.add_argument("--seg-slack", type=int, default=0)
    ap.add_argument("--bless", action="store_true",
                    help="print the verdict first, then update **both** baselines")
    ap.add_argument("--no-waivers", action="store_true",
                    help="ignore the waivers for known outliers "
                         "(py/census_waivers.py)")
    a = ap.parse_args(argv)

    # reject --bless on a restricted run at the door (same reasoning as in
    # fixcensus / quick_regress)
    if a.bless and set(a.levels) != set(range(1, 23)):
        print("--bless is only accepted on a full 22-level run "
              "(blessing a --levels subset erases the baseline of every level "
              "that did not run)")
        return 2

    qa = SimpleNamespace(levels=a.levels, plans=a.plans, leveldp=a.leveldp,
                         tmp=a.tmp, parallel=a.parallel, tol=a.tol,
                         seg_step=a.seg_step, seg_len=a.seg_len,
                         seg_start=a.seg_start, seg_slack=a.seg_slack,
                         whole=False, bless=a.bless)

    # THE RUN COMMITS TO ITS INPUTS HERE, exactly as fixcensus does. verify had
    # NO input guard at all, and the consequence was not that it lacked a
    # nicety: `inputguard.claim`'s contract is "a tool with no guard installed
    # pays nothing", so quick_regress.band_track_args' claim of
    # gdref/lv*.bandtrack.txt was a silent no-op on this path. That is THE ONE
    # FILE whose contamination we have actually observed (2026-09-06), left
    # unwatched on the path the repair loop uses to decide whether a plan is
    # good -- so an unguarded input here can move a SOLVE, not merely a reading.
    #
    # The watch list is fixcensus's, and it was checked rather than copied:
    # cut.json is read by seg_jobs / seg_check / check_level (all reached from
    # run_segments), fixcensus.json by census_report, and both are read-only on
    # this path -- cut.json's only write is under `if a.record:`, which verify
    # never enters. A file a run both reads and writes must be claimed, not
    # watched, or the guard fires on the run's own legitimate write.
    guard = inputguard.install()
    guard.watch(a.leveldp)
    guard.watch_many([qr.REF / "cut.json", qr.REF / "fixcensus.json"])
    for lv in a.levels:
        # qr.plan_of, not fixcensus.plan_of: it is DEFINED at
        # quick_regress.py:173 and only re-exported through fixcensus, so
        # reaching it through fixcensus depends on that module's import list
        # staying as it is. Take it from the deciding source.
        plan = qr.plan_of(lv, a.plans)
        guard.watch_many([qr.REF / f"lv{lv}.csv", plan]
                         + [LEVEL_DATA / f"{n}_lv{lv}.txt" for n in
                            ("objrects", "triggers", "objgroups", "obb")]
                         + [Path(str(plan) + s) for s in
                            (".groups.txt", ".groups.deep.txt",
                             ".groups.live.txt", ".groups.bank.txt")])
    # gdref/lv*.bandtrack.txt is deliberately NOT watched: this run rewrites it,
    # so its state beforehand says nothing. It is claimed inside
    # quick_regress.band_track_args right after our write -- which is what the
    # missing install() above was making inert.

    refs = {lv: qr.read_ref(lv) for lv in a.levels}

    def census_of(job: dict) -> list[dict]:
        lv, t0 = job["level"], job["t0"]
        return fixcensus.eval_trace(lv, t0, job["t1"] - t0,
                                    Path(job["trace"]), refs[lv], a.eps)

    t0 = time.time()
    try:
        now, n_segs, found = qr.run_segments(qa, extra=census_of)
        elapsed = time.time() - t0

        # BEFORE ANYTHING IS WRITTEN OR PRINTED, and before --bless. Both
        # verdicts below are read off the same traces, so a trace another run
        # wrote would move the regression AND the census by the same invisible
        # row. Same refusal, same exit 2, as fixcensus.
        # The INPUT channel first: did anything the sections were measured
        # against move while they were being measured? Separate refusal from the
        # trace verdict below because the remedies differ -- an input that moved
        # means someone else is writing the lab tree, a trace that moved means
        # someone else is writing this --tmp.
        moved = guard.check()
        if moved:
            text = inputguard.banner(moved, "verify")
            print(text)
            print(text, file=sys.stderr)
            return 2

        rc_t = qr.trace_verdict(qa, "verify")
        if rc_t:
            return rc_t

        # On the clean path, SAY SO, and say how many. A guard whose success is
        # silent cannot be told apart from a guard that was never installed --
        # which is the exact bug being fixed here, so it would be a poor joke to
        # fix it with something carrying the same failure mode one level up. The
        # count is the part that matters, and it is enumerable: 1 exe + cut.json
        # + fixcensus.json + TEN per level (lv{n}.csv, plan, 4 LEVEL_DATA, 4
        # .groups) = 13 watched, PLUS the bandtrack that band_track_args claims
        # after writing it -- count() is len(_seen) and _record holds watches
        # and claims alike. So one level prints 14.
        # 13 AND 14 ARE THE TWO OUTCOMES THAT HAVE TO BE TOLD APART: 13 means
        # the guard installed but the claim did not land, which is the no-op
        # this commit exists to fix and which any nonzero count would hide.
        # Enumerate it; do not just check it is not zero.
        # stderr, so a guarded run stays byte-comparable with an unguarded one
        # on stdout (the same reason trace_verdict does it).
        print(f"[inputguard] {guard.count()} inputs verified unchanged "
              f"across {round(elapsed, 1)}s of replay", file=sys.stderr)

        # leave real examples of the divergences (so looking inside a family
        # needs no re-run)
        try:
            (qr.REF / "last_census.json").write_text(
                json.dumps(sorted(found, key=lambda d: (d["lv"], d["t"])),
                           indent=1), encoding="utf-8")
        except OSError:
            pass

        rc_r = qr.report(now, qa, elapsed)
        print()
        rc_c = fixcensus.census_report(found, n_segs, elapsed, top=a.top,
                                       bless=a.bless, levels=a.levels,
                                       waivers=not a.no_waivers)
        return max(rc_r, rc_c)
    finally:
        qr.tmp_of(qa).release()


if __name__ == "__main__":
    raise SystemExit(main())
