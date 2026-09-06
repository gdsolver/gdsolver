# -*- coding: utf-8 -*-
"""The rotated-pad activation gate, as a leaf that runs without the game.

WHAT THIS IS. GD decides whether a type-8 pad activates in
`collisionCheckObjects` (2.2081 win 0x214960) with two conjuncts:

    inclusive AABB overlap  player->getObjectRect() n obj->getObjectRect()
                                                    0x214b09-0x214b56
    and, when obj->m_isOriented [+0x2e8],
        overlaps1Way(objOBB, plOBB) && overlaps1Way(plOBB, objOBB)
                                       0x214b5e / 0x214bb0 / 0x214bbf

with the player's OBB a 30 x 30 square TURNED by `player->getRotation()`
(PlayerObject::getObjectRotation 0x3a0670). The model's copy of that is
`orientedHit`/`obbSat` in dp/src/dp/object.hpp, called from the pad loop in
dp/src/dp/step.hpp. Every input to it closes in about a dozen numbers, which is
what makes it a leaf: given the player's x, y, rotation and half, and the
board's centre, bound and raw sprite size, the answer is arithmetic.

WHY IT EXISTS -- the failure it is built to catch. Two 2026-09-06 commits:

    59ba018   the SHAPE: the player's square is turned, not flattened
              (a874728 that morning had asserted the opposite and is retracted
              in its own comment at the site)
    351f9de   the INPUT: the angle comes from the gravity the pad's own call
              saw, not the gravity at the end of the tick

59ba018's work was written against a base that did not yet have 351f9de. There
the angle it was being fed was 6.9 degrees wrong, and at that error the turned
square cleared the board anyway -- so the shape rule changed nothing, and its
acceptance table said so honestly (measure-padplayerrot-2026-09-06 section 4:
all 22 traces byte-identical, in every arm). Rebased onto a base that HAS
351f9de, the same rule moves lv20's firing from 7,296 to 7,298. Neither commit
moved this pad on its own; the pair was order-dependent, and no instrument in
this repository could see it, because they all ask one question -- "does the
whole replay change" -- and a leaf that is RIGHT BUT STARVED answers it exactly
like a leaf that is WRONG.

This module lets the two be asked apart:

    Q1  given GD's OWN inputs, does the predicate return GD's answer?
    Q2  given the same situation, do the MODEL's inputs equal GD's?

Q1 is `gate()` fed from a GD dump. Q2 is `sat_angle()` against the dump's `rot`
column. py/test_leaf_padgate.py pins both on lv20 uid 7030.

THIS IS A TRANSCRIPTION, AND IT CAN DRIFT. Nothing here executes the C++. The
functions below are hand-copied from dp/src/dp/{object,step,level_loader,
constants}.hpp and a change over there does not change them. So:

  * a green Q1 does NOT prove the C++ predicate correct. It proves that THIS
    reading of GD's rule reproduces GD's answers on the pinned rows, which is
    a statement about the rule, not about the shipped implementation;
  * Q2 needs no transcription at all -- it compares two angle series -- and
    that is where most of this test's value sits;
  * `check_transcription()` is the guard: it fingerprints the code lines of
    the four C++ spans that were copied and refuses when they move. It cannot
    tell a real divergence from a rename; it can only refuse to stay silent.

Answering Q1 against the C++ itself would need an entry point in dp/ that
evaluates the predicate at arbitrary inputs. There is none, and dp/ was out of
bounds for this change.
"""
from __future__ import annotations

import csv
import hashlib
import math
import re
from dataclasses import dataclass
from pathlib import Path

from gdtas.paths import REPO

# --------------------------------------------------------------- constants
# Every one of these is copied from a named site; none is fitted here.

PI = 3.14159265358979          # object.hpp's literal, digit for digit
PAD_REACH = 0.0                # kPadReach, level.hpp:141
PLAYER_HALF_FULL = 15.0        # playerHalf(cube, mini=0) -- the 30 x 30 square
PLAYER_HALF_MINI = 9.0         # playerHalf(cube, mini=1)
CUBE_SPIN_STEP = 1.7307692     # step.hpp's one-step advance, full size
CUBE_SPIN_STEP_MINI = 2.25     # ...and mini
# constants.hpp:392 -- which modes may hand the gate the player's real angle.
PAD_PLAYER_ROT_MODES = (0, 1, 4)      # cube / ship / wave


