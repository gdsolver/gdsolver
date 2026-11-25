#!/usr/bin/env python
"""Line up one run's per-iteration [fp] lines against reference runs and say where they first part.

    python py/fp_compare.py RUN [REF ...] [--baseline data/cold_baseline.json --level 22]   (at least one REF or --baseline)

RUN and each REF are text files that contain `dpsolve:   [fp] it=N plan=... t=... x=...` lines: the mod's
result.txt from the data root, the Geode log (the mod writes the same line there), or a cold run's coldlog_lvNN.txt. The
Geode log's own prefix is ignored. A REF is compared to RUN iteration by iteration, field by field; the first
iteration where any field differs is the first place the two loops chose differently, and everything before it agrees.

Besides the [fp] lines it lists, for each file, the section solves the loop entered: the `secsolve-enter:` lines (why, the
iteration, the deepest t / x, the last death's kind and killer, the stall counter -- written since the Android port) and the
older `dpsolve:   [secauto]` lines (the rule that fired and the wall). A reference with none is the Windows cold, where
the section solve is not meant to run on official level 22.

--baseline reads data/cold_baseline.json (or cold_baseline_coins.json): its `fp` for --level is the LAST iteration's line
of the reference cold, compared against RUN's last iteration.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

FP_RE = re.compile(r"\[fp\]\s+(it=\d+\s.*)$")
FIELDS = ("plan", "att", "t", "x", "band1", "fix", "horizon", "backoff", "live", "deep")


def parse_fp(rest: str) -> dict[str, str]:
    return dict(tok.split("=", 1) for tok in rest.split() if "=" in tok)


def read(path: Path):
    """(fps by iteration -- the last line of an iteration wins, the other lines of interest in file order)."""
    fps: dict[int, dict[str, str]] = {}
    notes: list[str] = []
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = FP_RE.search(raw)
        if m:
            d = parse_fp(m.group(1))
            if "it" in d:
                fps[int(d["it"])] = d
            continue
        if "secsolve-enter:" in raw or "[secauto]" in raw or "secsolve:" in raw:
            notes.append(raw[raw.find("secsolve-enter:"):] if "secsolve-enter:" in raw else raw.strip())
    return fps, notes


def compare(run, ref, run_name: str, ref_name: str) -> None:
    common = sorted(set(run) & set(ref))
    print(f"\n== {run_name}  vs  {ref_name}")
    print(f"   iterations: run {len(run)} (last it={max(run) if run else '-'}), ref {len(ref)} (last it={max(ref) if ref else '-'})")
    for it in common:
        diff = [f for f in FIELDS if run[it].get(f) != ref[it].get(f)]
        if diff:
            print(f"   FIRST DIFFERENCE at it={it}: {', '.join(diff)}")
            for f in diff:
                print(f"     {f:8s} run {run[it].get(f)}   ref {ref[it].get(f)}")
            if it > 1:
                prev = it - 1
                print(f"   (it={prev} agreed: t={run[prev].get('t')} x={run[prev].get('x')} plan={run[prev].get('plan')})")
            return
    if not common:
        print("   no iteration in common")
    elif len(run) != len(ref):
        print(f"   all {len(common)} common iterations agree; the run has {len(run)}, the reference {len(ref)}")
    else:
        print(f"   all {len(common)} iterations agree")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("run", type=Path)
    ap.add_argument("refs", nargs="*", type=Path)
    ap.add_argument("--baseline", type=Path, help="data/cold_baseline.json (or the coins one)")
    ap.add_argument("--level", default="22")
    args = ap.parse_args()

    run, run_notes = read(args.run)
    if not run:
        print(f"no [fp] line in {args.run}")
    print(f"{args.run}: {len(run)} iterations with an [fp] line")
    print("\nsection solves entered in the run:")
    for n in run_notes:
        print("  ", n[:400])
    if not run_notes:
        print("   none")

    if not args.refs and not args.baseline:
        ap.error("name at least one reference file (a cold run's coldlog_lvNN.txt), or --baseline")
    for ref_path in args.refs:
        if not ref_path.exists():
            print(f"\n(reference {ref_path} not found)")
            continue
        ref, ref_notes = read(ref_path)
        compare(run, ref, args.run.name, str(ref_path))
        print(f"   section-solve lines in the reference: {len(ref_notes)}")

    if args.baseline:
        base = json.loads(args.baseline.read_text(encoding="utf-8")).get(str(args.level))
        if base and run:
            last = parse_fp(base["fp"])
            it = int(last["it"])
            print(f"\n== baseline {args.baseline.name} level {args.level}: last iteration it={it} ({base.get('iters')} iters)")
            if it in run:
                diff = [f for f in FIELDS if run[it].get(f) != last.get(f)]
                print("   the run's it=%d %s the baseline's" % (it, "differs from: " + ", ".join(diff) if diff else "equals"))
            else:
                print(f"   the run has no it={it} (its last is it={max(run)})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
