#!/usr/bin/env python
"""Count from the data WHICH LEVELS a change touching a given mechanic can move.

    python py/affected_levels.py --touch-triggers
    python py/affected_levels.py --types 41 43 --ids 1594
    python py/affected_levels.py --trigger-ids 1346

A regression OFTEN DOES NOT need to run all 21 levels. Levels that never enter
the code path are bit-identical by definition, so running them adds no
information. Measured 2026-08-11: a full regression was launched for a change
that fixed the classification of touch triggers, but the only levels that can
get `mask != 0` are lv19 and lv20, the ones holding triggers with touch=1; the
remaining 19 were a pure loss of 50 minutes.

ALWAYS DECIDE FROM THE DATA. If you drop levels on a guess like "probably only
the flight mode", the reason for dropping them is not left in the record, and
the next person cannot trust that scope.
"""

from __future__ import annotations

import argparse
import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas.paths import LEVEL_DATA

LEVELS = list(range(1, 23))


def objrects_rows(lv: int):
    p = LEVEL_DATA / f"objrects_lv{lv}.txt"
    if not p.exists():
        return
    with p.open(newline="", encoding="utf-8-sig", errors="replace") as fh:
        yield from csv.DictReader(fh)


def trigger_rows(lv: int):
    p = LEVEL_DATA / f"triggers_lv{lv}.txt"
    if not p.exists():
        return
    with p.open(encoding="utf-8-sig", errors="replace") as fh:
        cols = fh.readline().rstrip("\n").split(",")
        for line in fh:
            f = line.rstrip("\n").split(",")
            if len(f) >= len(cols):
                yield dict(zip(cols, f))


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--types", nargs="*", type=int, default=[],
                    help="GameObjectType (the type column of objrects)")
    ap.add_argument("--ids", nargs="*", type=int, default=[],
                    help="object id (the editor's number)")
    ap.add_argument("--trigger-ids", nargs="*", type=int, default=[],
                    help="trigger id (901=Move, 1346=Rotate, 1006/1007=...)")
    ap.add_argument("--touch-triggers", action="store_true",
                    help="levels that have a touch trigger (touch=1)")
    ap.add_argument("--spawn-triggers", action="store_true")
    ap.add_argument("--levels", nargs="*", type=int, default=LEVELS)
    a = ap.parse_args(argv)

    if not any([a.types, a.ids, a.trigger_ids, a.touch_triggers,
                a.spawn_triggers]):
        ap.error("name at least one thing to count")

    hit: dict[int, list[str]] = {}
    for lv in a.levels:
        notes = []
        if a.types or a.ids:
            ct: dict[str, int] = {}
            for r in objrects_rows(lv):
                if a.types and int(r["type"]) in a.types:
                    ct[f"type{r['type']}"] = ct.get(f"type{r['type']}", 0) + 1
                if a.ids and int(r["id"]) in a.ids:
                    ct[f"id{r['id']}"] = ct.get(f"id{r['id']}", 0) + 1
            notes += [f"{k}x{v}" for k, v in sorted(ct.items())]
        if a.touch_triggers or a.spawn_triggers or a.trigger_ids:
            touch = spawn = 0
            tid: dict[str, int] = {}
            for r in trigger_rows(lv):
                if r.get("touch") == "1":
                    touch += 1
                if r.get("spawn") == "1":
                    spawn += 1
                if a.trigger_ids and int(r["id"]) in a.trigger_ids:
                    tid[f"trig{r['id']}"] = tid.get(f"trig{r['id']}", 0) + 1
            if a.touch_triggers and touch:
                notes.append(f"touch={touch}")
            if a.spawn_triggers and spawn:
                notes.append(f"spawn={spawn}")
            notes += [f"{k}x{v}" for k, v in sorted(tid.items())]
        if notes:
            hit[lv] = notes

    for lv in sorted(hit):
        print(f"lv{lv:<3} {'  '.join(hit[lv])}")
    other = [lv for lv in a.levels if lv not in hit]
    print(f"\ncould be affected: {len(hit)} levels "
          f"({' '.join(str(l) for l in sorted(hit)) or '-'})")
    print(f"never enter the code path: {len(other)} levels "
          f"({' '.join(str(l) for l in other) or '-'})")
    # lv22 is unsolved, so it is not a regression target (whether it clears is
    # checked separately)
    todo = [l for l in sorted(hit) if l != 22]
    if todo:
        print("\nregression:\n  python py/cold_regress.py --levels "
              + " ".join(str(l) for l in todo) + " --pool 90 92 93 94 95 96")
    else:
        print("\nregression: **not needed** (no solved level enters the code path)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
