#!/usr/bin/env python3
"""Where can a level be cut in x without a trigger that is kept reading a group that is cut in two?

A development aid for cfg `stagecuts` (docs/LEVEL_SLICE.md, "Stages"). A piece of a staged solve is the
level with everything past the cut gone. Most of the level does not care. A trigger that reads a group
as a POSITION does: a Rotate or Scale about a centre group, a Move to a target group, a Follow of a
group, an Area effect timed off the group as a whole (the rule of src/solver/slice.hpp: "which groups
are read as a position"). Cut so that the trigger is still there (x <= cut) but the group it reads is
partly or wholly gone, and the part that is left behaves differently from the same part of the level.

For one reader (trigger T at x_T, acting on group Gt, reading the groups Gp as positions), a cut c is
unsafe when
    c >= x_T                          the trigger is kept,
    c <  max x of Gp's members        something it reads is dropped,
    c >= min x of Gt's and Gp's members   and something it moves or reads is kept.

This is a PREDICTOR from the level string alone. It does not know which objects the run reads (the game
classes an id by the object it builds; the mod's slice does), so it reads every mover. The mod's own
check is the truth: the log line `stage: hand-off N at x=... - consistent | DISAGREE` compares the first
flight on the next piece with the clearing flight of the piece before it, tick for tick.

    python py/cutsafety.py LEVEL.lvl                       # the unsafe stretches and the gaps between them
    python py/cutsafety.py LEVEL.lvl --check 20000 37300   # each cut: safe, or the readers it splits
    python py/cutsafety.py LEVEL.lvl --suggest 20000,40000 --window 3000   # nearest safe x to each wish

Reads the raw level string (the .lvl files of py/cutlevel.py).
"""
import argparse
import collections
import sys

MOVE, ROTATE, FOLLOW, SCALE = "901", "1346", "1347", "2067"
MOVERS = {MOVE, ROTATE, FOLLOW, SCALE, "1814"}     # 1814: follow player Y
AREA = {"3006", "3007", "3008", "3009", "3010"}     # area move, rotate, scale, fade, tint
ROTGAMEPLAY = {"2899", "2900"}                      # 2900 turns the frame itself: x stops


def parse(path):
    raw = open(path, encoding="latin-1").read()
    objs = []
    for t in raw.split(";")[1:]:
        if not t:
            continue
        kv = t.split(",")
        objs.append(dict(zip(kv[0::2], kv[1::2])))
    return objs


def num(d, k, default=0.0):
    try:
        return float(d.get(k, default))
    except ValueError:
        return default


def groups_of(d, key="57"):
    g = d.get(key)
    return [int(z) for z in g.split(".") if z] if g else []


def readers(objs):
    """[(x_T, id, Gt, [Gp...], kind)] for every trigger that reads a group as a position."""
    out = []
    for d in objs:
        i = d.get("1")
        if i not in MOVERS and i not in AREA:
            continue
        gt = int(num(d, "51"))
        if gt <= 0:
            continue
        gp = []
        c = int(num(d, "71"))
        if c > 0:
            gp.append(c)                                   # centre / follow target
        tm = int(num(d, "395"))
        if tm > 0:
            gp.append(tm)                                  # move-to-target reference
        if i in AREA or num(d, "100") == 1 or num(d, "397") == 1:
            gp.append(gt)                                  # move-to-target / dynamic / area: the group itself
        if gp:
            out.append((num(d, "2"), i, gt, gp))
    return out


def merge(intervals):
    out = []
    for lo, hi in sorted(intervals):
        if out and lo <= out[-1][1]:
            out[-1][1] = max(out[-1][1], hi)
        else:
            out.append([lo, hi])
    return out


def analyse(objs):
    members = collections.defaultdict(list)
    for d in objs:
        x = num(d, "2")
        for g in groups_of(d) + groups_of(d, "274"):
            members[g].append(x)
    rows = []
    for x_t, i, gt, gp in readers(objs):
        gpx = [x for g in gp for x in members.get(g, [])]
        gtx = members.get(gt, [])
        if not gpx:
            continue
        lo = max(x_t, min(gtx + gpx))
        hi = max(gpx)
        if hi > lo:
            rows.append({"trig": x_t, "id": i, "target": gt, "reads": gp, "lo": lo, "hi": hi})
    rot = sorted(num(d, "2") for d in objs if d.get("1") in ROTGAMEPLAY)
    return rows, rot


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("level")
    ap.add_argument("--check", nargs="*", type=float, default=[])
    ap.add_argument("--suggest", default="")
    ap.add_argument("--window", type=float, default=3000.0)
    ap.add_argument("--margin", type=float, default=120.0, help="px kept clear on each side of a stretch")
    ap.add_argument("--list", action="store_true", help="print every reader whose stretch is not empty")
    a = ap.parse_args()

    objs = parse(a.level)
    rows, rot = analyse(objs)
    maxx = max(num(d, "2") for d in objs)
    stretches = merge([(r["lo"] - a.margin, r["hi"] + a.margin) for r in rows])
    frames = merge([(x - 400.0, x + 400.0) for x in rot])
    unsafe = merge(stretches + frames)
    print(f"{a.level}: {len(objs)} objects, last x {maxx:.0f}; {len(rows)} readers with a stretch of unsafe "
          f"cuts; {len(rot)} rotate-gameplay/option triggers")
    if a.list:
        for r in sorted(rows, key=lambda r: r["lo"]):
            print(f"  trigger {r['id']:>5} at x={r['trig']:8.0f} on group {r['target']:>5} reads {r['reads']}: "
                  f"unsafe cuts x {r['lo']:.0f}..{r['hi']:.0f} ({r['hi'] - r['lo']:.0f} px)")
    cov = sum(hi - lo for lo, hi in unsafe)
    print(f"unsafe stretches: {len(unsafe)}, {cov:.0f} px of {maxx:.0f} ({100 * cov / max(maxx, 1):.1f}%)")
    gaps = []
    prev = 0.0
    for lo, hi in unsafe:
        if lo > prev:
            gaps.append((prev, lo))
        prev = max(prev, hi)
    if prev < maxx:
        gaps.append((prev, maxx))
    print("safe gaps (width >= 500 px):")
    for lo, hi in gaps:
        if hi - lo >= 500:
            print(f"  {lo:8.0f} .. {hi:8.0f}  ({hi - lo:6.0f} px)")
    for c in a.check:
        hit = [r for r in rows if r["lo"] <= c < r["hi"]]
        fr = [x for x in rot if abs(x - c) < 400]
        if not hit and not fr:
            print(f"cut x={c:.0f}: no reader is split")
        else:
            print(f"cut x={c:.0f}: splits {len(hit)} reader(s)" + (" and sits in a rotated-gameplay stretch" if fr else ""))
            for r in sorted(hit, key=lambda r: r["lo"])[:12]:
                print(f"    trigger {r['id']} at x={r['trig']:.0f} on group {r['target']} reads {r['reads']}: "
                      f"{r['lo']:.0f}..{r['hi']:.0f}")
    if a.suggest:
        for w in a.suggest.split(","):
            want = float(w)
            best = None
            for lo, hi in gaps:
                if hi - lo < 2 * a.margin:
                    continue
                c = min(max(want, lo + a.margin), hi - a.margin)
                if best is None or abs(c - want) < abs(best - want):
                    best = c
            if best is None or abs(best - want) > a.window:
                print(f"wish x={want:.0f}: no safe x within {a.window:.0f} px")
            else:
                print(f"wish x={want:.0f}: {best:.0f} ({best - want:+.0f})")


if __name__ == "__main__":
    sys.exit(main())
