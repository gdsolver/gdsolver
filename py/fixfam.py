# -*- coding: utf-8 -*-
"""fixfam: a census that groups fixups_log_lv*.txt into families by cause.

The loop's own fixup pass only looks at the within-run file
(.fixups.txt). This one cuts THE LAST RUN out of the append-only log
(fixups_log_lvN.txt) and counts families across every level. The tool for
choosing targets in the fixup clean-up work (from 2026-08-17 on).

The last run is cut out by watching iter= wind backwards (the log is
append-only; .fixups.txt is deleted at the start of a run but fixlog survives.
iter increases monotonically within a run).

Grouping is the same as in fixup_causes (cause, in, kill). So is the verdict:
  spread of edy <= 0.15 -> constant off / larger -> wrong formula (kill apart)

Usage:
  python py/fixfam.py                 # all levels, last run only
  python py/fixfam.py --level 16 22   # only the given levels
  python py/fixfam.py --top 30        # top 30 families
  python py/fixfam.py --family <sig>  # print every record of one family (in=
                                      # and kill= are given by appending
                                      # /inN /kill after sig)
"""
import argparse
import re
import sys
from pathlib import Path

import sys as _sys
_sys.path.insert(0, str(Path(__file__).resolve().parent))
from gdtas.paths import LEVEL_DATA as DATA  # noqa: E402

REC = re.compile(
    r"^(?P<ts>\S+) iter=(?P<it>\d+) t=(?P<t>\d+)(?P<noop> noop)? (?P<body>.*)$")


def parse_log(path: Path, run: int = -1) -> list[dict]:
    """Read one file, return only the records of the given run (default: last) as dicts."""
    runs: list[list[dict]] = []
    cur: list[dict] = []
    prev_it = None
    for ln in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = REC.match(ln)
        if not m:
            continue
        it = int(m.group("it"))
        if prev_it is not None and it < prev_it:
            runs.append(cur)
            cur = []
        prev_it = it
        d = dict(re.findall(r"(\w+)=([^,\s]+)", m.group("body")))
        d["_ts"] = m.group("ts")
        d["_iter"] = it
        d["_t"] = int(m.group("t"))
        d["_noop"] = bool(m.group("noop"))
        cur.append(d)
    if cur:
        runs.append(cur)
    try:
        return runs[run] if runs else []
    except IndexError:
        return []


