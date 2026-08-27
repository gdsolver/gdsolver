"""Parsing the lines of result.txt.

The line prefixes and key names are a contract with the MOD side (writeResult in
src/phase1.cpp). DO NOT CHANGE THEM ON A WHIM - change both sides together.

    session_start
    serve: loaded <N> inputs        signal that the supplied plan has landed
    serve: initial plan <N> inputs  the plan put on the attempt at startup
    serve: FAILED to load plan_in.txt
    death:      attempt tick x wallMs speedX
    complete:   attempt step tick x levelMaxX goalX pct
    stop:       attempt tick x y yvel mode ...
    inject:     t x y vy mode          (the tick key is `t`, not `tick`)
    clearcheck: x levelMaxX goalX margin tick solve
    killer:     tick id type ox oy px py
    gt_last:    attempt rows depth     signal that grouptrace_last.txt is finalised
"""

from __future__ import annotations

import re
from pathlib import Path

SESSION_START = "session_start"
SESSION_END = "session_end"
SERVE_LOADED = "serve: loaded"
SERVE_INITIAL = "serve: initial plan"
SERVE_FAILED = "serve: FAILED"
DEATH = "death:"
COMPLETE = "complete:"
STOP = "stop:"
INJECT = "inject:"
CLEARCHECK = "clearcheck:"
KILLER = "killer:"
GT_LAST = "gt_last:"
COIN_COMPLETE = "coin: level complete"

# Threshold on GD's own progress percentage. levelComplete is called mid-level too, so
# `level_complete` on its own is not evidence.
#
# 95, matching the guard the MOD applies before it will save a solution
# (hooks_playlayer.cpp, the refusing-a-clear branch), and for the same measured
# reasons: the false clear that guard was written for said 4.85% (level
# 140155559, levelComplete at x=2,458 of 19,570), and lv22 -- whose final cube
# section RETREATS from x=22,375 to 21,853, so a genuine completion sits 2,232px
# "short of" the end portal at 24,085 -- says 96.1. This used to be 99.0 against
# a "mid-level false clear is ~97.9" that has no surviving case anywhere in the
# tree, and it rejected every real lv22 clear.
PCT_FLOOR = 95.0
# ...and the other half of the mod's rule: a completion within this much x of the
# goal is a completion whatever the percentage says. Same value as the mod's
# g_clearMargin, so the two authorities cannot disagree about a saved solution.
CLEAR_MARGIN_X = 400.0


def read_lines(path: Path | str) -> list[str]:
    """Read result.txt. It can vanish or be held mid-run, so errors are swallowed."""
    p = Path(path)
    if not p.exists():
        return []
    try:
        return p.read_text(encoding="utf-8-sig", errors="replace").splitlines()
    except OSError:
        return []


def parse_kv(line: str) -> dict[str, str]:
    """Turn `prefix: k=v k=v ...` into a dict (the first token is the prefix; drop it)."""
    out: dict[str, str] = {}
    for tok in line.split()[1:]:
        if "=" in tok:
            k, v = tok.split("=", 1)
            out[k] = v
    return out


def _num(kv: dict, key: str, default=-1, cast=int):
    try:
        return cast(kv[key])
    except (KeyError, ValueError):
        return default


def first(lines: list[str], prefix: str) -> str | None:
    for l in lines:
        if l.startswith(prefix):
            return l
    return None


def last_index(lines: list[str], prefix: str) -> int:
    """Index of the last line starting with prefix, or -1 if there is none.

    Trust ONLY THE LAST serve load marker: until the replacement lands, attempts keep
    running on the previous plan, and their death gets mistaken for this run's result.
    """
    idx = -1
    for i, l in enumerate(lines):
        if l.startswith(prefix):
            idx = i
    return idx


def parse_death(line: str) -> dict:
    kv = parse_kv(line)
    return {"attempt": _num(kv, "attempt"), "tick": _num(kv, "tick"),
            "x": _num(kv, "x", -1.0, float), "wall_ms": _num(kv, "wallMs"),
            "speed_x": _num(kv, "speedX", 0.0, float)}


