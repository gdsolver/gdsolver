# -*- coding: utf-8 -*-
"""brief-017 part A: where is a truth run worth doing?

    python py/sections.py --levels 22 --census C:\\GD-lab\\data\\census_div.json

A verified solution only ever walks where the model believed it could, so the
places the model is WRONG do not all show up the same way. Four sources see
different halves of it, and the section list is their union:

  fixups     dp's own repairs -- where GD disagreed with the model and the loop
             had to learn something. Carries a kill flag.
  census     the first divergence of every 400-tick section (fixcensus --json;
             the blessed baseline aggregates families and drops the ticks, so
             the json is what carries WHERE).
  deaths     `dpsolve: iter N: death t=..` -- the round-by-round wall, which
             includes the kill-only deaths that leave no fixup.
  veto       the deadband boxes the loop declared impassable. THIS IS THE ONLY
             SOURCE THAT SEES OVER-KILL: where the model wrongly forbids an
             area, the plan routes around it, so no fixup and no death is ever
             recorded there -- the veto is the only trace left.

Derive, do not maintain: the table is a cache of files that already exist, so it
is rebuilt rather than edited. The log regexes are imported from
itermap_from_log rather than copied, because two parsers for one log format
drift and the one that drifts is the one nobody is looking at.

Output: sections_lv<N>.json, a list of {t0, t1, sources, points, priority}.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas.paths import DATA, LEVEL_DATA                      # noqa: E402
from itermap_from_log import RE_ITER, RE_VETO                 # noqa: E402

# The window around a point of interest. A divergence is the FIRST tick the two
# disagree, so the interesting state is behind it and the consequence ahead:
# one second before, two after, at 240 ticks per second.
PRE, POST = 240, 480
# Points nearer than this belong to the same window; further apart they are
# separate work. Needed because the fixup log is a HISTORY -- lv22 holds 5,900
# rows over 110 iterations at 1,261 distinct ticks, whose median gap is ONE, and
# padding each of them by PRE/POST merged the level into four windows, the first
# 13,540 ticks wide. A window that size is not a section anyone can DP.
GAP = 120
# ...and no window is allowed past this, because the cost is per tick: the
# yardstick is 60,000 restores ~ 2.5-3 min for a window, and a window that
# swallows a third of the level swallows the night with it. Longer clusters are
# CUT, not dropped -- the pieces keep their own sources and rank on their own.
MAX_SPAN = 1200

RE_FIXUP_LOG = re.compile(r"\biter=(\d+) t=(\d+) x=(-?[\d.]+)")


def from_fixups(lv: int) -> list[tuple[int, str]]:
    p = LEVEL_DATA / f"fixups_log_lv{lv}.txt"
    out = []
    if not p.exists():
        return out
    for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
        m = RE_FIXUP_LOG.search(line)
        if m:
            out.append((int(m.group(2)), "fixup"))
    return out


def from_census(path: Path, lv: int) -> list[tuple[int, str]]:
    if not path or not path.exists():
        return []
    return [(int(d["t"]), "census")
            for d in json.loads(path.read_text(encoding="utf-8"))
            if int(d["lv"]) == lv]


def from_coldlog(lv: int) -> tuple[list[tuple[int, str]], list[tuple[float, float]]]:
    """Deaths (ticks) and vetoed x-boxes, from the level's cold log."""
    ticks: list[tuple[int, str]] = []
    boxes: list[tuple[float, float]] = []
    for p in (DATA / f"coldlog_lv{lv}.txt", LEVEL_DATA / f"coldlog_lv{lv}.txt"):
        if not p.exists():
            continue
        for line in p.read_text(encoding="utf-8", errors="replace").splitlines():
            m = RE_ITER.match(line)
            if m:
                ticks.append((int(m.group(2)), "death"))
                continue
            v = RE_VETO.match(line)
            if v:
                boxes.append((float(v.group(1)), float(v.group(2))))
        break
    return ticks, boxes