def pad_player_rot_mode(mode: int) -> bool:
    """constants.hpp:392 `padPlayerRotMode`."""
    return mode in PAD_PLAYER_ROT_MODES


# --------------------------------------------------------------- the board

@dataclass(frozen=True)
class Board:
    """One object as the model builds it -- level_loader.hpp:829-880.

    `hw`/`hh` are the recorded bound (objrects' w,h halved), which for a turned
    object is `getBoundingRect` of the OBB (0x1978a2). `ohw`/`ohh` are the real
    box, recovered from that bound because objrects' w0/h0 are the RAW sprite
    size and a scaled object's real box is k times it.
    """

    uid: int
    obj_id: int
    type: int
    cx: float
    cy: float
    hw: float
    hh: float
    rot: float
    oriented: bool
    ohw: float
    ohh: float
    rc: float
    rs: float
    scale: float

    @classmethod
    def from_objrects_row(cls, row: dict) -> "Board":
        rot = float(row["rot"])
        w0, h0 = float(row["w0"]), float(row["h0"])
        hw, hh = float(row["w"]) / 2.0, float(row["h"]) / 2.0
        radius = float(row.get("radius", 0.0) or 0.0)
        # `oriented` is set only off a quarter turn; at 0/90 the recorded rect
        # IS the box and the SAT cannot move a digit.
        # The C++ gate also carries `!o.slope`, which objrects does not report
        # (the loader derives it from the id). A pad is not a slope, so it is
        # dropped here rather than guessed -- this constructor is for pads.
        m = abs(math.fmod(rot, 90.0))
        oriented = (radius == 0.0 and w0 > 1.0 and h0 > 1.0
                    and 0.5 < m < 89.5)
        th = rot * PI / 180.0
        denom = (w0 * 0.5) * abs(math.cos(th)) + (h0 * 0.5) * abs(math.sin(th))
        k = hw / denom if denom > 1.0 else 1.0
        if abs(k - 1.0) < 0.01:            # within 1% is dump rounding
            k = 1.0
        W, H = w0 * k, h0 * k
        if not (W > 1.0 and H > 1.0):
            oriented = False
        thc = -th                          # cocos getRotation() is CW-positive
        return cls(uid=int(row["uid"]), obj_id=int(row["id"]),
                   type=int(row["type"]), cx=float(row["cx"]),
                   cy=float(row["cy"]), hw=hw, hh=hh, rot=rot,
                   oriented=bool(oriented), ohw=W * 0.5, ohh=H * 0.5,
                   rc=math.cos(thc), rs=math.sin(thc), scale=k)


def read_objrects(path: Path | str, uids=None) -> dict[int, Board]:
    """Every row of an objrects dump as a Board, keyed by uid."""
    want = None if uids is None else {int(u) for u in uids}
    out: dict[int, Board] = {}
    with Path(path).open(newline="", encoding="utf-8-sig") as f:
        for row in csv.DictReader(f):
            uid = int(row["uid"])
            if want is not None and uid not in want:
                continue
            out[uid] = Board.from_objrects_row(row)
    return out


# --------------------------------------------------------------- the predicate

def obb_sat_margin(b: Board, px: float, py: float, half: float,
                   p_rot_deg: float) -> float:
    """The tightest of `obbSat`'s four axes, in px. > 0 on all four == contact.

    object.hpp:163-201. Both angles are negated together: GD's rotations are
    cocos's (clockwise positive) and obbSat is written counter-clockwise, so
    negating only one mirrors one box against the other.
    """
    a1 = -math.atan2(-b.rs, b.rc)          # rc/rs hold cos/sin(-th)
    a2 = -p_rot_deg * PI / 180.0
    dx, dy = px - b.cx, py - b.cy
    c1, s1 = math.cos(a1), math.sin(a1)
    c2, s2 = math.cos(a2), math.sin(a2)
    axes = ((c1, s1), (-s1, c1), (c2, s2), (-s2, c2))
    worst = None
    for ux, uy in axes:
        r1 = b.ohw * abs(c1 * ux + s1 * uy) + b.ohh * abs(-s1 * ux + c1 * uy)
        r2 = half * abs(c2 * ux + s2 * uy) + half * abs(-s2 * ux + c2 * uy)
        m = r1 + r2 - abs(dx * ux + dy * uy)
        worst = m if worst is None else min(worst, m)
    return worst


