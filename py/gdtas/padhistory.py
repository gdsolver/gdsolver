# -*- coding: utf-8 -*-
"""Which pads a recorded run had already fired, tick by tick. No game needed.

WHAT IT IS FOR. GD's `activatedByPlayer` latch is permanent for the attempt:
`collisionCheckObjects` drops a latched object before any shape test (the
`vf560` skip at 2.2081 win 0x214960, and `canBeActivatedByPlayer` +0x2178c0
returns `vf560(obj, player) == 0`). The model reproduces it with
`State::usedPad`, which the anchor of a mid-level comparison (`--start`) cannot
carry -- so a pad the run fired long before t0 comes back live and the anchored
arm fires it a second time. This module rebuilds the missing history from the
recording, and `leveldp --spentpad` burns it in (dp/src/dp/cli.hpp).

PRECEDENT AND DEPARTURE. The gravity-portal latch has the same hole and is
seeded the same way -- the producer walks the run and names uids, dp maps them
to bits (`--anchor-state owns=portal`, src/mod/repair.hpp portalPayload, and
the `if (t0 > 0 && g_ownsPortal)` block in cli.hpp). cli.hpp's note there says
the portal producer "takes the passes out of gdref's x,y rather than out of the
firings", because a portal is spent on OVERLAP.

    A PAD IS NOT. step.hpp evaluates the blue pad's polarity gate
    (`if (upNow != objFacingDown(*pd)) continue;`) BEFORE the line that writes
    `c.usedPad[slot] = pd`, so a gravity pad the player overlaps with the wrong
    gravity is neither fired nor consumed, and it is still live afterwards.
    Overlap alone therefore OVER-seeds, and an over-seed suppresses a firing GD
    performs -- the one error that would turn this instrument fix into a
    manufactured green. So this producer walks the loop instead of the boxes.

WHAT IT REPRODUCES, and from which site:

    contact         step.hpp's two window lines plus the oriented SAT --
                    gdtas.padgate.gate, transcribed and pinned there
    pad population  level_loader.hpp:984, types 8/9/10/34 (not 44)
    pad order       level_loader.hpp:1374 sorts L.pads by cx
    polarity        step.hpp's `upNow != objFacingDown(*pd)`, with
                    object.hpp:136's isFacingDown port
    one flip a tick step.hpp's `gravPadFlipped`: the first gravity pad of a
                    tick flips, and every later one is judged against the
                    flipped value (this is what makes lv21 t=7,250 -- two blue
                    pads entered together -- consume ONE pad and not two)

A PAD CARRIED BY MOVING GEOMETRY is named here on its load-time cx and then
DROPPED by dp, which counts it as `carried by moving geometry` in its own
`spentpad:` line. Its position at t0 is not in the objrects dump and the
contact test below would be measured against the wrong place. There are 35 of
them in the corpus (lv19 16, lv20 15, lv21 2, lv22 2) and none in lv1-18; one
of them is lv20 uid 7030, the corpus's only rotated pad.

WHAT IT DOES NOT REPRODUCE, and what is done about it. The loop also carries
`spiderWarpedThisTick` and the teleport uid ordering (`teleUid`), neither of
which is visible in gdref. Both live on ticks where the player is moved
bodily, so a tick whose x or y jumps is refused outright (`JUMP_X`/`JUMP_Y`)
rather than guessed at. Refusing costs nothing but a seed; guessing costs a
suppressed firing.

THE VERDICT COLUMN HAS ITS OWN TEST. Every tick's simulated gravity is checked
against the recording's own `upsideDown` on the next row. When they disagree
something outside this model moved gravity (a portal fires before the pad loop
in the same tick), and the tick's gravity-pad firings are dropped rather than
recorded. `walk()` returns the count, so a zero is distinguishable from "never
looked".

PHASE. The gate is fed row t's x and y and row t's rot -- the convention
py/test_leaf_padgate.py pins against GD's own activation tick on lv20 uid 7030
-- while the polarity gate is fed row t-1's `upsideDown`, because gravity in
the recording is the value at the END of a tick and the pad loop runs before
the tick's own flip is written.
"""
from __future__ import annotations

import bisect
import csv
from dataclasses import dataclass
from pathlib import Path

