# -*- coding: utf-8 -*-
"""COUNT FIXUP FAMILIES WITHOUT A FULL REGRESSION. (no GD, no solve; minutes)

A fixup records "the first divergence between the GD replay and the model".
The driver takes them while it is solving, which needs a full cold run (hours),
but THE DIVERGENCE ITSELF CAN BE MEASURED FROM THE VERIFIED PLANS AND gdref
ALONE. Here we replay with the same section anchors as quick_regress and count
the first divergence of every section, signed with `gdtas.solveutil.cause_of`
(THE VERY SAME FUNCTION as the recorder).

So "did this change make that family disappear" can be answered without
running a full regression. The numbers are not one-to-one with the family
census of fixups_log (py/fixfam.py) -- a real run also picks up divergences on
other, mid-solve plans, so its counts are higher. What is looked at here is
DIVERGENCE ON THE VERIFIED CORPUS, and the sets of families mostly overlap.

    python py/fixcensus.py                     # all levels, census per family
    python py/fixcensus.py --bless             # save the current result as baseline
    python py/fixcensus.py --levels 10 19      # narrow down
    python py/fixcensus.py --family <cause>    # print real examples of that family
    python py/fixcensus.py --leveldp <exe>     # A/B against a differential build
    python py/fixcensus.py --gd-ground corroborated
                                               # sign the gdg letters with
                                               # grounded_of instead of the raw
                                               # onGround column (same
                                               # divergences, some other names)
"""
from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import census_waivers                             # noqa: E402  known outliers
from gdtas.solveutil import (GD_GROUND_CORROBORATED, GD_GROUND_RAW, MODE_ID,
                             cause_of)            # noqa: E402  recorder's classification
from gdtas.paths import LEVELDP_EXE               # noqa: E402
import quick_regress as qr  # noqa: E402  rot_anchor_args (one-shot history of 2900)
from quick_regress import (DATA, LEVEL_DATA, REF, ctrlwin_args, groups_args,
                           has_grouped_colliders, plan_of, read_ref,
                           start_fields)  # noqa: E402


def eval_trace(lv: int, t0: int, span: int, trace: Path,
               gd: dict[int, dict], eps: float,
               gd_ground: str = GD_GROUND_RAW) -> list[dict]:
    """Return THE FIRST DIVERGENCE from an already replayed trace (does not replay).

    Same stance as the recorder: a divergence is read as "the transition delta
    of that tick" (dy/dvy). In order to ride along with the section replay of
    quick_regress (proposal A, 2026-08-18), replay and evaluation were split
    apart here.
    """
    if not trace.exists():
        return []
    rows: dict[int, list[str]] = {}
    with trace.open(newline="", encoding="utf-8-sig", errors="replace") as f:
        rd = csv.reader(f)
        next(rd, None)
        for q in rd:
            if q:
                try:
                    rows[int(q[0])] = q
                except ValueError:
                    pass
    out = []
    for t in range(t0 + 1, t0 + span + 1):
        if t not in rows or (t - 1) not in rows or t not in gd or (t - 1) not in gd:
            continue
        m0, m1 = rows[t - 1], rows[t]
        g0, g1 = gd[t - 1], gd[t]
        try:
            dy_g = float(g1["y"]) - float(g0["y"])
            dv_g = float(g1["yvel"]) - float(g0["yvel"])
            edy = dy_g - (float(m1[2]) - float(m0[2]))
            edvy = dv_g - (float(m1[3]) - float(m0[3]))
        except (ValueError, KeyError):
            continue
        if abs(edy) < eps and abs(edvy) < eps:
            continue
        # g0, not g1: the model's half of the signature comes from m0 (t-1), so
        # GD's has to come from the same row. GD's values on the way out are
        # passed separately and surface as gdgo/gdmo when they differ -- see
        # cause_of's docstring for the four families the old mismatch misread.
        # THE ROW IS RIGHT; THE COLUMN IS RAW BY DEFAULT. `onGround` goes in
        # untranslated, while quick_regress.start_fields -- reading the very
        # same reference -- puts it through grounded_of first. The two are not
        # the same predicate (grounded_of's docstring has the per-mode gap), so
        # what a census record means by `gdg1` in ship/ufo/wave is "GD's flag
        # was set", not "GD was resting". Neither choice is wrong; the key just
        # has to be read as the one it is -- and `--gd-ground corroborated`
        # hands the whole row over so the other one can be read off the same
        # sections. The rows go in either way; only the flag decides which of
        # them cause_of looks at.
        cause = cause_of(m0, m1, g0.get("onGround", "?"),
                         MODE_ID.get(g0.get("mode", ""), -1),
                         g1.get("onGround", "?"),
                         MODE_ID.get(g1.get("mode", ""), -1),
                         gd_ground=gd_ground, gd_row=g0, gd_row_out=g1)
        out.append({"lv": lv, "t": t, "x": round(float(m0[1]), 1),
                    "cause": cause, "in": m1[10] if len(m1) > 10 else "?",
                    "edy": round(edy, 4), "edvy": round(edvy, 4)})
        break            # same as the recorder: only the first hit per section
    return out


