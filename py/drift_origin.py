#!/usr/bin/env python
"""Report the TICK WHERE a sub-tol drift the fidelity diff cannot catch was BORN.

    python py/drift_origin.py --levels 9 10 --what dvy

fidelity_diff.py reports the first divergence at tol=0.3. But a single tick's
contact test is decided at 0.02px, so a drift hiding below tol surfaces as "the
clamp fires one tick late" (the vy 0.001 grid of 2026-08-07, top of
docs/findings.md). This tool reads the trace/dump that fidelity_diff left behind
and reports

  --what dy    ticks where the y drift jumped     (no jump = pure accumulation)
  --what dvy   ticks where the vy drift jumped    (which event created the drift)
  --what grid  ticks where GD's vy left the 0.001 grid, plus the breakdown

RUN fidelity_diff.py FIRST (and pass the same --tmp).

If not a single jump shows up, the cause is not a rule but accumulated error or
floating-point rounding. If a jump does show up, the mode/onGround/act of that
tick is the suspect.

THE DRIFT PRINTED HERE IS model - GD (fixcensus's `edy` is the other way
round). It is not spelled out in the output, so it is spelled out by the type:
the subtraction goes through gdtas.compare, which will not produce a delta
without naming its direction, its half, and the window both sides cover. Two
consequences that were not true before 2026-09-06:

  * --tmax past the tick the model trace reaches is now refused instead of
    quietly averaging over whatever the replay got to. A mover level replayed
    without --groups dies at a few percent and its trace looks byte-identical
    as far as it went.
  * --half p2 reads the second body's columns on both sides. It cannot be
    made to hold a p2 value against a p1 one.

Without --tmax the window is the ticks both files cover, which is what this
tool always did, and the output is unchanged.
"""

from __future__ import annotations

import argparse
import csv
import sys
from collections import Counter
from decimal import Decimal
from pathlib import Path

_PY = Path(__file__).resolve().parent
sys.path.insert(0, str(_PY))

from gdtas import compare as C                                    # noqa: E402


def _load(p: Path) -> dict[int, dict]:
    if not p.exists():
        return {}
    out: dict[int, dict] = {}
    with p.open(newline="", encoding="utf-8-sig", errors="replace") as f:
        for r in csv.DictReader(f):
            try:
                out[int(r["tick"])] = r
            except (KeyError, ValueError, TypeError):
                continue
    return out


def _pair(tmp: Path, lv: int) -> tuple[dict, dict]:
    return _load(tmp / f"fid_lv{lv}.dump.csv"), _load(tmp / f"fid_lv{lv}.trace.csv")


def _cell(row: dict, kind: str, half: C.Half, name: str) -> str:
    """One field of the printed line, for whichever half is being read."""
    try:
        return row[C.column(kind, half, name)]
    except (C.Refused, KeyError):
        return "?"


