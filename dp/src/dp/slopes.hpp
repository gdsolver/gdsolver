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
