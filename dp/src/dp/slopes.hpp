#pragma once
#include "dp/search_key.hpp"

namespace dp {

// Everything a tick needs. ONE implementation for both the search and the
// witness re-simulation: they used to be two copies and drifted -- the resim
// never learned about pads, so its trace disagreed with GD from the first pad
// onward and made every comparison useless (measured on lv2, t=292).
// Slope riding, measured on lv19's first two slopes (a 60x30 at x=300 and two
// 30x30 at x=615/645, speed 0.9, full size):
//
//  * the player's foot sits on the surface sampled at x + pHalf*tan(theta/2),
//    where theta is the slope's own angle. Measured offsets 3.5405 px for the
//    m=0.5 slope and 6.213 px for the m=1.0 one; 15*tan(atan(0.5)/2) = 3.5410
//    and 15*tan(atan(1)/2) = 6.2132. That is just the lowest corner of the
//    player's box once GD has rotated it to the slope's angle (the dump's `rot`
//    column follows the angle exactly).
//  * the surface is clamped to the slope's own x range, so the player keeps
//    riding the top edge until its reference point leaves the object.
//  * LEAVING the top launches the player. Measured vy 3.999 (m=0.5) and 7.405
//    (m=1.0), applied like a pad -- y moves with the OLD velocity on that tick
//    and vy is set afterwards. Two points only, so the table is interpolated
//    linearly in m and flagged; a third angle (the 30x60, m=2) is unmeasured.
inline double slopeXOffset(double m, double pHalf) {
    return pHalf * std::tan(std::atan(std::fabs(m)) * 0.5);
}
// Which side of the surface line is solid. GD's own test, read off the binary
// at 0x3921B5 (PlayerObject::collidedWithObjectInternal, the m_currentSlopes
// loop): `dir == 1 || (dir - 3 <= 3 && dir != 4)`, i.e. exactly {1,3,5,6}.
// m_slopeUphill is a pure function of m_slopeDirection ({0,3,6,7} rise,
// {1,2,4,5} fall -- checked over all 2,631 type-25 objects in the 22 dumps),
// so the flag splits each of those families 2-2, and the half that GD picks
// out is the CEILING half: lv20's corridors pair 0 with 3, 2 with 1, 4 with 5
// and 7 with 6, low member first, every time. 4..7 are the 90-degree-rotated
// sprites of the same four shapes, not walls.
inline bool slopeIsCeiling(uint8_t dir) {
    return dir == 1 || dir == 3 || dir == 5 || dir == 6;
}
// m_slopeUphill, which GD stores at obj+0x440. It is a pure function of the
// direction ({0,3,6,7} rise, {1,2,4,5} fall) -- checked over all 2,631 type-25
// objects in the 22 dumps, and confirmed on the rig calib_slopeflags, where
// every one of 160 id/rotation/flip combinations reported sup=1 on exactly
// that set.
inline bool slopeIsUphill(uint8_t dir) {
    return dir == 0 || dir == 3 || dir == 6 || dir == 7;
}

// ---------------------------------------------------------------------------
// THE +-2.000 SLOPE NUDGE (collidedWithSlopeInternal, 2.2081).
//
// Which of five velocity writes a ramp contact runs is decided by ONE bit,
// `m_isUpsideDown XOR isTop`, at 0x38fe00-0x38fe25 (isTop = m_slopeDirection in
// {1,3,5,6} = slopeIsCeiling). XOR == 0 is the GRAVITY-FACING side (the ramp is
// the player's floor: upright on a floor ramp, flipped on a ceiling ramp) and
// XOR == 1 is the UNDERSIDE.
inline bool slopeUnderside(bool flip, uint8_t dir) {
    return flip != slopeIsCeiling(dir);
}

// `bVar22`, the second gate both nudges read. Four terms, in order
// (0x38f8c8-0x38f9f8, spelled out in measure-slope-underside-gate-2026-09-05
// section 2):
//
//   bVar22 = (m_slopeUphill == 0)
//            XOR goingLeft                       (m_isGoingLeft, or the
//                                                 platformer x velocity's sign)
//            XOR (objDeltaAlongTravel > m_playerSpeed * m_speedMultiplier * dt)
//            XOR (m_isUpsideDown != m_isSideways)
//
// LEAF: only the first and last terms are modelled here, i.e. the static,
// forward-travelling, non-sideways case, which is what every witness on disk
// is. The mover term needs the ramp's own travel this tick against the
// player's, and `goingLeft`/`m_isSideways` need the reverse and rotated-frame
// bits; a ramp that is moving, a reversed player and a rotated frame each read
// this bit the other way and are NOT covered.
inline bool slopeBVar22(bool flip, uint8_t dir) {
    return (!slopeIsUphill(dir)) != flip;
}

// The four modes that can set either nudge flag at all: ship, UFO, wave, swing
// (0x3900ab-0x3900c6 for bVar24, 0x38ff0c-0x38ff27 for bVar20). A cube, ball,
// robot or spider sets NEITHER, on either side -- what a cube gets on the
// underside is the crush push (V0, 0x39028b), which is a different rule.
inline bool slopeNudgeMode(int mode) {
    return mode == 1 || mode == 3 || mode == 4 || mode == 7;
}

// ---------------------------------------------------------------------------
// WHICH MODES CARRY GD'S VELOCITY-LIMIT EXEMPTION (State::boost, the byte
// [player+0x952] in 2.2081). NOT the same set as slopeNudgeMode above: the
// wave is missing.
//
// The byte is written by ten sites across the binary (boostPlayer 0x39ff0a,
// ringJump 0x39987a, bumpPlayer 0x39f831, redirectPlayerForce 0x39fe99,
// rotateGameplay 0x399faa, postCollision 0x38ee4a, updateJump 0x38bc60,
// update 0x389310 -- the force block --, stopDashing 0x3966f4,
// teleportPlayer 0x21057a) with no mode test at any of them, and it is READ in
// exactly three places, all inside updateJump's flying branch:
//
//   0x38c5be / 0x38c5d8   the SHIP's acceleration selector (it replaces the
//                         thrust/drag factor while the body is moving with
//                         gravity -- see ShipModel::stepVy)
//   0x38ca9f              the terminal clamp for ship / UFO / swing
//
// so ship (m_isShip +0x9b9), UFO (m_isBird +0x9ba) and swing (m_isSwing
// +0x9c4) are the three modes in which it can change anything. The WAVE is
// excluded by GD itself one instruction after the latch test -- 0x38caa8
// `cmp byte [rdi+0x9bc], 0 / jne past the clamp` exempts the dart
// unconditionally, so the latch has nothing left to switch off (and the model
// never reaches this code for a wave anyway: mode 4 has its own branch at the
// head of stepOne with its own kWaveClamp). A cube, ball, robot or spider
// reaches neither read; their terminal is the bare +-15.0 at 0x38c2f2.
//
// The clear is the band test at the head of the flying branch,
// 0x38c527-0x38c59e: `[+0x952] = 0` iff the INCOMING gravity-frame velocity is
// strictly inside (-6.4/chi, +8.0/chi). It runs before the per-mode dispatch,
// so the swing shares that band even though its own CLAMP is the symmetric
// +-8.0 with chi forced to 1.0 at 0x38c959.
//
// Measurements: measure-ship-terminal-clamp-2026-09-06 (the disassembly) and
// the corpus census in measure-boostlatch-2026-09-06 (lv16 8,913-8,918, both
// halves, and lv16 14,366-14,370).
inline bool boostLatchMode(int mode) {
    // --no-boostlatch: the pre-2026-09-06 scope, where only the swing carried
    // the exemption. Every one of the sites below reads this helper, so the
    // flag restores the old behaviour exactly.
    if (g_noBoostLatch) return mode == 7;
    return mode == 1 || mode == 3 || mode == 7;
}

// bVar20 (0x38ff3e), the flag that both fires the gravity-facing nudge and, as
// `bVar3` (0x38ff4f), SKIPS the whole landing block at 0x39078a -- so on a tick
// where this is set `hitGround` does not run and vy is not zeroed, whether or
// not the nudge's own velocity gate lets the write through (GD leaves
// vy=-2.101 at lv16 dump t=18,690 for exactly that reason).
inline bool slopeSkipsHitGround(bool flip, uint8_t dir, int mode, bool held) {
    return held && slopeNudgeMode(mode) && !slopeUnderside(flip, dir)
        && !slopeBVar22(flip, dir);
}

// The nudge itself. Returns the vy after it; unchanged when it does not fire.
//
//   V5 bVar20 @0x390a19  gravity-facing, `jb && flight && !bVar22`
//                        vy := +2.0 upright / -2.0 flipped  (AGAINST gravity)
//                        gate: upright vy < +2.0, flipped vy > -2.0
//   V4 bVar24 @0x3909d5  underside,      `!jb && flight && bVar22`
//                        vy := -2.0 upright / +2.0 flipped  (ALONG gravity)
//                        gate: upright vy > -2.0, flipped vy < +2.0
//
// The sign is `m_isUpsideDown` ALONE (`eax = m_isUpsideDown ? -1 : +1`, then
// `eax + eax` for V5 and `eax * -2.0f` for V4): no gradient factor and no
// travel direction. Both are plain assignments of 2.000, and both re-test the
// gate every tick, so "once" is emergent -- see the lv16 dump ticks 14,344 /
// 14,350 / 14,352 / 14,355, where the same contact fires four times as the
// button is released, and 14,358, where the same release does NOT fire because
// the tick's own gravity step had already carried vy to -2.030.
inline float slopeNudge(float vy, bool flip, uint8_t dir, int mode, bool held) {
    if (!slopeNudgeMode(mode)) return vy;
    const double gs = flip ? -1.0 : 1.0;
    const double v = (double)vy;
    if (slopeUnderside(flip, dir)) {
        if (!held && slopeBVar22(flip, dir) && gs * v > -2.0)
            return (float)(-gs * 2.0);
    } else {
        if (held && !slopeBVar22(flip, dir) && gs * v < 2.0)
            return (float)(gs * 2.0);
    }
    return vy;
}
// GD REFUSES TO RESOLVE A SOLID THAT A RAMP IS ALREADY GOVERNING.
// PlayerObject::collidedWithObjectInternal walks the player's slope map before
// touching the solid, and two sites decide it: the top face at 0x3924ff-0x392536
// and the underside at 0x392a60-0x392ab1. Witnesses: lv16 t=8,696 (GD leaves the
// flipped ship at 573.337 where the model clamped to 569.000) and lv20 t=5,278
// (GD reaches 285.000 through the ramp, not through the 1.5px bar uid5263 --
// `hbox: obj=5263 hit=0`, and the model's own ballCornerM comment had already
// called it "the neighbouring slope hijacking the test").
//
// `faceIsTop` says which of the two sites is asking: the player came down onto
// the solid's top face, or up into its underside. The 2.0 is ONE-SIDED per face
// (0x622E60) -- reading it as |line - face| <= 2.0 rejects every case where the
// solid sits below the line, which on the rig was 67 of 1,673 ticks.
//
// LEAF: `slopes` stands in for GD's slope map [player+0x6E0]. GD inserts a ramp
// in preSlopeCollision when the player enters one of two 1px probes (a vertical
// strip at the ramp's high end, a horizontal one along its flat face) while
// m_isOnSlope == 0; scanning every nearby ramp instead is wider. It has to scan
// them all rather than just the adjacent one: at lv16 8,696 the ramp that
// carries the veto is 3763, one further down the chain, not the neighbour 3741.
// Would GD acquire this ramp with the player here? The acquisition gate is
// `targetY > y` (0x38fea5) against the CAPPED target of measure-slopeypos section
// 4: max(min(slopeYPos(cx) + playerH/(2 cos t), objMaxY + playerH/2), objMinY).
//
// This exists because the model's own `onSlope` is the wrong proxy for it. GD
// holds m_isOnSlope for as long as the player's box overlaps the ramp's -- the
// cap pins the target at the ramp's top corner, so a player standing on a solid
// at that height still passes the gate -- while the model's ride ends at the
// span. Measured directly (hbox with m_isOnSlope/m_wasOnSlope/m_currentSlope at
// the solid pass): every refused tick of the rig's d=0 unit reads
// onslp=1 wasslp=1 curslp=107, and lv22 t=4,592 reads 0/0/-1. The per-tick dump
// shows 0 for both because checkCollisions zeroes the flag at the top of a tick
// and the slope pass sets it again.
// `continuing` picks which reach test GD applies: a NEW contact goes through the
// object rect INSET 1px top and bottom (0x38fc3c-0x38fc73: y+1.0, h-2.0, no x
// inset), a continuing one through the cheap edge test (0x38fc0e-0x38fc2c: the
// ride ends once pos.y - h/(2 cos t) - 4.0 rises past the rect's top).
//
// The inset is not a detail. At lv22 t=4,592 the swing's foot is 389.98 and the
// ramp's rect top is 390; without the inset the contact stands by 0.02px, the
// bypass follows, and the model vetoes a solid GD resolves. With it the foot is
// 0.98px clear of the inset top 389.0 and the row comes out right.
inline bool slopeWouldAcquire(const Obj* R, double px, double py, double pHalf,
                              bool continuing) {
    const double rx0 = R->cx - R->hw, rx1 = R->cx + R->hw;
    const double ry0 = R->cy - R->hh, ry1 = R->cy + R->hh;
    if (rx1 <= rx0) return false;
    if (!(px + pHalf >= rx0 && px - pHalf <= rx1)) return false;
    const double m = (R->sy1 - R->sy0) / (rx1 - rx0);
    const double cosT = std::cos(std::atan(std::fabs(m)));
    if (continuing) {
        if (py - pHalf / cosT - 4.0 > ry1) return false;
    } else if (!(py + pHalf >= ry0 + 1.0 && py - pHalf <= ry1 - 1.0)) {
        return false;
    }
    const double half = pHalf / cosT;
    const double line = R->sy0 + (R->sy1 - R->sy0) * (px - rx0) / (rx1 - rx0);
    if (!slopeIsCeiling(R->slopeDir)) {
        const double t = std::max(std::min(line + half, ry1 + pHalf), ry0);
        return t > py;
    }
    const double t = std::min(std::max(line - half, ry0 - pHalf), ry1);
    return t < py;
}
inline bool slopeVetoesSolid(const Obj* o, const std::vector<const Obj*>* slopes,
                             double px, double py, double pHalfW, double pHalfH,
                             bool faceIsTop, double prevX, double prevY,
                             int curSlopeUid) {
    if (g_noSlopeVeto || !slopes || o->type != 0) return false;
    const double sx0 = o->cx - o->hw, sx1 = o->cx + o->hw;
    const double sy0 = o->cy - o->hh, sy1 = o->cy + o->hh;
    for (const Obj* R : *slopes) {
        // A spiked ramp is NOT skipped. GD's scan has no kind filter and no
        // direction filter, and slopeYPos already carries the hazard's own +-4
        // shift (0x623748 / 0x622E90) -- which reaches here for free, because
        // sy0/sy1 are the MOD's samples of GD's slopeYPos at the rect edges.
        // (An earlier draft skipped them; nothing in the disassembly justified
        // it, and skipping is not the safe direction -- it silently drops a veto
        // GD performs.)
        const bool isTop = slopeIsCeiling(R->slopeDir);
        const bool uphill = slopeIsUphill(R->slopeDir);
        // The sub-condition is mirrored between the two sites (0x392520 /
        // 0x392a9b): the top-face site wants a floor ramp or the one being
        // ridden, the underside site wants a ceiling ramp or the one being ridden.
        if (faceIsTop ? (isTop && R->uid != curSlopeUid)
                      : (!isTop && R->uid != curSlopeUid)) continue;
        // getObjectRect(1.2f, 1.1f) @0x622C84/0x622C54 scales the object's LOCAL
        // W and H and then swaps for rotation (obj+0x390), so a quarter-turned
        // ramp takes 1.1 across world x and 1.2 up world y. Measured on the pair
        // at d=+2.0: the rot-0 unit still vetoes at cx+18.000 and the rot-90 one
        // has stopped by cx+16.5 (lv22's ramp inflates 30x60 -> 33x72).
        const bool turned = std::fabs(std::fmod(std::fabs(R->rot), 180.0) - 90.0) < 45.0;
        const double iw = R->hw * (turned ? 1.1 : 1.2);
        const double ih = R->hh * (turned ? 1.2 : 1.1);
        const double ix0 = R->cx - iw, ix1 = R->cx + iw;
        const double iy0 = R->cy - ih, iy1 = R->cy + ih;
        if (!(ix0 <= sx1 && ix1 >= sx0 && iy0 <= sy1 && iy1 >= sy0)) continue;
        if (!(ix0 <= px + pHalfW && ix1 >= px - pHalfW
              && iy0 <= py + pHalfH && iy1 >= py - pHalfH)) continue;
        // 0x3920cf-0x392137. A ramp the player is not on, was not on last tick
        // and is not currently riding only counts when the solid's own position
        // lies strictly inside the ramp's x span. This is what lets lv16 6,046
        // and lv22 4,592/4,595 resolve (8820 < 8895 < 8880 and 6600 < 6645 < 6630
        // are both false) while lv18 9,297 and lv19 254/506 do not.
        // The bypass GD actually reads is `m_isOnSlope || m_wasOnSlope ||
        // partner == NULL || ramp == m_currentSlope`, and the first two are the
        // acquisition of THIS tick and the last one, not a ride window.
        const bool cont = (R->uid == curSlopeUid);
        if (!(cont
              || slopeWouldAcquire(R, px, py, pHalfH, cont)
              || slopeWouldAcquire(R, prevX, prevY, pHalfH, cont))) {
            const double rx0 = R->cx - R->hw, rx1 = R->cx + R->hw;
            if (!(rx0 < o->cx && o->cx < rx1)) continue;
        }
        // The line is read off the BARE rect, at the edge the direction picks
        // (0x3921d0): `(uphill == isTop) ? maxX : minX`. There is no "nearest
        // edge" -- evaluating at minX throughout vetoes lv18 11,978 and lv16
        // 6,973, which GD resolves.
        const double xEdge = (uphill == isTop) ? sx1 : sx0;
        const double rx0 = R->cx - R->hw, rx1 = R->cx + R->hw;
        if (rx1 == rx0) continue;
        const double line = R->sy0 + (R->sy1 - R->sy0) * (xEdge - rx0) / (rx1 - rx0);
        if (faceIsTop ? (sy1 - line <= 2.0) : (line - 2.0 <= sy0)) return true;
    }
    return false;
}
inline double slopeExitVy(double m, uint8_t mode, float dxF, bool mini) {
    const double a = std::fabs(m);
    // BALL. The old single point (4.316, "lv16 t=4565") was wrong -- read one
    // tick late off a route that no longer exists. Re-measured directly, both
    // at speed 1.1 and full size, on the launch tick itself:
    //   m = 0.5, lv16 t=4744->4745 : y 353.000 -> 352.971, vy 3.729
    //   m = 1.0, lv16 t=4563->4564 : y 375.000 -> 374.971, vy 4.028
    // (the -0.029 is one tick of ball gravity applied to the OLD velocity, so
    // the launch value is exactly what the dump shows on that tick).
    // Scaling 4.316 linearly through the origin would have given 2.158 at
    // m=0.5 -- 1.6 vy out, which is what the lv16 loop kept diverging on.
    // The two points sit on a line whose intercept IS the ball's tap value
    // (3.729 - 0.5*0.598 = 3.430 vs ballFlipFor(1.1) = 3.4260), so it is
    // anchored on the tap and therefore scales with speed and size the way
    // every other ball impulse does. OPEN: the 0.598 per unit of m is two
    // points at ONE speed, and m=2 (the 30x60 ramps) is unmeasured -- lv16's
    // ball section has none.
    // CUBE. The 3.999 / 7.405 pair was measured on lv19 at the base speed. It
    // scales with the X SPEED, not with the jump: lv16 t=6515->6516 launches a
    // cube off an m=1.0 ramp at speed 1.1 with vy = 9.208, and
    // 7.405 * (1.6142578 / 1.29825044) = 9.2074. The jump's own ratio (1.0215)
    // would have given 7.564, i.e. 1.6 vy short, which is what the model kept
    // planning the rest of lv16's ramp section with.
    const double sp = dxF / kDxF;
    // THREE ANCHORS, MEASURED DIRECTLY on a purpose-built calibration map
    // (2026-08-17, py/mklevel.py `ramps` + py/calib_extract.py: 48 units, one
    // zero-input run, 48/48 clean exits). The official 22 levels contain only
    // FIVE slope exits in total, each occurring once, and none of them is a
    // forward cube on |m|=2 -- which is why this row had to be extrapolated
    // and then mode-split as a workaround. The map settles it:
    //   |m|      0.5     1.0     2.0
    //   cube    3.999   7.405  10.507
    //   ball    2.999   5.554   7.880   (= cube * 0.75 to 5 digits)
    //   robot   3.999   7.405  10.507   (= CUBE, not 0.75)
    //   spider  3.999   7.405  10.507   (= CUBE, not 0.75)
    // and mini equals full size in every one of the 24 pairs.
    // 10.507 also explains the reverse mini-cube launch that forced the
    // mode split (lv22 t=18,568: 10.507 * sp(1.24268) = 13.057 against GD's
    // 13.064, +0.05%, where the swing-derived 10.402 was 1.1% out), so the
    // split is gone and every mode shares one table again.
    const double cubeExit = (a <= 0.5)
        ? 3.999 * (a / 0.5) * sp
        : (a <= 1.0)
            ? (3.999 + (7.405 - 3.999) * (a - 0.5) / 0.5) * sp
            : (7.405 + (10.507 - 7.405) * (a - 1.0)) * sp;
    if (mode == 2) {
        // BALL = CUBE x 0.75. Disassembled (collidedWithSlopeInternal
        // 0x390AE0, 2.2081): m_slopeVelocity is computed mode-agnostically and
        // then multiplied by the constant 0.75 when ANY non-cube gamemode flag
        // is set. Confirmed to 4 digits on lv16's own dump: full ball, m=1.0,
        // speed 1.1, full-ramp ride -> GD 6.906 = 9.2074 (the cube exit) x
        // 0.75. The old tap-anchored line (ballFlipFor + 0.598*m) was fit to
        // a RIDE-CONTAMINATED point: its m=1.0 anchor (4.028) was measured on
        // a 14-tick ride, i.e. 6.906 x ramp(14) -- see slopeRampFactor.
        // MINI ball: the old x0.625 point (5.755, lv16 t=12,720) is exactly
        // 0.75 x ramp(20) = 0.625, so it is PLAUSIBLY the same contamination
        // (a 20-tick ride) and mini needs no factor of its own. OPEN: re-probe
        // a mini ball on a >=24-tick ride to confirm; until then mini shares
        // the 0.75.
        // CORRECTED 2026-08-17: the old note here said "GD applies the 0.75 to
        // every non-cube mode (ship/UFO/robot/spider flags OR'd together)",
        // read off the disassembly. The calibration map says otherwise --
        // ROBOT and SPIDER launch at the CUBE value (7.405 at |m|=1, 10.507 at
        // |m|=2, 12 units each, mini and full alike), and only the ball (and
        // the swing) take the 0.75. Measurement wins over the flag reading.
        if (g_oldSlope) {
            if (mini) return cubeExit * 0.625;
            return ballFlipFor(dxF) + 0.598 * a;
        }
        return cubeExit * 0.75;
    }
    // ...and the SWING is a non-cube mode, so it gets the same 0.75 the ball
    // does. The comment above already recorded this as a KNOWN GAP ("GD applies
    // the 0.75 to every non-cube mode"), it just had no rider to apply it to
    // until lv22's swing section.
    // Measured on lv22 uid5430 (m=2, full size, dx=1.6143, a 4-tick ride so the
    // ramp factor is its 0.4 floor): the model launched at 7.071
    // (= cubeExit 17.68 x 0.4) where GD launches at 3.919. The 0.75 takes it to
    // 5.303 -- still 1.4 out, so the m=2 branch is NOT closed; the residual
    // ratio is 0.739 and needs a second point (a longer ride, or another of
    // lv22's fourteen 1744 ramps) before it can be fitted. Applying the 0.75 is
    // not a guess though: it is what the disassembly says.
    if (mode == 7) {
        // ...and the linear-in-m extrapolation over-shoots badly past m=1.
        // Measured on lv22 uid5430 (m=2, full size, dx=1.6143) with three
        // injection probes that differ ONLY in how long the ride is, so the
        // ramp factor separates cleanly (all three launch on the same tick,
        // t=4,584, from the same vy of 1.652):
        //   ride  4 ticks (4,580..4,583)  factor 0.40000  GD 3.919  base 9.798
        //   ride 12 ticks (4,572..4,583)  factor 0.50000  GD 4.813  base 9.626
        //   ride 17 ticks (4,567..4,583)  factor 0.70833  GD 6.854  base 9.677
        // i.e. base 9.70 +-1%, against 13.26 from the extrapolation -- 27% out.
        // Backing the 0.75 and the speed factor out gives a cube-equivalent
        // 10.402 at m=2, so the table gets a THIRD ANCHOR and stays piecewise
        // linear between measured points, exactly as it already is below m=1.
        // This is data, not a law: 3.999 / 7.405 / 10.402 fit no closed form
        // tried so far (k*tan(atan(m)/2) matches m=0.5 and m=2 to 0.7% but
        // misses m=1 by 5.5%), so do NOT replace the anchors with a formula
        // until a fourth point says which one is wrong.
        return cubeExit * 0.75;
    }
    // SHIP / UFO / WAVE take the same 0.75. Which modes take it is not a guess
    // any more -- GD enumerates them, and the list is right there in the
    // disassembly (2.2081, the tail of the slope branch reached from
    // postCollision; `rdi` is the PlayerObject):
    //
    //   0x390a82  movss [rdi+0x9b4], xmm0     ; m_slopeVelocity = ...
    //   0x390aa7  cmp byte [rdi+0x9b9], 0 / jne -> apply
    //   0x390ab0  cmp byte [rdi+0x9ba], 0 / jne -> apply
    //   0x390ab9  cmp byte [rdi+0x9bc], 0 / jne -> apply
    //   0x390ac2  cmp byte [rdi+0x9c4], 0 / jne -> apply
    //   0x390acb  cmp byte [rdi+0x9bb], 0 / je  -> SKIP
    //   0x390ae0  mulss xmm0, 0.75
    //   0x390ae8  movss [rdi+0x9b4], xmm0
    //
    // The offsets decode uniquely. bindings has PlayerObject's mode flags as one
    // contiguous bool run (m_isShip, m_isBird, m_isBall, m_isDart, m_isRobot,
    // m_isSpider, m_isUpsideDown), and m_isUpsideDown is 0x9bf -- fixed
    // independently by the blue pad's gate, which reads [player+0x9bf] against
    // isFacingDown and reproduces 158/158 activations. Counting back:
    //   0x9b9 m_isShip   0x9ba m_isBird  0x9bb m_isBall  0x9bc m_isDart
    //   0x9bd m_isRobot  0x9be m_isSpider  0x9bf m_isUpsideDown
    //   ... 0x9c3 m_isSideways (the pad gate's other field)  0x9c4 m_isSwing
    // so the five tested flags are ship / UFO / ball / wave / swing and
    // **robot and spider are the two that are missing from the list** -- exactly
    // what the 2026-08-17 calibration map measured (both launch at the CUBE
    // value, 12 units each). The reading and the measurement now agree; the
    // earlier "every non-cube mode" note was a mis-read of this same code.
    //
    // The ship confirms it in play: lv19 t=6,121 (uid m=+1.0, sp0.9, ride 10)
    // launches at GD 2.546 against the cube value 3.394 -- 0.75000 to five
    // digits.
    // UFO and WAVE are covered by the flag list but have no slope exit in the
    // 22-level corpus, so they ride on the disassembly alone.
    if (mode == 1 || mode == 3 || mode == 4) return cubeExit * 0.75;
    // mini CUBE exit is unmeasured -- returns the full-size value, as before.
    // CUBE / ROBOT / SPIDER: no factor (their flags are not in GD's list).
    return cubeExit;
}

// GD ramps the slope-exit impulse up over the first 0.1 s of the ride.
// Disassembled (postCollision 0x38D837, 2.2081, getModifiedSlopeYVel inlined):
//   diff   = m_totalTime - m_slopeStartTime          // seconds on the slope
//   factor = diff >= 0.1 ? 1.0 : max(0.4, diff * 10)
//   exitVy = m_slopeVelocity * factor
// (the bindings' ios decompile has the branches inverted -- trusting it cost a
// day. The Windows code is unambiguous: comisd 0.1 > diff -> mulsd 10.0,
// maxsd 0.4.)
// Confirmed on lv16's dump to 4 digits at both ends: ride 14 -> 6.906 x 14/24
// = 4.0285 (dump: 4.028), ride 33 -> factor 1 -> 6.906 (dump: 6.906).
// `rideTicks` is the tick count at the EXIT tick: state.slopeT + 1 (the
// counter is 0 on the contact tick, diff = exitTick - contactTick).
inline double slopeRampFactor(int rideTicks) {
    if (g_oldSlope) return 1.0;
    const double diff = rideTicks / 240.0;
    if (diff >= 0.1) return 1.0;
    return std::max(0.4, diff * 10.0);
}

// The player's half-box. Split out of stepOne's `pHalf` so the touch-trigger
// test cannot drift from the collision test -- the firing boundary was measured
// to 1 px (see TouchTrig) and a different half here would silently move it.
inline double playerHalf(uint8_t mode, bool mini) {
    return (mode == 4) ? (mini ? kWaveHalfMini : kWaveHalf)
         : (mode == 6) ? (mini ? kSpiderHalfMini : kSpiderHalf)
                       : (mini ? kMiniHalf : kCubeHalf);
}

}  // namespace dp