def jumps(lv: int, tmp: Path, what: str, thr: float, tmax: int | None,
          limit: int, half: C.Half = C.Half.P1) -> list[str]:
    """Ticks where dy / dvy jumped. Returns the lines to display."""
    gp, mp = tmp / f"fid_lv{lv}.dump.csv", tmp / f"fid_lv{lv}.trace.csv"
    if not gp.exists() or not mp.exists():
        return [f"lv{lv:<3} (no fid_lv{lv}.dump.csv / .trace.csv)"]
    quantity = "y" if what == "dy" else "vy"
    try:
        gd = C.read_table(gp, C.GD)
        md = C.read_table(mp, C.MODEL)
        if not gd.rows or not md.rows:
            return [f"lv{lv:<3} (no fid_lv{lv}.dump.csv / .trace.csv)"]
        both = C.overlap(gd, md)
        # No --tmax means "the ticks both sides have", which is what this tool
        # has always compared. A --tmax IS a request, and a side that stops
        # short of it is a refusal rather than a shorter answer.
        win = both if tmax is None else C.Window(both.t0, tmax)
        diff = C.compare(gd, md, quantity, window=win, half=half,
                         direction=C.MODEL_MINUS_GD)
    except C.Refused as e:
        return [f"lv{lv}: jumps in {what}", f"  REFUSED: {e}"]
    gcol = C.column(C.GD, half, quantity)
    mcol = C.column(C.MODEL, half, quantity)
    lines, prev, n = [], 0.0, 0
    for t in win.ticks():
        gr, mr = gd.row(t), md.row(t)
        d = diff.at(t)
        if abs(d - prev) > thr:
            lines.append(
                f"  t={t:<8}x={float(_cell(gr, C.GD, half, 'x')):<10.1f}"
                f"{_cell(gr, C.GD, half, 'mode'):<7}"
                f"{what} {prev:+.6f} -> {d:+.6f}   "
                f"gd={float(gr[gcol]):<11.4f}model={float(mr[mcol]):<12.6f}"
                f"onG={_cell(gr, C.GD, half, 'ground')} "
                f"grd={_cell(mr, C.MODEL, half, 'ground')} "
                f"act={_cell(mr, C.MODEL, half, 'act')} "
                f"up={_cell(gr, C.GD, half, 'gravity')} "
                f"vsz={float(_cell(gr, C.GD, half, 'vsize')):.2f}")
            n += 1
            if n >= limit:
                lines.append("  ... (truncated)")
                break
        prev = d
    if not lines:
        lines.append(f"  (no jumps = accumulation only. Threshold {thr})")
    return [f"lv{lv}: jumps in {what}"] + lines


def grid(lv: int, tmp: Path, limit: int) -> list[str]:
    """Ticks where GD's vy left the 0.001 grid. Leaving it means a halving just before."""
    g, _ = _pair(tmp, lv)
    if not g:
        return [f"lv{lv:<3} (no fid_lv{lv}.dump.csv)"]
    off, modes, first = 0, Counter(), []
    ticks = sorted(g)
    for t in ticks:
        d = Decimal(g[t]["yvel"])
        rem = (abs(d) * 1000) % 1
        if rem == 0:
            continue
        off += 1
        modes[g[t]["mode"]] += 1
        if len(first) < limit:
            prev = g.get(t - 1)
            first.append(
                f"  t={t:<8}x={float(g[t]['x']):<10.1f}{g[t]['mode']:<7}"
                f"vy={d}  (previous tick {prev['yvel'] if prev else '-'}, "
                f"ratio {float(d) / float(prev['yvel']):.4f})"
                if prev and float(prev["yvel"]) else
                f"  t={t:<8}x={float(g[t]['x']):<10.1f}{g[t]['mode']:<7}vy={d}")
    pct = 100.0 * off / max(len(ticks), 1)
    return ([f"lv{lv}: off the grid {off}/{len(ticks)} ({pct:.2f}%) by mode={dict(modes)}"]
            + first)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--levels", nargs="+", type=int, required=True)
    ap.add_argument("--what", choices=["dy", "dvy", "grid"], default="dvy")
    ap.add_argument("--thr", type=float, default=1e-4,
                    help="smallest difference that counts as a jump. Keep it "
                         "above the float storage error (~1e-6)")
    ap.add_argument("--tmax", type=int, default=None,
                    help="stop at this tick. A trace that does not reach it is "
                         "REFUSED, not silently shortened. Default: the ticks "
                         "both files cover")
    ap.add_argument("--half", choices=["p1", "p2"], default="p1",
                    help="which body to read, on BOTH sides (dual levels)")
    ap.add_argument("--limit", type=int, default=20,
                    help="how many lines to show per level")
    ap.add_argument("--tmp", default=str(_PY.parent / "build" / "fidelity"),
                    help="pass the same one as fidelity_diff.py's --tmp")
    a = ap.parse_args(argv)

    tmp = Path(a.tmp)
    half = C.Half(a.half)
    for lv in a.levels:
        for l in (grid(lv, tmp, a.limit) if a.what == "grid"
                  else jumps(lv, tmp, a.what, a.thr, a.tmax, a.limit, half)):
            print(l)
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
