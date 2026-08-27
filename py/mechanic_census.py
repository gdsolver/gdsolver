"""Coverage matrix of which mechanic appears in which official level.

    python py/mechanic_census.py --levels 1-22

Reads objrects_lv<N>.txt (the real-size hitbox rectangles + group counts that
refresh_objrects.py emits). The table for deciding the solver's implementation
order by "what that level forces on you" rather than "in id order".
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas.paths import LEVEL_DATA

# CLASSIFYING BY id ALONE MISJUDGES LEVELS. What the solver implements is the
# object TYPE (8=yellow pad, 10=blue pad, 11=yellow orb, ...), so a level whose
# only "pad" was of an unimplemented type looks implemented in this table.
# Always read this together with the type census below.
TYPE_NAMES = {8: "padY", 9: "padP", 10: "padB", 11: "orbY", 12: "orbP",
              13: "orbB", 14: "mirror?", 4: "grav", 5: "ship", 6: "cube"}

# object ids bundled by the capability the solver has to model
GROUPS: dict[str, tuple[int, ...]] = {
    "ball": (47,),
    "ufo": (111,),
    "wave": (660,),
    "robot": (745,),
    "spider": (1331,),
    "swing": (1933,),
    "mini": (101,),
    "grav": (10, 11),
    "dual": (286, 287),
    "mirror": (45, 46),
    "teleprt": (747,),
    "speed": (200, 201, 202, 203, 1334),
    "pad": (35, 67, 140, 1332),
    "orb": (36, 84, 141, 1022, 1330, 1333, 1704, 1751),
    "dashorb": (1704, 1751),
    "move": (901,),
    "rotate": (1346,),
    "toggle": (1049,),
    "spawn": (1268,),
    "follow": (1347,),
    "touch": (1595,),
}


def parse_levels(spec: str) -> list[int]:
    out: list[int] = []
    for part in spec.replace(",", " ").split():
        if "-" in part[1:]:
            a, b = part.split("-", 1)
            out.extend(range(int(a), int(b) + 1))
        else:
            out.append(int(part))
    return out


def scan(path: Path) -> tuple[dict[int, int], dict[int, int], int]:
    """(count by id, count by type, number of solid/hazard that triggers can move)."""
    ids: dict[int, int] = {}
    types: dict[int, int] = {}
    movable = 0
    for line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
        f = line.split(",")
        if len(f) < 7 or f[0] == "id":
            continue
        ids[int(f[0])] = ids.get(int(f[0]), 0) + 1
        t = int(f[1])
        # a solid/hazard belonging to a group can be driven by a trigger
        if f[1] in ("0", "2") and int(f[6]) > 0:
            movable += 1
        if t not in (0, 2, 7):
            types[t] = types.get(t, 0) + 1
    return ids, types, movable


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--levels", default="1-22", help='e.g. "1-22" or "1 3 5"')
    ap.add_argument("--data-dir", default=str(LEVEL_DATA))
    a = ap.parse_args(argv)

    data_dir = Path(a.data_dir)
    levels = parse_levels(a.levels)
    cols = list(GROUPS) + ["movable"]

    rows: list[dict] = []
    type_census: list[tuple[int, dict[int, int]]] = []
    for lv in levels:
        p = data_dir / f"objrects_lv{lv}.txt"
        if not p.exists():
            continue
        ids, types, movable = scan(p)
        row = {"lv": lv}
        for k, group in GROUPS.items():
            row[k] = sum(ids.get(i, 0) for i in group)
        row["movable"] = movable
        rows.append(row)
        type_census.append((lv, types))

    print("lv  " + "".join(c.rjust(8) for c in cols))
    for r in rows:
        line = f"{r['lv']:<4}"
        for c in cols:
            line += ("." if r[c] == 0 else str(r[c])).rjust(8)
        print(line)

    # greedy cover: the smallest set of levels that touches every mechanic present
    need = {c for c in cols if sum(r[c] for r in rows) > 0}
    have: set[str] = set()
    pick: list[dict] = []
    pool = list(rows)
    while need - have and pool:
        best, best_gain = None, -1
        for r in pool:
            gain = sum(1 for c in need - have if r[c] > 0)
            if gain > best_gain:
                best_gain, best = gain, r
        if best_gain <= 0:
            break
        pick.append(best)
        have |= {c for c in need if best[c] > 0}
        pool = [r for r in pool if r["lv"] != best["lv"]]

    print("\nobject types present (solver implements 0/2/4/5/6/8/10/11 today):")
    for lv, types in type_census:
        s = " ".join(f"{TYPE_NAMES.get(t, f't{t}')}x{types[t]}" for t in sorted(types))
        print(f"  lv{lv:<3} {s}")

    print("\ngreedy cover: " + " -> ".join(f"lv{r['lv']}" for r in pick))
    missing = sorted(need - have)
    if missing:
        print("uncovered: " + " ".join(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main())
