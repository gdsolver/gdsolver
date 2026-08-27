#pragma once
#include "dp/prelude.hpp"

namespace dp {

// Tolerance for the "gap between the foot and the surface" within which the player
// still counts as standing (the static-surface part; a moving surface adds + |dcy|
// -- see the note at the support test).
//
// **0.6 is far too wide.** GD's grounding IS the collision test (do the boxes
// overlap?), so if the surface is below the foot the player falls unless touching.
// Measured (lv16 t=15,686, where it moves onto the thin floor uid7859 whose top is
// 419.95 at x=24,015):
//   GD     y 435.000 -> 434.952 (one tick of gravity) -> lands at 434.950
//   model  y 435.000 unchanged (the 0.05px gap fell inside 0.6)
// and it runs 0.05px too high from then on. --supporttol A/Bs it.
inline double kSupportTol = 0.01;
constexpr double kDx = 1.29825;
// ...but GD does NOT step by the float32 nearest to 1.29825. Read straight out
// of the dump's first increment (x(1) = 0, x(2) = 1.29825044) on lv1/3/7/10/11
// and lv12 alike, GD's per-tick advance is the float32 1.2982504367828369 --
// four ULPs above (float)1.29825 = 1.2982499599456787.
// Four ULPs sounds like nothing; it is not. The error accumulates over the
// level and lands on contact boundaries: lv12 t=3340 has the cube leaving a
// ledge whose edge + half is exactly 4335.0000, GD sits at 4335.0005 (gone) and
// the model at 4335.0000 (still supported, the test being inclusive), so the
// model fell one tick late and stayed a tick behind for the rest of the level.
// Padding the contact tests to absorb this was tried and broke five levels at
// once (see kContactEps); reproducing GD's constant is the actual fix.
constexpr float kDxF = 1.29825044f;
// Speed portals (GameObjectType 20, object ids 200/201/202/203/1334). lv1-14
// contain none, lv15 is the first: id 202 at x=2,379 and id 203 at x=19,939.
// The per-tick advance for each is GD's own x speed divided by the 240 Hz
// physics step. 0.9 and 1.1 are measured here (the dump carries a `speed`
// column): lv15 t=129 -> 130 steps 1.29825 and t=130 -> 131 steps 1.61425
// exactly, and the switch happens on the tick the player's box first touches
// the portal's (x = 2,338.95, portal box left edge 2,353.5, half 15). The
// other three rows are GD's published speeds and are NOT yet verified.
// GD's own speed multiplier at a re-anchor (the dump's `speed` column), or 0
// when the caller did not supply one. See the --start notes.
inline double g_startSpeedMul = 0.0;
// ...and the same table keyed by that multiplier instead of by object id.
// 1.3 was 1.9492188f, "measured on lv15" -- but that measurement was of the
// FLOAT ACCUMULATION, not of the constant. GD advances x with `x += dx` in a
// float32 (advanceX does the same), so the observable per-tick step is the
// constant rounded to the ulp of wherever x happens to be:
//   lv15's 1.3 section sits at x = 19,900..21,650, ulp 0.001953125
//        1.95 / 0.001953125 = 998.4  -> 998 ulps = 1.94921875   (what was read)
//   lv20's 1.3 section sits at x = 4,994..6,700, ulp 0.00048828125
//        1.95 / 0.00048828125 = 3993.6 -> 3994 ulps = 1.9501953125
// and GD's own dumps give exactly those two numbers, averaged over 899 and 855
// ticks respectively. One constant, two readings -- so the constant is
// 468/240 = 1.95, which is also GD's published x speed for 1.3, and the model
// reproduces BOTH readings for free because it accumulates the same way.
// The old value only matched at x ~ 20,000 and drifted 1 px per ~1,020 ticks
// anywhere else; that drift is lv20's wall after the teleport fix (its first
// divergence is a wave section whose dx is exactly this).
inline float dxForSpeedMul(double mul) {
    if (mul < 0.8) return 251.16f / 240.0f;   // 0.7
    if (mul < 1.0) return 1.29825044f;        // 0.9  normal
    if (mul < 1.2) return 1.6142578f;         // 1.1
    if (mul < 1.45) return 468.0f / 240.0f;   // 1.3
    return 576.0f / 240.0f;                   // 1.6
}
// ...and the inverse. Some rules need GD's `speed` multiplier itself (the rotation
// easing while grounded; it is not proportional to dx -- the dx for 1.3 is 1.95,
// not 1.29825 x 1.3/0.9 = 1.875). The thresholds are cut the same way as cubePhysFor.
inline double speedMulForDx(double dxF) {
    if (dxF > 2.2) return 1.6;
    if (dxF > 1.78) return 1.3;
    if (dxF > 1.45) return 1.1;
    if (dxF > 1.15) return 0.9;
    return 0.7;
}
inline float dxForSpeedId(int id) {
    switch (id) {
        case 200: return 251.16f / 240.0f;   // 0.7  slow      UNVERIFIED
        case 201: return kDxF;               // 0.9  normal
        case 202: return 1.6142578f;         // 1.1  measured on lv15
        case 203: return 468.0f / 240.0f;    // 1.3  = 1.95, see the note above
        case 1334: return 576.0f / 240.0f;   // 1.6            UNVERIFIED
        default: return kDxF;
    }
}
// The CUBE's jump impulse and gravity are speed-dependent too, and not
// monotonically. Measured on lv15 (each twice, at two different places in the
// level, by injecting the cube onto a floor upstream of the speed portal so it
// really passes through it, then pressing):
//     0.9   jump 11.180   gravity 0.216
//     1.1   jump 11.420   gravity 0.215
//     1.3   jump 11.230   gravity 0.216
// A model that keeps 11.18/0.216 everywhere is ~0.24 out on the very first tick
// of every jump in a 1.1 section and drifts from there.
// 0.7 and 1.6 are NOT measured -- they fall back to the 0.9 row, and the first
// level that uses one has to be re-checked. Ball, ship and UFO constants are
// also still 0.9-only; nothing has needed them at another speed yet.
// RINGS scale with the same factor as the jump; PADS do not. Measured at 1.1:
//   yellow ring  11.180 -> 11.420   (= jump, exactly as at 0.9)
//   gravity ring  4.472 ->  4.568   (ratio 1.0214669, the jump's to 7 digits)
//   yellow pad   16.000 -> 16.000   (unchanged)
// So the scale is a property of the "jump impulse" family. The pink ring is
// assumed to be in that family too (same object class) but is UNVERIFIED, and
// so are the ball-mode ring values at speeds other than 0.9.
struct CubePhys { double jump, g; };
inline CubePhys cubePhysFor(float dxF) {
    if (dxF > 1.78f) return {11.230, -0.216};   // 1.3
    if (dxF > 1.45f) return {11.420, -0.215};   // 1.1
    if (dxF > 1.15f) return {11.180, -0.216};   // 0.9
    // 0.7. Measured on lv18 t=15,293 (full size, on flat ground at y=225,
    // speed 0.7): GD leaves at vy = 10.620 and the next ticks step down by
    // 0.212 (10.408 / 10.196 / 9.984 ...). This row used to fall back to 0.9's
    // 11.180 / 0.216, which is 0.56 vy of extra jump on every press in a 0.7
    // section -- lv18 goes 0.7 the moment it takes the size portal at
    // x=20,386, so the whole route after it was planned too high.
    // 1.6 is still unmeasured and still falls back here.
    return {10.620, -0.212};
}
inline double ringScaleFor(float dxF) { return cubePhysFor(dxF).jump / 11.180; }
// OPEN QUESTION (2026-08-01) -- the ball's tap is NOT one number at 1.1:
//   flat ground, upward tap : 3.4260  (lv15 t=10665) = 3.354 * ring scale
//   on a slope, upward tap  : 3.7056  (lv16 t=4306 with m=1.0, t=4314 m=0.5)
//   on a slope, downward tap: 3.4260  (lv16 t=4501)
// The +0.2796 does not scale with the slope's gradient, so it is not the
// surface's own velocity, and it is not a landing-speed effect either (the two
// slope samples came in at -12.714 and -1.935 and gave the same value).
// Until that is understood the FLAT value is used everywhere: it is the one a
// cleared level (lv15) depends on, and re-anchoring can absorb the 0.28 the
// slope case is out by.
inline double ballFlipFor(float dxF) { return 3.354 * ringScaleFor(dxF); }
// ...and the slope case gets a flat +0.2796 on top, which is what the three
// measurements above actually say. lv1-15 contain no slope at all, so this
// cannot move any level that is already cleared.
constexpr double kBallSlopeTapBonus = 0.2796;
// [2026-08-21 r92] The 0.2796 above was **the sp1.1 value**. On the ceilhold
// calibration rig, the bonus in the 6 ball cells (1x, |m| 0.5/1/2 x normal/mini) is
//   0.225 / 0.417 / 0.591  (the same for normal and mini)
// = **0.075 times the ball's slope-exit launch slopeExitVy(|m|,2) at that gradient
//   and that speed** (2.999 / 5.554 / 7.880 x 0.075 = 0.2249 / 0.4166 / 0.5910).
// At sp1.1, exit(0.5)=3.729, so 0.075x3.729 = 0.2797 = the old constant itself
// (the corpus's lv16 t=4,701 reproduces as before). The old form scaled only with
// the gradient via the exit ratio and **did not scale with speed**, so it was 24%
// too high at 1x.
// 0.075 = 0.25 x 0.30 (1/4 of the launch = the cube's bonus rule x the ball's tap
// ratio 3.354/11.180), but the origin of that product is unconfirmed, so it is kept
// as the one measured constant.
constexpr double kBallSlopeExitBonus = 0.075;
inline bool isSpeedId(int id) {
    return id == 200 || id == 201 || id == 202 || id == 203 || id == 1334;
}
constexpr double kYScale = 0.225;
// GD's m_yVelocity lives on a 0.001 grid. Measured over 440,000 ticks of the 19
// verified replays (build/fidelity/fid_lv*.dump.csv, dumped at precision 9):
// 99.8% of every yvel GD reports is an exact multiple of 0.001, and of the 107
// half-grid values that do occur -- all of them the direct output of a HALVING
// (a ball tap, or the ship's leave-the-ground halving) -- not one survives to
// the next tick. So the rounding is not in the impulse and not at end of tick:
// it is inside the gravity/thrust integration, which is also the value GD's own
// position update uses (lv9 t=10571: vy 2.9555 + 0.069 = 3.0245, GD stores
// 3.024 and moves y by 0.225*3.024 = 0.6804, exactly the dy in the dump).
//
// The model integrated in full precision instead, so every halving left a
// ~0.0005 residue that then rode along untouched for thousands of ticks and
// pulled y off by 0.02..0.10 px. That is small enough to hide under the
// fidelity diff's 0.3 tolerance and large enough to decide a contact: on lv9
// the ship's head reached the block underside 0.076 px short and clamped one
// tick late, and on lv10 the same 0.023 px meant it never clamped at all (the
// block's x had gone by), which is the death at t=4026.
inline double qVy(double v) { return std::round(v * 1000.0) / 1000.0; }
constexpr double kCubeG = -0.216, kCubeJump = 11.18, kCubeTerm = -15.0;
// Ball, measured on lv10's ball zone (data/ballpress.dump, portal at x=2295):
//   gravity     -0.1290 / tick   (69 consecutive airborne ticks, no variation)
//   tap         does NOT jump -- it FLIPS gravity and starts the ball moving
//               toward the new floor at 3.354 (t=1820 grounded y=105 vy=0 ->
//               t=1821 upsideDown=1 vy=+3.354, then +0.129 per tick)
//   rest height surface + 15, same as the cube (measured y=105 on the ground)
constexpr double kBallG = -0.129;
constexpr double kBallFlip = 3.354;
// UNVERIFIED: the ball was still accelerating at |vy| = 11.997 when it left the
// zone, so no plateau was observed. The cube's -15 is used as a stand-in; a
// taller ball section is needed to measure the real cap.
constexpr double kBallTerm = -15.0;
// SWING (mode 7, portal type 41; lv22 x=4,641 is the first anywhere).
// Measured on lv22's reference replay (dx=1.6143, update 76):
//   - the tap TOGGLES gravity; holding does nothing extra (push t=3700,
//     release t=3706, accel stayed positive for 100+ ticks after the release)
//   - in the player frame the whole mode is: gravity -0.086/tick, terminal 8,
//     and each tap flips the frame with vp := -0.8 * vp. Both measured flips
//     match to 0.001: -6.424 -> -0.8*(-6.424)-0.086 = -5.053 (world -5.053)
//     and 8.000 (world, flip=1, vp=-8) -> -0.8*(-8)-0.086 = 6.314 (world 6.314)
//   - terminal: vy sat at exactly 8.000 for many ticks
// OPEN: measured at this section's speed only; the grounded
// tap (launch off a surface) untested -- treated as the same toggle.
// [2026-08-20] What used to say "mini untested" was measured on the flyramps
// calibration rig: **the mini swing is 0.129/tick** (= 0.086 x 1.5). In the free
// flight right after a ramp launch GD steps 5.554 -> 5.425 -> 5.296 -> 5.167, i.e.
// by 0.129, while the same rig at normal size steps by 0.086. The same in 3 units
// (m=1 mini n=3/n=1, m=2 mini). No counter-example in the corpus (swing exists only
// in lv22, where everything is vsize=1.0 and 0.086).
constexpr double kSwingG = 0.086;
constexpr double kSwingGMini = 0.129;
inline double swingG(bool mini) { return mini ? kSwingGMini : kSwingG; }
// [2026-08-20 r39] The ship's ladder **while riding a ramp**. Read directly off
// the fixups of hp32's lv16 cold run (6 consecutive ticks at x=23,148..23,156:
// 2.000 2.086 2.172 2.258 2.344 2.430). Same value as the normal-size swing, but
// it has a different origin so it gets its own name -- the mini ship is unmeasured
// (the mini swing is 1.5x, but there is no basis yet for applying that to the ship).
constexpr double kShipRampG = 0.086;
constexpr double kSwingTerm = 8.0;
constexpr double kSwingFlipDamp = 0.8;
// --dynhazpad <px>: inflation of the moving hazards' boxes (default 0 = behaviour
// unchanged). The note is where L.dyn.samples gets filled. A provisional
// prescription for lv22's 0.19px miss.
inline double g_dynHazPad = 0.0;
// --maxplayy <y>: GD's MAX GAMEPLAY Y, the world-y bound above which
// checkCollisions declares the player out of bounds and (after two consecutive
// ticks, latch at player+0xc38) destroys it with a NULL object
// (GJBaseGameLayer::updateMaxGameplayY writes layer+0x36a8: dynamic-height
// levels use max(1200, max object y) + 90 + 300, every other level a fixed
// 2790; the kill gate is checkCollisions+0x999). The mod reads the layer's
// live value and passes it; without the flag the model keeps its old
// "the sky is open" behaviour. Measured on lv22 (bound 3,675 = 3,285 + 390,
// injection-bisected to 0.4px at two x) and lv9 (default 2,790: y=1,010
// lives).
inline double g_maxPlayY = 1e18;
inline int g_shiftDbgUid = -1;       // --shiftdbg <uid>
inline bool g_shiftDbgDone = false;

}  // namespace dp