def parse_complete(line: str) -> dict:
    kv = parse_kv(line)
    return {"attempt": _num(kv, "attempt"), "tick": _num(kv, "tick"),
            "x": _num(kv, "x", -1.0, float),
            "level_max_x": _num(kv, "levelMaxX", 0, float),
            "goal_x": _num(kv, "goalX", 0.0, float),
            "pct": _num(kv, "pct", None, float)}


def parse_stop(line: str) -> dict:
    kv = parse_kv(line)
    d = {"attempt": _num(kv, "attempt"), "tick": _num(kv, "tick"),
         "x": _num(kv, "x", -1.0, float)}
    d["state"] = {k: kv[k] for k in ("y", "yvel", "mode", "onGround", "vsize")
                  if k in kv}
    return d


def parse_inject(line: str) -> dict:
    return parse_kv(line)


def parse_killer(line: str) -> dict:
    return parse_kv(line)


def gt_last(lines: list[str], attempt: int | None = None) -> dict | None:
    """The marker that finalises grouptrace_last.txt (the MOD's rollGroupTrace).

    The MOD emits this BEFORE `death:` / `complete:`. Passing attempt accepts only the
    marker of that run - if it does not match, the file we grabbed belongs to a
    different run, and harvesting it makes a run with the same inputs stop reproducing.
    rows=0 means "_last was left as it was".
    """
    for l in reversed(lines):
        if not l.startswith(GT_LAST):
            continue
        kv = parse_kv(l)
        d = {"attempt": _num(kv, "attempt"), "rows": _num(kv, "rows", 0),
             "depth": _num(kv, "depth")}
        if attempt is not None and d["attempt"] != attempt:
            return None
        return d
    return None


def clear_verdict(lines: list[str], pct_floor: float = PCT_FLOOR) -> tuple[bool, str]:
    """Whether the replay really cleared the level. Returns (clear, explanation).

    Two signals, either of which is a clear -- the same pair the MOD applies before it
    will file a plan as a solution, so the two cannot disagree about the same run:

      * GD's own progress percentage (`pct=` = PlayLayer::getCurrentPercent) at or
        above pct_floor, or
      * the player within CLEAR_MARGIN_X of the goal.

    Neither alone is enough. Percentage alone rejects a level whose ENDING RUNS
    BACKWARDS (lv22 completes at 96.1% because its last section retreats 2,232px);
    distance alone cannot tell that same ending from a level that raised
    levelComplete in a sub-area (the measured false clear: x=2,458 of 19,570).
    """
    cl = None
    for l in lines:
        if l.startswith(COMPLETE) and "levelMaxX=" in l:
            cl = l
            break
    if cl is None:
        d = first(lines, DEATH)
        return False, d or "(no complete:/death: line)"
    c = parse_complete(cl)
    if c["pct"] is not None:
        goal = c.get("goal_x") or c["level_max_x"]
        near = goal and goal > 1.0 and (goal - c["x"]) <= CLEAR_MARGIN_X
        if c["pct"] >= pct_floor or near:
            return True, first(lines, COIN_COMPLETE) or cl
        return False, f"MID-LEVEL COMPLETE pct={c['pct']} x={int(c['x'])}"
    # Fall back to x only for old MODs that do not emit pct. This 100 is a value for
    # "the x read after the end-of-level suck-in", and cannot be reused for the guard
    # inside the MOD that watches the levelComplete() entry (even on a genuine clear
    # you are still ~290 px short at that point).
    mx, cx = c["level_max_x"], c["x"]
    if mx <= 0 or cx >= mx - 100:
        return True, first(lines, COIN_COMPLETE) or cl
    return False, f"MID-LEVEL COMPLETE x={int(cx)} levelMaxX={int(mx)} (no pct)"
