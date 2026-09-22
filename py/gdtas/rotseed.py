"""The rotation queue's seed at an anchor, derived from a GD recording.

The loop hands the rotation queue (`--rotqueue --startrotq`) only to the solver
calls whose seed the attempt's own recording fixes exactly: cfg dprotseed, on
at level A by default, is `rotseed::seedFor` in src/mod/repair.hpp, and it
comes paired with the touch Toggle rule (`--rotqtoggle --touchseed`,
`touchSeedArg`). The section instruments (quick_regress, fixcensus) anchor on
the reference recording instead of an attempt of their own, so this is the
same derivation in Python for their `--rotseed` switch.

It is kept close to the C++ on purpose -- same names, same order of tests --
so the two can be read side by side. Only level A is ported: the reference is
one attempt, so level F's "an earlier attempt showed it" has nothing to read.

The queue's ORDER is dp's own (buildRotQueue). It is read from the `rotq:`
table a `--rotqueue` call prints, not re-derived here, for the same reason the
loop takes it from the solve outcome. Positions and object ids come from the
level's rotgameplay table, because the printed table rounds positions.
"""
from __future__ import annotations

import re
import subprocess
import tempfile
from pathlib import Path

_QUEUE: dict[int, list[dict]] = {}
_TOGGLES: dict[int, list[dict]] = {}

# dp's playerHalf for the touch test, as touchSeedArg spells it: wave, spider,
# everything else; mini, full.
_WAVE, _SPIDER = 4, 6


def queue(level: int, exe: Path, level_data: Path) -> list[dict]:
    """dp's queue for `level`, in consumption order per channel. [] = none."""
    if level in _QUEUE:
        return _QUEUE[level]
    rg = level_data / f"rotgameplay_lv{level}.txt"
    out: list[dict] = []
    if rg.exists():
        exact: dict[int, tuple[float, float, int]] = {}
        lines = rg.read_text(encoding="utf-8-sig", errors="replace").splitlines()
        for ln in lines[1:]:
            c = ln.split(",")
            if len(c) >= 4:
                try:
                    exact[int(c[0])] = (float(c[2]), float(c[3]), int(c[1]))
                except ValueError:
                    continue
        with tempfile.TemporaryDirectory(prefix="rotq_") as td:
            argv = [str(exe), str(level_data / f"objrects_lv{level}.txt"),
                    "--rotgameplay", str(rg), "--rotqueue",
                    "--out", str(Path(td) / "probe"), "--horizon", "1"]
            trig = level_data / f"triggers_lv{level}.txt"
            grp = level_data / f"objgroups_lv{level}.txt"
            if trig.exists() and grp.exists():
                argv += ["--triggers", str(trig), "--objgroups", str(grp)]
            r = subprocess.run(argv, capture_output=True, text=True, timeout=300)
        ch = -1
        for ln in r.stdout.splitlines():
            m = re.match(r"\s+rotq ch=(\d+) ", ln)
            if m:
                ch = int(m.group(1))
                continue
            m = re.match(r"\s+\[\d+\] uid=(\d+) ord=-?\d+ \([-\d.]+,[-\d.]+\) swarm=(\d+) "
                         r"swch=(-?\d+) chanOnly=(\d+) gnddir=(-?\d+)", ln)
            if m and ch >= 0:
                uid = int(m.group(1))
                if uid not in exact:
                    continue
                px, py, oid = exact[uid]
                out.append({"ch": ch, "uid": uid, "px": px, "py": py,
                            "swarm": int(m.group(2)), "swch": int(m.group(3)),
                            "chanOnly": int(m.group(4)), "gnddir": int(m.group(5)),
                            "id": oid})
    _QUEUE[level] = out
    return out


def _visible(e: dict) -> bool:
    return e["id"] == 2900 and not e["chanOnly"]


def _passed(gframe: int, x: float, y: float, e: dict, rev: bool) -> bool:
    vertical = (gframe & 1) != 0
    ref = y if vertical else x
    p = e["py"] if vertical else e["px"]
    return (ref <= p) if rev else (p <= ref)


def _near2(e: dict, x: float, y: float) -> bool:
    return min(abs(e["px"] - x), abs(e["py"] - y)) <= 2.0


def _row(rows: dict, t: int):
    r = rows.get(t)
    if r is None:
        return None
    try:
        return int(float(r["gframe"])), float(r["x"]), float(r["y"])
    except (KeyError, ValueError, TypeError):
        return None