from gdtas.padgate import PAD_REACH, Board, gate

# level_loader.hpp:984. Type 44 is a pad in some dumps' numbering but the
# loader does not put it in L.pads, so it has no `usedPad` slot to seed.
PAD_TYPES = (8, 9, 10, 34)

# constants.hpp: kCubeHalf / kMiniHalf / kWaveHalf(Mini) / kSpiderHalf(Mini),
# selected by slopes.hpp:529 playerHalf(mode, mini).
_HALF = {4: (5.0, 2.0), 6: (13.5, 8.1)}
_HALF_DEFAULT = (15.0, 9.0)

# A tick whose position jumps by more than this is a teleport or a spider warp:
# the loop's `teleUid` ordering and `spiderWarpedThisTick` decide it and gdref
# cannot say what they decided, so nothing is recorded there.
JUMP_X = 50.0
JUMP_Y = 100.0

MODE_ID = {"cube": 0, "ship": 1, "ball": 2, "ufo": 3, "wave": 4, "robot": 5,
           "spider": 6, "swing": 7}


def player_half(mode: int, mini: bool) -> float:
    """slopes.hpp:529 playerHalf."""
    full, small = _HALF.get(mode, _HALF_DEFAULT)
    return small if mini else full


def obj_facing_down(rot: float, flip_y: int) -> bool:
    """object.hpp:136 objFacingDown, verbatim -- including the truncation of
    getRotation() toward zero that GD's cvttss2si performs."""
    r = int(rot)                      # C's (int) cast truncates toward 0
    f = bool(flip_y)
    if r % 90 == 0:
        return (not f) if abs(r) == 180 else f
    inside = (91 <= r <= 269) or (-269 <= r <= -91)
    return (not inside) if f else inside


@dataclass(frozen=True)
class Pad:
    uid: int
    type: int
    cx: float
    rot: float
    flip_y: int
    board: Board


def read_pads(objrects_path: Path | str) -> list[Pad]:
    """Every pad of a level, in the order the model's loop meets them.

    Sorted by cx, which is level_loader.hpp:1374's `byX` -- NOT by uid and not
    by file order. Which of two gravity pads entered on the same tick gets
    consumed depends on it.
    """
    pads: list[Pad] = []
    with Path(objrects_path).open(newline="", encoding="utf-8-sig") as f:
        for row in csv.DictReader(f):
            try:
                ty = int(row["type"])
            except (KeyError, ValueError):
                continue
            if ty not in PAD_TYPES:
                continue
            pads.append(Pad(uid=int(row["uid"]), type=ty, cx=float(row["cx"]),
                            rot=float(row["rot"]),
                            flip_y=int(float(row.get("flipy") or 0)),
                            board=Board.from_objrects_row(row)))
    pads.sort(key=lambda p: p.cx)
    return pads


@dataclass
class Walk:
    """The result of one pass over a recording."""

    fired: dict[int, list[int]]     # pad uid -> the ticks it was CONSUMED on
    ticks: int = 0                  # rows the walk actually decided
    skipped_jump: int = 0           # ticks refused as a teleport / warp
    gravity_mismatch: int = 0       # ticks whose simulated gravity did not check out
    contacts: int = 0               # gate-passing contacts seen
    # Contacts a gravity pad's polarity gate refused, counted only for pads NOT
    # already latched. Before the one-consumption-per-contact latch this also
    # counted every continuing tick of a contact whose pad had already fired --
    # flipping at t makes the polarity wrong at t+1, so a held contact fell here
    # every tick. On the 22-level corpus that is 286 of 914 gate-passing
    # contacts before and 72 after, with `contacts` itself unchanged at 914.
    # A figure quoted from a run before 2026-09-06 18:3x means the old sense.
    refused_polarity: int = 0