def aabb_conjunct(b: Board, px: float, py: float, half: float) -> bool:
    """The pad loop's two window lines -- step.hpp, with kPadReach = 0.

    NOT identical to GD's: GD's four comparisons reject on `ja` (strictly
    greater), so exact touching passes there and fails here. Pre-existing, and
    no pinned row sits on the equality.
    """
    return (abs(px - b.cx) < b.hw + half + PAD_REACH
            and abs(py - b.cy) < b.hh + half)


def gate(b: Board, px: float, py: float, half: float,
         p_rot_deg: float) -> bool:
    """Both conjuncts: the AABB window and, for a turned board, the two-way SAT.

    THIS IS NOT THE WHOLE PAD GATE. step.hpp's loop also carries the teleport
    uid ordering (`teleUid`), the `usedPad` latch, the spider warp break and
    the per-type branches. Rows where this returns True and GD does not
    activate are pinned in the test as exactly that -- see lv20 uid 7025/7026
    at t=7,295, which this accepts and GD skips because the pad is ordered
    before the tick's teleport.
    """
    if not aabb_conjunct(b, px, py, half):
        return False
    if not b.oriented:
        return True
    return obb_sat_margin(b, px, py, half, p_rot_deg) > 0.0


# --------------------------------------------------------------- the input (Q2)

def sat_angle(rot_prev: float, rot_neg_prev: int, mini: bool = False,
              mode: int = 0, advance: bool = False) -> float:
    """The angle the MODEL hands the SAT, in degrees. step.hpp's pad site.

    PHASE, and it is the trap this measurement fell into first: `s.rot` is the
    state at the END of the previous tick, so a model trace's row for tick t-1
    is what the gate reads while producing tick t. Reading row t instead moves
    every margin by one tick and reproduces neither GD's answer nor the note's.

    `advance` is the C++ site's `g_noSatRotRaw` arm, i.e. `--no-satrotraw`, and
    it is False here for the same reason it is off there. Until 2026-09-06 the
    four oriented-contact sites added one spin step signed by `s.rotNeg` before
    the call; the cube's own rotation law writes `c.rot += (spinSign ? -rate :
    rate)` with `spinSign` = that same field, so the advance subtracted the step
    the law was about to add and the SAT was handed an angle two steps off GD's
    (38.166 against 34.681 on lv20 t=7,296). Both arms are kept because the C++
    keeps both, and because the historical traces this leaf pins were produced
    by builds that advanced.
    """
    if not pad_player_rot_mode(mode):
        return 0.0
    if not advance:
        return rot_prev
    step = CUBE_SPIN_STEP_MINI if mini else CUBE_SPIN_STEP
    return rot_prev + (1.0 if rot_neg_prev else -1.0) * step


def mod90(deg: float) -> float:
    """Rotations are compared mod 90: the square has four-fold symmetry and
    GD's column runs to -418 while the model's runs to +135. Subtracting them
    raw is a number with no meaning."""
    return deg % 90.0


def angle_error_mod90(model_deg: float, gd_deg: float) -> float:
    """MODEL - GD, in degrees, on the circle mod 90 (result in (-45, +45])."""
    d = (model_deg - gd_deg) % 90.0
    return d - 90.0 if d > 45.0 else d


# --------------------------------------------------- transcription drift guard

# The C++ spans this module was copied from, as (file, first-line, last-line)
# regexes over the COMMENT-STRIPPED source. Comments at these sites are edited
# constantly, so only code lines are fingerprinted.
SOURCE_SPANS: dict[str, tuple[str, str, str]] = {
    "obbSat/orientedHit": (
        "dp/src/dp/object.hpp",
        r"^\s*inline bool obbSat\(",
        r"^\s*-pRot \* 3\.14159265358979 / 180\.0\);",
    ),
    "pad gate angle + call": (
        "dp/src/dp/step.hpp",
        r"^\s*double pRotPad = \(double\)s\.rot;",
        r"orientedHit\(\*pd, x, \(double\)c\.y, pHalf, pRotPad\)",
    ),
    "oriented box from the bound": (
        "dp/src/dp/level_loader.hpp",
        r"^\s*if \(g_oriented && o\.radius == 0\.0",
        r"^\s*o\.rc = std::cos\(thc\); o\.rs = std::sin\(thc\);",
    ),
    "padPlayerRotMode": (
        "dp/src/dp/constants.hpp",
        r"^\s*inline bool padPlayerRotMode\(",
        r"^\}$",
    ),
}

