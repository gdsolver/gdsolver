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
// The 1.1 constant is GD's published x speed too, 387.42 / 240 = 1.61425 (the 1.3 note above); 1.6142578 was the float accumulation read at lv15's x, not the constant. As a constant it is exactly
// 826.5 ulps of x above 16,384 (ulp 0.001953125), a tie that rounds to even, so the model stepped 827 ulps where GD steps 826 every tick (lv22 reversed, x 21,100..22,400: GD 22,345.939453 -> 22,344.326172, ctraw rows 18,626..18,631; the model 22,344.324219). One ulp of x, 0.002 px, which opens a
// contact edge a few ticks later (B-v2: 11 windows of lv22 at 2e-3, all of them mini cube in the reversed stretch).
inline float dxForSpeedMul(double mul) {
    if (mul < 0.8) return 251.16f / 240.0f;   // 0.7
    if (mul < 1.0) return 1.29825044f;        // 0.9  normal
    if (mul < 1.2) return 387.42f / 240.0f;      // 1.1
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
// --seatspeed: a speed as one byte (State::seatSpd), 1..5 for 0.7 .. 1.6, 0 for none.
inline uint8_t speedCodeForDx(double dxF) {
    const double mul = speedMulForDx(std::fabs(dxF));
    return (uint8_t)(mul < 0.8 ? 1 : mul < 1.0 ? 2 : mul < 1.2 ? 3 : mul < 1.45 ? 4 : 5);
}
inline float dxForSpeedId(int id) {
    switch (id) {
        case 200: return 251.16f / 240.0f;   // 0.7  slow      UNVERIFIED
        case 201: return kDxF;               // 0.9  normal
        case 202: return 387.42f / 240.0f;   // 1.1  = 1.61425, see the note above
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
// THE BALL'S SPIN RATE, in degrees per tick, ready to be staked into
// State::rotRate. From PlayerObject::runBallRotation (@0x38d350, grounded) and
// runBallRotation2 (@0x38d480, airborne):
//
//     m_rotationSpeed = +-120 / (S * 0.20 * V)      grounded
//     m_rotationSpeed = +-340 / (S * 0.80 * V)      airborne
//     applied as (dt/60) * speed with dt = 0.25, i.e. speed / 240
//
// S is `(scale == 1.0) ? 1.0 : 0.8` -- the scale field is read but used only as
// a boolean, and the 0.8 is a literal, so the mini sprite's 0.6 never reaches
// the arithmetic. That is why mini/full is 5/4 and not 1/0.6.
//
// V is the speed divisor, and it works out to kDxF/dx: at 0.9 it is 1 and the
// rates are the round 2.5 / 3.125 (grounded) and 1.7708333 / 2.2135417 (air).
// Writing it against dx is what makes the whole family look "proportional to
// distance travelled" -- it is not. GD stakes a RATE and spends it per tick, so
// it does NOT change on a slope, where the distance covered per tick does.
//
// Checked against the corpus: grounded full 2.5000 (7,658 ticks), grounded mini
// 3.1250 (2,366), and the 1.1 / 1.3 tiers land on 3.1085 / 3.7551 to four
// decimals. air/ground = 17/24 = 0.7083333 reproduces the measured mode over
// 24,551 airborne ticks.
inline double ballRotRate(bool mini, bool airborne, double dx) {
    const double S = mini ? 0.8 : 1.0;
    const double V = (dx > 0.0) ? ((double)kDxF / dx) : 1.0;
    const double rate = airborne ? (340.0 / (S * 0.80 * V))
                                 : (120.0 / (S * 0.20 * V));
    return rate / 240.0;
}

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
    // [2026-08-30] "1.6 is still unmeasured and still falls back here" USED TO
    // STAND ON THIS LINE AND WAS FALSE IN BOTH HALVES. 1.6 is GD's 4x, its dx is
    // 2.400 (measured), and 2.400 > 1.78 -- so it takes the FIRST branch, never
    // this one. And it is not unmeasured any more: calib_speedcal_cube_s4, one
    // press on flat ground, gives jump 11.230 and -0.216/tick for the whole arc,
    // which is exactly what that branch already returned. The 1x control on the
    // same rig gives 11.180 / -0.216, matching the 0.9 row to the digit.
    // So the 1.3 row covers 1.3 AND 1.6, now on measurements of both.
    // What actually falls back here is 0.5x, and that IS the measured row below.
    return {10.620, -0.212};
}
inline double ringScaleFor(float dxF) { return cubePhysFor(dxF).jump / 11.180; }
inline bool isSpeedId(int id) {
    return id == 200 || id == 201 || id == 202 || id == 203 || id == 1334;
}
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
constexpr double kBallFlip = 3.354;
// --dynhazpad <px>: inflation of the moving hazards' boxes (default 0 = behaviour
// unchanged). The note is where L.dyn.samples gets filled. A provisional
// prescription for lv22's 0.19px miss.
inline double g_dynHazPad = 0.0;
// --dynseamgap <ticks> (default 30000 = the old fixed rule): a gap between two of a moving object's recorded rows larger than this is read as a seam between attempts
// and the rows after it are dropped. The recording keeps a row only when the object changes, so an object at rest for a long stretch has a real gap; a level longer than
// 30000 ticks (the 58000-tick horizon one has a saw that first moves at t=33715) lost every row after the first.
inline int g_dynSeamGap = 30000;
inline int g_dynSeamCut = 0;          // moving objects whose recorded rows the seam rule cut (printed with the level line)
constexpr double kSeamMinPxPerTick = 1.0;

// THE LEVEL'S OWN COMPATIBILITY FLAGS (LevelSettingsObject). The mod writes
// them beside objrects as `levelsettings.txt`; the loader picks the file up by
// name, so nothing has to be threaded through a caller.
//
// Of the 22 official levels only lv22 has any of them set, and it has five
// (lab: flags-levelsettings-branches-2026-09-01.md, collected for all 22 --
// every other level is all-zero). All five are carried here because reading
// four of them costs nothing and the next question about lv22 will want them;
// only fixRadiusCollision has a reader today.
//
// fixRadiusCollision is kA39 at LevelSettingsObject+0x1cf, and
// GJBaseGameLayer::playerCircleCollision (0x211df0) branches on it in its first
// instruction: zero picks the player's RECT against the circle, non-zero picks
// centre distance. See hazardHit.
inline int g_fixRadiusCollision = 0;
inline int g_fixGravityBug = 0;
inline int g_fixNegativeScale = 0;
inline int g_fixRobotJump = 0;
inline int g_dynamicLevelHeight = 0;
// platformerMode is LevelSettingsObject+0x155. GJBaseGameLayer::loadLevelSettings (0x23ad00) copies
// it to the layer (+0x309e) and to both players (+0xb70). Its one reader here is --movelockx.
inline int g_platformerMode = 0;
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
// --goalgd (default off): the goal is where GD ends the level. PlayLayer::checkForEnd (0x3b4cf0) finishes it on the
// tick the player's x reaches EndPortalObject::getSpawnPos (0x1381a0): the end portal's x - 300 / the camera zoom
// (the layer's +0x1a8), plus instead of minus for a portal flipped in x (m_flippedX). L.maxX + 60 is past the last
// object, and objects can lie well past the portal: one custom level completes at x 23,466.17 against its portal's
// 23,765 where the model's goal was 26,056, another at 4,803.66 against 5,103 (both 300 short of the portal to
// within a tick), the replay of its solution dying 15 ticks after GD's end in the pit the level ends over.
// The portal comes from the mod's levelsettings dump (endPortalX, endPortalFlipX). With no portal (an older
// dump, or a level without one) or a flipped one (this goal test runs rightward), the goal stays L.maxX + 60.
// An End trigger (id 3600) can finish a level earlier; the portal's rule still holds as a point GD does not
// pass, so the model then asks for more than GD does, never less. The zoom is unwitnessed: every completion
// measured ended at camscale 1.
inline bool g_goalGd = false;
inline double g_endPortalX = 0.0;   // levelsettings endPortalX (0 = not in the dump)
inline int g_endPortalFlipX = 0;    // levelsettings endPortalFlipX
// levelsettings reverseSync (the level's kA42, LevelSettingsObject::m_reverseSync): PlayerObject::reversePlayer books the
// mirror debt (revReflect) only with it set. -1 = the dump has no such line (older dumps): treated as set, as before.
inline int g_reverseSync = -1;
// levelsettings enablePlayerSqueeze (the level's kA31, LevelSettingsObject::m_enablePlayerSqueeze, +0x1c9 in 2.209): the mod writes the line only for a level
// that has it (so every other level's file is the bytes it was); 0 = no line = off. GJBaseGameLayer's level start stores `(+0x1c9 == 0 && layer+0x317e == 0)` into
// both players' +0xc48 (RVA 0x34532a / 0x345352), and PlayerObject::postCollision skips its crush rule while +0xc48 is set (collide.hpp "crushed:"): the rule
// runs only on a level with Enable Player Squeeze. The shadow's template image gets +0xc48 = g_enablePlayerSqueeze ? 0 : 1 (shadow_step.hpp).
inline int g_enablePlayerSqueeze = 0;
// --rawvalues (EXPERIMENT): the gravity step BEFORE the 0.001 grid. The literals the cube branch
// uses (0.216 / 0.215 / 0.212, the ball's 0.129, the robot's 0.194 / 0.195) are GD's steps as
// they show on the grid, and they are right whenever vy is already on it. From an off-grid vy
// they are not: a mini ball's tap leaves at -2.6832, and GD's next vy is -2.813 where
// -2.6832 - 0.129 rounds to -2.812 (a custom level t=9,386, and the drift is where eight of the old
// custom levels' divergences start). GD steps by g x 0.225 (x0.6 ball, x0.9 robot) with g its
// per-speed gravity (accelSwitchVyForSpeed's table) and rounds after; -2.6832 - 0.129357 =
// -2.8126 -> -2.813. Every literal above is that value on the grid, so an on-grid vy is unchanged.
// The ball's is flat across speeds (0.129 measured at all four), so it takes the 0.9 row.
// The same holds for a ring's value, which ringJump sets as a raw product: the ball's gravity
// ring is 4.472 x 0.7 = 3.1304 in GD (a custom level t=18,078 stores 3.13039995), where the model's
// constant was the rounded 3.130 -- on-grid itself, so the next tick rounds the other way.
// On by default (with --padtable (removed)); --no-rawvalues turns it off.
constexpr int kRawValuesDefault = 1;
inline int g_rawValues = kRawValuesDefault;
// ...and the other half of the same grid: the launches GD writes ON it. A slope's exit launch
// (6.906 at lv16 t=5,401 / lv22 t=3,806, 2.499 at lv18 t=10,824, the ceiling release, the
// swing portal's 2.762) and a ring's jump x ratio (red 11.18 x 1.38 -> 15.428 at lv22 t=297,
// pink 8.05 / 8.222) come out on the grid, where the model's formulas leave them a fraction off
// (6.905590, 2.499375, 15.4284). With the rounded gravity that fraction happened to round away
// on the next tick; with the raw one it rounds the other way, so under --rawvalues the launch
// is rounded where it is written. A ring's POST multipliers (ball / spider x0.7, the gravity
// ring's x0.5) come after the rounding and stay raw: 3.1304 and the pink ball's 6.0263 =
// q(11.18 x 0.77) x 0.7 are both what GD stores.
// The float cast first: lv17 t=18,572's slope exit is -3.7025 exactly on paper, GD's float of it
// (-3.70250010) rounds to -3.703 where the double rounds to -3.702.
inline double rawQ(double v) { return g_rawValues ? qVy((double)(float)v) : v; }
// --waveslopeminik <R,L,Y> (default none): the mini wave's three ramp constants set one by one instead of by one scale (constants.hpp: kWaveSlopeDxR 3.072,
// DxL 4.068, Dy 4.0 at full size). The scale 0.663 above fits one corner pose and misses the next: a second one of the same plan (t=37,578, a ramp's
// corner where the sample is clamped to the box edge, so only Dy shows) has GD alive up to +0.25..0.50 px above the pose where 4.0 x 0.663 = 2.65 kills, i.e.
// Dy is 1.97..2.22, not 2.65 -- and the first one (t=25,302, unclamped) has Dx + Dy = 5.35. A rig of the mini wave 4x fast against ramp 1338 in its four
// orientations (GD only, injection four ticks before the pose, window matched to the model's: py/mklevel.py waveslope_mini, oneoff wsl_sweep.py)
// gives Dy = 2.003 (the clamped flat part), Dx_L = 3.237 and Dx_R = 2.241 (the two sloped parts), i.e. 0.50 / 0.80 / 0.73 of the full-size values. The Dx_L
// used here (3.246) is the first plan pose's (the larger of the two witnesses, so the more careful side of the kill line) and Dy 2.1 sits inside the
// second's bracket; Dx_R is the rig's alone. NOT a calibration: it is two corners of one level and one rig.
inline double g_wsMiniR = 2.241, g_wsMiniL = 3.246, g_wsMiniY = 2.1;   // on by default (release flags, 2026-10-17): the rig's Dx_R, the first plan pose's Dx_L, a Dy inside the second pose's bracket (see above); --no-waveslopeminik restores the single scale
inline double g_startRevOff = std::nan(""), g_startRevCarry = std::nan("");
// Always on (was --balldashsprite): the ball's angle on the tick its dash ends is the dash art's spin, not 0. updateDashArt
// (0x3961c0) runs a CCRepeatForever of CCRotateBy (0.3 s, 288 degrees: 960 a second, 16 a scheduler frame at 1/60 s) on two
// child sprites when a cube or a ball starts a dash, and stopDashing (0x396650) hands the angle to the player at the end
// (the cube's law is --dashsprite (removed)). The game steps the action manager once per update call of 4 ticks, with the dt scaled
// by the scheduler's time scale, which is the Time Warp in force (applyTimeWarp, also above 1), and a cocos action drops
// the time of its first update. So the angle on the release's row is the sum over the frames f from f0 + 2 to f1 of
// 16 x warp(x of row 4f + 1), f = (row - 1) >> 2, f0 the frame the dash began in, f1 the release's. The spin on from
// there is the ball's own stake (3.755 a tick at speed 1.3), as before. Measured on a custom level's ball dash rings
// (dash ring rot -68, one repeated warp 1.5 / 1.0 section): 21 dashes of 1 to 40 ticks in 20 attempts' rows, release
// angles 0 to 216 (0, 16, 24, 96, 120, 128, 144, 152, 160, 168, 192, 216), every one within 0.0005 degrees of the sum
// (a constant 24 a frame, 16 x 1.5, fits 17 of them: the four others straddle a 1.5 / 1.0 step of the warp). A dash of 4 ticks or fewer reads 0, which is the case --balldashrot (removed) was measured on. What a
// non-upright ball does (the sign) is the cube's rule and is not measured here. Hangs on --balldashrot (removed).
// --stopjump (ON since 2026-10-24; --no-stopjump is the off arm): Player Control (id 1932) with Stop Jump (property 540). GJBaseGameLayer::
// activatePlayerControlTrigger (0x2174e0) does, to each targeted player (property 138 = player 1, 200 =
// player 2, neither = both), what PlayerObject::releaseButton(1) (0x3981d0) does: in a classic level it
// clears the player's held-button entry for the jump button, and when the player is not locked it puts
// a streak point down, zeroes m_jumpBuffered (+0x985) and sets +0x99c, and ends a dash. NOTHING PERSISTS --
// no flag stays up -- so the jump works again at the next press. The bot hands the game only the edges of
// its plan, so a plan that keeps holding stays released until its own next release-and-press: here the
// effective button is 0 from the tick after the trigger fires until the plan's raw level goes to 0
// (State::pcRel). The trigger fires, in the collision pass at the end of tick t, when (touch-started,
// property 11) the player's box overlaps its box -- once, not on every tick of the overlap -- or (neither
// touch- nor spawn-started) the move of tick t carries x past it; the effect is on tick t+1's physics.
// NOT modelled: the other three components (Stop Move is a platformer button; Stop Rotation 542 and Stop
// Slide 543 zero fields the model does not carry), player-2-only triggers, a dual, a turned frame, and
// spawn-started triggers (the level scan names those).
inline bool g_stopJump = true;
// --reversetrig (ON since 2026-10-24; --no-reversetrig is the off arm): the Reverse trigger (id 1917). GJBaseGameLayer::reverseDirection (0x218160) calls
// PlayerObject::doReversePlayer (0x398510) on each player that is not locked: it TOGGLES m_isGoingLeft (+0x9c2) -- no
// correction owed, unlike the ring or pad (reversePlayer 0x398430, --revcarry) -- and the next tick's x step runs the
// other way at the same speed. A crossing trigger fires in the collision pass of the tick whose move carries x past it;
// the first tick that runs left is the next one. Forward crossings only: a Reverse met while going left (the queue's
// direction there is not measured), and touch- or spawn-started ones, are left as they were.
inline bool g_reverseTrig = true;
// --spidernoextcol: the spider's surface search (step.hpp spiderTargetGd) does not see an Extended Collision object.
// spiderTestJumpInternal asks staticObjectsInRect / damagingObjectsInRect, which walk the section grid, and
// addToSection files such an object in the layer's always-checked list instead (memory: the --bigsection note).
// A custom level t=7,037: an upside-down spider taps down past a 477 x 30 id-3610 hazard with extcol=1; GD's
// hazard query returned another object, its solid strip a block it landed on 588 px down, where the model
// counted the 3610 as found, widened the solid query and landed on a column beside the strip 78 px down.
inline bool g_spiderNoExtCol = true;
// Always on (was --autopartsmove): in the loader's no-op-touch test, an object an autonomous Move places counts as MOVED when any one of its Moves has an
// offset, not only when the offsets' SUM is non-zero (level_loader.hpp autoMovesAny). A custom level's thorn uid 38544 is moved -90 (602 ticks) by one Move and +90 (658 ticks) by the next: the sum is 0, so the loader read it as
// moved by nothing but the touch box it also sits in (a no-op box), classed it touch-controlled with no fired controller, and held it at its base x for ever while GD's
// recording slides it 13 px by t=862 and 88 px by the cube's death at 1262 -- the cube GD dies on 1.4 px of that thorn, the model flew through.
// Always on (was --facecarry): a rider is carried by the FACE of the surface under it, not by its centre. A solid that is scaled down
// (an Area Scale, a Scale trigger) keeps its centre and its face sinks with the half-height; GD keeps a grounded player on it (onGround 1, vy 0, y following the
// face -- 0.089, 0.276, 0.276 px a tick), where the model, reading only the centre's step (Obj::dcy), let it fall and re-land. A custom level's ball on a 192 x 30 bar
// (id 212, rot 90) whose half-height goes 15.0, 14.9, 14.6 ...: the model's ball is 0.03 px above the face on the first tick and 0.2 px by the third.
// Always on (was --slideseat): an armed wave (the slide counter, +0xb78) that a block's top face pushes out is not killed by that block's kill box on the same tick.
// collidedWithObjectInternal (0x391a70) lets it fall through into the common push-out instead of the kill; --waveramp already skips the kill for a plain-size wave on a ramp, this is the same
// for every size. A custom level's mini wave sliding along a block top (y 738 = top 735 + 3) is put back on it every tick, and the model killed it where the free fall (734.1) was 14.1 px from the block.
// Always on (was --waveminiramp): the mini wave rides plain ramps like the plain-size one (its own 6x6 rect, the slide counter's gates). A custom level's mini wave
// sliding along a block top (y 738) into the foot of a 30x60 ramp is put up on it (+3.739) for one tick and killed on the next, as the model's plain-size path does; before, the model killed it a tick early.
// Always on (was --revxhi): a reversed anchor (running back) carries its farthest x too (--startxhi). The forward crossing queue sits at that x whatever way the body runs now, so a trigger
// behind it does not fire again when the body turns round and comes forward past it (a custom level: a teleport trigger at x=6825, fired on the way, crossed again after the anchor's reverse run; GD does not fire it).
// Always on (was --portprevhalf): "already inside" a dual's gravity portal is asked of the box the body had on the
// PREVIOUS tick -- its size and mode before that tick's portals -- not of the size it has now. A custom level: a mini UFO takes a normal-size
// portal and the cube portal on one tick, and the gravity portal 4 px behind them in the same stack; the body is normal-sized by the next
// tick, whose half (15) puts the previous position inside the portal's box (24.1 <= 12.5 + 15) where the mini's (9) did not (24.1 > 21.5), and
// the model skipped the portal GD fires on that tick (both bodies' gravity swapped, vy halved).
// --followtouch (default OFF until a rig measures it; --followtouch turns it on): a Follow Player Y (1814) that the player starts by touching its box is
// loaded like a crossing one, but starts per state: when that state's box overlaps the trigger's (State::followOwn / followN). The
// group's objects stay where the recording puts them for a state that has not touched it.
inline bool g_followTouch = false;
// Always on (was --landjumpbonus): a cube or robot that LANDS on a ramp and jumps on that same tick gets the ramp's jump bonus (a quarter of its exit velocity at the
// ride's first-ticks factor, capped at 0.4 of the jump). GD's pass makes the contact first (the on-slope flag), then the buttons phase reads it; the model's jump is taken in the solid
// pass, before the ramp pass, and only a ride that began on an earlier tick (State::onSlope) carried the bonus. A custom level: robot jump on the landing tick GD 6.369, model 5.590.
// Always on (was --slopewarp): the launch off a ramp's end is scaled by the time warp at the player's x. GD's exit velocity is the ramp's m_slopeVelocity x the ride's
// factor, and under a Time Warp trigger of 0.95 a custom level's robot leaves at 6.473 where the model gave 6.814 = 6.473 / 0.95.
// Always on (was --anchorrot): an object whose recording starts by TURNING in place (rotation or box size, no position change) keeps the earliest controller as its
// anchor -- the pivotless Rotate whose crossing starts those rows -- instead of --anchormove (removed)'s first Move. A custom level's spinning spike is turned from x=16,425 and moved from
// x=16,545; its first recorded row is the turn, the Move anchor read the recording 61 ticks early, and the model put it 6 px from its recorded place in the run that recorded it.
// Always on (was --wavegrace): an armed wave keeps riding ramps for one more tick, the tick after the counter was last set (State::slideT 0), as the counter's
// decay would. A custom level's mini wave rises past a locked 1755 (30x30) and GD still rides the ramp the tick it is 22.9 px above its centre, a tick after the last touch.
// Always on (was --padinv): a turned pad's real box is rebuilt from BOTH sides of its bound when the uniform scale the loader assumes does not reproduce the bound's height.
// A custom level's pad (raw 25 x 4, rot 29.46, bound 118.6 x 69.4) is 134 x 4 -- stretched along its length only -- and the uniform scale read 125 x 20, a box five times too thick: the
// pad fired a tick before the game's.
// Always on (was --padorientlive): a TURNED pad meets the box the player has after a mode portal of the same tick, as a turned portal does (the r32 rule: GD re-reads the player's
// oriented box on the spot for a rotated object, and keeps the entry box for an axis-aligned one). --padwaveold (removed)'s wave box stays for the axis-aligned pad it was measured on.
// Always on (was --portprevhalforient): --portprevhalf's previous-tick half for a dual's gravity portal also holds for a TURNED portal. Its fire test reads the live box (pHalfLive,
// the box a mode portal of the same tick has just changed); "already inside" has to read the box of the previous tick, whichever kind of portal it is.
// Always on (was --landtaprot): a ball that LANDS on the press tick and taps takes the tap's rotation too -- the air step rate, the spin turned the other way, the 0x728 stake -- as the
// ordinary tap does. The landing tap set the flip and the velocity and left the spin as it was: a custom level's ball keeps turning its old way after the tap and is 313 degrees off the game's
// spin 330 ticks later, which turned a ship portal's oriented test a tick early.
// Always on (was --ceilseatland): a ship a ceiling ramp's underside pushes down onto a floor block (its foot ends on the block's top face) lands on the SAME
// tick -- vy 0, grounded -- as the game does. The model took the underside's release write (-2.000) and landed one tick later, which put its hover on a floor under a ramp's end a tick behind.
// Always on (was --wavecorner): a normal-size wave's kill margin over a FLOOR ramp stops at the ramp's high end (the larger of its two line heights). A custom level's peak of two
// spiked ramps: the game's boundary, measured by injecting the wave at 12 x positions and bisecting, follows the rising line's margin up to the apex height and then stays flat at the apex for the 10 px
// around it; the model kept the 4 px margin above the apex there.
// Always on (was --onlag): a recorded row that switches a hazard ON takes effect the tick after it (the OFF row already does, --offafter), and on that tick the sweep over the tick
// leaves out its first sample, the previous tick's end position. A custom level: a mini wave zig-zagging 22.8 px from a saw (radius 21.6) that a toggle brings on at t=16,381 -- the game lets it through,
// and the next tick tests it only from the end position (bisected in the game on three ticks: 6.0 px under the model's boundary, then equal, then 3.9 px over it).
// Always on (was --waveunder): an armed wave that a ramp seated into the underside of a slide block is put back under the block's bottom, after the ramp's seat (the block's push-out is
// decided on the free-flight y, before the ramp lifts the wave). A custom level's mini wave riding a slide ramp's last stretch under a block: the game 250.0, the model 250.21 and then a launch into the block.
// Always on (was --zerosaw): a round hazard whose rect is empty (w = h = 0, a saw at scale 0) is not a hazard. A custom level has twenty of id 1619 exported with radius 25 and
// size 0; a mini wave flies through the centre of one at t=16,600 and the game does not touch it.
// Always on (was --tapfeed; read only under --worldgates): a Tap whose chain only feeds a Pickup is kept as a gate when a kept Count / Item Compare reads that item. Without it the Count of a
// custom level (item 10 == 4, which starts a Move of 529 objects) never reached 4 in the model and its spike stood 24 ticks longer than in the game.
// Always on (was --tap2; read only under --worldgates): the Taps that feed a kept Count are counted ALL, not one per call. A call held one (State::taps, the first the level lists) and read a second as a box that gives once: a custom level's
// Count (item 2 == 6, which turns a 72-px spike back on) never reached 6 in the model and the spike stayed off, where the game's six presses turned it on at t=34,136 and killed the wave. State::tapsX holds the others' counts, up to g_tap2Max in all.
// Always on (was --dashface): a dash moving against the body's gravity stops on the solid face it crosses. A custom level t=10,472: a flipped spider dashing down (6.594 a tick, the button held) meets the top of a row of blocks (270.5);
// GD puts it at 284.000 = 270.5 + 13.5 and holds it there for the four ticks of the hold left (then gravity takes it up from 284.029), the model dashed on through the blocks to 259.794.
inline int g_tap2Max = 4;
// --spdseenall: the nodes of State::spdSpill. Appended under a lock (a push is a portal the four places could not hold, rare).
struct SpdSpillNode { int32_t uid; const SpdSpillNode* prev; };
inline std::mutex g_spdSpillMu;
// A node is made once for each (list so far, portal): two states that gave up the same portal on the same list share it, so the pool grows with the lineages that met a crowd of portals and not with the
// states that did. It is NEVER freed: dp calls can run side by side in one process (the lanes), the nodes are reached from states of any of them, and a call that frees "its" nodes at its start left the
// other call's states pointing into freed memory (the first run with the flag on crashed in spdSpillHas on its first search).
inline std::unordered_map<uint64_t, std::vector<SpdSpillNode*>> g_spdSpillPool;
inline size_t spdSpillCount() {
    std::lock_guard<std::mutex> lk(g_spdSpillMu);
    size_t n = 0;
    for (const auto& kv : g_spdSpillPool) n += kv.second.size();
    return n;
}
// ...and how many times a portal found all six slots full (it then fires on every tick of the overlap, silently): printed in the summary of a search (after capstat:) and of a replay, so a level that
// has a still longer row of portals shows it. Counted over every state the call steps, the search's dead branches included. Only under the flag.
inline std::atomic<long long> g_portSeenOverflow{0};
// Always on (was --waveunderflip): the end-of-tick limit of a wave a ramp lifted under a block (--waveunder: the block's bottom less the half stays the limit after the ramp's seat) holds for a FLIPPED wave
// as well. It was written for the upright one; a block's bottom does not care which way the wave is turned. the long custom level t=28,899: a flipped mini wave carried up a ramp under a block (bottom 990.0, x to 37,943.9)
// is held at 987.000 by the game for two ticks (vy 0), the model's ramp lifted it 2 px a tick past the limit (989.0 at 28,900) and it left the block's x range 2 px high; a 59-tick plan is then killed
// at 28,958 where the model's replay carries on. With the flag the replay follows the game's dump to its death, on the same tick.
// The --start hold field (its 7th, held_before(plan, t0)); -1 when no --start was given.
inline int g_startHoldField = -1;
// Ring first (was --ringfirst (removed), always on since 2026-09-26):
// a press that finds a ring is the ring's. pushButton fires the touched
// rings and returns; it calls updateJump only when there are none. The model runs the grounded
// jump or tap in its physics branch and the ring loop after it, so a grounded ball pressing inside
// a gravity ring flipped twice. A custom level t=11,833: a ball on the ceiling presses in gravity ring
// uid 1981; GD comes out upright at -3.1304 (the ring alone), the model tapped (flip) and then
// fired the ring (flip back) and stayed on the ceiling at +3.1304. The tap's flip and spin are
// undone when a ring fires on the tick of the press that made them.
// Ring order (was --ringorder (removed), always on since 2026-09-26): which touched ring a press fires is
// GD's m_touchedRings order, carried in State::touchRing, instead of "in contact a tick ago beats
// not, then the lowest uid". The array
// keeps a ring's first-contact position; at a tick's head resetTouchedRings drops the rings the
// tick before did not touch, and the collision pass appends new contacts in ascending uid. One
// tick of history cannot tell apart two rings entered on different earlier ticks: a custom level t=2,741,
// a ship in contact with yellow 447 (entered ~2,735) and gravity 443 / yellow 446 (a tick later,
// one square), and GD fires 447 where the lowest-uid rule fired 443.
// Gravity pad chain (was --gravpadchain (removed), always on since 2026-09-26):
// the gravity pads a tick enters are taken in uid order and every one
// the facing gate lets through flips. collisionCheckObjects (0x2158f9) sets gravity to the pad's
// own direction (!isFacingDown) when it differs, then propellPlayer and flipGravity, pad by pad
// in the bucket's uid order -- so two pads facing opposite ways flip twice. The model's "one flip
// per tick" (gravPadFlipped) is right only for pads that agree, which the gate already reduces to
// one. A custom level t=1,307 enters a wall of sideways blue pads (rot -90, flipY alternating = facing
// alternating); GD comes out upside down at +6.4 where the model stopped after the first flip at
// -6.4 (another custom level t=6,960 and a third custom level t=8,103 are the same wall kind).
// Pad first (was --padfirst (removed), always on since 2026-09-26):
// a pad that fires on the tick of a grounded press leaves the press
// unspent. The pad fires in the collision pass and propellPlayer drops the ground flag; the buttons
// run after it, so updateJump finds the player airborne and does not jump, and +0x986 stays set
// for a ring touched later in the hold. The model jumped in its physics branch (spending the
// press) and let the pad overwrite the velocity. A custom level t=15,114: an upside-down cube presses on
// the tick a blue pad flips it (both give -6.4), and on the next tick GD fires yellow orb uid 1503
// with that press (11.18) while the model, its press spent, did not.
// One-way ceiling (was --onewayceil (removed), always on since 2026-09-26):
// a one-way platform's world underside is a ceiling for the flying
// modes. collidedWithObjectInternal lets ship, UFO, wave and swing into its ceiling resolution
// unconditionally; the cube's head branch is gated (the arms), which is what the "transparent from
// below" measurement saw (lv13, a cube jumping up through type-21 blocks). A custom level t=9,220: an
// upright ship holding up into plate uid 840 (id 143, underside 136) stops at y=121 with vy 0 in
// GD, where the model flew through. It still never kills (the plate's side test stays off).
// Portal flap (was --portalflap (removed), always on since 2026-09-26):
// a fresh press on the tick a cube enters a UFO portal is the UFO's, and
// it flaps on the NEXT tick. The press reaches pushButton in the buttons phase, after the collision
// pass made the player a UFO, and a UFO flaps in updateJump on the next update -- the ship->UFO
// rule's pending flap (State::pFlap). The model jumped as the cube and re-issued the flap on the
// portal tick itself, one tick early (a grounded cube), or dropped the press (an airborne one).
// A custom level t=15,402 and the calib_portalpress cells cube->ufo at offset -1: GD flaps at the press + 2,
// grounded and airborne. The reverse (ufo/ship -> cube, press one tick before the portal: GD jumps
// on the portal tick) is not representable in the model's per-tick input -- its in(t) = 1 on that
// tick already means the old mode's action -- and a plan the search writes never contains it.
// --heldall (EXPERIMENT): State::held is the button level of the tick in EVERY mode. Only the wave
// and flying branches wrote it, so a body that entered a wave from the ground read whatever the
// last flying section (or the anchor) left: the wave's first direction is `s.held`. Official lv21's
// quick_regress section from t=13,000 anchors in a robot section with the button down, the robot
// never rewrites held, and at the robot -> wave portal (t=14,391) the model climbs at +6.457 where
// GD dives at -6.457 with the button up. The ship's release re-clamp (holdRelU) reads it too.
inline int g_heldAll = 1;
// --heldcellcap (EXPERIMENT, only with --heldall): the alive cap counts CELLS, not states. Under
// --heldall a cell whose states differ only in the button (held, jumpBuf) keeps them apart, as it
// should, but each twin then takes a slot of the cap and the water-fill's stride runs over twins
// standing side by side, so the layer keeps fewer cells than it would without the flag (lv19:
// x~3,541 needs cap 2,000 with --heldall where 125 did without). With this the water-fill samples
// one state per cell and every state of a sampled cell is kept.
inline int g_heldCellCap = 0;
// --heldcelltwins N (with --heldcellcap; 0 = no limit): keep at most N states of a sampled cell --
// its first state, then the states whose held differs from it, then the rest, each in layer order.
// A cell holds up to four (held x jumpBuf), so without a limit a layer can keep four times the cap:
// measured over the official 22 at t=0 (2026-09-26), lv20's stepped children went
// 60.1M -> 346.7M and its peak frontier to 22x the cap, lv14 2.8M -> 125.7M.
inline int g_heldCellTwins = 0;
// --sametick (default on since 2026-10): a replayed plan's press and release on ONE press tick are read as GD's button
// pass takes them, not as the tick's last level. The mod feeds both to handleButton in that tick, in order.
// pushButton (0x397f40) sets the hold bytes (+0x985/+0x986) and acts AT ONCE: ringJump on each touched
// ring, or -- none touched -- updateJump(0) when the body is on the ground (+0x9c1) and is the ball
// (+0x9bb), or is none of ship (+0x9b9), UFO (+0x9ba), wave (+0x9bc) and swing (+0x9c4). releaseButton
// (0x3981d0) clears the hold bytes (and stops a dash) before the update, and updateJump's flap, thrust and
// climb all read them, so nothing else of the press survives. Here a tap holds for its effect tick only
// when a trial step shows that work (a ring used, or a grounded ball's, cube's, robot's or spider's jump);
// otherwise the old reading (no press) stands. A release then a press while held is a fresh press, seen
// as a rising edge on the effect tick.
// Measured in GD: a custom level t=16,500 (a loop splice left a mini UFO's press and the tail's seam
// release, written a latency early, on one tick): GD jumps one row after the tap wherever it is put
// (16,499 / 16,500 / 16,502), through an orb that moves with the player. Two custom levels whose mini UFO
// taps touch no ring (one at t=11,678, one at 4,387, both upside down): GD does not flap. The trunk
// solutions' pairs are a ship's: no thrust (lv20 t=14,095).
inline bool g_sameTick = true;
inline bool sameTickTapMode(uint8_t mode) { return mode == 0 || mode == 2 || mode == 5 || mode == 6; }
// Ground-grow (was --groundgrow (removed), always on since 2026-09-26):
// a cube or ball that a size portal grows while it stands on the WORLD
// ground is lifted to the new seat on the NEXT tick, by the ground clamp, not on the portal's own
// tick. GD's ground plane is enforced in PlayerObject::update, before checkCollisions fires the
// portal, so the grown body spends one tick embedded; a block is resolved in checkCollisions and
// can re-seat it the same tick (lv12 t=3,286, which stays). Old custom level t=17,939: a
// mini ball on the ground (y=99) meets RegularSize portal uid 2257 -- GD reads vsize 1 at y=99
// and 105 only at t=17,940, where the model went to 105 at once. The calibration rig's reading
// (cube next tick, ball and UFO the same tick; the retracted r69 note at the re-seat) is left
// alone for the flying modes, which have their own floor clamp.
// Ring press pin (was --ringpresspin (removed), always on since 2026-09-26):
// a ring taken off a block by a FRESH press leaves y on the block. GD
// reaches a ring by two paths: a button already down fires it from the collision pass
// (playerTouchedRing), before the solid pass puts the body back on the block, so y keeps that
// tick's gravity step; a press fires it from pushButton in the buttons phase, after the solid pass
// has re-seated it. The model released the block pin for every ring. Old custom level
// t=2,126: a mini cube on block uid 204 taps (press line 2,125) inside yellow orb uid 200; GD
// leaves at y=150.000 with 8.944, the model at 149.951. The orb evidence at the release (lv19
// t=254/506, lv16 t=6,404, lv18 t=10,824) has no press at those ticks in today's plans.
// Solid order (was --solidorder (removed), always on since 2026-09-26):
// the two main collision loops (ground and flight) take the near list
// in DESCENDING uid, the order GD resolves solids in. The cx sort leaves equal-cx objects in the
// unstable sort's order. Read from GD's own collidedWithObject calls (hbox lines, the ppre of each
// call being the previous call's result) over a custom level t=1..3,099: 306 ticks with two or more
// solids, 306 in descending uid, 0 ascending, 86 of them across two 100-unit x cells.
// The same custom level, t=9,235: slab 4959 lands the ball (y 240), one-way plate 4956 then pushes its head
// down to y=231, slab 4931 does nothing, and spike 4927 kills at 231. This is the solids only:
// the bucket walk that meets rings and pads (ascending uid, 0x205210) is a different pass.
// --solidorddbg t0:t1 (diagnostic): print `solidord t= uid=` for each solid a collision loop visits
// while the player's box overlaps it, in visiting order, for ticks t0..t1 -- the model's side of
// GD's hbox call order, for the consumer that checks the two orders tick by tick. Replays only:
// the search visits the same tick once per state.
inline long long g_solidOrdDbgT0 = -1, g_solidOrdDbgT1 = -1;
// --spentorb uid,uid,... : the rings GD fired earlier in the attempt, oldest first, seeded into the
// anchor state through noteRingFired. A ring fires once per attempt, but an anchored state starts
// with usedOrb empty, so the model re-fired a ring GD had spent just before t0. Old custom level
// six kitref episodes anchored at t=2,561 in a field of stacked yellow and gravity rings:
// GD fired a ring at 2,555..2,558 and, on the next press, the one beside it; the model fired the
// spent one again. The mod's cfg dpspentorb passes the list (ringseed).
inline std::vector<int> g_spentOrb;
// --spentp2 (default on since 2026-10): ...and --spentorb2, the second body's, into its own ring memory (usedOrb2 /
// usedOrbOld2 / usedOrbHist2, the halves swapHalves trades). GD keeps a ring's "fired" per body
// (EnhancedGameObject::hasBeenActivatedByPlayer: +0x5b4 for p1, +0x5b5 for p2), and collisionCheckObjects
// skips a ring that answers yes. --spentorb carried p1's alone, so an anchored model's p2 fired a ring GD's
// p2 had spent: a custom level's dual cube took yellow ring 18683 at t=12,912 in GD (its act / actP went
// 0 -> 1 that tick, on the ring's edge), and after the 12,918 anchor the model took it again at 12,938.
// The mod passes --spentorb2 only to a call that does not carry --no-spentp2.
inline std::vector<int> g_spentOrb2;
// --spentglobal (default on, --no-spentglobal is the off arm): ...and the rings --spentorb / --spentorb2 name stay spent for the whole call. The
// fired-ring memory is five slots (usedOrb, usedOrbOld, usedOrbHist[3]), and GD's is every ring of the attempt: a ring that fired before t0 is
// the same fact in every branch, so it needs no slot, just the uid set the anchor was given. A custom level's yellow ring is carried across the
// level by Move triggers and overtakes the player once at t=8,060 (GD fires it, vy 11.42); at t0=8,965 it was the fifth-newest spent ring, and
// by t=9,320 four rings fired since had pushed it out -- the model took it again at its new place (a 0.5 px corner overlap) where GD, whose
// ring answers "fired" for the rest of the attempt, ran on at -6.02. Found by a kitref episode (a custom level, iteration 4) and measured on the game: a
// static ring's contact edge is the box sum (33), and this ring never fired at all while the cube went through it, only its neighbour did.
// The maps are indexed by uid, one per body (ringFiredBefore's second argument picks the second body's).
inline bool g_spentGlobal = true;
inline std::vector<uint8_t> g_spentMap1, g_spentMap2;
inline void markSpentGlobal(std::vector<uint8_t>& m, int uid) {
    if (uid < 0 || uid > 4000000) return;
    if ((size_t)uid >= m.size()) m.resize((size_t)uid + 1, 0);
    m[(size_t)uid] = 1;
}
// --histstat <file> (print only): after a replay, one line appended to <file> with how full the
// fixed-length histories got and how often one dropped an entry: touchRing
// (4 slots, --ringorder (removed)), portSeen (3, --portalonce (removed)), usedPad (4) and the fired-ring memory
// (usedOrb + usedOrbOld + usedOrbHist = 5, --ringonce (removed)); plus how many ring fires in the replay were
// of a ring that had already fired in it -- what a memory without a limit would have refused.
inline bool g_histStatOn = false;
inline std::string g_histStatPath;
struct HistStat {
    int touchMax = 0, touchDrop = 0, portMax = 0, portFull = 0, padMax = 0, padFull = 0;
    int orbFired = 0, orbDrop = 0, orbRefire = 0;
    // ...and pads fired a second time in the replay: usedPad forgets a pad once the player is
    // clear of it in x, where GD's latch holds for the whole attempt (cli.hpp's --spentpad note)
    int padRefire = 0;
};
inline thread_local HistStat g_hist;
inline thread_local std::vector<int> g_histFired;
inline double pinkRingRatio(int mode) {
    return mode == 2 ? 0.77 : mode == 1 ? 0.37 : mode == 3 ? 0.42 : 0.72;
}
inline int g_shiftDbgUid = -1;       // --shiftdbg <uid>
inline bool g_shiftDbgDone = false;
// --portalobbmode (default on; --no-portalobbmode is the off arm): --portalobb's GD box (obb.txt) also for the portals that change the MODE (ball, UFO, wave, robot, spider, swing), whose oriented extents
// are otherwise rebuilt from the bound by a uniform-scale inversion. A mode portal stretched along one axis only (34 x 288.5 at rot 45) came out 45.7 x 115.6 wide, 12.7 px
// off at the end, and the model took it a tick away from GD (t=6085, t=7442).
inline bool g_portalObbMode = true;


// Always on (was --shrinksnap): on the tick a cube turns mini the stair snap is taken by the FIRST solid of the contact order only (step.hpp).
// The later solids of the pass meet the mini box, which no longer reaches them, so they do not become m_objectSnappedTo. Over GD's run of
// the official level 11 the model's order (descending section x, section y, uid; last wins) gives GD's snap object on 24 of the 26 ticks
// that touch two solids -- and the two it misses are both mini-portal ticks (t=3384 and t=15519), where GD's is the first of them.
// Always on (was --ceilfarfree; with --ceilfarbox (removed)): the far-box acquisition asks what GD's own test asks (slopeWouldAcquire: the seat target
// short of the FREE y -- the y this tick's move gives the body, a solid already counted), not the y the body had last tick. Without it
// a flipped cube a long way under a ceiling ramp's far end (30 px below its seat, still rising at vy 15) was seated on the ramp.
// Always on (was --ringhalfstep): the late ring loop meets a TURNED ring, from the previous tick's end (--ringrotground (removed)), at the midpoint of the
// previous and this tick's positions instead of at the previous end. GD's own sweep on a custom level (a mini cube, 2.4 px a tick, a
// dash ring turned 9 deg): the press fires it up to |dx| 25.60 of the effect tick's row and not from 25.70 -- 24.445 (ring half 15.445 + 9) of
// the row less half a step, where the model's box at the previous end fired it at 25.83.
// ...and not on a tick the body went through a teleport portal (stepOne's teleportedThisTick): the midpoint of the previous end and this tick's place is then a place the body never was. the long custom level t=26,201:
// a cube carried from y 189.5 to 295 on the tick of a press had a turned green orb (rot -33, 49.8 wide, 40 px above-right of the new place) tested at the midpoint y 242, 13 px away, and fired it; GD did not
// (vy -8.676 = the gravity step alone, the model's -8.496), the orb being 40.0 from the new place against a reach of 39.9.
// Always on (was --flipgracehead): the flip-grace seat (--flipgrace (removed), a solid met within 25 ticks of a gravity flip) grounds the body only when
// it was moving ALONG its gravity (a landing). Against it the contact is the head's, GD clamps y and vy and leaves onGround 0. All 62 flipgrace
// seats of the kitref episodes where the model has GD's y and vy, and a long custom level's: moving against gravity -> GD og 0 (spider 13, cube 11), along
// it -> og 1 (ball 6, cube 3). The model's grounded 1 let --lingerhold (removed) jump a tick later (a custom level, t=30590, GD does not).

}  // namespace dp
