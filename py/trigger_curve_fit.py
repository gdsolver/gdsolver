"""Verify the closed-form formula for moving geometry against a real recording
(the foundation for opening doors without any recording).

    python py/trigger_curve_fit.py --level 20 \
        --groups boot_lv20.groups.txt --dump boot_lv20.dump.csv

Pass the grouptrace and dump.csv OF THE SAME RUN (a single nodeath=1
grouptrace=1 bootstrap emits both). Mixing two runs with different time axes
puts the phase out.

What it does: from triggers_lv<N>.txt + objgroups_lv<N>.txt it assembles "which
trigger moves which uid by how much (offset / duration / easing)" with the same
rules as leveldp, then compares the closed-form prediction

    position(t) = base + offset * ease(kind, rate, (t - t0) / (dur * 240))
    t0 = "the first tick at which the player's x crosses the trigger's cx" + 1

against the real recording and reports the maximum residual per easing kind.

Why this is needed: a recording rides on "the time axis of the run it was taken
from", so on a different run the phase does not line up (the 1,957-tick wall of
update 51). If the closed form matches the measurement, moving geometry can be
placed exactly from static data alone, with no recording.

MEASURED (2026-08-09, the 249 objects of lv20): maximum residual 0.004px with
the formula above. There were two traps -- the duration is NOT `round(dur*240)`
but dur*240 VERBATIM (off by 4.5px on lv20's 240px door), and the delay is 1
tick, not 2 (the "2" was an artefact of grouptrace not writing anything below
0.05px).
"""

from __future__ import annotations

import argparse
import math
import sys
from collections import defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from gdtas.paths import LEVEL_DATA

# GD's EasingType (solver.hpp dumps these raw integers)
EASE_NAME = {
    0: "None", 1: "EaseInOut", 2: "EaseIn", 3: "EaseOut",
    4: "ElasticInOut", 5: "ElasticIn", 6: "ElasticOut",
    7: "BounceInOut", 8: "BounceIn", 9: "BounceOut",
    10: "ExpInOut", 11: "ExpIn", 12: "ExpOut",
    13: "SineInOut", 14: "SineIn", 15: "SineOut",
    16: "BackInOut", 17: "BackIn", 18: "BackOut",
}

# Relation between the fire tick and the player-x crossing (measured; see above)
FIRE_DELAY = 1


def _bounce_out(u: float) -> float:
    if u < 1.0 / 2.75:
        return 7.5625 * u * u
    if u < 2.0 / 2.75:
        u -= 1.5 / 2.75
        return 7.5625 * u * u + 0.75
    if u < 2.5 / 2.75:
        u -= 2.25 / 2.75
        return 7.5625 * u * u + 0.9375
    u -= 2.625 / 2.75
    return 7.5625 * u * u + 0.984375


def _elastic_out(u: float, period: float) -> float:
    if u <= 0.0 or u >= 1.0:
        return u
    s = period / 4.0
    return 2.0 ** (-10.0 * u) * math.sin((u - s) * math.pi * 2.0 / period) + 1.0


def _elastic_in(u: float, period: float) -> float:
    if u <= 0.0 or u >= 1.0:
        return u
    s = period / 4.0
    u -= 1.0
    return -(2.0 ** (10.0 * u)) * math.sin((u - s) * math.pi * 2.0 / period)


