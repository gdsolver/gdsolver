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
from gdtas.paths import LEVELDP_EXE                # noqa: E402


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--levels", type=int, nargs="+", default=list(range(1, 23)))
    ap.add_argument("--plans", default=str(qr.DATA / "solution_lv{}_dp.txt"))
    ap.add_argument("--leveldp", default=str(LEVELDP_EXE))
    ap.add_argument("--tmp", default=str(qr.DATA / "tmp_quickregress"))
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

    refs = {lv: qr.read_ref(lv) for lv in a.levels}

    def census_of(job: dict) -> list[dict]:
        lv, t0 = job["level"], job["t0"]
        return fixcensus.eval_trace(lv, t0, job["t1"] - t0,
                                    Path(job["trace"]), refs[lv], a.eps)

    t0 = time.time()
    now, n_segs, found = qr.run_segments(qa, extra=census_of)
    elapsed = time.time() - t0

    # leave real examples of the divergences (so looking inside a family needs
    # no re-run)
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


if __name__ == "__main__":
    raise SystemExit(main())
