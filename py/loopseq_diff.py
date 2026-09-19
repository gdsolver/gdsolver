#!/usr/bin/env python3
"""Compare two cold runs of the repair loop round by round.

    python py/loopseq_diff.py A/coldlog_lv22.txt B/coldlog_lv22.txt

This is the acceptance instrument for a change that is meant to leave the LOOP'S
SEQUENCE alone while changing how it spends its time -- `dpprobe` (src/mod/repair.hpp)
is the one it was written for. With the probe off and on, the two runs must produce
the same rounds: the same anchors, the same backoffs, the same deaths, the same
fixups. What they must NOT produce is the same wall clock, and that is the point.

What is compared, per iteration:

    re-anchored t / backoff   `dpsolve:   re-anchored at t=... (backoff ...)`
    death t / death x         `dpsolve: iter N: death t=... x=...`
    fix=N/sig                 `dpsolve:   [fp] ... fix=N/<sig>`

What is NOT compared, and why (the `--ignore-plan-hash` note): `plan=` and `att=`
in the [fp] line are EXPECTED to differ. A round the probe ended carries the plan
the game actually flew -- the search's common prefix, which stops shortly after the
death instead of covering the rest of the level -- so its fingerprint is a different
string for the same round, and every probe attempt the game flies bumps the level's
attempt counter. Both are differences in what was flown, not in what the loop
decided. `--plan-hash` puts them back in for a run where nothing should have
touched them at all.

Also printed, per log: the total of `dpsolve: done in %.1fs` (the search time the
run paid for) and the count of `dpsolve: cancelled` lines (rounds the probe ended).
A run with the probe on should show a much smaller total and a non-zero count; a run
with it off must show zero cancellations.

Exit status: 0 when the compared sequences agree, 1 when they differ or a log
cannot be read.
"""
import argparse
import re
import sys
from pathlib import Path

RE_ITER = re.compile(r"^dpsolve: iter (\d+): death t=(-?\d+) x=(-?[\d.]+)")
RE_ANCHOR = re.compile(r"^dpsolve:\s+re-anchored at t=(-?\d+) \(backoff (-?\d+)\)")
RE_FP = re.compile(r"^dpsolve:\s+\[fp\] it=(\d+) plan=(\S+) att=(\d+).*?\bfix=(\d+)/(\S+)")
RE_DONE = re.compile(r"^dpsolve: done in ([\d.]+)s")
RE_CANCEL = re.compile(r"^dpsolve: cancelled after ([\d.]+)s")


def parse(path, plan_hash):
    """Rounds in the order the log reports them, plus the run's two totals.

    A round is keyed by the iteration number the loop prints, not by position:
    a log that stops early is then still comparable against a longer one for as
    far as it goes, and a missing round shows up as a missing key rather than
    silently shifting every later comparison by one.
    """
    try:
        text = Path(path).read_bytes().decode("utf-8", "replace")
    except OSError as exc:
        print(f"{path}: {exc}", file=sys.stderr)
        return None
    rounds, order = {}, []
    # The re-anchor of the job spawned by iteration N's death is printed before
    # iteration N+1's death line, so it is filed against the round that is open
    # when it appears. `cur` is 0 before the first death (the opening solve).
    cur = 0
    done_total, cancels, cancel_total = 0.0, 0, 0.0
    for line in text.splitlines():
        m = RE_ITER.match(line)
        if m:
            cur = int(m.group(1))
            r = rounds.setdefault(cur, {})
            if cur not in order:
                order.append(cur)
            r["death_t"] = int(m.group(2))
            r["death_x"] = m.group(3)
            continue
        m = RE_ANCHOR.match(line)
        if m:
            r = rounds.setdefault(cur, {})
            if cur not in order:
                order.append(cur)
            # A round can re-anchor more than once only if the loop prints it
            # twice; keep them all so a duplicate is a difference, not a silent
            # overwrite.
            r.setdefault("anchors", []).append((int(m.group(1)), int(m.group(2))))
            continue
        m = RE_FP.match(line)
        if m:
            it = int(m.group(1))
            r = rounds.setdefault(it, {})
            if it not in order:
                order.append(it)
            r["fix"] = (m.group(4), m.group(5))
            if plan_hash:
                r["plan"] = m.group(2)
                r["att"] = m.group(3)
            continue
        m = RE_DONE.match(line)
        if m:
            done_total += float(m.group(1))
            continue
        m = RE_CANCEL.match(line)
        if m:
            cancels += 1
            cancel_total += float(m.group(1))
    return {
        "rounds": rounds,
        "order": order,
        "done_total": done_total,
        "cancels": cancels,
        "cancel_total": cancel_total,
    }


def describe(name, run):
    print(f"{name}: {len(run['rounds'])} rounds, "
          f"dpsolve done in {run['done_total']:.1f}s total, "
          f"{run['cancels']} cancelled ({run['cancel_total']:.1f}s)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a", help="the first log (coldlog_lvN.txt or result.txt)")
    ap.add_argument("b", help="the second one")
    ap.add_argument("--plan-hash", action="store_true",
                    help="also compare plan= and att= from the [fp] line. Off by "
                         "default: see the note at the top of this file -- with the "
                         "probe on they differ for a reason that is not a difference "
                         "in what the loop decided.")
    ap.add_argument("--max", type=int, default=10,
                    help="stop after this many differing rounds (default 10)")
    a = ap.parse_args()

    ra = parse(a.a, a.plan_hash)
    rb = parse(a.b, a.plan_hash)
    if ra is None or rb is None:
        return 1
    describe(a.a, ra)
    describe(a.b, rb)

    keys = sorted(set(ra["rounds"]) | set(rb["rounds"]))
    shown, first = 0, None
    for k in keys:
        fa = ra["rounds"].get(k)
        fb = rb["rounds"].get(k)
        if fa == fb:
            continue
        if first is None:
            first = k
        if shown >= a.max:
            continue
        shown += 1
        print(f"\niter {k}:")
        if fa is None or fb is None:
            print(f"  {a.a}: {fa}")
            print(f"  {a.b}: {fb}")
            continue
        for field in sorted(set(fa) | set(fb)):
            va, vb = fa.get(field), fb.get(field)
            if va != vb:
                print(f"  {field}: {va!r}  vs  {vb!r}")

    if first is None:
        print(f"\nSAME SEQUENCE over {len(keys)} rounds "
              f"(anchors, backoffs, deaths, fixups)")
        return 0
    print(f"\nFIRST DIFFERENCE at iter {first}; "
          f"{sum(1 for k in keys if ra['rounds'].get(k) != rb['rounds'].get(k))} "
          f"of {len(keys)} rounds differ")
    return 1


if __name__ == "__main__":
    sys.exit(main())