def ease(kind: int, rate: float, u: float) -> float:
    """Same convention as cocos2d-x's CCEase* (In=t^rate, Out=t^(1/rate)).

    0/1 are confirmed to within a 0.004px residual over lv20's 249 objects.
    2/3 and the Elastic/Bounce families are cocos' formulas transcribed as-is
    (for any family not yet verified, fixing this one function fixes every path).
    """
    u = 0.0 if u < 0.0 else (1.0 if u > 1.0 else u)
    p = rate if rate > 0.0 else 2.0
    if kind == 0:
        return u
    if kind == 1:
        t = 2.0 * u
        return 0.5 * t ** p if t < 1.0 else 1.0 - 0.5 * (2.0 - t) ** p
    if kind == 2:
        return u ** p
    if kind == 3:
        return u ** (1.0 / p)
    if kind in (4, 5, 6):          # Elastic InOut / In / Out
        period = p if p > 0.0 else 0.3
        if kind == 6:
            return _elastic_out(u, period)
        if kind == 5:
            return _elastic_in(u, period)
        return (_elastic_in(2.0 * u, period) * 0.5 if u < 0.5
                else _elastic_out(2.0 * u - 1.0, period) * 0.5 + 0.5)
    if kind in (7, 8, 9):          # Bounce InOut / In / Out
        if kind == 9:
            return _bounce_out(u)
        if kind == 8:
            return 1.0 - _bounce_out(1.0 - u)
        return ((1.0 - _bounce_out(1.0 - 2.0 * u)) * 0.5 if u < 0.5
                else _bounce_out(2.0 * u - 1.0) * 0.5 + 0.5)
    if kind in (10, 11, 12):       # Exponential InOut / In / Out
        if kind == 12:
            return 1.0 - 2.0 ** (-10.0 * u)
        if kind == 11:
            return 0.0 if u == 0.0 else 2.0 ** (10.0 * (u - 1.0))
        return (0.5 * 2.0 ** (10.0 * (2.0 * u - 1.0)) if u < 0.5
                else 0.5 * (2.0 - 2.0 ** (-10.0 * (2.0 * u - 1.0))))
    if kind in (13, 14, 15):       # Sine InOut / In / Out
        if kind == 15:
            return math.sin(u * math.pi / 2.0)
        if kind == 14:
            return 1.0 - math.cos(u * math.pi / 2.0)
        return -0.5 * (math.cos(math.pi * u) - 1.0)
    if kind in (16, 17, 18):       # Back InOut / In / Out
        o = 1.70158
        if kind == 18:
            u -= 1.0
            return u * u * ((o + 1.0) * u + o) + 1.0
        if kind == 17:
            return u * u * ((o + 1.0) * u - o)
        o *= 1.525
        u *= 2.0
        if u < 1.0:
            return 0.5 * (u * u * ((o + 1.0) * u - o))
        u -= 2.0
        return 0.5 * (u * u * ((o + 1.0) * u + o) + 2.0)
    return u


def load_triggers(path: Path):
    rows = {}
    with path.open() as f:
        f.readline()
        for line in f:
            c = line.rstrip("\n").split(",")
            if len(c) < 13:
                continue
            r = {
                "uid": int(c[0]), "id": int(c[1]),
                "cx": float(c[2]), "cy": float(c[3]),
                "target": int(c[6]), "center": int(c[7]),
                "touch": int(c[8]), "spawn": int(c[9]),
                "dur": float(c[10]), "ox": float(c[11]), "oy": float(c[12]),
                "ease": int(c[13]) if len(c) > 13 else 0,
                "erate": float(c[14]) if len(c) > 14 else 2.0,
                "lockx": int(c[15]) if len(c) > 15 else 0,
                "locky": int(c[16]) if len(c) > 16 else 0,
            }
            rows[r["uid"]] = r
    return rows


def load_groups_map(path: Path):
    by_group = defaultdict(list)
    with path.open() as f:
        f.readline()
        for line in f:
            parts = line.split()
            if not parts:
                continue
            uid = int(parts[0])
            for g in parts[1:]:
                by_group[int(g)].append(uid)
    return by_group


def resolve_controls(trig, by_group):
    """Autonomous MOVE triggers -> {uid: (dx, dy, durTicks, ease, erate, lock)}.

    Walks the graph the same way as leveldp's loadAutoTriggers (only a hop that
    actually moves contributes a duration). The easing is taken from "the
    longest moving hop" (same rule as duration).
    durTicks is NOT ROUNDED (the real number dur*240).
    """
    out = {}
    for T in trig.values():
        if T["touch"] or T["spawn"] or T["target"] == 0:
            continue
        if T["id"] != 901:
            continue
        moves = (T["ox"] != 0.0 or T["oy"] != 0.0)
        stack = [(T["target"], T["ox"], T["oy"],
                  T["dur"] * 240.0 if moves else 0.0,
                  T["ease"] if moves else 0, T["erate"] if moves else 2.0,
                  T["lockx"] or T["locky"])]
        ctl = {}
        guard = 0
        while stack and guard < 4096:
            guard += 1
            grp, dx, dy, dur, ek, er, lock = stack.pop()
            for uid in by_group.get(grp, ()):
                t2 = trig.get(uid)
                if t2 is not None:
                    if t2["target"] == 0:
                        continue
                    m2 = (t2["ox"] != 0.0 or t2["oy"] != 0.0)
                    d2 = t2["dur"] * 240.0 if m2 else 0.0
                    if m2 and d2 >= dur:
                        nk, nr = t2["ease"], t2["erate"]
                    else:
                        nk, nr = ek, er
                    stack.append((t2["target"], dx + t2["ox"], dy + t2["oy"],
                                  max(dur, d2), nk, nr,
                                  lock or t2["lockx"] or t2["locky"]))
                else:
                    ctl[uid] = (dx, dy, dur, ek, er, lock)
        if ctl:
            out[T["uid"]] = ctl
    return out


