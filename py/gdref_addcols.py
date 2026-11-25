"""gdref_addcols.py -- append columns a newer recording has to a gdref (the GD reference traces), after proving that it is the same run.

`data/gdref/lv<N>.csv` is what `quick_regress.py` anchors its sections on. The 22 official ones were recorded before the dump had the columns `boost`,
`revoff`, `revcar`, `freemode`, `rotch`, `rotidx` and `rotrev` (REF_COLS in quick_regress.py keeps them if a dump has them), so a section anchored on them
cannot be told that the game's Free Mode byte ([layer+0x311]) was set: `freemode_anchor_args` sends nothing for a reference without the column, and the
dp side then does not know that no band clamps the body.

Re-recording needs the game and every level's solution. A newer recording of the same plans (a lab tree such as lab_fm3's gdref, or any directory
of `lv<N>.csv` / `lv<N>.dump.csv`) already has the columns, and the game is deterministic: the same plan gives the same run. This tool takes the columns
from it, row by row on (attempt, tick), and refuses unless the two are the same run -- the same ticks, and equal `x`, `yvel`, `mode`, `dual` and
`upsideDown` on every row and `y` within --tol-y. The columns it already has are written back as they were (the strings, not re-formatted); the new
ones are appended in REF_COLS order. It writes to --out, never over --ref: the reference is read by every instrument at once.

    python py/gdref_addcols.py --src <dir> --out <dir> [--ref <dir>] [--cols freemode ...] [--levels 1 2 ...] [--copy-rest]
    GDSOLVER_GDREF=<out> python py/quick_regress.py ...        # quick reads the new reference

Exit code 0: every level written. 1: a level was refused (named, with the first row that differs). 2: usage.
"""
from __future__ import annotations

import argparse
import csv
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

KEY = ("attempt", "tick")
SAME = ("x", "yvel", "mode", "dual", "upsideDown")   # equal as numbers (or text) on every row
# the order quick_regress.REF_COLS keeps; new columns are appended in it
ORDER = ["boost", "revoff", "revcar", "freemode", "rotch", "rotidx", "rotrev"]


def read_rows(path: Path) -> tuple[list[str], list[list[str]]]:
    with path.open(newline="", encoding="utf-8-sig") as f:
        rows = list(csv.reader(f))
    if not rows:
        raise ValueError(f"{path}: empty")
    return rows[0], rows[1:]


def source_for(src: Path, level: int) -> Path | None:
    for name in (f"lv{level}.csv", f"lv{level}.dump.csv"):
        if (src / name).exists():
            return src / name
    return None


def _num_equal(a: str, b: str) -> bool:
    if a == b:
        return True
    try:
        return float(a) == float(b)
    except ValueError:
        return False


def merge(ref_path: Path, src_path: Path, cols: list[str], tol_y: float) -> tuple[list[str], list[list[str]], str]:
    """(header, rows, reason): reason is "" when the two are the same run, else why not (header / rows are then empty)."""
    rh, rr = read_rows(ref_path)
    sh, sr = read_rows(src_path)
    for c in (*KEY, *SAME, "y"):
        if c not in rh or c not in sh:
            return [], [], f"column {c} missing ({'reference' if c not in rh else 'source'})"
    missing = [c for c in cols if c not in sh]
    if missing:
        return [], [], f"the source has no column {', '.join(missing)}"
    ri = {c: rh.index(c) for c in rh}
    si = {c: sh.index(c) for c in sh}
    skey = {(r[si["attempt"]], r[si["tick"]]): r for r in sr}
    if len(skey) != len(sr):
        return [], [], "the source repeats a (attempt, tick)"
    if len(rr) != len(sr):
        return [], [], f"{len(rr)} reference rows against {len(sr)} source rows"
    new_cols = [c for c in cols if c not in rh]
    out = []
    for n, r in enumerate(rr, 1):
        s = skey.get((r[ri["attempt"]], r[ri["tick"]]))
        if s is None:
            return [], [], f"row {n}: (attempt {r[ri['attempt']]}, tick {r[ri['tick']]}) is not in the source"
        for c in SAME:
            if not _num_equal(r[ri[c]], s[si[c]]):
                return [], [], f"row {n} (tick {r[ri['tick']]}): {c} {r[ri[c]]} in the reference, {s[si[c]]} in the source"
        try:
            dy = abs(float(r[ri["y"]]) - float(s[si["y"]]))
        except ValueError:
            return [], [], f"row {n}: y is not a number"
        if dy > tol_y:
            return [], [], f"row {n} (tick {r[ri['tick']]}): y {r[ri['y']]} in the reference, {s[si['y']]} in the source (> {tol_y})"
        out.append(r + [s[si[c]] for c in new_cols])
    return rh + new_cols, out, ""


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ref", type=Path, default=None, help="the gdref to extend (default: data/gdref of the lab)")
    ap.add_argument("--src", type=Path, required=True, help="a directory of lv<N>.csv or lv<N>.dump.csv with the columns")
    ap.add_argument("--out", type=Path, required=True, help="where the extended copy goes (must not be --ref)")
    ap.add_argument("--cols", nargs="+", default=["freemode"], help="the columns to take (default: freemode)")
    ap.add_argument("--levels", type=int, nargs="+", default=list(range(1, 23)))
    ap.add_argument("--tol-y", type=float, default=1e-3, help="largest |y| difference still taken as the same run (the dumps print 6 and 9 digits)")
    ap.add_argument("--copy-rest", action="store_true", help="also copy the reference's other files (baseline.json, lv<N>.bandtrack.txt, ...)")
    a = ap.parse_args(argv)
    if a.ref is None:
        from gdtas.paths import LEVEL_DATA
        a.ref = LEVEL_DATA / "gdref"
    bad = [c for c in a.cols if c not in ORDER]
    if bad:
        print(f"unknown column(s) {', '.join(bad)}; known: {', '.join(ORDER)}")
        return 2
    cols = [c for c in ORDER if c in a.cols]
    if a.out.resolve() == a.ref.resolve():
        print("--out is the reference itself; the reference is not overwritten")
        return 2
    a.out.mkdir(parents=True, exist_ok=True)
    refused = 0
    for lv in a.levels:
        rp = a.ref / f"lv{lv}.csv"
        sp = source_for(a.src, lv)
        if not rp.exists() or sp is None:
            print(f"lv{lv}: SKIP ({'no reference' if not rp.exists() else 'no source'})")
            continue
        header, rows, why = merge(rp, sp, cols, a.tol_y)
        if why:
            print(f"lv{lv}: REFUSED -- {why}")
            refused += 1
            continue
        tmp = a.out / f"lv{lv}.csv.tmp"
        with tmp.open("w", newline="", encoding="utf-8") as f:
            w = csv.writer(f, lineterminator="\n")
            w.writerow(header)
            w.writerows(rows)
        tmp.replace(a.out / f"lv{lv}.csv")
        nc = [c for c in cols if c not in read_rows(rp)[0]]
        summary = ", ".join(f"{c}: {sum(1 for r in rows if r[header.index(c)] not in ('0', '0.0', ''))} nonzero" for c in nc) or "nothing to add"
        print(f"lv{lv}: {len(rows)} rows, {summary}")
    if a.copy_rest:
        for p in a.ref.iterdir():
            if p.is_file() and not (a.out / p.name).exists():
                shutil.copy2(p, a.out / p.name)
    return 1 if refused else 0


if __name__ == "__main__":
    raise SystemExit(main())