def seg_diverge(lv: int, t0: int, span: int, exe: Path, tmp: Path,
                eps: float, gd_ground: str = GD_GROUND_RAW) -> list[dict]:
    """Anchor one section from the real GD state, replay it, hand it to eval_trace."""
    plan = plan_of(lv, str(DATA / "solution_lv{}_dp.txt"))
    gd = read_ref(lv)
    objrects = LEVEL_DATA / f"objrects_lv{lv}.txt"
    if not plan.exists() or t0 not in gd:
        return []
    base = tmp / f"fc_lv{lv}_{t0}"
    r0 = gd[t0]
    a = [str(exe), str(objrects), "--replay", str(plan),
         "--start", start_fields(t0, r0, plan, gd.get(t0 - 1), gd),
         "--out", str(base)]
    if r0.get("pmin") and r0.get("pmax"):
        a += ["--startband", f"{r0['pmin']},{r0['pmax']}"]
    # same band recording as the driver (--bandtrack). Without it, on levels
    # holding camera-driven bands (lv22) we count divergences into families
    # that do not exist in the deployed configuration
    a += qr.band_track_args(lv, gd)
    trig, grp = LEVEL_DATA / f"triggers_lv{lv}.txt", LEVEL_DATA / f"objgroups_lv{lv}.txt"
    if trig.exists() and grp.exists():
        a += ["--triggers", str(trig), "--objgroups", str(grp)]
    obb = LEVEL_DATA / f"obb_lv{lv}.txt"
    if obb.exists():
        a += ["--obb", str(obb)]
    a += groups_args(plan)
    a += ctrlwin_args(lv)
    a += qr.rot_anchor_args(lv, t0)
    # ...and the pads the run had already fired before t0 (the same producer
    # quick_regress uses, so the two instruments anchor identically). Without
    # it every re-entered pad in lv13/14/18/21 shows up here as a family.
    a += qr.pad_anchor_args(lv, t0, gd)
    subprocess.run(a, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return eval_trace(lv, t0, span, Path(str(base) + ".trace.csv"), gd, eps,
                      gd_ground)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--levels", type=int, nargs="*", default=list(range(1, 23)))
    ap.add_argument("--leveldp", default=str(LEVELDP_EXE))
    ap.add_argument("--tmp", default=r"C:\GDtmp\fixcensus")
    ap.add_argument("--seg-step", type=int, default=400)
    ap.add_argument("--seg-len", type=int, default=400)
    ap.add_argument("--seg-start", type=int, default=200)
    ap.add_argument("--eps", type=float, default=0.05,
                    help="the same default as the recorder's --fixup-eps")
    ap.add_argument("--parallel", type=int, default=8)
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--bless", action="store_true")
    ap.add_argument("--family", default=None)
    ap.add_argument("--no-waivers", action="store_true",
                    help="ignore the waivers for known outliers and print the raw numbers")
    ap.add_argument("--gd-ground", choices=[GD_GROUND_RAW,
                                            GD_GROUND_CORROBORATED],
                    default=GD_GROUND_RAW,
                    help="which definition of 'GD was grounded' the gdg/gdgo "
                         "letters of a family key are minted from. raw (the "
                         "default, and what the blessed baseline is named by) "
                         "is the dump's onGround column; corroborated puts the "
                         "same row through gdtas.solveutil.grounded_of, which "
                         "in ship/ufo/wave also demands onGround2 and a "
                         "near-zero yvel. The A/B answers 'is this family "
                         "signed by physics or by the raw column's looseness?' "
                         "-- it moves NO divergence, only the names")
    ap.add_argument("--no-spentpad", action="store_true",
                    help="the A/B arm: still name the pads the run had fired "
                         "before each anchor, but tell the solver to ignore "
                         "them (pre-2026-09-06 seeding)")
    ap.add_argument("--json", default="", dest="json_out",
                    help="write the divergences themselves (with their ticks) "
                         "to this file -- brief-017's section list needs WHERE "
                         "they are, which the family baseline does not carry")
    a = ap.parse_args(argv)
    qr.NO_SPENTPAD = bool(a.no_spentpad)
    # reject --bless on a restricted run at the door (same reasoning as
    # quick_regress: the baseline file replaces the census of every level
    # wholesale, so a bless with --levels narrowed erases the families of the
    # levels that did not run from the baseline).
    if a.bless and set(a.levels) != set(range(1, 23)):
        print("--bless is only accepted on a full 22-level run "
              "(blessing a --levels subset erases the families of every level "
              "that did not run)")
        return 2
    # ...and only in the naming the baseline is in. The corroborated arm spells
    # some keys differently, so blessing from it would rewrite the baseline in
    # the other definition without saying so, and every later comparison would
    # be against names nothing else mints.
    if a.bless and a.gd_ground != GD_GROUND_RAW:
        print(f"--bless is only accepted with --gd-ground {GD_GROUND_RAW} "
              f"(the baseline's family names are minted from the raw column; "
              f"blessing a {a.gd_ground} run would silently rename them)")
        return 2
    tmp = Path(a.tmp)
    tmp.mkdir(parents=True, exist_ok=True)
    cuts = json.loads((REF / "cut.json").read_text()) \
        if (REF / "cut.json").exists() else {}

    jobs = []
    for lv in a.levels:
        gd = read_ref(lv)
        if not gd:
            continue
        if has_grouped_colliders(LEVEL_DATA / f"objrects_lv{lv}.txt") \
                and not groups_args(plan_of(lv, str(DATA / "solution_lv{}_dp.txt"))):
            continue
        cut = cuts.get(str(lv)) or max(gd)
        t = a.seg_start
        while t + a.seg_len <= cut:
            jobs.append((lv, t))
            t += a.seg_step

    t0 = time.time()
    found: list[dict] = []
    with ThreadPoolExecutor(max_workers=a.parallel) as ex:
        futs = [ex.submit(seg_diverge, lv, t, a.seg_len, Path(a.leveldp),
                          tmp, a.eps, a.gd_ground) for lv, t in jobs]
        for f in futs:
            found += f.result()

    if a.family:
        # THE TABLE'S SPELLING AND THIS ONE WERE DIFFERENT KEYS. census_report
        # renders `cause + "/in" + act`, while the match here was against the
        # raw `cause`, which never carries the suffix -- so a family name copied
        # out of the table could not match, and the only symptom was a bare
        # "family <name>: 0" with no warning. Read as "that family is gone" it
        # is the same silent zero twice over: once for a query that missed, once
        # for a world that does not contain it. Accept either spelling, and when
        # neither hits, say whether the suffix was the reason.
        rows = [d for d in found
                if a.family in (d["cause"], f"{d['cause']}/in{d['in']}")]
        print(f"family {a.family}: {len(rows)}")
        if not rows and "/in" in a.family:
            stem, _, tail = a.family.rpartition("/in")
            if tail.isdigit():
                alt = [d for d in found if d["cause"] == stem]
                if alt:
                    print(f"  (0 as written; without the trailing /in{tail} it "
                          f"matches {len(alt)} -- both spellings are accepted, "
                          f"so this one is genuinely absent for that `in` value)")
        for d in sorted(rows, key=lambda d: (d["lv"], d["t"])):
            print(f"  lv{d['lv']:<2} t={d['t']:<6} x={d['x']:<10} "
                  f"in={d['in']} edy={d['edy']:<9} edvy={d['edvy']}")
        return 0

    return census_report(found, len(jobs), time.time() - t0,
                         top=a.top, bless=a.bless, levels=a.levels,
                         waivers=not a.no_waivers, json_out=a.json_out)


def _base_count(v, levels: set[int]) -> int:
    """Count for one family in the baseline. The new format ({lv: n}) can be
    narrowed by level. The old format (int) only holds the all-level total, so
    it is returned as is."""
    if isinstance(v, dict):
        return sum(n for l, n in v.items() if int(l) in levels)
    return int(v)


def census_report(found: list[dict], n_segs: int, elapsed: float, *,
                  top: int, bless: bool, levels: list[int],
                  waivers: bool = True, json_out: str = "") -> int:
    """Aggregate, print, compare against the baseline, and bless the families.
    found is the accumulation of eval_trace's return values.

    The baseline is written in a PER-LEVEL NESTED FORMAT {fam: {"10": 2, ...}}
    (2026-08-18). With it, even a run with --levels narrowed can be compared
    against ONLY THE MATCHING LEVELS of the baseline -- with the old format the
    families of the levels that did not run all looked like they had
    "disappeared". Old-format baselines still read fine (for an all-level run
    the meaning is the same).
    """
    # TAKE THE WAIVERS OUT BEFORE COUNTING. Only the ones whose key
    # (lv/tick/cause/in/edy/edvy) matches exactly. If the way it diverges
    # changes, the key comes off and it shows up red as usual.
    waived: list[tuple[dict, dict]] = []
    if waivers:
        found, waived = census_waivers.split(found)
    # --json: the divergences themselves, not the family aggregate. The blessed
    # baseline is {family: {level: count}} and carries no ticks, so brief-017's
    # section list -- which needs WHERE each divergence is -- cannot be built
    # from it. This writes what `found` already holds: lv, t, x, cause, in, edy,
    # edvy, one record per diverging section.
    if json_out:
        Path(json_out).write_text(json.dumps(found, indent=1), encoding="utf-8")
        print(f"json -> {json_out} ({len(found)} divergences)")

    fams: dict[tuple, list[dict]] = {}
    for d in found:
        fams.setdefault((d["cause"], d["in"]), []).append(d)
    ranked = sorted(fams.items(), key=lambda kv: -len(kv[1]))

    # the layers are cut the same way as fixfam --triage
    def bucket(d: dict) -> str:
        e = max(abs(d["edy"]), abs(d["edvy"]))
        return "A" if e <= 0.3 else ("B" if e <= 2.0 else "C")

    buck = {"A": 0, "B": 0, "C": 0}
    for d in found:
        buck[bucket(d)] += 1
    per_lv: dict[int, int] = {}
    for d in found:
        per_lv[d["lv"]] = per_lv.get(d["lv"], 0) + 1

    wtag = f" / {len(waived)} waived" if waived else ""
    print(f"{n_segs} sections / {len(found)} divergences / "
          f"{len(fams)} families{wtag} ({round(elapsed, 1)}s)")
    print(f"  by size: A(<=0.3)={buck['A']}  B(0.3..2)={buck['B']}  "
          f"C(>2)={buck['C']}")
    print("  by level: " + " ".join(f"lv{k}x{v}" for k, v in
                                    sorted(per_lv.items(), key=lambda kv: -kv[1])))
    print()
    print(f"{'n':>4} {'lv':<14} {'cause':<56} {'edy med':>9} {'edvy med':>9}")
    for (cause, act), rows in ranked[:top]:
        lvs: dict[int, int] = {}
        for d in rows:
            lvs[d["lv"]] = lvs.get(d["lv"], 0) + 1
        tag = ",".join(f"{k}x{v}" for k, v in
                       sorted(lvs.items(), key=lambda kv: -kv[1])[:3])
        ey = sorted(d["edy"] for d in rows)[len(rows) // 2]
        ev = sorted(d["edvy"] for d in rows)[len(rows) // 2]
        print(f"{len(rows):>4} {tag:<14} {cause + '/in' + act:<56} "
              f"{ey:>+9.3f} {ev:>+9.3f}")

    # ALWAYS PRINT THE WAIVERS WHERE THEY CAN BE SEEN. Hidden, they read
    # as "fixed".
    if waived:
        print(f"\nwaived as known outliers ({len(waived)}, py/census_waivers.py"
              " -- --no-waivers for the raw numbers):")
        for d, w in waived:
            print(f"  lv{d['lv']} t={d['t']} x={d['x']} "
                  f"edy={d['edy']:+.3f} edvy={d['edvy']:+.3f} "
                  f"(since {w['since']})")
            print(f"    {w['why'][:110]}...")

    base_path = REF / "fixcensus.json"
    lvset = set(levels)
    now: dict[str, dict[str, int]] = {}
    for (c, i), v in fams.items():
        e = now.setdefault(f"{c}/in{i}", {})
        for d in v:
            e[str(d["lv"])] = e.get(str(d["lv"]), 0) + 1
    now_n = {k: sum(v.values()) for k, v in now.items()}
    # PRINT THE COMPARISON FIRST, THEN BLESS. --bless used to return before
    # the comparison, so every acceptance paid the same 3.5 minutes twice:
    # "once to judge it, once more to bless it" (2026-08-18).
    rc = 0
    partial = lvset != set(range(1, 23))
    if base_path.exists():
        base = json.loads(base_path.read_text(encoding="utf-8"))
        old_fmt = any(not isinstance(v, dict) for v in base.values())
        if partial and old_fmt:
            print("\nthe baseline is in the old format (no per-level breakdown), "
                  "so a level-limited comparison cannot be made. The next full "
                  "--bless writes the new format")
        else:
            bc = {k: _base_count(v, lvset) for k, v in base.items()}
            gone = sorted(k for k, n in bc.items() if n > 0 and k not in now_n)
            worse = sorted(k for k, v in now_n.items() if v > bc.get(k, 0))
            tag = f" (lv {','.join(str(l) for l in sorted(lvset))} only)" \
                if partial else ""
            print(f"\nvs baseline{tag}: {len(gone)} families gone / "
                  f"{len(worse)} grown")
            for k in gone[:10]:
                print(f"  - {k} ({bc[k]} -> 0)")
            for k in worse[:10]:
                print(f"  + {k} ({bc.get(k, 0)} -> {now_n[k]})")
            rc = 1 if worse else 0
    else:
        print("\nno baseline yet (--bless creates one)")
    if bless:
        base_path.write_text(json.dumps(now, indent=1, sort_keys=True),
                             encoding="utf-8")
        print(f"baseline updated: {base_path} ({len(now)} families)")
        return 0
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