def load_samples(path: Path, want):
    """Pick just the (t, cx, cy) of the wanted uids out of the grouptrace."""
    sm = defaultdict(list)
    with path.open() as f:
        f.readline()
        for line in f:
            c = line.split(",", 5)
            if len(c) < 5:
                continue
            uid = int(c[1])
            if uid not in want:
                continue
            sm[uid].append((int(c[0]), float(c[2]), float(c[3])))
    for v in sm.values():
        v.sort()
    return sm


def load_player_x(path: Path):
    """The x of attempt=1 in dump.csv. Returned as a list sorted by ascending tick."""
    xs = []
    with path.open() as f:
        f.readline()
        for line in f:
            c = line.split(",", 5)
            if len(c) < 4 or c[1] != "1":
                continue
            xs.append((int(c[2]), float(c[3])))
    xs.sort()
    return xs


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--level", type=int, required=True)
    ap.add_argument("--groups", required=True)
    ap.add_argument("--dump", required=True,
                    help="the dump.csv **of the same run** (needed to derive "
                         "the crossing tick)")
    ap.add_argument("--data-dir", default=str(LEVEL_DATA))
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args(argv)

    data = Path(a.data_dir)
    trig = load_triggers(data / f"triggers_lv{a.level}.txt")
    by_group = load_groups_map(data / f"objgroups_lv{a.level}.txt")
    ctls = resolve_controls(trig, by_group)

    xs = load_player_x(Path(a.dump))

    def crossing(cx):
        for t, x in xs:
            if x >= cx:
                return t
        return None

    # Restrict to uids moved by exactly one trigger (superposition is another story)
    owner = defaultdict(list)
    for tuid, ctl in ctls.items():
        for uid in ctl:
            owner[uid].append(tuid)
    solo = {uid: (t[0], ctls[t[0]][uid]) for uid, t in owner.items() if len(t) == 1}
    print(f"lv{a.level}: {len(ctls)} autonomous MOVE triggers, "
          f"{len(owner)} controlled uids, {len(solo)} of them single-owner")

    sm = load_samples(Path(a.groups), set(solo))
    stat = defaultdict(lambda: [0, 0.0, 0.0, None])   # (kind,rate) -> [n,max,disp,uid]
    skipped = defaultdict(int)
    for uid, (tuid, (dx, dy, dur, ek, er, lock)) in sorted(solo.items()):
        s = sm.get(uid)
        if not s or len(s) < 3:
            skipped["no recording"] += 1
            continue
        if lock:
            skipped["lockToPlayer"] += 1
            continue
        if dur <= 0 or (abs(dx) < 0.5 and abs(dy) < 0.5):
            skipped["zero movement or duration"] += 1
            continue
        cr = crossing(trig[tuid]["cx"])
        if cr is None:
            skipped["never crossed"] += 1
            continue
        t0 = cr + FIRE_DELAY
        bx, by = s[0][1], s[0][2]
        worst = disp = 0.0
        for t, cx, cy in s:
            e = ease(ek, er, (t - t0) / dur)
            worst = max(worst, abs(cx - (bx + dx * e)), abs(cy - (by + dy * e)))
            disp = max(disp, abs(cx - bx), abs(cy - by))
        st = stat[(ek, er)]
        st[0] += 1
        if worst > st[1]:
            st[1] = worst
            st[3] = uid
        st[2] = max(st[2], disp)
        if a.verbose:
            print(f"  uid={uid:6d} ease={EASE_NAME.get(ek, ek)}({er}) "
                  f"off=({dx:.1f},{dy:.1f}) dur={dur:.2f} t0={t0} n={len(s)} "
                  f"maxres={worst:.3f}px")

    print("\n easing        rate   n   worst resid   max travel  worst uid")
    for (ek, er), (n, res, disp, uid) in sorted(stat.items()):
        print(f" {EASE_NAME.get(ek, ek):<13} {er:<5} {n:>3}  {res:>8.3f}  "
              f"{disp:>8.1f}  {uid}")
    if skipped:
        print("\n excluded: " + "  ".join(f"{k}={v}" for k, v in skipped.items()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