def fnum(d: dict, k: str) -> float:
    try:
        return float(d.get(k, "nan"))
    except ValueError:
        return float("nan")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--level", type=int, nargs="*", default=None)
    ap.add_argument("--top", type=int, default=25)
    ap.add_argument("--family", default=None,
                    help="cause signature (exact match); /inN and /kill may be appended")
    ap.add_argument("--noop", action="store_true",
                    help="count the noops too (the marks left where the transition agreed)")
    ap.add_argument("--run", type=int, default=-1,
                    help="which run to read, as a negative index. -1 = the last "
                         "one; use -2 while a run is still going")
    ap.add_argument("--triage", action="store_true",
                    help="sort what is left by size: rounding, phase, or a hole in a rule")
    a = ap.parse_args()

    levels = a.level or sorted(
        int(m.group(1)) for f in DATA.glob("fixups_log_lv*.txt")
        if (m := re.match(r"fixups_log_lv(\d+)\.txt", f.name)))

    # split off the trailing modifiers of the --family argument
    want_cause, want_in, want_kill = a.family, None, None
    if want_cause:
        while True:
            if (m := re.search(r"/in(\d)$", want_cause)):
                want_in = m.group(1)
                want_cause = want_cause[:m.start()]
            elif want_cause.endswith("/kill"):
                want_kill = "1"
                want_cause = want_cause[:-5]
            else:
                break

    fams: dict[tuple, list[dict]] = {}
    per_level: dict[int, dict[str, int]] = {}
    for lv in levels:
        f = DATA / f"fixups_log_lv{lv}.txt"
        if not f.exists():
            print(f"  (lv{lv}: no log)", file=sys.stderr)
            continue
        recs = parse_log(f, a.run)
        st = per_level.setdefault(lv, {"real": 0, "noop": 0, "kill": 0,
                                       "ts0": "", "ts1": ""})
        if recs:
            st["ts0"], st["ts1"] = recs[0]["_ts"], recs[-1]["_ts"]
        for d in recs:
            d["_lv"] = lv
            if d["_noop"]:
                st["noop"] += 1
                if not a.noop:
                    continue
            else:
                st["real"] += 1
                if d.get("kill") == "1":
                    st["kill"] += 1
            key = (d.get("cause", "nocause"), d.get("in", "?"),
                   d.get("kill", "0"))
            fams.setdefault(key, []).append(d)

    if want_cause:
        rows = [d for (c, i, k), v in fams.items() for d in v
                if c == want_cause
                and (want_in is None or i == want_in)
                and (want_kill is None or k == want_kill)]
        rows.sort(key=lambda d: (d["_lv"], d["_t"]))
        print(f"family: {want_cause} in={want_in or '*'} kill={want_kill or '*'}"
              f"  ({len(rows)})")
        for d in rows:
            print(f"  lv{d['_lv']:<2} t={d['_t']:<6} x={d.get('x', '?'):<12} "
                  f"y={d.get('y', '?'):<12} vy={d.get('vy', '?'):<12} "
                  f"edy={d.get('edy', '?'):<9} edvy={d.get('edvy', '?'):<9} "
                  f"dy={d.get('dy', '?'):<9} dvy={d.get('dvy', '?')}"
                  f"{'  [noop]' if d['_noop'] else ''}")
        return 0

    # ---- per-level summary ----
    print("per level (last run):")
    tot_r = tot_n = tot_k = 0
    for lv in levels:
        st = per_level.get(lv)
        if not st:
            continue
        tot_r += st["real"]
        tot_n += st["noop"]
        tot_k += st["kill"]
        print(f"  lv{lv:<2} real={st['real']:<4} (kill={st['kill']:<3}) "
              f"noop={st['noop']:<4}  {st['ts0']} .. {st['ts1']}")
    print(f"  total real={tot_r} (kill={tot_k}) noop={tot_n}")
    print()

    if a.triage:
        # STRATIFICATION THAT ANSWERS "I want only rounding left".
        # The recording side already diverts everything with both |edy| and
        # |edvy| below 0.05 into noop (--fixup-eps), so every real left here
        # is above that.
        # How the layers are cut:
        #   A phase/accumulation class (max|e| <= 0.3): below the divergence
        #     detection threshold (0.3px). Inside the range explained by float
        #     accumulation, the 0.001 grid of vy and sub-1tick phase shifts, so
        #     adding a rule may not make it go away = effectively the floor
        #   B small hole in the rules (0.3 < max|e| <= 2.0): a constant off, or
        #     a 1 tick delay
        #   C hole in the rules (max|e| > 2.0): a discrete event that was missed
        #   kill: a hole in the rules regardless of size (life and death split)
        buckets: dict[str, list[dict]] = {"A": [], "B": [], "C": [], "kill": []}
        for (cause, act, kill), rows in fams.items():
            for d in rows:
                e = max(abs(fnum(d, "edy")), abs(fnum(d, "edvy")))
                if kill == "1":
                    buckets["kill"].append(d)
                elif not (e == e):        # NaN (old recording)
                    buckets["B"].append(d)
                elif e <= 0.3:
                    buckets["A"].append(d)
                elif e <= 2.0:
                    buckets["B"].append(d)
                else:
                    buckets["C"].append(d)
        names = {"A": "phase / accumulation (<=0.3, effectively the floor)",
                 "B": "a small hole in a rule (0.3..2)",
                 "C": "a hole in a rule (>2)",
                 "kill": "kill (they disagree on life and death)"}
        tot = sum(len(v) for v in buckets.values())
        print(f"triage of the {tot} remaining "
              f"({tot_n} noops are already excluded, all under 0.05):")
        for k in ("A", "B", "C", "kill"):
            v = buckets[k]
            if not v:
                print(f"  {k}: 0     {names[k]}")
                continue
            lvs: dict[int, int] = {}
            fam = set()
            for d in v:
                lvs[d["_lv"]] = lvs.get(d["_lv"], 0) + 1
                fam.add((d.get("cause", "?"), d.get("in", "?")))
            tag = " ".join(f"lv{lv}x{n}" for lv, n in
                           sorted(lvs.items(), key=lambda kv: -kv[1]))
            print(f"  {k}: {len(v):3d} in {len(fam):2d} families  {names[k]}")
            print(f"       {tag}")
        real_gap = len(buckets["B"]) + len(buckets["C"]) + len(buckets["kill"])
        print(f"\nnot explained by rounding or phase = {real_gap} "
              f"(B+C+kill), in families = "
              f"{len({(d.get('cause'), d.get('in'), d.get('kill')) for k in ('B', 'C', 'kill') for d in buckets[k]})}")
        return 0

    # ---- family ranking ----
    ranked = sorted(fams.items(), key=lambda kv: -len(kv[1]))
    nf = len(ranked)
    print(f"families ({sum(len(v) for _, v in ranked)} -> {nf} families, "
          f"top {a.top}):")
    print(f"  {'n':>4} {'by level':<18} {'cause':<58} "
          f"{'edy med':>8} {'width':>7} {'edvy med':>8} {'width':>7}  verdict")
    for (cause, act, kill), rows in ranked[:a.top]:
        edys = sorted(e for d in rows
                      if not (e := fnum(d, "edy")) != e)  # drop NaN
        edvs = sorted(e for d in rows
                      if not (e := fnum(d, "edvy")) != e)
        lvs: dict[int, int] = {}
        for d in rows:
            lvs[d["_lv"]] = lvs.get(d["_lv"], 0) + 1
        lvtag = ",".join(f"{lv}x{n}" for lv, n in
                         sorted(lvs.items(), key=lambda kv: -kv[1])[:4])
        if len(lvs) > 4:
            lvtag += f",+{len(lvs) - 4}"
        med = edys[len(edys) // 2] if edys else float("nan")
        spread = (edys[-1] - edys[0]) if edys else float("nan")
        vmed = edvs[len(edvs) // 2] if edvs else float("nan")
        vspread = (edvs[-1] - edvs[0]) if edvs else float("nan")
        if kill == "1":
            verdict = "kill"
        elif len(edys) >= 3:
            # if either edy or edvy is wide it is the formula; both narrow = a constant
            wide = max(spread, vspread)
            verdict = "constant is off" if wide <= 0.15 else "formula is wrong"
        else:
            verdict = "-"
        print(f"  {len(rows):>4} {lvtag:<18} {cause + '/in' + act:<58} "
              f"{med:>+8.3f} {spread:>7.3f} {vmed:>+8.3f} {vspread:>7.3f}  "
              f"{verdict}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