def veto_ticks(lv: int, boxes, track: Path | None = None) -> list[tuple[int, str]]:
    """A veto is recorded in x, and the sections are in ticks. Translate through
    a trajectory: the first tick the run is inside the box.

    The trajectory is brief-017 part B's own dump.csv when one is given -- that
    pass already writes tick, x and y for the whole level, so the conversion
    does not depend on whether a level happens to have a gdref. gdref is the
    fallback, and a level with neither has its boxes REPORTED AND DROPPED
    rather than guessed at: veto is the only source that sees over-kill, so
    losing it silently would lose exactly the half the section list exists for.
    """
    src = track if (track and track.exists()) else \
        (LEVEL_DATA / "gdref" / f"lv{lv}.csv")
    if not boxes or not src.exists():
        return []
    import csv
    hits: list[tuple[int, str]] = []
    seen = set()
    with open(src, encoding="utf-8-sig") as f:
        for r in csv.DictReader(f):
            x = float(r["x"])
            for i, (a, b) in enumerate(boxes):
                if i not in seen and a <= x <= b:
                    seen.add(i)
                    hits.append((int(r["tick"]), "veto"))
    return hits


def windows(points: list[tuple[int, str]]) -> list[dict]:
    """Cluster the points, pad, and cut anything too long to run.

    Clustered on the GAP between points rather than by merging padded spans:
    padding first and merging after is what turned lv22 into four windows, one
    of them two thirds of the level.
    """
    if not points:
        return []
    pts = sorted(points)
    groups: list[list[tuple[int, str]]] = [[pts[0]]]
    for p in pts[1:]:
        if p[0] - groups[-1][-1][0] <= GAP:
            groups[-1].append(p)
        else:
            groups.append([p])

    out: list[dict] = []
    for g in groups:
        lo, hi = max(0, g[0][0] - PRE), g[-1][0] + POST
        src = defaultdict(int)
        for _t, s in g:
            src[s] += 1
        # cut, keeping each piece's own share of the sources rather than
        # pretending the whole cluster's evidence applies to every piece
        n = max(1, -(-(hi - lo) // MAX_SPAN))
        step = -(-(hi - lo) // n)
        for i in range(n):
            a = lo + i * step
            b = min(hi, a + step)
            inside = [s for t, s in g if a <= t <= b]
            piece = defaultdict(int)
            for s in (inside or list(src)):
                piece[s] += 1
            out.append({"t0": a, "t1": b, "sources": dict(piece),
                        "points": len(inside), "cut": n > 1})
    for w in out:
        # a window several sources agree on is worth more than a busy one only
        # one source sees -- the sources are what see different halves. veto is
        # weighted up because it is the ONLY witness to over-kill.
        bonus = 500 if "veto" in w["sources"] else 0
        w["priority"] = len(w["sources"]) * 1000 + bonus + min(w["points"], 499)
    return sorted(out, key=lambda w: -w["priority"])


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--levels", type=int, nargs="+", required=True)
    ap.add_argument("--census", default="", help="fixcensus --json output")
    ap.add_argument("--track", default="",
                    help="dump.csv from part B's pass; used to place the veto "
                         "boxes in time (falls back to gdref)")
    ap.add_argument("--out-dir", default=str(LEVEL_DATA))
    a = ap.parse_args(argv)
    cen = Path(a.census) if a.census else None
    trk = Path(a.track) if a.track else None

    for lv in a.levels:
        pts = from_fixups(lv) + from_census(cen, lv)
        deaths, boxes = from_coldlog(lv)
        pts += deaths
        vt = veto_ticks(lv, boxes, trk)
        pts += vt
        wins = windows(pts)
        per = defaultdict(int)
        for _t, s in pts:
            per[s] += 1
        out = Path(a.out_dir) / f"sections_lv{lv}.json"
        out.write_text(json.dumps(wins, indent=1), encoding="utf-8")
        print(f"lv{lv}: {len(pts)} points "
              f"({' '.join(f'{k}x{v}' for k, v in sorted(per.items()))})"
              f" -> {len(wins)} windows -> {out.name}")
        if boxes and not vt:
            print(f"   {len(boxes)} veto boxes could not be placed in time "
                  f"(no gdref for lv{lv}) -- over-kill coverage is incomplete")
    return 0


if __name__ == "__main__":
    sys.exit(main())
