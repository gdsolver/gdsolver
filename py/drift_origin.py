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


def jumps(lv: int, tmp: Path, what: str, thr: float, tmax: int,
          limit: int) -> list[str]:
    """Ticks where dy / dvy jumped. Returns the lines to display."""
    g, m = _pair(tmp, lv)
    if not g or not m:
        return [f"lv{lv:<3} (no fid_lv{lv}.dump.csv / .trace.csv)"]
    gcol, mcol = ("y", "y") if what == "dy" else ("yvel", "vy")
    lines, prev, n = [], 0.0, 0
    for t in sorted(set(g) & set(m)):
        if t > tmax:
            break
        gr, mr = g[t], m[t]
        d = float(mr[mcol]) - float(gr[gcol])
        if abs(d - prev) > thr:
            lines.append(
                f"  t={t:<8}x={float(gr['x']):<10.1f}{gr['mode']:<7}"
                f"{what} {prev:+.6f} -> {d:+.6f}   "
                f"gd={float(gr[gcol]):<11.4f}model={float(mr[mcol]):<12.6f}"
                f"onG={gr['onGround']} grd={mr['grounded']} act={mr['act']} "
                f"up={gr['upsideDown']} vsz={float(gr['vsize']):.2f}")
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
    ap.add_argument("--tmax", type=int, default=10 ** 9,
                    help="stop at this tick")
    ap.add_argument("--limit", type=int, default=20,
                    help="how many lines to show per level")
    ap.add_argument("--tmp", default=str(_PY.parent / "build" / "fidelity"),
                    help="pass the same one as fidelity_diff.py's --tmp")
    a = ap.parse_args(argv)

    tmp = Path(a.tmp)
    for lv in a.levels:
        for l in (grid(lv, tmp, a.limit) if a.what == "grid"
                  else jumps(lv, tmp, a.what, a.thr, a.tmax, a.limit)):
            print(l)
        print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