# Pinned at 6a3c721 (2026-09-06), the commit this leaf was transcribed from.
# `pad gate angle + call` re-pinned the same day when the one-step advance was
# deleted and put behind `--no-satrotraw`: the span's `if (s.mode == 0)` became
# `if (g_noSatRotRaw && s.mode == 0)`, which is the whole diff inside it. The
# other three spans did not move, and this alarm firing is what sent the
# transcription (`sat_angle`) after the C++ rather than leaving it behind.
PINNED_FINGERPRINTS: dict[str, str] = {
    "obbSat/orientedHit": "dfdb012e89906db3",
    "pad gate angle + call": "716e422ff69e6a4e",
    "oriented box from the bound": "10fc68d7604a7684",
    "padPlayerRotMode": "067068fc6e480d00",
}
PINNED_AT = "6a3c721, pad span re-pinned at the g_noSatRotRaw landing"


def _strip_cpp_comments(src: str) -> list[str]:
    """`//` and `/* */` removed, string literals kept, blank lines dropped.

    The same rule tools/check_code_only_diff.py applies; kept here rather than
    imported because tools/ is not on the path when py/ is.
    """
    out: list[str] = []
    buf: list[str] = []
    src = src.lstrip("﻿")
    i, n = 0, len(src)
    while i < n:
        c = src[i]
        if c in '"\'':
            q = c
            buf.append(c)
            i += 1
            while i < n and src[i] != q:
                if src[i] == "\\" and i + 1 < n:
                    buf.append(src[i]); buf.append(src[i + 1]); i += 2
                    continue
                if src[i] == "\n":
                    break
                buf.append(src[i]); i += 1
            if i < n and src[i] == q:
                buf.append(q); i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                if src[i] == "\n":
                    buf.append("\n")
                i += 1
            i += 2
            continue
        buf.append(c)
        i += 1
    for line in "".join(buf).split("\n"):
        s = line.rstrip()
        if s.strip():
            out.append(s)
    return out


def _span_lines(lines: list[str], start: str, end: str) -> list[str] | None:
    s = re.compile(start)
    e = re.compile(end)
    for i, ln in enumerate(lines):
        if s.search(ln):
            for j in range(i, len(lines)):
                if e.search(lines[j]):
                    return lines[i:j + 1]
            return None
    return None


def fingerprints(repo: Path | None = None) -> dict[str, str | None]:
    """Fingerprint each copied span. None = the span was not found."""
    root = Path(repo) if repo is not None else REPO
    cache: dict[str, list[str]] = {}
    out: dict[str, str | None] = {}
    for name, (rel, start, end) in SOURCE_SPANS.items():
        p = root / rel
        if not p.exists():
            out[name] = None
            continue
        if rel not in cache:
            cache[rel] = _strip_cpp_comments(
                p.read_text(encoding="utf-8", errors="replace"))
        span = _span_lines(cache[rel], start, end)
        if span is None:
            out[name] = None
            continue
        body = "\n".join(" ".join(l.split()) for l in span)
        out[name] = hashlib.sha256(body.encode("utf-8")).hexdigest()[:16]
    return out


def check_transcription(repo: Path | None = None) -> list[str]:
    """Names of the spans whose code has moved since PINNED_AT, or is missing.

    An empty list means every copied span still reads as it did when this
    module was written. A non-empty one is not proof of a bug -- a rename trips
    it too -- but it is proof that the transcription has not been re-read.
    """
    got = fingerprints(repo)
    bad = []
    for name, want in PINNED_FINGERPRINTS.items():
        have = got.get(name)
        if have is None:
            bad.append(f"{name}: span not found in {SOURCE_SPANS[name][0]}")
        elif have != want:
            bad.append(f"{name}: {SOURCE_SPANS[name][0]} now {have}, "
                       f"pinned {want} at {PINNED_AT}")
    return bad
