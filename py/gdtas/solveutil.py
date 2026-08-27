"""Facts about a run that both the solver driver and the measuring tools need.

These used to live in `dp_solve_level.py`, which made the driver a dependency of
the regression that judges it: `quick_regress` (tier 1, 81 s, no worker),
`fixcensus`, `fidelity_diff` and `regen_groups` all imported it. They are not
driver policy -- they are how a dump, an objrects table and a plan file are
read -- so they belong on the library side, and the driver imports them from
here like everyone else.

The bodies are the driver's, moved unchanged: this is a lift, not a rewrite, and
every consumer is judged by byte-identical output.

Nothing here decides anything. No solving, no worker, no argv.
"""

from __future__ import annotations

import re
import shutil
import time
from pathlib import Path

from gdtas.worker import wall_event

# Where a diagnostic line goes. The driver owns a real log; a tool that has
# none leaves this alone and the line is dropped (the wall_event above is the
# part that must not be lost).
log = None


def _say(msg: str) -> None:
    if log is not None:
        log(msg)


# GD's mode name -> the ordering leveldp uses (`--start`'s 5th field).
#
# swing (mode 7) reached leveldp on 2026-08-12 and was left out of this table:
# for every re-anchor GD reported as swing, the driver passed mode=0. Measured
# on lv22 (2026-08-13): at t=4,879 GD is swing / upsideDown=1 / vy=8 while the
# solve was told cube, and the death point oscillated between x=6,051 and 6,247
# because of it. lv1-21 have no swing section, so nothing else ever saw it.
MODE_ID = {"ship": 1, "ball": 2, "ufo": 3, "wave": 4, "robot": 5, "spider": 6,
           "swing": 7}
# The flying modes keep onGround set while airborne (it is sticky), so a
# grounded test there needs onGround2 and vy ~ 0 as well. The cube family's
# flag can be believed as it stands.
FLYING = (1, 3, 4)

# The types the model actually collides with or reads. If any of them carries a
# group id, the level has geometry that moves.
_COLLIDER_TYPES = {"0", "2", "47", "25", "3", "4", "5", "6", "16", "17", "18",
                   "19", "21", "23", "24", "26", "27", "33", "41",
                   "8", "9", "10", "34", "11", "12", "13", "29", "32", "35"}


def read_lines(path) -> list[str]:
    """Get-Content's equivalent. utf-8-sig: existing artefacts carry a BOM."""
    try:
        return Path(path).read_text(encoding="utf-8-sig",
                                    errors="replace").splitlines()
    except OSError:
        return []


def copy_held_file(src, dst) -> bool:
    """Copy a file the worker still holds open. NEVER raises.

    A failed snapshot is not a reason to kill a level -- the caller degrades and
    carries on -- but it must not be silent either, because a missing snapshot
    reads exactly like a physics wall.
    """
    for attempt in range(3):
        try:
            with open(src, "rb") as fin, open(dst, "wb") as fout:
                shutil.copyfileobj(fin, fout)
            # A retry that succeeded is a wall-clock branch too (the heavier the
            # load, the likelier it is). Succeeding silently would let the run
            # be recorded as "the same conditions".
            if attempt:
                wall_event("snapshot-retry", f"{Path(src).name} ok on try {attempt + 1}")
            return True
        except OSError as e:
            if attempt == 2:
                wall_event("snapshot-failed", f"{Path(src).name}: {e}")
                _say(f"  [snapshot] cannot copy {src}: {e}")
                return False
            time.sleep(0.2)
    return False


def has_grouped_colliders(path) -> bool:
    """Does any collider carry a group id? (objrects' `groups` column.)"""
    p = Path(path)
    if not p.exists():
        return False
    with p.open("r", encoding="utf-8-sig", errors="replace") as f:
        header = f.readline()
        if not header:
            return False
        cols = header.strip().split(",")
        if "type" not in cols or "groups" not in cols:
            return False
        i_type, i_grp = cols.index("type"), cols.index("groups")
        for line in f:
            fl = line.rstrip("\n").split(",")
            if len(fl) <= i_grp or fl[i_grp] == "0":
                continue
            if fl[i_type] in _COLLIDER_TYPES:
                return True
    return False


