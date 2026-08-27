"""Readers that return the observation files a worker emitted, SUMMARISED.

The one design rule here: never return raw CSV. The moment we start returning raw
logs we fall back to the old way of "grepping a huge CSV" and the whole point of
building the MCP disappears. Every tool has columns, a tick range, a stride and a
limit, without exception.
"""

from __future__ import annotations

import csv
from pathlib import Path

from gdtas.paths import LEVEL_DATA

DUMP_COLS = ["frame", "attempt", "tick", "x", "y", "yvel", "rot", "mode",
             "upsideDown", "onGround", "onGround2", "dead", "speed",
             "gravityMod", "platXVel", "vsize"]
OBJ_COLS = ["id", "type", "cx", "cy", "w", "h", "groups", "uid", "radius"]
MAIN_DATA = LEVEL_DATA   # the main line's data. READ ONLY


def _num(s: str):
    try:
        f = float(s)
        return int(f) if f.is_integer() and abs(f) < 1e15 else round(f, 4)
    except ValueError:
        return s


def read_dump(path: Path, t0: int = 0, t1: int | None = None,
              cols: list[str] | None = None, stride: int = 1,
              limit: int = 200) -> dict:
    """Return dump.csv narrowed by tick range, columns and stride.

    In serve mode dump.csv is truncated on every supply, so its contents only cover
    "the single plan that was run last".
    """
    if not path.exists():
        return {"error": f"no {path.name} (nothing has been run yet?)"}
    cols = cols or ["tick", "x", "y", "yvel", "mode", "onGround"]
    with path.open(newline="", encoding="utf-8-sig", errors="replace") as f:
        r = csv.DictReader(f)
        head = r.fieldnames or DUMP_COLS
        bad = [c for c in cols if c not in head]
        if bad:
            return {"error": f"unknown columns {bad}; available={head}"}
        rows, total, i = [], 0, 0
        for rec in r:
            try:
                t = int(rec["tick"])
            except (KeyError, ValueError):
                continue
            if t < t0 or (t1 is not None and t > t1):
                continue
            total += 1
            if i % stride:
                i += 1
                continue
            i += 1
            if len(rows) < limit:
                rows.append([_num(rec[c]) for c in cols])
    return {"cols": cols, "rows": rows, "n_matched": total,
            "truncated": total // max(1, stride) > len(rows)}


def read_trace(path: Path, t0: int = 0, t1: int | None = None,
               kinds: list[str] | None = None, limit: int = 200) -> dict:
    """Narrow the trace.csv events by tick range and kind.

    kinds is matched as a prefix (e.g. "orb" catches the whole orb family). Omitting
    it means all kinds.
    """
    if not path.exists():
        return {"error": f"no {path.name}"}
    rows, total, seen = [], 0, {}
    with path.open(newline="", encoding="utf-8-sig", errors="replace") as f:
        for rec in csv.DictReader(f):
            try:
                t = int(rec["tick"])
            except (KeyError, ValueError, TypeError):
                continue
            if t < t0 or (t1 is not None and t > t1):
                continue
            ev = rec.get("event", "")
            if kinds and not any(ev.startswith(k) for k in kinds):
                continue
            total += 1
            seen[ev] = seen.get(ev, 0) + 1
            if len(rows) < limit:
                rows.append([t, ev, _num(rec.get("a", "")),
                             _num(rec.get("b", "")), _num(rec.get("c", ""))])
    return {"cols": ["tick", "event", "a", "b", "c"], "rows": rows,
            "n_matched": total, "kinds_seen": dict(sorted(
                seen.items(), key=lambda kv: -kv[1])[:20]),
            "truncated": total > len(rows)}


def objects_path(data_root: Path, level: int) -> Path | None:
    """Find the object table that carries hitboxes.

    What the worker itself emitted takes priority. Failing that, fall back READ-ONLY to
    data/objrects_lv<N>.txt collected by the main line (it is invariant once taken, so
    there is no reason to pay for a clearance=1 run every time).
    """
    own = data_root / "objrects.txt"
    if own.exists() and own.stat().st_size > 0:
        return own
    shared = MAIN_DATA / f"objrects_lv{level}.txt"
    return shared if shared.exists() else None


def read_objects(path: Path, x0: float, x1: float,
                 y0: float | None = None, y1: float | None = None,
                 types: list[int] | None = None,
                 ids: list[int] | None = None,
                 limit: int = 300) -> dict:
    """Return the objects in a range with their real-size hitboxes.

    w,h are measured from GD's own getObjectRect(). DO NOT USE A GUESSED 30x30: that is
    how the lv19 wall once got misread as "a solid block with no gaps".
    radius>0 means a circular test (saw blades and the like); killing on the whole
    rectangle throws routes away.
    """
    if path is None or not path.exists():
        return {"error": "no objrects. Run once with cfg clearance=1, or put "
                         "data/objrects_lv<N>.txt in place first."}
    rows, total = [], 0
    with path.open(newline="", encoding="utf-8-sig", errors="replace") as f:
        r = csv.DictReader(f)
        for rec in r:
            try:
                cx, cy = float(rec["cx"]), float(rec["cy"])
                oid, otype = int(rec["id"]), int(rec["type"])
            except (KeyError, ValueError, TypeError):
                continue
            if not (x0 <= cx <= x1):
                continue
            if y0 is not None and not (y0 <= cy <= (y1 if y1 is not None else 1e9)):
                continue
            if types and otype not in types:
                continue
            if ids and oid not in ids:
                continue
            total += 1
            if len(rows) < limit:
                rows.append([oid, otype, _num(rec["cx"]), _num(rec["cy"]),
                             _num(rec["w"]), _num(rec["h"]),
                             _num(rec.get("groups", 0)), _num(rec.get("uid", 0)),
                             _num(rec.get("radius", 0))])
    rows.sort(key=lambda r: r[2])
    return {"cols": OBJ_COLS, "rows": rows, "n_matched": total,
            "truncated": total > len(rows), "source": str(path)}


# Only the events worth seeing just before a death. Back when all kinds were emitted
# by default, PO_update_pre / checkCollisions / processCommands produced 3 lines per
# tick, and out of 300 lines the only meaningful one was the single destroyPlayer.
DEATH_EVENT_KINDS = ["destroyPlayer", "orb", "ring", "pad", "portal", "snap",
                     "handleButton", "INJECT", "hbox", "hbin"]


def _player_half(vsize) -> float:
    """The player's half width / half height. Mini (vsize 0.6) is 9, normal is 15."""
    try:
        return 9.0 if float(vsize) < 0.9 else 15.0
    except (TypeError, ValueError):
        return 15.0


def death_suspects(objs: dict, px: float, py: float, half: float,
                   limit: int = 6) -> dict:
    """List which objects were overlapping, and by how much, as seen from the death.

    This is the thing that got calculated by hand most often in this session. Just
    returning a table of nearby objects means redoing "for a circle of radius 21.6,
    the nearest point of the box is..." every time here. A NEGATIVE VALUE = OVERLAP,
    sorted ascending.

    The meaning of clearance depends on the shape:
      radius>0 (saw blade): distance from the box's nearest point to the circle - radius
      rectangle:            THE LARGER of the x and y gaps (an AABB overlap needs both
                            axes negative, so max below 0 means an overlap)
    """
    rows = []
    for r in objs.get("rows", []):
        oid, otype, cx, cy, w, h, groups, uid, radius = r[:9]
        cx, cy, w, h, radius = float(cx), float(cy), float(w), float(h), float(radius)
        if radius > 0:
            dx = max(0.0, abs(px - cx) - half)
            dy = max(0.0, abs(py - cy) - half)
            clear = (dx * dx + dy * dy) ** 0.5 - radius
            shape = f"circle r={radius:g}"
        else:
            gx = abs(px - cx) - (w / 2 + half)
            gy = abs(py - cy) - (h / 2 + half)
            clear = max(gx, gy)
            shape = f"rect {w:g}x{h:g}"
        rows.append([oid, otype, round(cx, 1), round(cy, 1), shape,
                     round(clear, 3), round(px - cx, 1), round(py - cy, 1)])
    rows.sort(key=lambda a: a[5])
    return {"cols": ["id", "type", "cx", "cy", "shape", "clearance", "dx", "dy"],
            "rows": rows[:limit],
            "note": "clearance < 0 means overlap. The player radius is taken as "
                    f"half={half:g} (9 for mini)"}


def _best_attempt_rows(path: Path) -> dict[int, dict]:
    """Return only THE ATTEMPT WITH THE MOST ROWS from dump.csv, as tick -> row.

    When a run contains several attempts, naively reading all of them makes the same
    tick appear again and again and throws the comparison off. The longest one is
    "the attempt that got furthest in that run".
    """
    if not path.exists():
        return {}
    per: dict[str, dict[int, dict]] = {}
    with path.open(newline="", encoding="utf-8-sig", errors="replace") as f:
        for rec in csv.DictReader(f):
            try:
                t = int(rec["tick"])
            except (KeyError, ValueError, TypeError):
                continue
            a = rec.get("attempt", "0")
            per.setdefault(a, {}).setdefault(t, rec)
    if not per:
        return {}
    return max(per.values(), key=len)


def diff_trace(trace_path: Path, dump_path: Path, t0: int = 0,
               t1: int | None = None, tol: float = 0.3,
               limit: int = 8) -> dict:
    """Match the model trace (leveldp's .trace.csv) against the GD dump by tick and
    return a few lines starting from THE FIRST TICK WHERE THEY DISAGREE.

    The most repeated operation of this session. It is exactly the practice of "do not
    look at the death point; emit the same log on both sides and find the first tick
    that diverges", which used to mean rewriting the same python into the scratchpad
    every single time.

    tol is a tolerance in px (and the same number for vy). The default 0.3 is the
    yardstick for "a divergence that means something"; lowering it to 0.001 picks up
    even float rounding (which is occasionally needed too).
    """
    if not trace_path.exists():
        return {"error": f"no {trace_path}"}
    model: dict[int, dict] = {}
    with trace_path.open(newline="", encoding="utf-8-sig", errors="replace") as f:
        for rec in csv.DictReader(f):
            try:
                model[int(rec["tick"])] = rec
            except (KeyError, ValueError, TypeError):
                continue
    gd = _best_attempt_rows(dump_path)
    if not gd:
        return {"error": f"no readable rows in {dump_path} (nothing run yet?)"}
    common = sorted(set(model) & set(gd))
    if not common:
        return {"error": "the ticks do not overlap at all -- this may be the "
                         "trace of a different run"}
    rows = []
    first = None
    for t in common:
        if t < t0 or (t1 is not None and t > t1):
            continue
        m, g = model[t], gd[t]
        try:
            dx = float(m["x"]) - float(g["x"])
            dy = float(m["y"]) - float(g["y"])
            dv = float(m["vy"]) - float(g["yvel"])
        except (KeyError, ValueError, TypeError):
            continue
        # The second player. The culprit behind a whole session of misdiagnosing
        # "p1 matches exactly and yet only GD dies" late in lv16 (it was p2 that died).
        # Compare only when both sides have the columns.
        dy2 = dv2 = 0.0
        if m.get("dual") == "1" and g.get("dual") == "1":
            try:
                dy2 = float(m["y2"]) - float(g["p2y"])
                dv2 = float(m["vy2"]) - float(g["p2vy"])
            except (KeyError, ValueError, TypeError):
                dy2 = dv2 = 0.0
        if max(abs(dx), abs(dy), abs(dv), abs(dy2), abs(dv2)) <= tol:
            continue
        if first is None:
            first = t
        rows.append([t, round(float(g["x"]), 3),
                     round(float(g["y"]), 3), round(float(g["yvel"]), 4),
                     g.get("mode", ""), g.get("vsize", ""),
                     round(float(m["y"]), 3), round(float(m["vy"]), 4),
                     round(dy, 4), round(dv, 4), round(dx, 4),
                     round(dy2, 4), round(dv2, 4)])
        if len(rows) >= limit:
            break
    out = {"model_ticks": len(model), "gd_ticks": len(gd),
           "common": len(common), "tol": tol,
           "cols": ["tick", "x", "gd_y", "gd_vy", "gd_mode", "vsize",
                    "m_y", "m_vy", "dy", "dvy", "dx", "dy2", "dvy2"],
           "rows": rows}
    if first is None:
        out["verdict"] = (f"agree within a tolerance of {tol} "
                          f"({len(common)} ticks in common). If GD is the only "
                          "one that ends early, the suspect is not a divergence "
                          "but **a kill the model is not making**")
    else:
        out["first_divergence"] = first
    return out


def death_context(data_root: Path, level: int, death_tick: int, death_x: float,
                  back_ticks: int = 40, x_pad: float = 120.0) -> dict:
    """Return only the moments before a death: state, events and nearby geometry.

    A bundle that closes "where and how did it die" in a single call. It cuts the
    round trips compared with asking three times separately, and structurally prevents
    forgetting to ask (blaming x without ever looking at what was nearby).
    """
    t0 = max(0, death_tick - back_ticks)
    dump = read_dump(data_root / "dump.csv", t0, death_tick,
                     cols=["tick", "x", "y", "yvel", "mode", "onGround", "vsize"],
                     stride=1, limit=back_ticks + 1)
    # Emitting all kinds by default buries this in 3 update events per tick (measured:
    # 1 meaningful line, destroyPlayer, out of 302). Narrow it to the kinds we want.
    events = read_trace(data_root / "trace.csv", t0, death_tick,
                        kinds=DEATH_EVENT_KINDS, limit=40)
    events["note"] = ("narrowed to the kinds " + "/".join(DEATH_EVENT_KINDS) +
                      ". For all of them, call gd_events with no kinds")
    op = objects_path(data_root, level)
    near = read_objects(op, death_x - x_pad, death_x + x_pad, limit=80) if op else \
        {"error": "no objrects"}
    # The real position and player size just before death. destroyPlayer's coordinates
    # are a sub-position beyond the end of the tick, so use the death coordinates
    # rather than the last row of state
    last = dump.get("rows") or []
    vsize = last[-1][6] if last and len(last[-1]) > 6 else 1
    half = _player_half(vsize)
    death_y = float(last[-1][2]) if last else 0.0
    return {"death_tick": death_tick, "death_x": round(death_x, 2),
            "window": [t0, death_tick], "state": dump, "events": events,
            "suspects": death_suspects(near, death_x, death_y, half),
            "nearby_objects": near}