def walk(pads: list[Pad], gd: dict[int, dict], t_end: int | None = None) -> Walk:
    """Replay the pad loop over a GD recording and note every consumption.

    `gd` is quick_regress.read_ref's dict: tick -> raw csv row.
    """
    out = Walk(fired={})
    if not pads:
        return out
    # Index by cx so a tick costs a bisect and a handful of pads instead of the
    # level's whole population: 22 levels x 20,000 ticks x 80 pads is minutes.
    cxs = [p.cx for p in pads]
    span = max(p.board.hw for p in pads) + max(_HALF_DEFAULT) + PAD_REACH
    latched: set[int] = set()      # consumed and still being touched
    ticks = sorted(t for t in gd if t_end is None or t <= t_end)
    for t in ticks:
        r = gd[t]
        prev = gd.get(t - 1)
        if prev is None:
            continue
        try:
            x, y = float(r["x"]), float(r["y"])
            px, py = float(prev["x"]), float(prev["y"])
            rot = float(r.get("rot") or 0.0)
            vsize = float(r.get("vsize") or 1.0)
        except (KeyError, ValueError, TypeError):
            continue
        if abs(x - px) > JUMP_X or abs(y - py) > JUMP_Y:
            out.skipped_jump += 1
            continue
        out.ticks += 1
        mode = MODE_ID.get(r.get("mode", ""), 0)
        half = player_half(mode, vsize < 0.9)
        up = 1 if prev.get("upsideDown") == "1" else 0
        flipped = False
        grav_here: list[int] = []
        lo = bisect.bisect_left(cxs, x - span)
        hi = bisect.bisect_right(cxs, x + span)
        touching: set[int] = set()
        for p in pads[lo:hi]:
            # The x window is the same expression the loop's release uses, so
            # a pad the player is nowhere near costs one comparison.
            if abs(x - p.cx) >= p.board.hw + half + PAD_REACH:
                continue
            if not gate(p.board, x, y, half, rot):
                continue
            out.contacts += 1
            touching.add(p.uid)
            # ONE CONSUMPTION PER CONTACT. Without this a contact lasting N
            # ticks put N entries in `fired`, so the field held the ticks the
            # pad was TOUCHED on while its name and this function's docstring
            # both said consumed. Measured before the fix: 224 consecutive
            # pairs across the corpus, every one of them exactly 1 tick apart,
            # i.e. every "second firing" was the same contact continuing.
            #
            # Gravity pads never showed it, which is why it survived: flipping
            # at t changes `up`, so at t+1 the polarity gate refuses them and
            # they fall to refused_polarity. The 224 are types 8/9/34, which
            # have no polarity gate to hide behind.
            #
            # The uid SET is unchanged, so spent_uids and --spentpad are
            # behaviour-preserving; what changes is that `fired`'s tick lists
            # now mean what they are called.
            if p.uid in latched:
                continue
            if p.type == 10:
                if up != (1 if obj_facing_down(p.rot, p.flip_y) else 0):
                    out.refused_polarity += 1
                    continue
                if not flipped:
                    up = 1 - up
                    flipped = True
                grav_here.append(p.uid)
            out.fired.setdefault(p.uid, []).append(t)
            latched.add(p.uid)
        # A pad the player has left is armed again. GD skips a latched pad
        # BEFORE the shape test, so it cannot notice the contact ending; the
        # latch is cleared for it by the collision system. Re-testing here is
        # the same release one tick later at worst, and it is what lets this
        # walk tell a second contact from a continuing one at all.
        latched &= touching
        # The verdict column's own test: if the recording's gravity at the end
        # of this tick is not what the walk just produced, something the walk
        # does not model moved it (a gravity portal fires before the pad loop
        # in the same tick), and this tick's gravity-pad verdicts are worthless.
        if (1 if r.get("upsideDown") == "1" else 0) != up:
            out.gravity_mismatch += 1
            for uid in grav_here:
                out.fired[uid].pop()
                if not out.fired[uid]:
                    del out.fired[uid]
                # ...and un-latch it, or a pad whose firing was just rolled
                # back stays armed-as-spent for the rest of the contact and
                # can never be recorded at all.
                latched.discard(uid)
    return out


def spent_uids(w: Walk, t0: int) -> list[int]:
    """The pads consumed at or before t0, uid-sorted so the argument is stable.

    Everything ever consumed, not only what is still nearby: which of them can
    still matter is the x-window question, and dp answers it at the seeding
    site with its own `o.hw + sHalf + kPadReach` (cli.hpp), so that the rule
    lives in one place.
    """
    return sorted(u for u, ts in w.fired.items() if ts and ts[0] <= t0)