def _crossings2899(rows: dict, q: list[dict]) -> list[tuple[int, int, bool]]:
    """crossings2899: (tick, uid, ctrlOff changed within one tick) for every
    crossing of a 2899's point on the axis of the frame being travelled."""
    ticks = sorted(rows)
    chg = []
    prev = None
    for t in ticks:
        r = rows[t]
        c = r.get("ctrlOff")
        if prev is not None and c != prev:
            chg.append(t)
        prev = c
    out = []
    prev_r = None
    for t in ticks:
        rr = _row(rows, t)
        if rr is None:
            continue
        if prev_r is not None:
            vertical = (prev_r[0] & 1) != 0
            c0 = prev_r[2] if vertical else prev_r[1]
            c1 = rr[2] if vertical else rr[1]
            for e in q:
                if e["id"] != 2899:
                    continue
                p = e["py"] if vertical else e["px"]
                if c0 != c1 and (c0 - p) * (c1 - p) <= 0.0:
                    changed = any(t - 1 <= c <= t + 1 for c in chg)
                    out.append((t, e["uid"], changed))
        prev_r = rr
    return out


def seed_for(rows: dict, t0: int, q: list[dict], level: int = 1) -> tuple[str, str, str]:
    """(class, why, seed) at `level` (1 = A, 2 = E). Only class "exact" carries a seed."""
    if not q or t0 <= 0:
        return "unavailable", "no queue", ""
    by = [[e for e in q if e["ch"] == c] for c in range(16)]
    chan, rev, ptr, spent = 0, [False] * 16, [0] * 16, []
    skipped: list[tuple[int, int]] = []
    timeline = [(0, 0)]
    pg = -1
    ticks = sorted(t for t in rows if t <= t0)
    for t in ticks:
        rr = _row(rows, t)
        if rr is None:
            continue
        g, x, y = rr
        if pg >= 0 and g != pg:
            lst = by[chan]
            k = ptr[chan]
            sk = []
            while k < len(lst) and not _visible(lst[k]):
                if lst[k]["swarm"]:
                    return ("ambiguous", f"channel {chan} switched by unseen uid "
                                         f"{lst[k]['uid']} before t={t}", "")
                sk.append(lst[k]["uid"])
                k += 1
            if k >= len(lst) or not _near2(lst[k], x, y):
                return ("underivable", f"gframe change at t={t} is not channel {chan}'s "
                                       f"next visible entry", "")
            e = lst[k]
            for u in sk:
                skipped.append((t, u))
                spent.append(u)
            spent.append(e["uid"])
            ptr[chan] = k + 1
            if e["swarm"] and 0 <= e["swch"] <= 15:
                chan = e["swch"]
                rev[chan] = 0 <= e["gnddir"] - 2 < 2
                timeline.append((t, chan))
        pg = g
    last = None
    for t in reversed(ticks):
        last = _row(rows, t)
        if last is not None:
            break
    if last is None:
        return "underivable", "no recorded row at or before t0", ""
    lg, lx, ly = last
    for e in by[chan][ptr[chan]:]:
        if _passed(lg, lx, ly, e, rev[chan]):
            return "ambiguous", f"C2 active channel {chan} uid {e['uid']} passed", ""
    cr = _crossings2899(rows, q) if level >= 2 else []

    def active_at(t: int) -> int:
        c = 0
        for tt, ch in timeline:
            if tt <= t:
                c = ch
        return c

    for t, u in skipped:   # C1: a skipped entry needs a crossing with ctrlOff changing
        if not any(x[1] == u and x[0] <= t and x[2] for x in cr):
            return "ambiguous", f"C1 uid {u} passed over unseen before t={t}", ""
    later = sorted(t for t in rows if t > t0)
    for c in range(16):
        if c == chan:
            continue
        for e in by[c][ptr[c]:]:
            if not _passed(lg, lx, ly, e, rev[c]):
                continue
            ok = False
            if _visible(e):   # A: it fires visibly later in this same recording
                pg = lg
                for t in later:
                    rr = _row(rows, t)
                    if rr is None:
                        continue
                    if rr[0] != pg and _near2(e, rr[1], rr[2]):
                        ok = True
                        break
                    pg = rr[0]
            if not ok and level >= 2 and e["id"] == 2899:   # E: crossed inactive, not consumed
                ok = any(x[1] == e["uid"] and x[0] <= t0 and not x[2] and active_at(x[0]) != c
                         for x in cr)
            if not ok:
                return "ambiguous", f"C2 channel {c} uid {e['uid']} passed, no witness", ""
    mask = 0
    for c in range(16):
        if rev[c]:
            mask |= 1 << c
    seed = f"{chan},{mask:x}" + "".join(f",{u}" for u in spent)
    return "exact", "", seed


def _reverses(rows: dict, t: int, g: int) -> bool:
    """Whether the travel coordinate turns back between (t-1 -> t) and (t -> t+1)."""
    a, b, c = _row(rows, t - 1), _row(rows, t), _row(rows, t + 1)
    if a is None or b is None or c is None:
        return False
    k = 2 if (g & 1) else 1
    d0, d1 = b[k] - a[k], c[k] - b[k]
    return d0 * d1 < 0.0