# Trace columns, for cause_of:
#   0 tick 1 x 2 y 3 vy 4 mode 5 grounded 6 dual 7 y2 8 vy2 9 flip2 10 act
#   11 onslope 12 slopem 13 slopet 14 bandf 15 bandc 16 mini 17 held 18 dx
def cause_of(row: list, nxt: list | None = None, gd_grounded: str = "?",
             gd_mode: int = -1) -> str:
    """One signature for what the model was DOING across this transition.

    **The key that groups by cause rather than by point.** If many records share
    a signature, what needs fixing is one rule, not n local overrides. A small
    spread in dy means a constant is off (fit it); a large one means the formula
    is wrong (fix the code) -- and point corrections could not tell those apart.
    """
    if len(row) < 18:
        return "notrace"          # an old trace (the columns are not there)
    # **GD's grounded goes in too.** `air` is the model's own claim, so without
    # this "integration error in free flight" and "the model missed a contact"
    # share a signature. Measured on lv20: m1/mini1/g0/air was 47 of 59 records
    # with an 11.8 px spread in dy -- the mark of two causes mixed, not one.
    parts = [f"m{row[4]}", f"mini{row[16]}", f"g{row[5]}", f"gdg{gd_grounded}"]
    # **A tick where GD's MODE differs is a different cause.** Portal boundaries
    # are a known +/-1 tick class, and there the two are running different
    # physics. Mixed in, it reads as "the mini ship's integration error"
    # (measured: GD alone gains 2.1 of vy in one tick -- 16x the mini ship's
    # maximum acceleration of 0.127, which cannot happen in one mode).
    if gd_mode >= 0 and str(gd_mode) != row[4]:
        parts.append(f"gdm{gd_mode}")
    if len(row) >= 19:
        # Velocity arrives in px/tick, so it goes back to the familiar multiplier
        # for the signature (0.9 -> 1.29825). The thresholds are per speed, so
        # without this the families cannot be split.
        try:
            parts.append(f"sp{float(row[18]) / 1.29825 * 0.9:.1f}")
        except ValueError:
            pass
    # **The clamp and the neighbourhood are read from the NEXT row.** Both happen
    # inside that step, so neither appears on the t-1 row. This used to read them
    # off mp0.
    ev = nxt if (nxt is not None and len(nxt) >= 21) else row
    if len(ev) >= 21 and ev[20] not in ("-", ""):
        # **The name of the clamp that zeroed vy, and what it hit.** A wrong
        # clamp shows up as "only the model loses its speed", but which one fired
        # cannot be worked out from outside (measured: it was happening where
        # there was neither a static object nor a flight band).
        parts.append(f"clamp:{ev[20]}")
        if len(ev) >= 22 and ev[21] not in ("-1", ""):
            parts.append(f"uid{ev[21]}")
    if len(ev) >= 20 and ev[19] == "1":
        # A tick with something that can deliver an impulse within reach. The
        # axis that isolates the divergence where GD alone gains about 2 units of
        # velocity (an order of magnitude away from any difference in
        # acceleration).
        parts.append("orbnear")
    if row[11] == "1":
        try:
            parts.append(f"slope{float(row[12]):+.2f}")
        except ValueError:
            parts.append("slope?")
        try:
            st = int(row[13])
            parts.append("ride24+" if st >= 24 else f"ride{st}")
        except ValueError:
            pass
    else:
        parts.append("air")
    try:
        y, bf, bc = float(row[2]), float(row[14]), float(row[15])
        if bc < 1e8:              # only while the flight band is live
            if abs(y - bc) < 20.0:
                parts.append("nearceil")
            elif abs(y - bf) < 20.0:
                parts.append("nearfloor")
    except ValueError:
        pass
    return "/".join(parts)


def grounded_of(mode: int, on_ground: str, on_ground2: str, yvel: str) -> int:
    if mode in FLYING:
        return 1 if (on_ground == "1" and on_ground2 == "1"
                     and abs(float(yvel)) < 0.01) else 0
    return 1 if (on_ground == "1" or float(yvel) == 0) else 0


def held_before(plan_path, tick: int) -> int:
    """Whether the last input before `tick` was down.

    The ship's acceleration branches on it, so assuming 0 accumulates
    trajectory error from the anchor onwards.
    """
    held = 0
    for l in read_lines(plan_path):
        m = re.match(r"^input=(\d+),(\d)$", l)
        if m and int(m.group(1)) < tick:
            held = int(m.group(2))
    return held


def rot2900s(objrects_path) -> list[tuple[int, float, float]]:
    """The id 2900 (gameplay rotation) objects as (uid, cx, cy).

    lv1-21 have none, so the spentrot / trigraw machinery sleeps entirely there.
    """
    out: list[tuple[int, float, float]] = []
    try:
        for i, ln in enumerate(read_lines(objrects_path)):
            if i == 0:
                continue
            c = ln.split(",")
            if len(c) < 10 or c[0] != "2900":
                continue
            out.append((int(c[7]), float(c[2]), float(c[3])))
    except (OSError, ValueError):
        out = []
    return out


def spent2900_from_dump(path, attempt: int, t0: int,
                        trigs: list[tuple[int, float, float]]) -> list[int]:
    """Which 2900s had already fired before t0, from the dump's gframe changes.

    A 2900's one-shot (firedT) cannot travel through `--start`. Re-anchor behind
    the maze without it and the model re-fires a spent trigger at the next
    crossing, entering a rotated frame and breaking the world (lv22 t=14,321,
    uid6337: GD consumed it at t=12,795 and passes straight through; a fixup
    cannot carry a frame, so this cost 30 iterations of treading water).

    The culprit for a transition is the nearest 2900 to the player at that tick.
    Measured over lv22's 13 transitions the worst is 27 px, and the 2900s sit
    300+ px apart, so the cut is at 100.
    """
    if not trigs:
        return []
    p = Path(path)
    if not p.exists():
        return []
    spent: list[int] = []
    try:
        f = p.open("r", encoding="utf-8-sig", errors="replace")
    except OSError:
        return []
    with f:
        header = f.readline()
        if not header:
            return []
        cols = header.strip().split(",")
        if "gframe" not in cols:
            return []
        i_att, i_tick = cols.index("attempt"), cols.index("tick")
        i_x, i_y, i_gf = cols.index("x"), cols.index("y"), cols.index("gframe")
        prev = None
        for line in f:
            fl = line.rstrip("\n").split(",")
            if len(fl) <= max(i_gf, i_x, i_y):
                continue
            if attempt >= 0 and fl[i_att] != str(attempt):
                continue
            try:
                t = int(fl[i_tick])
                gf = int(float(fl[i_gf]))
            except ValueError:
                continue
            if t > t0:
                break
            if prev is not None and gf != prev:
                try:
                    x, y = float(fl[i_x]), float(fl[i_y])
                except ValueError:
                    prev = gf
                    continue
                best, bd = -1, 100.0 ** 2
                for uid, cx, cy in trigs:
                    d = (cx - x) ** 2 + (cy - y) ** 2
                    if d < bd:
                        bd, best = d, uid
                if best >= 0 and best not in spent:
                    spent.append(best)
            prev = gf
    return spent