def seed_sim(rows: dict, t0: int, q: list[dict]) -> tuple[str, str, str]:
    """(class, why, seed): GD's own consumption loop replayed over the recording.

    GJBaseGameLayer::checkSpawnObjects (0x21a8f0, notes 2026-09-01 section 4):
    every tick, after physics, the ACTIVE channel's bucket is walked from its
    cursor; each element the player has passed on the player's axis and the
    channel's direction is consumed, in order, until the first that has not
    been passed. Other channels are never looked at. So which elements are
    consumed follows from the recorded positions alone.

    What an element DOES when consumed is read off the recording. A visible
    2900 turns the player (a gframe change on that row) or, pointing at the
    frame the player is already in, reverses the travel (the travel coordinate
    turns back on the next row, lv22 uid16659). If the recording shows neither,
    the element was consumed with neither half -- its group switched off by a
    touch Toggle (checkSpawnObjects skips triggerObject on +0x28e and moves the
    cursor on) -- and it switches no channel either. A turn on a row where the
    walk consumed no visible 2900 makes the seed underivable."""
    if not q or t0 <= 0:
        return "unavailable", "no queue", ""
    by = [[e for e in q if e["ch"] == c] for c in range(16)]
    chan, rev, ptr, spent = 0, [False] * 16, [0] * 16, []
    pg = None
    for t in sorted(t for t in rows if t <= t0):
        rr = _row(rows, t)
        if rr is None:
            continue
        g, x, y = rr
        axis = g if pg is None else pg
        acted = pg is not None and (g != pg or _reverses(rows, t, g))
        turned = 0
        for _guard in range(64):
            lst = by[chan]
            if ptr[chan] >= len(lst):
                break
            e = lst[ptr[chan]]
            if not _passed(axis, x, y, e, rev[chan]):
                break
            ptr[chan] += 1
            spent.append(e["uid"])
            if _visible(e) and not (acted and turned == 0):
                continue   # consumed with neither half (switched off)
            if e["id"] == 2900 and e["swarm"] == 1 and 0 <= e["swch"] <= 15:
                chan = e["swch"]
                rev[chan] = 0 <= e["gnddir"] - 2 < 2
            if _visible(e):
                turned += 1
                # rotateGameplay has turned the player, and the axis the rest of
                # this tick's walk tests is the new one (player1+0x9c3). The
                # recorded row is that frame.
                axis = g
        if pg is not None and g != pg and turned == 0:
            return "underivable", f"t={t}: the recording turns and the walk fired nothing", ""
        pg = g
    mask = 0
    for c in range(16):
        if rev[c]:
            mask |= 1 << c
    return "exact", "", f"{chan},{mask:x}" + "".join(f",{u}" for u in spent)

def toggles(level: int, level_data: Path) -> list[dict]:
    """The level's touch Toggles (1049 with touch=1), as loadRotObjs reads them."""
    if level in _TOGGLES:
        return _TOGGLES[level]
    out: list[dict] = []
    p = level_data / f"objrects_lv{level}.txt"
    if p.exists():
        for i, ln in enumerate(p.read_text(encoding="utf-8-sig", errors="replace").splitlines()):
            if i == 0 or not ln.startswith("1049,"):
                continue
            c = ln.split(",")
            try:
                if len(c) > 43 and int(float(c[43])) == 1:
                    out.append({"uid": int(c[7]), "cx": float(c[2]), "cy": float(c[3]),
                                "hw": float(c[4]) * 0.5, "hh": float(c[5]) * 0.5})
            except ValueError:
                continue
    _TOGGLES[level] = out
    return out


def touch_seed(rows: dict, t0: int, togs: list[dict], mode_id: dict,
               now_only: bool = False) -> str:
    """touchSeedArg: `uid:tick` of each touch Toggle's first entry by t0 (gframe 0 only).

    now_only is cfg dptouchseednow: the row's own y alone, as dp's markTouched
    reads it under --touchprey=button. Off (the loop's default) it also accepts
    the previous row's y."""
    parts = []
    for T in togs:
        for t in range(1, t0 + 1):
            r = rows.get(t)
            if r is None:
                continue
            try:
                if int(float(r["gframe"])) != 0:
                    continue
                x, y = float(r["x"]), float(r["y"])
                mini = float(r.get("vsize") or 1.0) < 0.99
            except (KeyError, ValueError, TypeError):
                continue
            m = mode_id.get(r.get("mode"), 0)
            half = ((2.0 if mini else 5.0) if m == _WAVE
                    else (8.1 if mini else 13.5) if m == _SPIDER
                    else (9.0 if mini else 15.0))
            pr = rows.get(t - 1)
            in_x = abs(x - T["cx"]) < T["hw"] + half
            in_y = abs(y - T["cy"]) < T["hh"] + half
            if not in_y and not now_only and pr is not None:
                try:
                    in_y = abs(float(pr["y"]) - T["cy"]) < T["hh"] + half
                except (KeyError, ValueError, TypeError):
                    pass
            if in_x and in_y:
                parts.append(f"{T['uid']}:{t}")
                break
    return ",".join(parts)
