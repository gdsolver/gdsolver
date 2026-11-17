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
// --seatspeed: a speed as one byte (State::seatSpd), 1..5 for 0.7 .. 1.6, 0 for none.
inline uint8_t speedCodeForDx(double dxF) {
    const double mul = speedMulForDx(std::fabs(dxF));
    return (uint8_t)(mul < 0.8 ? 1 : mul < 1.0 ? 2 : mul < 1.2 ? 3 : mul < 1.45 ? 4 : 5);
}
inline float dxForSpeedCode(uint8_t code) {
    static const double kMul[6] = {0.9, 0.7, 0.9, 1.1, 1.3, 1.6};
    return dxForSpeedMul(kMul[code < 6 ? code : 0]);
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
// [2026-08-30] MEASURED TO BE WRONG IN TWO DIRECTIONS, and left alone anyway --
// read this before "fixing" it. calib_forcedrop_swing_f10 (a swing walked into a
// force box, m_force=10, no input):
//   FULL SIZE ignores this cap entirely. vy climbs +0.8140/tick straight through
//     8.140, 8.954, ... to 17.094 at t=281 and is still climbing. The model
//     clamps at 8 unless State::boost is set, and a force box is not one of the
//     setters (those are the ramp launch and the red orb/pad, 889549e).
//   MINI clamps -- but at 9.385, not 8, and it holds there exactly.
// So the cap is not one number and not simply conditional. It is NOT changed
// here because nothing in lv1-22 reaches it this way: lv22's carpet lifts its
// swing at ~0.216/tick over a few ticks and never approaches 8. Loosening a
// clamp on a rig that no corpus level exercises is how a change passes every
// regression and breaks a level later ([[gd-loosening-rule]]). What is needed
// first is a rig where the corpus's own mechanisms (orb, pad, ramp) push a swing
// past 8, so the two readings can be told apart.
constexpr double kSwingTerm = 8.0;
constexpr double kSwingFlipDamp = 0.8;
// --dynhazpad <px>: inflation of the moving hazards' boxes (default 0 = behaviour
// unchanged). The note is where L.dyn.samples gets filled. A provisional
// prescription for lv22's 0.19px miss.
inline double g_dynHazPad = 0.0;
// --dynseamgap <ticks> (default 30000 = the old fixed rule): a gap between two of a moving object's recorded rows larger than this is read as a seam between attempts
// and the rows after it are dropped. The recording keeps a row only when the object changes, so an object at rest for a long stretch has a real gap; a level longer than
// 30000 ticks (the 58000-tick horizon one has a saw that first moves at t=33715) lost every row after the first.
inline int g_dynSeamGap = 30000;
// --dynseamlen (default on since 2026-10-23; --no-dynseamlen is the off arm): the seam rule's threshold is the level's own length in ticks (its furthest x over the slowest speed, 0.7x = 1.01 px a tick) when that is longer
// than g_dynSeamGap. Official levels stay under 30000 ticks, so for them the rule does not change.
inline bool g_dynSeamLen = true;
// Always on (was --advfollow): an object an Advanced Follow trigger (3016) acts on is placed at the player's x of the tick before, from the tick after the player crosses the trigger.
// GD's own rows of such an object (a ground spike that trails the player) read spike(t) = player(t-1) to 0.0005 px over 4303 ticks and every speed; the recording the search reads
// (dp_groups_boot, indexed by tick) puts it where ANOTHER run's player was, 700 px off on a custom level, so the search let a state live that the replay (its own rows) kills.
// --waveportalslope (ON since 2026-10-24; --no-waveportalslope is the off arm): a ship or UFO that rides a ramp when a wave portal of a smaller uid fires is a wave when the pass reaches the ramp, which seats nothing: the portal tick leaves the body where the update put it (step.hpp, after --waveportalfree). Official level 20, the dual wave corridor at x=25,750: the UFO rides a rising ramp, the wave portal (uid 13881) comes before the ramps (13892 ...); GD's first wave row is the update's position (182.399 -> 182.370 with the push-out), the model's the ramp's seat (184.014), 1.64 px high, and the model lived on a tick longer than GD's wave inside the ramp.
// The same rule -- GD's collision pass walks its objects in uid order, so what an object does depends on which objects of a smaller uid have already acted -- is behind --sectionuid and --ringspeedfirst; three places in the model order their effects by hand, and a later pass could fold the uid order into one.
inline bool g_wavePortalSlope = true;
// --dashaway (ON since 2026-10-24; --no-dashaway is the off arm): a cube's dash that the last tick seated on a surface carries the body on, when its line leads AWAY from that surface. The ground branch of the update phase held a seated body still (the dash only outlived the landing), but a seat is the last tick's contact, and a dash moves the body before the collision pass looks: the body leaves and the pass finds nothing. Official level... a custom level, t=10,375-10,383: a flipped cube takes a dash ring at 70 degrees (down and right), is pushed to the underside of a block above on the first tick (y 412.064 -> 405.260, ground 1) and GD's next rows run on down the dash line at -6.594 a tick (398.666, 392.072, ...) with the ground flag clear; the model held it on the block (405.263, 405.265, ...) to the end of the run. Injection of the body's y at the tick before the ring's press (six values from -10 to +6 px): the dash runs the same line whether or not the block is touched, so the seat does not stop it.
inline bool g_dashAway = true;
// --spiderorbrot (ON since 2026-10-24; --no-spiderorbrot is the off arm): a cube that takes a spider orb's teleport is put down at 0 (180 when it ends flipped), as the spider pad's teleport already does (step.hpp, the pad arm). GD's rows at the
// teleport tick, three of them on two levels: a custom level t=3,204 (upside down, rot 434.42 -> 0, then 1.731, 3.462 ... as the spin goes on), official level 22's two orbs (338.08 -> 180 as it flips upside down,
// -68.08 -> 0 as it flips back). The model went on spinning from the angle it came in with, 70.96 degrees away from GD's by the portal 130 ticks later, and the turned box of a rotated ship portal (rot 92 -> 11.3
// by a Rotate trigger) read 0.52 px short of the player's: the model entered the ship one tick after GD. Anchoring a section after the orb (t=3,250) tracks GD through the portal and to the death at 0.01 px.
inline bool g_spiderOrbRot = true;
// --bandholdzoom (ON since 2026-10-24; --no-bandholdzoom is the off arm): the wave band the zoom builds (--wavebandzoom) is also built over a band-track row that --bandholdend keeps past the recording's end. The track is a recording of one attempt, so after its last tick the loop holds its last
// row until a mode portal hands the band back, and that held row [90, 390] was written over the band the zoom had just built: a camera zoom (id 1913) that starts after the recording's end never shrank the wave's ceiling in the search.
// A custom level, the call after the first death (t=3,087), a wave under zooms 1.1 / 1.2 / 1.3 (x 8,370-8,850): GD's camscale at t=5,911 is 1.26453, pmax 358.62, and a flipped wave rides pmax(t-1) - 6 = 352.40; the search clamped at 390 - 6 = 384, so its plan
// rose 2 px above the game's ceiling, the replay (every tick of the band recorded) clamped at it, and GD killed a route the search had called alive. Only where the recording's last tick lies before the first zoom trigger (the held row is then unzoomed: camscale 1), read from --xtrack.
// The ship and the UFO add the zoom after the track row (step.hpp, bandCeilNow), so they were not hit.
inline bool g_bandHoldZoom = true;
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
// --goalspare (EXPERIMENT): the two search prunes that stop a body ABOVE the level -- out-of-play
// over g_yBound and escapee-prune over every surface still ahead -- keep one that reaches the goal
// first. Both assume a body up there has nothing left to do, but one that stays alive to the goal
// has cleared the level: a custom level crosses its last wall with a gravity orb and rises untouched to the
// end (GD complete at y=1,635; the model pruned the same plan at y=1,308, and g_yBound 1,410 would
// have pruned it next). A body is kept only when even the fastest rise cannot carry it over MAX
// GAMEPLAY Y before the goal, so GD's own death up there is still modelled (maxplayy) and the
// frontier of escapees the prunes were built against (lv9) does not come back mid-level. Needs
// --maxplayy: without a known ceiling nothing is kept.
// ON BY DEFAULT with the SubZero coin set (the release reports what the defaults solve);
// --no-goalspare turns it off.
inline constexpr int kDefGoalSpare = 1;
inline int g_goalSpare = kDefGoalSpare;
inline double g_goalX = 1e18;   // the search's goal, L.maxX + 60 (cli.hpp)
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
inline bool reachesGoalUnderMaxPlayY(double x, double y, double vy, double dx, bool rev) {
    if (!g_goalSpare || g_maxPlayY >= 1e17 || g_goalX >= 1e17) return false;
    if (rev || dx <= 1e-6) return false;
    if (x >= g_goalX) return true;
    const double n = std::ceil((g_goalX - x) / dx);
    // The fastest rise per tick: terminal 15 at the wider integration coefficient (0.25; every
    // mode but the wave uses 0.225), or a wave's line at up to twice the x advance (mini). A time
    // warp scales the x advance and the rise alike, so the ratio holds through one.
    const double rise = std::max(std::max(std::fabs(vy), 15.0) * 0.25, 2.0 * dx);
    return y + n * rise <= g_maxPlayY;
}
// --upsidecoyote (on by default since 2026-09-26; --no-upsidecoyote is the off arm): GD's upside-down cube can still jump for a few ticks after it walks
// off the surface it hangs from. playerIsFallingBugged's classic arm tests `vy > 2g` when flipped
// where the upright arm tests `vy < 2g` -- the flipped side never negates g -- and updateJump
// clears m_isOnGround only when that test says "falling". Walking off a ceiling starts at vy 0
// and gains 0.216 a tick, so og stays set until vy passes 2g (accelSwitchVyForSpeed), and the
// buttons phase, which reads og after the update, jumps from mid-air.
// Measured on a custom level (worker probe, a press swept tick by tick through t=18,414-18,431; the
// cube leaves the underside at 18,419): a press whose effect tick is 18,419 .. 18,427 jumps
// (vy -11.23 on that tick), 18,428 does not; og in the dump drops on 18,428, the first tick after
// vy(18,427) = 1.944 > 1.9224. On the air jump's tick y still moves by that tick's gravity step
// (255.0486 -> 255.1458 at k=2), where a jump off the surface leaves y where it was. The upright
// walk-off test (vy 0 < 2g) says "falling" on the first tick, which is why the upright cube
// cannot do this and why the model's support scan was right for it.
// The ball has the same window (its gravity step is 0.129, so it lasts 15 ticks). Measured on
// a custom level (mini ball, speed 0.9, leaves the underside at t=9,387): a press at every tick k=1..11
// taps with vy -2.6832 -- the mini ground tap -- and flips gravity, with y moved by that tick's
// step; k=12 there is a gravity portal. Frame 0, single body only.
// ONLY WHERE THE LEVEL KEEPS THE BUG: the level setting fixGravityBug (g_fixGravityBug, from
// --levelsettings) is what the "Bugged" in playerIsFallingBugged is about. Counted in the GD dumps of
// 201 old custom levels, every upside-down cube or ball leaving its surface with 0 < vy < 2g: with
// fixGravityBug=0, 560 kept og (the linger) and 1 dropped it; with fixGravityBug=1, 28 dropped it
// and none kept it. Official lv22 sets it, and ungated the flag jumped a mini cube off a moving
// block's edge at t=2,810 where GD had already cleared og -- the one cold wall this flag caused.
// The single fixGravityBug=0 drop (a custom level t=19,481, a mini ball) had its head 1.1 px under the
// band's top, not under an object; it is not covered here.
inline int g_upsideCoyote = 1;
// --lingerhold (off): a button already down (m_jumpBuffered, +0x985, State::jumpBuf) jumps an upside-down cube
// from the og GD left set, on the next tick's updateJump. updateJump tests `+0x985 && +0x9c1` FIRST and runs
// its falling test (playerIsFallingBugged, which is what clears the og above) only on the path without a
// jump, so the vy > 2g that ends a fresh press's window (the press comes AFTER the test, in the buttons
// phase) does not end a held button's. The drop ring is where the two meet: ringJump's type-0x20 arm writes
// neither +0x985 nor +0x9c1 for a cube, so a ring taken in the window leaves both set and the cube jumps
// on the next tick with vy 15 behind it. A custom level (fixGravityBug=0) t=6,150: a mini cube
// walking off block uid 19746's underside (og 1 from t=6,148, vy 0.215 / 0.430) presses at 6,149, the drop
// ring takes the press at 6,150 (vy 15, og 1 still), and GD's row 6,151 is a jump (vy -9.136, y moved by
// 9.136 x kYScale in that same tick, og 0). Swept on the game with one press at P instead (the plan up to
// 6,072 kept): P = 6,153 / 6,154 / 6,155 jump on the next row, P = 6,156 onwards does not (og is already 0
// by then, vy 1.935 > 2g), so the fresh press's own gate stays as it is. Cube only: the drop ring clears
// +0x985 for a ball, and the model has no linger for the other modes' hold.
inline bool g_lingerHold = true;   // on by default (release flags, 2026-10-17); --no-lingerhold is the off arm
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
// On by default (with --padtable); --no-rawvalues turns it off.
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
// --portalunpin (EXPERIMENT): a gravity portal taken by a cube standing on a block leaves y
// where that tick's gravity step put it (step.hpp, at vAtPortal).
// On by default (with --portalpress); --no-portalunpin turns it off.
constexpr int kPortalUnpinDefault = 1;
inline int g_portalUnpin = kPortalUnpinDefault;
// --hazendpoint (EXPERIMENT): the cube's and the flying body's hazard test at the tick's END only,
// not at the samples between its endpoints. The in-between samples pair the old x with a partly
// moved y -- a place the body never was -- and lv9 t=11,830 / 13,078 die on one (the ship's box
// 0.01 px into spike uid 2537 at f=0.75, clear of it in x at the endpoint) where GD lives.
// Sweeping y at the new x instead was tried first and is worse: deathref lost 4 of 42 (the model
// died 112-342 ticks before GD, on (new x, old y) samples -- just as fictional). Endpoints only
// keeps deathref at 42/47 and quick_regress's exact tracking loses nothing (lv9 gains 10,416
// ticks, 8 segments stop dying). kSubSteps = 1 was rejected on 2026-07-31 because lv11's COLD got
// worse on the model of that day; that is the measurement this flag still owes.
// Swept on the game over the modes and shapes the first sweep (165 phases, 10 classes) left out,
// on phases where only an interior sample overlaps: spider 12, swing 65, a turned hazard 51 and a
// saw 16 -- GD killed none. On by default; --no-hazendpoint turns it off.
constexpr int kHazEndpointDefault = 1;
inline int g_hazEndpoint = kHazEndpointDefault;
// --portalpress (EXPERIMENT): a press on the tick a MODE portal fires is the new mode's press. GD
// consumes a press in the buttons phase, after the collision pass that fired the portal (the
// coyote sweep showed the press line P acting at P+1 after that tick's move). The model ran the
// old mode's action in its branch and the portal after it: a custom level t=7,099 cube -> ship, the model
// jumped (11.18, halved by the portal to 5.59) where GD sat as a ship with vy 0 and held from the
// next tick; another custom level t=15,425 cube -> ball, GD tapped (3.354); a third custom level t=1,606 flipped ball -> cube,
// GD jumped (-11.18). Old mode cube/ball, new mode cube/ball/ship/wave: the transitions seen.
// On by default (with --portalunpin); --no-portalpress turns it off.
constexpr int kPortalPressDefault = 1;
inline int g_portalPress = kPortalPressDefault;
// --padtable (EXPERIMENT): a pad's launch from GD's own table instead of the cube's value with a
// ball ratio. GJBaseGameLayer::bumpPlayer (0x217b66) picks a strength by pad type and mode, and
// PlayerObject::propellPlayer (0x39f850) sets vy = float(sign x strength x 16 x mini 0.8), then
// x0.6 (double) for ball, spider and swing:
//   pink (9)  ship 0.35  UFO 0.4  ball/spider 0.7  else 0.65
//   red (34)  ship 0.63 (mini 0.95)  UFO 0.6 (mini 0.98)  else 1.25
//   any other pad (yellow)  1.0
// The model gave every flying mode the cube's pink 10.4: GD launches a ship off a pink pad at
// 5.6 (old custom levels, t=4,391 and t=3,158) and a mini UFO at 5.12 (another custom level, t=13,008).
// The cube's 16 / 10.4 / 20, the ball's 9.6 / 6.72 are this table's values; the swing's x0.6 is
// new (the model had none), and red on ball/spider keeps its x0.6.
// On by default (with --rawvalues); --no-padtable turns it off.
constexpr int kPadTableDefault = 1;
inline int g_padTable = kPadTableDefault;
inline double padTableVy(int type, int mode, bool mini) {
    const bool ship = mode == 1, ufo = mode == 3, ball = mode == 2, spider = mode == 6;
    float s = 1.0f;
    if (type == 9)
        s = ship ? 0.35f : ufo ? 0.4f : (ball || spider) ? 0.7f : 0.65f;
    else if (type == 34)
        s = ship ? (mini ? 0.95f : 0.63f) : ufo ? (mini ? 0.98f : 0.6f) : 1.25f;
    double v = (double)(s * 16.0f * (mini ? 0.8f : 1.0f));
    if (ball || spider || mode == 7) v *= 0.6000000238418579;
    return v;
}
// Fly-rings (was --flyrings, always on since 2026-09-26):
// a SHIP or UFO ring fires one tick before the model's input reaches the
// body. Both modes read the button 2 ticks after the press line (planEdges' latOf), but pushButton
// calls ringJump in the buttons phase of the tick after the press, testing the ring box at that
// tick's end. So GD sets the ring's velocity on the row BEFORE the one the model fired on, runs the
// next tick's update on it, and the press is spent (the UFO does not flap). Old custom levels,
// replayed without fixups:
//   a custom level, t=2,528, ship, gravity ring: GD 4.472 then 4.364 (held); the model fired a tick later
//   another custom level, t=7,319, mini UFO, gravity ring: GD 3.5775 then 3.679; a tick later the model flapped
//     (6.648), moved, and set 3.5775 after the move
// It also gives the pink ring its ship and UFO ratios (ringJump: ship 0.37, UFO 0.42) in place of
// the cube's 0.72: a custom level t=8,678 and another custom level t=11,196, mini ship, GD -3.309 = 11.18 x 0.37 x 0.8.
inline thread_local int g_flyRingNested = 0;
// --flydashring: ...the dash rings (37, and 38 that also turns gravity) take that path too. A custom level's
// gravity dash ring uid 68437 (id 1751, rot -70) and a UFO, the plan's press swept one tick at a time in GD:
// it fires for presses on 10,460..10,464 and not on 10,459 or 10,465, and the row after the press already
// carries the turned gravity at the y the press found -- the buttons phase of that tick, as the impulse
// rings. The model fired it through the late loop a tick later, from the row the flap had moved, for
// presses on 10,458..10,461. On by default since 2026-10-06; --no-flydashring is the off arm.
inline bool g_flyDashRing = true;
// --flypadhead (on by default since 2026-10): a pad that sends a ship, UFO or swing toward its head side hands GD's solid pass
// that vy, and the solid whose head-side face the head has crossed within the flight modes' reach (6 px, now or
// at the previous position) takes the head there: on the face, vy 0. collisionCheckObjects fires the pads in its
// object loop and resolves the solids only after it (the pass-order note), while the model ran its flying
// solids first, on the vy from before the pad, which pointed away from the face. A custom level, t=6,581: a ship
// a spider orb has just moved under the ceiling, upside down, its lower-left corner 0.68 x 3.26 px into block
// 3930; the upside-down yellow pad 2880 above sets vy -16 (GD's padfire), and GD's hitboxtrace has 3930 hit=1:
// y 1,304.75 = the block's top + 15, vy 0 -- then it sits under the ceiling slabs until the next pad, 4 ticks on.
// The model let the pad's -16 carry it away at once. A face that moves on the tick is left alone (unmeasured).
inline bool g_flyPadHead = true;
// --dashframe3 (off, needs --dashclamp): a dash ring met on GD's frame 3 (the player travelling up) takes its slope from the same clamp as on frame 0, with the frame's
// -90 (+0x9c3) put around it, so a ring whose rot is -90 -- along the travel axis -- dashes straight on. Without it the slope is the raw tan(-rot), which the clamp
// turns into tan 70 = 2.747 for a ring at -90 (or infinity): a custom level's UFO-less cube at t=4,542 takes the dash ring uid 8358 (rot -90) with the gameplay
// rotation uid 8364 on the same spot (GD frame 3 from that tick): GD moves y +2.4 a tick for two ticks and x not at all; the model moved y the same and x -6.594 a tick
// (2.4 x 2.747) and was 13 px off the cross axis, then died at t=4,583 where GD dies at 4,624. Only frame 3 is measured; frame 2 (+180) is not.
inline bool g_dashFrame3 = true;   // on by default (release flags, 2026-10-17); --no-dashframe3 is the off arm
// --crossworldx (off): the time warp's and the gravity trigger's crossing queue is read at the player's WORLD x when the frame is turned, not at the frame's own travel
// coordinate (the lists hold the triggers' world x). A custom level's cube is on GD's frame 3 from t=4,542, travelling up at world x 6,571: the model's frame
// coordinate runs 2,772 -> 2,778 and crosses the gravity trigger uid 65 (world x 2,775, x0.8) again, so its cross-axis gravity step drops from -0.216 to -0.173
// at t=4,554 and its trajectory leaves GD's; GD's steps stay -0.216 (the trigger at 2,775 and its partner at 3,075 fired long before, in frame 0).
inline bool g_crossWorldX = true;   // on by default (release flags, 2026-10-17); --no-crossworldx is the off arm
// --ringmemrot (off): applyRotation forgets the rings the body has used (usedOrb / usedOrbOld / usedOrbHist) on every frame change; with the flag they stay (by uid: usedOrb moves into usedOrbOld, as a pointer into the old frame's objects means nothing in the new one), as GD's
// once-per-attempt activation mark does. A dash ring (uid 8358, rot -90) fires at t=4,541 and the body turns into frame 3 on the next tick; the model wipes the ring's
// record there and the press at t=4,560, with the body back over the ring's top edge, fires it again (the dash line from 4,562: vy 0 and the cross axis frozen). Measured
// in GD with the plan flown with and without that press: the rows are identical (vy -3.456, -3.672, -3.888 ... on 4,560.. with no second dash), and the ring is never offered
// to the body again -- no ringJump call for it after the fire although the body overlaps its rect until t~4,560 (orbtrace: calls every tick from 4,530 to 4,541, none after).
inline bool g_ringMemRot = true;   // on by default (release flags, 2026-10-17); --no-ringmemrot is the off arm
// --ringrotbox: the contact a fly ring is fired from (step.hpp flyRingContact, the previous tick's end)
// tests a rotated ring against the player's own rotated box -- GD's collision pass rebuilds the player's
// oriented box for an object that has one (the portal pass's pHalfLive note). The same sweep: with the
// player's rotation (3.4 to 4.1 degrees) the two boxes overlap on exactly the five rows GD fired from
// (tightest margins +0.08 and -0.21 at the two ends); without it the window moves a row early.
// On by default since 2026-10-06; --no-ringrotbox is the off arm.
inline bool g_ringRotBox = true;
// --ringrectgate: ...only after the rect test (step.hpp flyRingContact), as GD's collision pass does
// (collisionCheckObjects, the object pass: the object's rect against the player's, then for an object with an
// oriented box both OBB2D::overlaps1Way, the player's box rebuilt by vfunc 0x578 at that moment). The rect is
// getObjectRect2's: for an oriented object the bound of its turned box, cached at +0x358 and rebuilt when
// setPosition / setRotation mark it (+0x368). A custom level's dash ring uid 2576 (1751, rot 15, own box 36)
// and an upside-down cube spinning past it: GD's seven-press sweep dashes from the row 35.78 px out and not
// from 37.40 (rect 22.05 + 15 = 37.05), where the turned boxes still overlap; another level's ring at -45
// (uid 93, a cube the right way up) is not fired from 44.2 px out, past 25.46 + 15, where the model dashed.
// A ring a Rotate turns (Obj::rotTurned) is left out: a cube the right way up fires a yellow ring turned to
// 340.8 from a row 34.95 px out, past the rect of that angle (33.59), so when GD rebuilds its rect is not
// known. Off from 10-12 10:39 while it was taken for a gravity rule (--ringflipunturned); on again since.
inline bool g_ringRectGate = true;
// --ringflipunturned (OFF since 10-12): an UPSIDE-DOWN cube's touched-ring contact tested with its box unturned.
// It was fitted to the uid 2576 sweep above, which the rect gate accounts for; GD's collision pass rebuilds the
// player's box at its rotation in either gravity (GameObject::updateOrientedBox reads getObjectRotation).
inline bool g_ringFlipUnturned = false;
// --ringheldnow: a turned ring entered with the button held from an earlier tick is met where the player is on this
// tick, not at the previous tick's end (--ringrotground's position, which is the press path's). GD's two fire paths
// (the tick map's ring note): pushButton walks m_touchedRings from the previous collision pass, while
// playerTouchedRing fires during this tick's object pass when the held and buffered copies (+0x989, +0x98a) are set;
// the player's box there is rebuilt (vfunc 0x578) at this tick's position and at the rotation updateRotation left a
// tick ago (it runs after the collisions). A custom level's dash ring uid 93 (1704, rot -45), a cube the right way up
// pressing on 4,741 and holding: GD dashes on 4,743, the first row whose box meets the ring by that rule (+0.09 px;
// -0.20 on 4,742); the model, reading the rows before, never met it.
inline bool g_ringHeldNow = true;
// --spiderpadafter: the pads GD's pass reaches after a spider pad (type 44) that warped the body are judged
// where the pad put it, on the same tick (step.hpp, the pad loop; the order is gdTeleAfter's, as for a
// teleport's landing). The loop used to stop at the warp. A custom level: a cube rides a floor onto a spider
// pad (uid 1553) and is put at 675 under two ceiling blue pads (uids 1554 / 1604); GD's dump has it the
// right way up again on that tick with vy -6.4 (the blue pad's), where the model sat it on the ceiling
// upside down for a tick and fell from the next -- 1.5 px off GD from there on, and the run's first plan
// went another way in GD. On by default; --no-spiderpadafter is the off arm.
inline bool g_spiderPadAfter = true;
// --spiderorbpass: a spider orb fired by a held press goes off inside GD's collision pass (playerTouchedRing's
// ringJump), so the portals the pass reaches after it (gdTeleAfter's order) are met where it put the body, on the
// same tick, and the solid pass that follows the walk seats the body on the solid face it found (vy 0, onGround).
// A fresh press fires it from pushButton after the pass, which is the arrival lv22 shows (onGround 0, vy +-1).
// A custom level: a cube holding the button meets a spider orb (uid 149) and is put under a ceiling at 735 with a
// mini portal (uid 182) there; GD's row has it mini, onGround 1 and vy 0 on that tick (hitboxtrace: the
// ceiling block resolved at size 0.6), where the model took the portal and the landing a tick later.
// On by default; --no-spiderorbpass is the off arm.
inline bool g_spiderOrbPass = true;
// --dashstopdead: a dash stop (id 1829) that ends a dash also clears m_jumpBuffered (collisionCheckObjects: id 0x725
// with +0x9e4 up calls stopDashing, then writes +0x985 = 0), so the button held through the dash is dead until it
// is let go (State::holdDead). A custom level: a cube presses into a dash ring (uid 11681) and holds through the
// stop and a teleport down to a floor; GD's pflags read +0x985 0 from the stop on and it lands without a jump, where
// the model jumped off the landing.
// On by default; --no-dashstopdead is the off arm.
inline bool g_dashStopDead = true;
// --tppinvy: a teleport portal met by a cube or ball that sits on a block keeps the update phase's gravity step
// in vy. GD's update has taken the step, the teleport moves the body inside the collision walk, and the solid pass
// after the walk finds no block at the exit to seat it on; the model's pin had already put vy back to 0. A custom
// level: a ball rolling on a floor at 2,505 is teleported (uid 615) to 1,635; GD's row has vy -0.129 there (the
// ball's step at speed 0.9), the model 0, and it ran 0.13 px high from there on.
// On by default; --no-tppinvy is the off arm.
inline bool g_tpPinVy = true;
// --portalunseat: a mode portal that gives a body sitting on a block a smaller box (a cube turned spider: 30 to 27)
// fires inside the collision walk, before the solid pass, so the pass meets the new box at the height the update
// phase left; with no solid under that box nothing seats the body and it keeps the update's gravity step. The
// model seated the old box first. A custom level: a cube rides a floor onto a spider portal at its edge; GD's
// hitboxtrace has no contact on that tick (box 27 x 27, its foot 1.46 above the floor) and the row is the cube's
// step, vy -0.195, where the model sat on the floor and fell a tick later.
// On by default; --no-portalunseat is the off arm.
inline bool g_portalUnseat = true;
// --pinwarp: the gravity step a body sitting on a block takes in the update phase (step.hpp prePinY, which a pad, a
// ring or a ramp's release leaves it at) is the time warp's, as every airborne step is: vy += g x warp, y += vy x
// warp. The pin took the unwarped one. A custom level under a 0.9 warp: a grounded cube takes a pad and GD's row
// is 1,334.9605 (1,335 - 0.195 x 0.2025), the model's 1,334.9514 (1,335 - 0.216 x 0.225); a ball, 1,274.9765
// against 1,274.9709.
// On by default; --no-pinwarp is the off arm.
inline bool g_pinWarp = true;
// --dashwarp: a dash ring's line climbs by its slope times the warped x step, the step x itself takes (step.hpp,
// x += useDx x warp). The dash branches took the unwarped one, so under a warp the line came out steeper by
// 1 / warp. A custom level under a 0.9 warp: a ball dashes off a ring at -4 degrees; GD's rows climb 0.1227 a
// tick (tan 4 x 1.755), the model's 0.1364 (tan 4 x 1.950).
// On by default; --no-dashwarp is the off arm.
inline bool g_dashWarp = true;
// --swingheldash: a dash ring does not spend the press that fires it (+0x986 stays up while held: GD's pflags through
// a held cube dash and a held ball dash), and the swing's updateJump flips on +0x985 and +0x986 alone, so a body
// that enters a swing portal dashing with that press held flips on the next tick as if pressed, the dash running
// that tick still; the flip clears the press and the dash ends after it. A custom level: a ball dashes with the
// button held into a swing portal (t=11,884); GD's row 11,885 is upside down still on the dash line, 11,886 falls
// toward the ceiling, where the model took the swing's gravity at once and went down.
// On by default; --no-swingheldash is the off arm.
inline bool g_swingHelDash = true;
// --ringride: a ring fired off a seat that rides a moving face leaves from the face's new height, as a jump does
// (step.hpp, jumpRide: the collision pass seats the rider on the face's NEW height and only then does the
// impulse fire). A custom level: a spider on a block easing down (568.587 -> 568.173) takes a ring on the
// next tick; GD's row is the seat 0.414 lower with the ring's vy 5.7554, the model's the old height with the
// same vy, 0.414 off from there on. On by default; --no-ringride is the off arm.
inline bool g_ringRide = true;
// --snapfacebelow: the stair snap's contact (step.hpp, the touch list before checkSnapJumpToObject) does not count a
// solid whose face lies below the foot by more than kSnapFaceBelowEps. GD calls the snap once per solid its
// collision pass resolves, and a face the cube stands clear of is not one. The old test took any face within
// 0.6 either way, which on grid-aligned blocks never mattered. A custom level: a cube rests on two moved blocks
// whose tops are 3180.03 (uid 2202, the one it stands on) and 3179.99 (uid 2201, 0.04 lower); GD's snaptrace
// has one call a tick, for 2202, and keeps it as the snap object into the next landing (90, 60.02 off, the
// `big` stair: +1.0 px). The model called both, ended on 2201, matched nothing and ran a pixel behind GD.
// On by default; --no-snapfacebelow is the off arm.
inline bool g_snapFaceBelow = true;
constexpr double kSnapFaceBelowEps = 0.01;
// --ringrotground (on by default since 2026-10): ...and the late ring loop (every mode but the fly rings' early
// press) does the same, where it projected an unturned player square onto the ring's axes, and it
// places that box where the player was at the previous tick's end, as the fly rings' contact does.
// Two airborne cubes spinning past a dash ring (id 1704, 36 x 36) that does not move (GD's ccl orect
// is its load position), the press swept one tick at a time in GD with the rest of the plan kept:
//   a custom level, ring rot -70, sprite ~254 deg: presses 7,960..7,964 dash (the sprite snaps to the
//     ring on the next row), 7,965 and 7,966 do not
//   another, ring rot -11, sprite ~147 deg: presses 1,095..1,098 dash, 1,099..1,101 do not
// Scored on GD's own rows, only the turned box at the previous row fits all four edges (+0.43 / -1.72
// and +1.77 / -1.43 px with the start-of-tick rotation); at the fire row it misses both last dashes
// (-1.59, -1.50). The unturned square at the previous row fires a tick late on the second (+3.01) and
// misses the first's last dash (-1.19), at the fire row that one too (-3.21). The model without the
// flag fell past the first ring and died on a hazard 20 ticks later.
// NOT the wave: --waveringbox's sweep (below) wants a turned half in (5.86, 6.50], which the wave's own
// 10 x 10 (half 5) misses, so the wave keeps that measured unturned square of half 6 rather than having
// the two rules stacked.
inline bool g_ringRotGround = true;
// --flystepup (default on): a grounded flying body whose foot is below a solid's top face, within the landing
// tolerance, is landed on that face (step.hpp, the flight landing gate), as an airborne one already was.
inline bool g_flyStepUp = true;
// --ringpobb (default on since 2026-10): ...and the late ring loop's rotated ring too, for every mode but the wave
// (step.hpp, its ring box was swept against the unturned test). GJBaseGameLayer::collisionCheckObjects
// (0x214960), after the rect test: when the object's latch (+0x2e8) is set it asks OBB2D::overlaps1Way both
// ways between the object's getOrientedBox and the player's, which updateOrientedBox has just turned by
// the player's rotation. The late loop projected an unturned player square onto the ring's axes. A custom
// level's dash ring (id 1751, rot 26.94, own box 36) and a cube in the air at 116.06 degrees, the model's
// angle equal to GD's: on the ring's axis the cube's turned box reaches 15.2, the unturned square 20.2, and
// the cube's centre is 37.39 out. The model dashed and GD did not, the same death ten attempts running.
inline bool g_ringPobb = true;
constexpr double kWaveRingHalf = 6.0;
// --tporbown: a teleport orb has its own mark of the press, so it fires alongside another ring the same press
// fires (step.hpp, the ring loop: the other ring first, then the orb). PlayerObject::ringJump (0x398c00) spends a
// press with one of three bytes by the ring's type -- +0x98c for type 0x24, +0x98d for the teleport orb (0x2e),
// +0x98b for every other ring -- and refuses a ring only when its own byte is already set. A custom level t=1,158:
// a robot presses inside a spider orb (uid 78) and a teleport orb (uid 153) placed on one spot; GD flips to the
// ceiling and is put at the orb's target (1605, 570) on that tick, the model took the spider orb alone and stayed
// at x 1,502 under the ceiling. On by default; --no-tporbown is the off arm, where one ring is picked.
inline bool g_tpOrbOwn = true;
// --dashrot: ...and a dashing CUBE's angle is the dash line's too, -atan(slope), with no spin (step.hpp,
// the cube's rotation); the release puts it back to 0 (on its own tick, which still moves along the
// line), and nothing turns it until a spin is staked. The cube spun on through the dash at the air rate, so its box came out of the dash at whatever
// angle the spin had reached. Two custom levels, GD's own rows:
//   a horizontal dash ring at t=12,490: rot 0.000 from the ring's tick on, 0.000 on the first tick after
//   the dash (vy -0.216), +1.731 a tick from the next (vy past -0.25). The model's had turned 165 degrees,
//   and a rotated yellow ring 20 ticks on met that box (the two-box test with the player's turn) where
//   GD's upright box missed it, so the model jumped where GD fell;
//   a -45 degree gravity dash: rot 45.000 through the dash, 0.000 on the release's tick (y still on the
//   line), held while vy is 0.216, then -1.731 a tick, flipped (the fall stake with the gravity in force).
// --no-dashrot turns it off.
inline bool g_dashRot = true;
// --dashsprite (off): ...and the cube's angle after the release is not 0. updateDashArt (0x3961c0) starts, on the
// player's two icon sprites (+0x738, +0x750), a CCRepeatForever of CCRotateBy(0.3 s, 288 x time warp x -+1) -- 960
// degrees a second, 16 a scheduler frame, the sign negative while the gravity is flipped XOR reversed -- and
// stopDashing (0x396650, a cube: none of the vehicle flags) stops the actions, reads the first sprite's angle and
// puts the PLAYER at it, so the box the next ticks run with is turned by the spin the dash accumulated. The action
// manager steps once a scheduler frame (4 physics ticks at the normal time warp), the step that finds the action
// new counts for nothing, and the sprite is read in the frame of the stop after that frame's step:
//   angle = 16 x (frame(stop) - frame(start) - 1), frame(t) = (t - 1) >> 2 for the dump's row t
// Measured with the vtime reference clock (cfg vtime = 1/60; the previous 0.000 came from the mod's batch-end
// action sweep, which stepped the sprites by whatever the batch was): a custom level, a horizontal dash ring
// at row 672 released at row 740: the sprite reads 0 at 672-675 and 16, 32 .. 240 at the frames from 676 on, and the
// player's angle on the release's row is 256.000 (frame 184 - frame 167 - 1 = 16 steps), then +1.731 a tick from the
// fall stake as before. A platformer (the dash velocity scales the spin) and a time warp are not modelled: the
// angle stays 0 there. --no-dashsprite is the off arm.
inline bool g_dashSprite = true;   // on by default (release flags, 2026-10-17); --no-dashsprite is the off arm
// --dashspriteonce (ON since 2026-10-24; --no-dashspriteonce is the off arm): ...but the release's angle is put down ONCE. The model's dash state outlives the release
// row by a tick (the branch that runs on the release's own tick leaves `dashing` set), so the tick after it ran the same
// sum again with one more scheduler frame in it, and the cube came out of every dash 16 degrees (a frame of the spin) past
// the game's: a custom level's dash ring released at row 740, GD rows 256.000 (release), 256.000, 257.731, 259.462 ... and
// the model 256.000, 272.000, 273.731 ... for the rest of the jump. The angle is the box of every rotated portal, pad and
// ring the cube meets on the way: a 12.5 x 37.5 gravity portal turned 45 degrees took the model's cube 1 tick early
// (t=794, margin +0.22 against GD's -0.26). The tick after the release now keeps the angle the release wrote.
inline bool g_dashSpriteOnce = true;
// --robotrot (off): the ROBOT has an angle. A dash ring (1751 here, rot -19) puts it on the dash line's -atan(slope)
// for the whole dash -- GD's rows: rot -19.000 from the ring's tick, the line rising 0.8264 over 2.4 a tick -- and
// the dash's end hands it to updateRotation's robot branch (0x391020 -> 0x391260), which eases it to 0 by
// 0.175 x the speed multiplier x 0.25 x 0.5 a tick (0.035 at 1.6: -19.000, -18.335, -17.693, -17.074 ...). The model
// never gave the robot an angle, so a turned portal's box was met by an upright robot: a custom level's teleport
// portal (25 x 90 turned 45 degrees, uid 153453) is touched by the robot at row 28,755 in GD and 28,754 in the
// model, one tick early (the tilted box leaves 0.4 px of margin there). Spider and the robot's other turns are
// left as they were. --no-robotrot is the off arm.
inline bool g_robotRot = true;   // on by default (release flags, 2026-10-17); --no-robotrot is the off arm
// --spiderrot: the SPIDER's angle, by the robot's law (needs --robotrot's constants, not the flag). A custom level's spider
// leaves a dash at rot -23.000 (the UFO before it took the dash line's angle at the ring: t=20,649 -23.000, held through the mode
// portal to the spider at t=20,675) and from the first tick off the dash eases toward 0 by 0.021875 x the speed multiplier:
// -20.0115, -19.6175, -19.2313, -18.8527 ... a constant ratio 0.98031 = 1 - 0.021875 x 0.9, the robot's. The model's spider was
// upright all the way (hazdbg: rot=0.0000), so its box met a 63-degree spike's corner 0.07 px deep where GD's, 17 degrees
// turned, is clear of it by 3 px: the model killed at t=20,730 a plan GD flies. A UFO takes the line's angle while it dashes
// (it is eased by its own law off the dash). --no-spiderrot is the off arm.
inline bool g_spiderRot = true;   // on by default (release flags, 2026-10-17); --no-spiderrot is the off arm
// --waveslopemini <s>: the mini wave's ramp-kill scale (constants.hpp kWaveSlopeMiniScale = 0.700, "UNVERIFIED", bracketed to (0.633, 0.779]).
// A GD-verified plan of a custom level puts a mini wave 4x fast, upside down, through a ramp's corner at t=25,302: injecting y offsets in GD,
// in the plan's own context, GD kills at +0.27 px and the model at -0.03, with the model's s=0.700 (the kill line is sy0 + (x - 4.068 s - sx0) -
// 4.0 s: 0.30 px lower than GD's) -- s = 0.663 reproduces GD's line there; that is one measurement, bracketing s to (0.633, 0.663]. 0.700 stays
// the default until a second measurement or a rig agrees.
inline double g_waveSlopeMini = 0.700;
// --waveslopeminik <R,L,Y> (default none): the mini wave's three ramp constants set one by one instead of by one scale (constants.hpp: kWaveSlopeDxR 3.072,
// DxL 4.068, Dy 4.0 at full size). The scale 0.663 above fits one corner pose and misses the next: a second one of the same plan (t=37,578, a ramp's
// corner where the sample is clamped to the box edge, so only Dy shows) has GD alive up to +0.25..0.50 px above the pose where 4.0 x 0.663 = 2.65 kills, i.e.
// Dy is 1.97..2.22, not 2.65 -- and the first one (t=25,302, unclamped) has Dx + Dy = 5.35. A rig of the mini wave 4x fast against ramp 1338 in its four
// orientations (GD only, injection four ticks before the pose, window matched to the model's: py/mklevel.py waveslope_mini, oneoff wsl_sweep.py)
// gives Dy = 2.003 (the clamped flat part), Dx_L = 3.237 and Dx_R = 2.241 (the two sloped parts), i.e. 0.50 / 0.80 / 0.73 of the full-size values. The Dx_L
// used here (3.246) is the first plan pose's (the larger of the two witnesses, so the more careful side of the kill line) and Dy 2.1 sits inside the
// second's bracket; Dx_R is the rig's alone. NOT a calibration: it is two corners of one level and one rig.
inline double g_wsMiniR = 2.241, g_wsMiniL = 3.246, g_wsMiniY = 2.1;   // on by default (release flags, 2026-10-17): the rig's Dx_R, the first plan pose's Dx_L, a Dy inside the second pose's bracket (see above); --no-waveslopeminik restores the single scale
// --waveringminipobb (off): a MINI wave meets a TURNED ring with its own 6 x 6 object rect (half kWaveHazHalfMini = 3) turned by its angle
// -- the same box-vs-box test (orientedHit) the other modes got from --ringpobb -- instead of --waveringminibox's unturned 3.6. A
// custom level's dash ring (id 1704 at 2.0 scale, rot -11, a 72 x 72 box) and the verified plan's mini wave at t=26,438 (the wave turned
// 2 degrees at the start of the tick, 13 degrees off the ring's axes): GD fires the ring from an offset along the ring's axis of 39.6 down,
// not from 39.75 (y injected in the plan's own context, the press and everything else unchanged): 36 + 3 (|cos 13| + |sin 13|) = 39.60. The
// unturned 3.6 (36 + 3.6 (|cos 11| + |sin 11|) = 40.22) fires the ring on the plan's own row, 0.22 px deep: the model dashes at t=26,439 and
// GD flies on. The earlier witness of the 3.6 (a -45 ring, 22.58 off its axis, the dash fires) is the same test only if the wave was
// turned about 13 degrees too; that row's angle is not recorded here.
inline bool g_waveRingMiniPobb = true;   // on by default (release flags, 2026-10-17); --no-waveringminipobb is the off arm
// --sectionround (off): the section cell of an object is its x / 100 ROUNDED, not truncated (step.hpp, gdSectionCell). A custom level's mini wave
// at t=29,517: a teleport portal (uid 158635, x 56,907) moves it from y 771 to 730 and GD fires a cube portal (uid 158644, x 56,895, y 674..760) on the
// same tick, which it can only do if the pass reaches the portal after the teleport; truncated, the cells are 568 and 569 and the model walks the
// cube portal first (at the old y, no contact), then never meets it (the next tick the wave is 20.9 px off in x): it stays a wave where GD is a
// cube. Rounded, both are cell 569 and the uid order puts the cube portal after the teleport.
// SUPERSEDED by --sectionuid (below): rounded cells put the same plan's next teleport wrong (t=30,399, a speed portal whose cell becomes the one after the
// teleport's: the model steps 1.35 px short from t=30,400). Kept for the record; do not turn it on.
inline bool g_sectionRound = false;
// --dropa1c (off): the drop ring's "terminal clamp skipped once" follows the byte [player+0xa1c], not playerIsFallingBugged on the firing tick.
// PlayerObject::updateJump (0x38c...): with 0xa1c set the gravity step has NO clamp (then the fall test clears the byte when falling); with it clear the
// step is clamped at +-15. ringJump sets 0xa1c for every ring but the drop ring, so after a drop ring the next tick is unclamped exactly when the byte was
// already set -- a jump or another ring since the last fall test said "falling". The model's rule (the rig's 2.0 / dropfall's 2g) is that fall test's
// proxy. A custom level (4x fast, flipped cube): the cube takes a drop ring at t=30,227 with the byte clear, and GD's next rows are vy 15.000, 15.000
// (injected from -14 to +14 in the plan's own context: never 15.216), where the proxy says "not falling" and skips: the model ran 0.05 px high for 2,000
// ticks, to a moving hazard at t=30,713. The latch the model tracks is a1cNow (step.hpp).
inline bool g_dropA1c = true;   // on by default (release flags, 2026-10-17); --no-dropa1c is the off arm
// --sectionuid (ON by default since 2026-10-22, --no-sectionuid is the off arm): the x section cells do not order the collision pass across a teleport; the uid does (gdTeleAfter keeps its reach gate and the
// dynamicLevelHeight y split). Three teleports, each measured against GD, agree with the uid alone and no x-cell rule fits all of them:
//   official lv20 t=15,777: teleport uid 12,654 (x 22,909, cell 229); a ufo portal (uid 12,605, x 22,891) and a size portal (12,644, x 22,893), cell 228,
//     smaller uids: GD does not fire them on the landing tick (ascending cells and the uid agree; descending cells break it: quick lv20 37 segments differ);
//   custom level t=29,517: cube portal uid 158,644 (x 56,895, cell 568) after teleport 158,635 (x 56,907, cell 569): GD fires it on the landing tick -- the one
//     ascending cells get wrong (the model walks the cube portal first and never meets it);
//   custom level t=30,399: speed portal uid 165,872 (x 59,055) and teleport 165,876 (x 59,037), one cell: GD's speed change shows a tick later (a smaller uid).
// --sectionround got the middle row and the lv20 row right and this last one wrong. The official levels' own cross-cell cases do not tell the two apart (quick
// is byte-identical with the flag on), and no one has read the walk's cell order out of the binary for x -- do that before this is on by default.
inline bool g_sectionUid = true;
// --robotheldmove: the HELD re-jump of a ROBOT also moves on its jump tick, as the cube's does (step.hpp, "the HELD re-jump also MOVES"). A weekly custom level, a
// mini robot hanging upside down in a dual section (t=8,116): it lands on a block's underside with the button down since t=8,113 (the press had not been
// spent), and GD's rows are y=261.000 vy=0 on the landing tick, then y=260.044 vy=-4.248 on t=8,117 -- the jump's velocity moves it in the same tick (-4.248 x kYScale
// = -0.956). The model set the same vy and left y at 261.000, one tick behind for the rest of the arc (a robot wall at t=8,460 where GD dies and the model dies 44 later).
// The cube-only gate was measured on the cube (lv1 t=164, lv22 t=10,556); the robot's arm of updateJump runs before the move the same way.
inline bool g_robotHeldMove = true;
// --robotbufjump: a ROBOT's press that nothing has spent jumps on the tick after the landing even when the button went up at the landing (the cube's buffered half,
// `input || jumpBuf`, step.hpp's grounded jump gate, which excluded the robot because a robot held through a spent press stays on the floor: that is +0x986 clear,
// State::pressSpent). The same weekly custom level, t=8,177..8,180: the plan presses at 8,177 in the air and releases at 8,179, the robot lands at 8,179, and GD's rows
// have the jump (vy=-4.248, y moved by it) on 8,180; the model waited for the next press and jumped at 8,182, two ticks late, and died 44 ticks after GD.
inline bool g_robotBufJump = true;
// --robotheldspent (ON since 2026-10-24; --no-robotheldspent is the off arm): a ROBOT whose button is held through a landing jumps again only when the press has not been spent: GD's updateJump takes
// m_jumpBuffered (+0x985) for every mode but the robot (+0x9bd), which needs +0x986 too, and the arm that jumps clears +0x986 (0x38c728); releaseButton puts it back.
// The model's gate for a held robot was `!ringHold`, which lets it re-jump after its own jump. Rig data/rigs/calib_holdjump_robot (flat ground, one press, never released):
// GD jumps once (t=31), lands at t=195 and is still on the floor at t=399; the model jumped again at t=196 and bounced every 100 ticks. In an official-level solution
// of the weekly cold (a robot lands at t=5,137 with the button down since t=4,977) the same re-jump put the model 12 px off in the first tick and killed it at t=5,394.
// The gate is State::pressSpent, which a jump, a tap or a ring sets and a release clears.
inline bool g_robotHeldSpent = true;
// Always on (was --spiderbufjump; quick is byte-identical with it on and off, and so are the 1,969 episodes of the 126 kitref kits, so
// what it changes is only a drop ring's press, measured on a two-player custom level): a SPIDER's press that a DROP ring (the black orb, type 32) took in the air is still the buffer's on landing. (Only that
// ring: a kitref episode with a spider orb's press, landing two ticks later, shows GD does not teleport again there; the rule first read any ring's hold and that episode failed.) A two-player custom level, upright
// spider falling: the plan presses at 3,694 (applied 3,695) inside a black (drop) orb, uid 5314, which takes the press (vy -16.5, State::ringHold, pressSpent), the spider lands on 3,696 with the
// button still down and releases on 3,697, and GD's row 3,697 is the teleport to the surface above (y=2,302.2, upsideDown); the model, which counted the press spent, stayed on the floor and
// the first body died 26 ticks later (74 px off). The same at 3,909..3,911 (orb on the floor, held, GD teleports on the next tick). Taken as the cube's buffered half is: s.jumpBuf on top of
// the ring's hold, released (c.ringHold = 0) when the landing teleport uses it, so a held button does not repeat it (step.hpp, the spider's tap branch).
// --ceilrobotjump: a flipped ROBOT that reaches the band's ceiling with a press not yet spent jumps on the landing tick, the mirror of the ground-plane robot jump
// (step.hpp, lv22 t=19,835) and of the flipped cube's (--ceilcubejump). The same weekly custom level, t=8,333..8,334: the plan presses at 8,333 while the mini robot
// is still rising (vy +13.4); GD's row 8,334 has y=351.000 (= 360 - 9, the ceiling) AND vy=-4.472, onGround 0, never a vy=0 tick. The model sat at 351.000 with vy 0 and
// jumped at 8,335, a tick behind for the whole arc (3 px by t=8,342).
inline bool g_ceilRobotJump = true;
// --p2touchbox: in a dual, the second body enters EVERY touch box at its own x and y (step.hpp markTouched), as --boxarm already had it enter a toggle block. Until now a box only
// the second body reaches was one the model could never fire (the --p2touch diagnostic's note). A weekly custom level: a pair of slabs that close on the corridor, 6 px over 24 ticks,
// are switched by a touch box that sits on the lower one (the mirror pair above has no box of its own); the second body, mini and upright at y=160 with its head 3 px into the slab's
// underside, touches it at t=8,457 and GD's recording has both slabs moving from t=8,458. The model's p1 hangs at y=290, never touches that box, so the slabs stood still; GD kills
// both bodies at t=8,461 (the lower slab's underside 164.447 against the second body's kill box top 164.478), the model flew on 44 ticks.
inline bool g_p2TouchBox = true;
// --spiderpadlatch: a ring the collision pass reached BEFORE a spider pad warped the body is judged where the pass found it, not where the pad put it, and a flying body's press (the
// wave's, the swing's) goes only to a ring that tick's pass had latched. GD walks the section cell's objects in ascending uid and the pad's warp (teleportPlayer) moves the body in the
// middle of that walk; m_touchedRings (State::touchRing) therefore holds the rings reached before it at the old position and the ones after it at the new one. The model latched all of
// them at the landing, and a wave press on the next tick, whose ring-either contact (pre or now) also takes the landing, fired a spider orb GD never offered. A weekly custom level,
// t=8,083..8,084: a wave in a 80 px corridor takes a spider pad (uid 22557) on the ceiling and lands on the floor, 10 px under a spider orb (uid 22556, the pad's neighbour in uid order)
// whose box the landing meets; the plan holds the button from 8,083, GD's row 8,084 is the wave still falling at y=213.95 (-4.186) and the orb fires at 8,088 from the next press
// (y=295, up). The model fired it at 8,084 and was 81 px away from GD for the rest of the section. The tick after a pad warp is marked in State::touchRingT bit 7.
inline bool g_spiderPadLatch = true;
// --areaoutsame: an Area Move / Scale / Rotate with "ease out" on is covered by the area formula when its out curve is its in curve (type, rate, buffer). GD's
// getEasedAreaValue (0x228260) then picks the in curve or the out curve from a per-object state, and two equal curves answer the same either way. A weekly custom
// level's pair of Area Moves (+-30 y on two groups of spikes, centre P1, length 360, ease in and out both type 1 rate 2) was left out of the covered shape for it: the spikes
// stayed at their editor y in the model while GD's rows have them 29.7 px off at t=8,386, and a ship that flew under one died on it 32 ticks before GD's death.
inline bool g_areaOutSame = true;
// --dualinlat: in a dual whose second body is a ship or a UFO (input latency 2) and whose first is not (latency 1), the second body takes the button one tick
// later than the first, as GD gives each body its own vehicle's latency. The plan's press is converted to an effect tick by the first body's mode (the walk's
// scheduler and the search's emitter), so the first body is right by construction; the second one now reads the first one's input of the tick before (State::action)
// and its own tick before that (State::action2). A weekly custom level, t=11,157..11,159: a cube and a UFO hung on the ceiling, one press at 11,157; GD's cube jumps
// on 11,158 (vy 10.62) and the UFO on 11,159 (vy -6.871), the model jumped both on 11,158; the UFO was a tick early for the rest of the section and the pair died
// at GD's tick only after the loop had fixed the same state 17 times. The opposite pairing (the first body slower) would need the second body to read the first
// one's NEXT tick, which a forward step does not have, and stays as it was.
inline bool g_dualInLat = true;
// Always on (was --revcarry): GD's reverse correction (State::revOff / revCarry): the x a reverse ring or pad owes after turning the player round is paid back in steps of
// up to 2% of a tick's advance. A weekly custom level's green ring (rev, uid 777 at x=13,785) fires with the player 25.8 px to its right: GD books
// 2 * (13,785 - 13,810.78) = -51.56 px and the player runs 0.0323 px a tick (1.6465 against 1.6143) farther for the next 1,596 ticks. The model ran the
// plain step, 0.0322 px a tick behind GD from the ring on, 51 px by the time the debt is paid, and every plan made from before the ring met the section's
// hazards that far off -- about 60 of the level's 94 loop iterations came after it. --startrev R,C seeds the two at an anchor (the mod writes it).
// Always on (was --exitfaceland): a body that leaves a ramp's ride keeps the landing the collision pass made on a solid's face in the same tick. The exit's launch (step.hpp,
// the uphill-launch block) undoes the tick's landing (releasePin) because on a ramp the ride's own seat is what GD never made; a flying body that was
// clamped against a SEPARATE solid has no such seat to undo, and GD's launch (postCollision, after the clamp) writes vy only. A weekly custom level, a
// flipped ship under a ceiling ramp chain at t=8,450: it leaves the ramp (launch -4.925) while it is pressed against the underside of a thin wall (uid 8879,
// y=299.5) at the ramp's end -- GD's row is y=284.500, the model's 284.581 (the free step the undo put back), and the 0.081 px rode 3,200 ticks until the ship
// met a 6 x 5.6 thorn at its edge (t=11,661).
// Always on (was --stickonce): two ramps stacked in one place (the same x span, lines 0.5 px apart) -- the second one in uid order does not pull a rider down to its own line once the
// first has seated the body this tick. The stick (stickHere) bypasses the acquisition test by design (a 4 px gap counts as "continuous"), so it reached the stack-mate
// with a rider the first ramp had just put 0.5 px higher. A weekly custom level, a ship's first tick on a double floor ramp (lines 119.5 and 120.0, m=2) at t=9,022:
// GD y=140.195, the model's 139.695 -- the 0.5 px lag rode the whole climb.
// Always on (was --slopelag): a moving spiked ramp's kill (the wave's and the flight modes' perpendicular distance from its line) reads the line as the PREVIOUS tick left it, not as the
// recording's row for this tick puts it. A custom level: a mini wave pressed against a spiked ramp that drifts 0.062 px a tick (uid with cy 237.003 -> 236.941 over the tick): the game killed the wave at y 226.793,
// which the previous tick's line puts inside the margin (limit 226.801) and the current row's outside it (226.738); bisected in the game: dead at 226.776, alive at 226.810.
// --slopelagplain (default on; --no-slopelagplain is the off arm): --slopelag for a PLAIN moving ramp too. A custom level's mini wave 3.03 px (perpendicular) above a plain ramp that sinks 0.29 px a tick is killed by GD, where the line
// as this tick's recording has it leaves the model 3.0 + 0.06 clear; read as the previous tick left it (0.29 higher) it is 2.9.
inline bool g_slopeLagPlain = true;
inline double g_startRevOff = std::nan(""), g_startRevCarry = std::nan("");
// --balldashrot: ...and the BALL's. A dashing ball holds -atan(slope) and the release's tick puts it to 0, as the
// cube's; and a dash ring (1704) stakes no spin -- ringJump leaves for the dash before its ball stake, so the roll
// goes on at the stake it had. A custom level's ball, upright, rolling +3.755 a tick: dash ring uid 108022 (rot -68)
// at t=20,916, GD's rot -68.000 through the dash, 0.000 on the release's row 20,920, +3.755 from 20,921. The
// model's spun on through the dash and came out turning -2.66 a tick (a ring's air stake), and its box, 15 degrees
// the wrong way, met the second dash ring on the same spot (uid 20866, rot -68) that GD's had left: the model
// dashed again at 20,924 where GD fell. The gravity dash ring (1751) flips gravity as well and is left as it was.
inline bool g_ballDashRot = true;
// Always on (was --balldashsprite): the ball's angle on the tick its dash ends is the dash art's spin, not 0. updateDashArt
// (0x3961c0) runs a CCRepeatForever of CCRotateBy (0.3 s, 288 degrees: 960 a second, 16 a scheduler frame at 1/60 s) on two
// child sprites when a cube or a ball starts a dash, and stopDashing (0x396650) hands the angle to the player at the end
// (the cube's law is --dashsprite). The game steps the action manager once per update call of 4 ticks, with the dt scaled
// by the scheduler's time scale, which is the Time Warp in force (applyTimeWarp, also above 1), and a cocos action drops
// the time of its first update. So the angle on the release's row is the sum over the frames f from f0 + 2 to f1 of
// 16 x warp(x of row 4f + 1), f = (row - 1) >> 2, f0 the frame the dash began in, f1 the release's. The spin on from
// there is the ball's own stake (3.755 a tick at speed 1.3), as before. Measured on a custom level's ball dash rings
// (dash ring rot -68, one repeated warp 1.5 / 1.0 section): 21 dashes of 1 to 40 ticks in 20 attempts' rows, release
// angles 0 to 216 (0, 16, 24, 96, 120, 128, 144, 152, 160, 168, 192, 216), every one within 0.0005 degrees of the sum
// (a constant 24 a frame, 16 x 1.5, fits 17 of them: the four others straddle a 1.5 / 1.0 step of the warp). A dash of 4 ticks or fewer reads 0, which is the case --balldashrot was measured on. What a
// non-upright ball does (the sign) is the cube's rule and is not measured here. Hangs on --balldashrot.
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
// --lawunderflip (ON since 2026-10-24; --no-lawunderflip is the off arm): the underside release also for a FLIPPED ship against a floor ramp that rises in the
// travel (the mirror of --lawunderrelease's upright body under a ceiling ramp that falls in it). The seat the law puts
// a body on is a contact like the push-down one, and leaving it launches away from the surface; the model armed that only
// for an upright body, so a flipped ship the law seats on a floor ramp's high end corner (855.000, one tick) kept its
// thrust where GD reads +1.791 = slopeExitVy(1, ship, dx 1.0465) x the one-tick factor 0.4 on the next row (a custom
// level's long route, t=31,228 -> 31,229; every earlier difference on that route heals by itself, this one does
// not). One witness: the sign, the mode (ship only) and the
// m > 0 side are read off it and off the mirror of the measured upright case, nothing else.
inline bool g_lawUnderFlip = true;
constexpr double kWaveRingHalfMini = kWaveRingHalf * 0.6;
// --portalslopeorder's re-run (frames.hpp; the end of stepOne): the uid of the gravity portal
// the nested step fires ahead of the ramp pass, -1 when no re-run is in progress.
inline thread_local int g_prePortalUid = -1;
// --modeslopeorder (on by default since 2026-10-14): the same re-run for a MODE portal that turns a flying body (ship, UFO,
// wave, swing) into a ground one (ball, robot, spider) on a tick a ramp with a larger uid would seat the
// new body. The game's object loop reaches the portal first and the ramp then meets the new mode's box
// (collisionCheckObjects: the portal at 0x214d8e, the ramp at 0x215e73, one loop); the model's ramp pass
// runs before its portal pass, so the new body sits a tick late. Official lv21 t=16,610: a wave rising
// off ramp uid 22024 takes spider portal uid 22017; GD's spider sits on the ramp at y 163.5 with vy 0 on
// that tick and leaves it with the exit launch (vy 4.203) on the next, while the model came out at
// y 161.0 with the halved 2.597, sat a tick later, and its held press then warped the spider to the
// ceiling. Kept to a non-dual body in frame 0, and taken only when the re-run seats the body on a ramp
// whose uid is larger than the portal's.
inline bool g_modeSlopeOrder = true;
// --spiderportalfirst (on by default since 2026-10-14): a grounded spider's press warps it after the collision pass, so a
// mode portal the spider stands in on that tick turns it first and the press is the new mode's. The
// model warps in the step, before its portal pass, and the portal then misses the warped body.
// Measured on SubZero 4002 t=17,217-17,218 (a Windows worker, the died plan with its press moved): a
// press on 17,215 or 17,216 warps the spider to the ceiling on the next tick, while the press on
// 17,217 -- whose next tick takes wave portal at the spider's feet -- leaves it on the floor as a
// wave, which climbs from 17,219 on the held button. So a warp is re-stepped without the tap when the
// body would have met a mode portal where it stood, and that run is taken if the portal turned it.
inline bool g_spiderPortalFirst = true;
// --spiderportalcube: ...a cube portal (type 6) among them, which the list left out. A custom level: an upside-down
// spider on a ceiling presses with cube portal uid 14683 at its feet; GD's row is a cube at y 3,345 with vy -11.23 (the
// press a flipped cube's jump), and the model warped the spider 63 px down to the floor and took the portal a tick later.
inline bool g_spiderPortalCube = true;
// --padbandin: a spider pad's search stops at a walled band only when the body is inside that band. GD's band
// (getMinPortalY / getMaxPortalY) holds the player; the model's state band can be one a portal left that the player
// is no longer in. A custom level: an upside-down cube under the zoomed camera band [3,851.6, 4,209.4] takes a
// ceiling spider pad at y 4,079.5 and GD drops it to the floor at 3,945; the model's band was [4,227.7, 4,567.2],
// above the cube, so the downward search found nothing above that floor and teleported the cube up to 4,242.7.
inline bool g_padBandIn = true;
inline thread_local bool g_noSpiderTap = false;
// --flipridepast (default on): a FLIPPED cube riding the solid-up side of a ceiling ramp past the
// ramp's far end is still on the ramp's extrapolated line, as the upright cube riding downhill is, so
// the press that tick still finds ground. The support test widened its window past the end only for an
// upright cube (step.hpp, the ramp support), and a flipped mini cube's press was refused with
// groundedNow=0 while GD jumped. A custom level (a mini cube flipped on a ceiling ramp, m=+0.339,
// t=9,191..9,199, 16..28 px beyond the ramp's x1): GD answers a press at 9,191 or 9,194 with the mini
// jump -8.944 on the next row and lives past the spike at 9,207 (a press sweep over 46 starts: only
// those two survive); the model rode on with vy 0 and died there, and so did every route the search
// tried. The window is the upright one's, with the slope's sign mirrored.
inline bool g_flipRidePast = true;
// --sizeseat (default on): a flipped mini cube seated under a solid that takes a regular-size portal is
// seated under it at the regular size on the portal's own tick (the model did it a tick later, from the
// next tick's support test). A custom level t=9,663: GD's row has y 1,874.750 (face 1,889.750 - 15), the
// model's 1,880.750 and then 1,874.750 on the next row; the 6 px was a divergence at every plan through there.
inline bool g_sizeSeat = true;
// --sizeexit (default on): a ship whose last ramp seat was made at the other size (a size portal fired
// since) does not hold the ramp window: the gap to the new size's line is the size change, not a stair, and
// GD loses the ramp and launches the ship. A custom level t=3,859 (cube turned ship on a ramp, then the
// mini portal): GD vy +1.802 on the exit, the model 5.73 px inside the 5.85 px window and no launch.
inline bool g_sizeExit = true;
// --padwaveold (default on): a pad after a mode portal that turned a WAVE this tick is judged with the
// wave's box, as before the portal (see the --padafterportal block in step.hpp).
inline bool g_padWaveOld = true;
// --rideoverspeed (default on): a ramp contact the 5.0 landing threshold refuses (the body arrives faster
// than that on its gravity side) still is a ride: collidedWithSlopeInternal writes the slope velocity at its
// tail whichever way the landing went, so the exit launch happens (see the rideLanded note in step.hpp).
inline bool g_rideOverSpeed = true;
// --rideoverspeedunder (ON since 2026-10-24; --no-rideoverspeedunder is the off arm): --rideoverspeed's exit launch also for a contact on the UNDERSIDE (m_isUpsideDown XOR
// isTop, see slopeUnderside: a flipped body on a floor ramp's top, an upright one under a ceiling ramp's underside). The gate
// excluded it, on the ground that the underside path has no velocity to put back; the launch is another write (the tail of
// collidedWithSlopeInternal), and GD makes it whatever the entry speed. Measured by injecting a flipped ship onto a floor
// ramp's top (m 1, speed 1.6, a custom level, t=29,122): the entry vy overwritten with -12 .. -6 (fast: away from its
// gravity at 6.1 .. 8.0, the ceiling of the ship's fall) gives vy 0 on the contact tick (29,124, y 765.000) and +4.107 on
// the next, which is slopeExitVy(1, ship) 10.27 x the ride factor 0.4; entering at -5 / -4 / -3 gives the +2.000 nudge on the
// contact tick and the same +4.107 on the next. The model had the slow entries (they landed) and none of the fast ones.
inline bool g_rideOverSpeedUnder = true;
// --ringspeedfirst (ON since 2026-10-24; --no-ringspeedfirst is the off arm): a ring's impulse scales with the speed of the moment it fires, and the game's collision pass runs
// its objects in uid order, so a speed portal of a smaller uid that fires on the same tick has already changed the speed
// when the ring is reached. The model's speed pass comes after the rings and scaled the impulse with the speed the tick began
// at. Two witnesses in GD's rows, one each way: a custom level t=30,404, cube, speed 1.6 -> 0.7, the 0.7x portal uid 165872 and the red
// ring uid 165875 on the same tick: GD's vy is -14.656 = 10.62 (the cube's jump at 0.7) x 1.38, the model's -15.497 = 11.23 (at 1.6) x 1.38;
// and t=38,400 of the same level, a UFO, speed 1.6 -> 0.9, the ring uid 211488 BEFORE the portal uid 211761: GD's vy -5.486 is what the
// model reaches with the old speed, to the third decimal. Only the impulse's speed scale is moved, for an unrotated portal
// the body is inside by the speed pass's own gates (box, the portals it fired lately).
inline bool g_ringSpeedFirst = true;
// --tpportalforce (default on): --tptrigforce's rule for a teleport PORTAL: no push the next tick when the
// place the pass moved the body from had none, judged with the size the body had when the pass began.
inline bool g_tpPortalForce = true;
// --forceminifly (default on): a force box pushes a MINI ship or UFO 1.25 times as hard as a full-size
// one. PlayerObject::update scales the push by a per-mode constant that has a second value for a mini body
// (ship 0.47 -> 0.5875, UFO 0.58 -> 0.725, both x1.25), and the cube's and ball's stay (measured mini rigs).
// A custom level, mini ship in a 1.7 box (t=17,553..): GD's rows climb 0.045 a tick faster than the model's,
// the push 0.225 where the model's was 0.180 (1.7 x 0.1058).
inline bool g_forceMiniFly = true;
// --lawfrombelowship (default on): --lawfrombelow's seat from below (a contact the acquisition gate throws
// away goes to GD's law, which has no bound on how far below the seat the body may be) for a SHIP too.
inline bool g_lawFromBelowShip = true;
// --tpsaveoff (default on): an id-747 teleport portal with m_saveOffset (objrects tpsave, tprx, tpry) keeps the
// body's place relative to the entry's REAL position: y' = exit y + (y - real y), x' = exit x + (x - real x).
// A custom level t=3,193: GD takes the cube from y 705 to 1,355.95 (1,371 - 15) and x back 14.70 (3,347 -
// 3,361.7; cx holds the bounding box centre 3,369), the same x shift whatever height it enters at (press sweep:
// entry y 705 to 762); the model put it at 1,371 and left x alone, then died 13 ticks before GD.
inline bool g_tpSaveOff = true;
// --waverampend (default on, with --waveramp): the wave rides plain ramps also on the tick its END position
// first touches a 1755. The counter that arms the ramp ride is set in collisionCheckObjects before the solid and
// ramp passes of that tick (the note at --slideend), but the ramp gate read the start-of-tick touch only. A custom
// level t=17,830: a wave coming down onto a rising ramp, its end box 1.8 px into a 1755, sits on the ramp's line
// (y 423.84 = line + 5 sqrt 2) where the model seated it on the block beside it (425.0).
inline bool g_waveRampEnd = true;
// --ceilheldnoland (default on): a UFO held under a ceiling ramp (the model's ceilT, GD's m_isOnSlope with the
// under-side byte set and the ceiling ramp as m_currentSlope) takes no floor ramp. GD's slope record for a custom
// level, t=6,118 to 6,128: onSlope 1, under 1, uid 128564 (a ceiling ramp, m -2) every tick while the UFO slid down
// its underside inside the 60 px channel a floor ramp of the same footprint leaves (67 px of body along the normal),
// vy -2.000 falling by gravity alone (-2.344 on 6,122); no floor ramp landed it and none zeroed vy. The model seated
// it on the floor ramp (uid 129559) from 6,122, 7.08 px above GD's line from 6,123.
inline bool g_ceilHeldNoLand = true;
// --ceilowner (default on): a flipped UFO pushed by a ceiling-side ramp takes that ramp as its current slope even
// when a floor ramp seated it earlier in the tick. GD's slope record for a custom level, t=12,752: a flipped mini
// UFO at the crossing of a rising ramp (uid 164426, m +1) and a descending one (uid 164427, m -1) reads onSlope 1 with
// uid 164427 and rides its line down 2.4 a tick at vy -2.000; the model kept 164426 as the owner, so its next
// tick's seat carried the different-top-ness offset (bVar9, 12 px for a mini) and ended 5.944 px high.
inline bool g_ceilOwner = true;
// --waveportalrot (default on, gravity portals only): a wave meets a turned portal's box at its rotation one easing step back. A custom
// level t=15,210: a mini wave (rotation -12.759 at the tick's start, -7.128 the tick before) comes 0.017 px short
// of a gravity portal's oriented box (7.67 x 23.25, turned -22.2) at the first, and 0.205 px inside it at the
// second; GD flips it on t=15,210, the model on 15,211, and every plan after it ran 9.6 px off GD's.
inline bool g_wavePortalRot = true;
// --waveportalrotmini (default on; --no-waveportalrotmini is the off arm): --waveportalrot's step back for a MINI wave only. It was measured on a mini wave (above); a full-size wave on a custom level (t=10,739, a gravity portal turned -45,
// 18.7 x 56.2, wave rotation -30.4) is flipped by GD a tick after the step-back form does it, and the form at the state's own rotation matches GD for the rest of the attempt.
inline bool g_wavePortalRotMini = true;
// --modeswitchkeep (default off again: measured only for a spider-to-cube switch, but the three sites below gate on
// any flipped seated body, and with it on one custom level took a different, slower route): a body seated on a CEILING (flipped) that changes to a mode with a larger half is re-seated on the switch tick
// only where the grown box newly reaches a solid face BEHIND it that the old, narrower box did not (a block it had not overlapped in x). Over
// a row of blocks the new box reaches the one behind it first; the one it already rode is left alone, and the one just ahead (also newly
// reached, a side contact) does not seat it either; the next tick's pass seats it (or a gravity portal firing first releases it). The game, official lv22 t=8,744: a spider riding a ceiling row (y=826.5, half 13.5) meets the
// cube portal; shifting the same tick by x injection (w96, the plan of the cold's iteration 5), switch at x=12,224.90 (the cube's box now
// reaches the block 12,180..12,210) seats at 825 on the switch tick; switch at x=12,225.09, 12,225.50, 12,225.69 or 12,226.10 (only the block
// the spider already had) stays at 826.5 and seats the tick after, or falls free where the gravity portal at x >= 12,227.5 fires first.
// The existing rule above it took every switch as a seat; a floor (lv22 t=1,168: the cube reaches the block AHEAD by 0.06 px and the game
// does seat it) is left as it was, and the same for a flipped body whose box newly reaches a block behind.
inline bool g_modeSwitchKeep = false;
// --waveportalsize (ON since 2026-10-24; --no-waveportalsize is the off arm): --waveportalrot's step back is not made on the tick after a size portal changed the wave. The closed form
// assumes the last tick eased with this tick's size; after a size portal it did not, and the rotation it returns is not one the wave ever had.
// A custom level t=26,918: a mini portal at t=26,917 left the wave at -3.085 (from 1.574); the form read 3.620 and met the gravity portal (uid 138944,
// turned -25) a tick before GD's t=26,919, where the state's own -3.085 is 0.122 px short of it (and the form 0.077 px inside), and every
// plan after it ran 9.6 px off. Checked on GD's dumps (that level's t=26,919; other custom levels' t=7,326 / 12,198 / 13,108; a long route's 45 wave flips
// stay matched): the state's own rotation separates the fire tick from the tick before at those sites, where the exact rotation one tick back
// does not (the first level's is 0.077 px inside at t=26,918). The t=15,210 site above (above) is the one site that needs the step back, and no size
// portal is near it, so it stays as it was.
inline bool g_wavePortalSize = true;
// --nocolceil (default on): a STATIC solid with the passable byte (--nocolstatic, Obj::oneway 2) is no
// ceiling for a ship: it rises through the block's underside. A custom level t=18,300: a ship rising at 8.0
// (gravity just turned by a green orb) enters the underside of such a block (uid 162458, bottom 480) and GD
// flies on through it (y 466.147, 467.947, ... vy 8.000) where the model pinned it at 465.000 with vy 0.
// The moving plates (oneway 1) and the type-21 plates keep their ceiling (the note above).
inline bool g_noColCeil = true;
// --slideunderdir (default on): an armed wave is pushed down out of a static underside only while it moves
// toward it. A custom level t=18,743: a flipped wave leaving a 1755 block's bottom face (static, bottom 540)
// downward, its previous head 4.04 px inside the face and 3.95 px across it, is not pushed in GD (y 536.641,
// 534.241, ... vy -9.6) where the model seated it at 535.000 with vy 0 and every route died there.
inline bool g_slideUnderDir = true;
// Always on (was --slidetopdir): the same for a top face. An armed wave that is rising (y above the tick before) is
// not pushed up onto a STATIC top face; it is pushed only while it moves toward it, or while the face rises
// to meet it. A custom level t=5,806: a wave rising at 45 deg past the left corner of a static block (uid 20513,
// top 390.75) with the dart-slide counter set (1755 box, dart=1 from t=5,803), its previous foot 5.1 px under the
// top, is not pushed in GD (y 390.646, 392.179, 393.713, ...) where the model seated it at 395.750 and it
// flew 3.57 px high over the next five ticks. The same wave 5 px under a block still rising (dcy +0.15 a tick,
// t=5,795) is lifted to its top in GD (injected from the tick before, 0.25 to 10 px across the face), and one under
// the static block at t=5,805 is not, whatever the offset (-3 to +8 px across it).
// Always on (was --slideminibox): a MINI wave touches a 1755 dart-slide box with the 6x6 rect GD tests objects with (half 3.0, kWaveHazHalfMini), not with the 2.0 bracket
// kWaveHalfMini (a speed portal's, a different code path). The arm then comes on the tick the rect first overlaps the box and the slide push-out seats the wave that tick,
// where the model seated it one tick late. A custom level, two sites: a mini wave falling onto a floor (box top 120, wave centre 17.21 above the box centre, half 3.0 overlaps,
// 2.0 does not) is seated by GD at 122.750 on t=21,499 and the model flew on to 122.210; a mini wave rising to an underside is seated at 177.000 on t=15,588, the model flew to 177.600.
// Always on (was --wavesolidend): a wave's solid kill (wave/solid) tests the tick's END position only, not the kSubSteps interpolated samples before it. GD tests a step's end
// position once: GJBaseGameLayer::update splits a frame into max(1, round(4 x dt x 60 / min(timeWarp, 1))) steps (one model tick each) and neither checkCollisions (0x2137f0)
// nor PlayerObject::update (0x388d80) loops inside a step. A custom level (t=28,928): a mini wave climbing past the lower right corner of a static block (23.28x30) overlaps the
// kill box by 0.44 x 0.21 px at one interpolated sample (f=0.125) while both tick ends are clear of it (dy 16.89 > 16.5 at the start, dx 14.797 > 13.14 at the end); GD flies on
// and the model died there, 195 ticks before GD's own death. With the flag the replay of that section follows GD to its death at the same tick and x.
// --seedchain (default on since 2026-10-23; --no-seedchain is the off arm): at an anchor, a Count or a Tap trigger that the attempt's rows cannot have entered (nothing for a body to touch: it fires from an item counter or a press) is opened from
// the recording of what it moves, as a touch box is, instead of being refused by --touchentered's list (which names only the boxes the rows overlapped). A custom level, anchor t=16,836: a Count
// (item 10 == 4, 529 objects) and a Tap fired at 16,810 / 16,800 in the walk from the earlier anchor and in the game (the recorded rows of a hazard it moves are 9 px up at 16,837), and the anchored
// search had them unfired (--seeddump: trig 0x30 against 0xf0), so its hazard sat at its base position: four plans in a row passed it in the search and died on it in the game.
inline bool g_seedChain = true;
// Always on (was --robotleavehover): the robot's hover budget (State::rHover, the ticks its jump keeps lifting while the button stays held) is dropped when a mode portal takes the body out of robot form, as it is on a
// release. The release is dropped by the robot's own branch, which a mode portal's tick does not run, so the budget stayed through the cube stretch after it and a robot portal that came next with the button held
// hovered on the old budget: a custom level's robot leaves a robot portal pressed at t=30,849 with 62 ticks left, turns cube, takes the next robot portal at 30,854 in the air with the button down, and the model
// held vy at 3.628 for 60 ticks where the game lets it fall (3.433, 3.238, ...) and killed a different plan 394 ticks sooner than the game.
// Always on (was --flyportalland): --portalceil's pass (a body a mode portal made a flying one meets the blocks on its head side, once for the new body) also runs for a FLYING body a mode portal turns into
// ANOTHER flying body on this tick (wave into ship), and puts it out of every block its new box meets in turn, not only the first, from its OLD box's position. A custom level's flipped wave takes a ship
// portal at t=13,562 over two blocks (tops 208 and 214, the wave's foot 1.95 above the higher): the game pushes the ship out of both that tick (y=229.000, vy 0), and the model left it at 219 and seated it on
// the lower block alone a tick later, 6 px low. With the flag the replay of that stretch matches the game to its own death, at the same tick and x.
// Always on (was --waveslideramprot): a wave the dart-slide push-out holds on a block while a floor ramp's line runs under it (within 6 dx, from the tick after the push-out
// first seated it) turns by the ramp rotation law (--waveramprot's), not by the angle it actually moved at. A custom level's mini wave on a block over a 45 degree ramp (t=21,500..21,503):
// GD's sprite eases toward 45 at 0.0603 a tick (the cube's slope law at speed 0.7: two half-angle steps of 0.04375 x 0.7 = 0.0306 each, PlayerObject::updateRotation 0x391020's
// m_isOnSlope branch), while the actual-motion law decays toward 0 at 0.1; the 14 deg difference put the box 6 ticks later on a ship portal's oriented box, and the model entered the
// ship a tick early. GD held m_isOnSlope through a gap of 6.07 and dropped it at 7.12; the 6 dx is read off those two.
// --waveseathaz (default on): the wave's hazards are also tested at the position the slide push-out leaves it
// in (see the block after the push-out in step.hpp).
inline bool g_waveSeatHaz = true;
// --tpramplaunch (on by default since 2026-10-14): a ramp's rider that a teleport takes off the ramp gets the ramp's exit
// launch (slopeExitVy x the ride's factor), as postCollision gives any rider that has left its ramp on
// the tick; the teleport runs in the object pass, before it. The model seats in its ramp pass before
// the teleport and the ride never ended for it. SubZero 4003 t=12,468: a cube climbing an m=1 ramp
// (reversed, 1x) walks into a teleport after 19 ticks on it; GD comes out with vy 6.171 =
// 7.405 x 20/24, the model with the gravity step.
inline bool g_tpRampLaunch = true;
// --waveholddead (on by default since 2026-10-14): the wave climbs on +0x985, the same hold latch the ship reads, so a
// button that a spider's warp or a ball's flip left dead while it stays down (State::holdDead) moves
// a wave as a released one. SubZero 4002 t=17,220-17,222: the spider warps on a held press, a wave
// portal turns it on 17,221, and GD's flipped wave falls toward its gravity on 17,222 while the
// model, reading the raw button, climbed.
inline bool g_waveHoldDead = true;
// --downdyrev (on by default since 2026-10-14; --no-downdyrev is the off arm): the floor ramp's feet band (+1, kWaveSlopeDescendDy) is bVar22's "descending", which XORs
// m_isGoingLeft into the ramp's own direction (slopes.hpp); the model read the ramp alone. SubZero 4003 t=12,328
// (Worker 99 hitboxtrace): a reversed wave runs down-left along ascending plain 1338 uid5320 at a constant 7.71 px
// above the line; GD destroys it (no object) at x=9,624.64 y=302.35, inside 301.71 + 1, where the cap (304, a
// fresh contact) had spared it the tick before.
inline bool g_downDyRev = true;
// --wavebandzoom (on by default since 2026-10-14; --no-wavebandzoom is the off arm): the wave's band clamp (step.hpp yMaxW) read the state's band as the portal wrote it, at
// zoom 1. GD's is getMinPortalY's branch B (win 0x213690): centre - H/2 / camScale, floored at 90, and the max
// is that plus H / camScale. SubZero 4003 coin run t=10,320: zoomed to 0.833333 at x=9,795, GD's band is
// [150, 510] and its wave rises to 470.95 and into a teleport; the model clamped it at 480 - 10 = 470.
inline bool g_waveBandZoom = true;
// --flybandcentre: ...and the same for the ship / UFO / ball band (step.hpp floorY / bandCeilNow), which kept the
// portal's floor and grew only the ceiling by H / camScale. SubZero 4001 t=20,379: zoomed to 0.833333, GD's ship
// band is [540, 900] around the portal's centre 720; the model's was [570, 930], put the falling ship on a floor
// of 585 and flew it over the spike GD kills it on at 20,393.
inline bool g_flyBandCentre = true;
// --spidernoextcol: the spider's surface search (step.hpp spiderTargetGd) does not see an Extended Collision object.
// spiderTestJumpInternal asks staticObjectsInRect / damagingObjectsInRect, which walk the section grid, and
// addToSection files such an object in the layer's always-checked list instead (memory: the --bigsection note).
// A custom level t=7,037: an upside-down spider taps down past a 477 x 30 id-3610 hazard with extcol=1; GD's
// hazard query returned another object, its solid strip a block it landed on 588 px down, where the model
// counted the 3610 as found, widened the solid query and landed on a column beside the strip 78 px down.
inline bool g_spiderNoExtCol = true;
// --autolockmoves: an object an x-crossing Move locks to the player's x (alock / alockY, level_loader.hpp) counts as
// moved by its autonomous controller, so a touch box that only switches it on or off (no offset) does not make it a
// touch-controlled object held at its first row until the box fires. The same custom level: block uid 51884 is
// locked to the player's x from the trigger at x 315 (its recording follows the player to x 9,712) and box 1 only
// turns it off; the model kept it at x 315, and the upside-down spider GD lands on it 588 px down missed it.
inline bool g_autoLockMoves = true;
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
// Always on (was --chantog): an x-crossing Toggle (1049) on a trigger channel that is not yet the active one does not switch its group when the player passes it. On a level
// with rotated gameplay such a trigger sits in its channel's queue and fires when that channel is active (frames.hpp g_rotQ), not at the first crossing. Here "not yet" is
// static: a rotate-gameplay trigger (2900) of an earlier channel lies ahead of it in x (triggers.hpp chanNotYetActive). The model
// switched the group at the crossing and its on/off then ran against GD's recording (a custom level: a pad and a slab group switched off at x=8,355 that the
// game keeps on, so the ship that GD stops under the slab flew through in the model). The recording's on/off holds instead.
// --chanmove (ON since 2026-10-24; --no-chanmove is the off arm): a Move (901) or a Toggle (1049) on a trigger channel fires only while that channel is the active one (for a Toggle it replaces --chantog's static test). GD walks the queue of the active channel only (checkSpawnObjects), and the channel is raised by a
// rotate-gameplay trigger (2900); a trigger whose channel is raised after the crossing fires on the tick it is raised. In an official lv22 solution a Move on channel 10 (x=15,945) is crossed at t=11,200 with channel 0
// active and does not fire (the dump's rotch column: 0 until t=11,339, 10 from t=13,704 to t=14,591); GD fires it at t=14,378, on the second pass of a loop the level sends the player round. The model fired it at
// the first crossing, 3,000 ticks early, which moved its block to the end of its path and killed a ship that GD lets through (t=14,673). The model's active channel is State::rotChan (the rotation queue), and the
// replay and the search both read it (AutoTrig::chan). The Toggle of the same level: a ship portal's group is switched off at t=10,100 and on again by a Toggle on channel 10 (x=15,765) at t=14,272; without the channel the
// search had the portal on from the start, fired it on the first pass, and never saw the ship it makes on the second.
// A channelled trigger is passed in the direction its rider travels: after a reversing rotate (State::rev) it fires when x falls to its cx, not when x reaches it, so a channel raised in a section that runs back over the level
// does not fire every trigger behind the rider at once (a custom level whose channels 1 and 2 hold hundreds of Moves).
// Only where the rotation queue is on: a level with more than 32 queue entries refuses it (State::rotSpent is a 32-bit cursor), nothing then tracks the active channel, and the trigger fires
// at its crossing as without the flag (a Toggle --chantog drops stays dropped). A custom level whose queue has 49 entries lost every channelled Move to it.
inline bool g_chanMove = true;
// Always on (was --landjumpbonus): a cube or robot that LANDS on a ramp and jumps on that same tick gets the ramp's jump bonus (a quarter of its exit velocity at the
// ride's first-ticks factor, capped at 0.4 of the jump). GD's pass makes the contact first (the on-slope flag), then the buttons phase reads it; the model's jump is taken in the solid
// pass, before the ramp pass, and only a ride that began on an earlier tick (State::onSlope) carried the bonus. A custom level: robot jump on the landing tick GD 6.369, model 5.590.
// Always on (was --slopewarp): the launch off a ramp's end is scaled by the time warp at the player's x. GD's exit velocity is the ramp's m_slopeVelocity x the ride's
// factor, and under a Time Warp trigger of 0.95 a custom level's robot leaves at 6.473 where the model gave 6.814 = 6.473 / 0.95.
// Always on (was --anchorrot): an object whose recording starts by TURNING in place (rotation or box size, no position change) keeps the earliest controller as its
// anchor -- the pivotless Rotate whose crossing starts those rows -- instead of --anchormove's first Move. A custom level's spinning spike is turned from x=16,425 and moved from
// x=16,545; its first recorded row is the turn, the Move anchor read the recording 61 ticks early, and the model put it 6 px from its recorded place in the run that recorded it.
// Always on (was --wavegrace): an armed wave keeps riding ramps for one more tick, the tick after the counter was last set (State::slideT 0), as the counter's
// decay would. A custom level's mini wave rises past a locked 1755 (30x30) and GD still rides the ramp the tick it is 22.9 px above its centre, a tick after the last touch.
// Always on (was --padinv): a turned pad's real box is rebuilt from BOTH sides of its bound when the uniform scale the loader assumes does not reproduce the bound's height.
// A custom level's pad (raw 25 x 4, rot 29.46, bound 118.6 x 69.4) is 134 x 4 -- stretched along its length only -- and the uniform scale read 125 x 20, a box five times too thick: the
// pad fired a tick before the game's.
// Always on (was --padorientlive): a TURNED pad meets the box the player has after a mode portal of the same tick, as a turned portal does (the r32 rule: GD re-reads the player's
// oriented box on the spot for a rotated object, and keeps the entry box for an axis-aligned one). --padwaveold's wave box stays for the axis-aligned pad it was measured on.
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
// --p2born (default on; --no-p2born is the off arm; two-player levels): the second body a dual portal makes is born holding nothing, and the plan's level 1 for its button (held since before it existed) does not hold it:
// the game gets an input only where the mask changes, so the body takes a held button only after a release and a press the plan makes. A custom level: the plan holds the second player's button from t=27,763 and the
// dual starts at 27,796; the game's second body lands on the ceiling at 27,870 and does not jump again while the model's re-jumped, and the plan replayed from the start died at 27,897 there.
inline bool g_p2Born = true;
// --p2held (default on since 2026-10-23, --no-p2held is the off arm; two-player levels): the second body's wave, whose line follows the button it held at the tick before, reads ITS OWN previous button. The second body is stepped from a
// copy of the pair's state, in which `held` is the first body's, so a wave born while only the first player holds went on climbing for as long as the first body's button said, and not the second's. the long custom level (a
// two-player level), t=22,350: the second body is born a wave; GD's turns down at 22,351 (its button is up), the model's at 22,353 (the first player released at 22,352), 8 px apart at once.
inline bool g_p2Held = true;
// --p2shared (default on since 2026-10-23, --no-p2shared is the off arm; two-player levels): a search call whose anchor has no second body yet steps that body, once a dual portal makes it, with the first body's button (advanceState gives
// stepBoth no input2), but the plan it writes carried the first body's levels alone, and a replay of it holds the second player's button down on nothing: the second body then dies where the search
// let it jump along. the long custom level t=22,433: the plan's second body, on the floor, never presses and meets the 6 px spike at x=30,247.6 (GD and the model die on the same tick); with its levels written as the
// first body's too (bit 1 = bit 0) the model's replay of the same plan goes on to t=22,799.
inline bool g_p2Shared = true;
// Always on (was --portseen6): a body that is not in a dual remembers the portals it overlapped on the previous tick in six slots, not three (the second body's three are free). GD activates a portal
// once, on the first tick it overlaps, and never again in the attempt (hasBeenActivated / ...ByPlayer are both 1 from that tick, read off the object by the ccl probe); the model keeps the
// portals seen on the previous tick (State::portSeen) and does not fire them again, and a fourth overlapped at once found the slots full and fired on every tick of the overlap. A long custom level,
// t=26,943..26,975: a row of seven size portals 14-16 px apart, the ship overlapping up to five at once; GD changes its size at 26,943, 26,950 and 26,971 (each portal on its first
// overlapping tick, in order of their left edges), the model stayed regular size from 26,944 (the last normal-size portal re-fired over the mini one on every tick), 0.056 a tick apart in vy
// from 26,951 and killed at x=35,067 where GD lives.
// Always on (was --spdseenall): every speed portal a body fired is remembered, with no limit. GD marks each speed portal it activates, per object, for the attempt and never fires it again; the model's
// memory of them (State::spdFired and --spdsameonce's spdMore) holds four, so when more than four overlap in one stay the oldest are forgotten and fire again, one after the other. A custom level,
// t=31,006: the mini cube passes four 0.9x and four 1.1x portals at once (two copies of each at two heights, the copies in other groups); GD takes the 1.1x ones at 31,010 and stays at 1.1x, the
// model forgot the 0.9x ones, fired them on 31,011, forgot the 1.1x ones and fired them on 31,012 and alternated 1.298 / 1.614 a tick: 3.2 px behind at 31,040, 4.7 px at 31,230, where the ship it had
// become lost its footing two ticks late. With the flag a portal that the four places would give up is pushed onto State::spdSpill, a persistent list shared by the states that came from one another
// (a node is never changed, so a copy of the state is a copy of one pointer), and the firing test reads it too: the number of portals a body can remember is the number of speed portals there are.
// --movingwallup (ON since 2026-10-24; --no-movingwallup is the off arm): an UPRIGHT body takes the moved-partner arms of collidedWithObjectInternal (the thin-top-moving and movingwallseat arms: seat on the top with the
// partner's velocity, past the slope map's veto) only when the partner moved UP. The function sets two flags from the partner's top now against its top a tick ago
// (0x391d9b-0x391e26): [rsp+0x37] when it rose, [rsp+0x39] when it fell. The upright path (0x392536: al = the flipped flag, 0) goes to the arm only through `cmp [rsp+0x37], bl; je 0x39288b`,
// i.e. only for a partner that rose, and a partner that fell leaves the ordinary block rules (and the slope map's veto); the flipped path (0x3926cd) tests [rsp+0x39] instead. The model ran
// the upright arms for any dcy != 0. A custom level t=34,061..34,069: a ball rides a floor ramp (uid 115083) beside the wall of a steeper ramp whose group moves down (0.06 px a tick); the
// ball's box overlaps the wall by 1.5 px and the model seated it on the wall's top (+2.2 px, then a launch), GD keeps it on its ride for nine more ticks.
inline bool g_movingWallUp = true;
// --stickuphill (ON since 2026-10-24; --no-stickuphill is the off arm): the ride's stick (a riding body stays on the ramp's line within kStickGap even where free fall would leave it above) is for a line running away downhill. On an
// UPHILL ramp (in the travel direction) GD does not hold a body above the line by more than the ramp's own movement a tick (collidedWithSlopeInternal adds the relative y velocity of the slope to
// its tolerance, and nothing else for a rider of this kind): the contact fails, the ride ends and the exit launch runs. GD injection probes on the ball, custom level, ramp m=0.5 in a group that
// sinks 0.059 / 0.040 a tick (y varied above the natural seat, free fall ending that far above the line): mid-span (t=34,060) it seats up to 0.055 and launches (vy 2.417) from 0.060; one tick
// before the end (t=34,068) it seats at 0.030 and launches from 0.040; at the end tick every y launches. The model held it within kStickGap = 4.034 (a ball 0.037 above the top of the ramp
// stuck to the line for sixty ticks where GD launched it at t=34,070). The ramp's movement is max(landAllow, |dcy|) here; a ramp that stands still keeps the 0.001 allowance. Past the ramp's far
// end (the sample outside the span) the stick does not hold at all: at the end tick (t=34,069 -> 34,070, x past the end by 0.040) every injected y launches, even one 0.04 under the line.
inline bool g_stickUphill = true;
// --ballcontride (ON since 2026-10-24; --no-ballcontride is the off arm): the BALL's continuing ride on a flipped ceiling ramp has no x window either. The model's continuation (contRide, step.hpp: GD's continuing contact is a y test with
// the ramp's far edge, 0x38fc08-0x38fc2f, and its seat is the extrapolated line clamped to the ramp's extent, slopeSeatTarget; the ridden ramp is skipped by preSlopeCollision, 0x38f2f0) was
// written for the cube and the ship; the ball left the ramp where its contact point crossed x1 + one tick, and flew free. Measured on a custom level (a human run, t=19,813): a flipped ball
// rides ceiling ramp uid13978 (m 0.5, span [26205, 26265]); the next ramp uid13979 (the collinear piece 0.3 px further on) is 0.004 px short of meeting the ball's box. GD stays on 13978's
// line to 0.0000 up to x=26,296 (t=19,829), 31 px past x1, with vy 0 and onGround 1 (13979's line is 0.05996 higher), then takes 13979 (from t~19,831). The model flew free from t=19,813 (vy up to
// 4.5), caught the next ramp at 19,824, and was 157 px off GD by 19,950 where it died (GD: 21,005).
inline bool g_ballContRide = true;
// --waveceilend (ON since 2026-10-24; --no-waveceilend is the off arm): a wave takes the corner tick of a downhill ceiling ramp's far end like the other flight modes. A fresh underside contact for the ship, UFO, spider and swing is
// taken while the contact point is up to a half box past the low end (freshEndU): the push goes to the corner flat (line(x1) - pH), one more step before it lets go. The wave was left out of that
// list. A custom level t=29,543: a wave flying under the tail of a steep ceiling ramp (m -2.149, x [39414, 39454]) with its contact point 0.15 past the end is pushed by GD from 1020.200 to
// 1018.995 = the corner flat, the model's own limit (printed as lim) and left 1.2049 px high for 330 ticks.
inline bool g_waveCeilEnd = true;
// --dualorblate (ON since 2026-10-24; --no-dualorblate is the off arm): both bodies of a dual pair press the SAME gravity ring (blue orb, type 13) on the tick the press begins. GD fires the two rings in the buttons phase, p1's first: p1 turns, its
// partner call turns p2 (halved), then p2's ring turns p2 back, kicks it, and its partner call turns p1 back (halved again). Net: neither body's polarity changes, p1 keeps half its kick, p2 the whole
// kick in its own gravity's direction. The model ran p2's ring on p2's unturned polarity (it turned p2 the other way, with the opposite kick) and p1's late partner call halved p2 again. A custom level,
// t=22,384 (the same plan, the press moved to 22,379 gives the same rows): p1 up 1 vy +10.24, p2 up 0 vy -8.728, ring (41,353, 733): GD p1 -2.246 (up 1), p2 -4.492 (up 0); the model p1 -4.492 (up 0), p2 +4.492 (up 1).
// With p2 alone in the ring (the press at 22,372 / 22,376) the model already matches GD, and so it does for a held press (--dualorbflip).
// SCOPE: this is a special case, not the general rule. It fires only when p1's late ring and p2's fresh ring are both type 13 and both bodies turned; the general form (each body's ring passed in turn, the partner call between)
// is left for after the release, and so are two green orbs (type 29) and a held press against a fresh one -- none of them measured.
inline bool g_dualOrbLate = true;
// --ringflipride (ON since 2026-10-24; --no-ringflipride is the off arm): a gravity ring (blue orb) pressed by a body that rides a moving face. GD seats the rider on the face's NEW height first (the collision pass) and the ring fires after that,
// so the leap starts from the carried y, as a ball's tap and a jump already do (step.hpp tapRide / jumpRide); the model left a ring that turns gravity out of that rule (jumpRide needs c.flip == s.flip, tapRide the
// ball's own tap), so the ball left from the old height and the 0.1 px stayed as an offset. A custom level, t=1,336 (a mini ball on a plate that sinks 0.107 a tick, the ring at (3195, 525)): GD y 523.947 -> 523.840 with
// vy +2.515, the model 523.947, the offset staying for the rest of the section (0.6 px by t=1,347, a death at 1,430 the model did not see).
inline bool g_ringFlipRide = true;
// --landtapspent (ON since 2026-10-24; --no-landtapspent is the off arm): a ball that ARRIVES at the ceiling (or floor) on the tick of a fresh press taps on that tick (step.hpp, 'ground/flipceil'), and that tap consumes the press, as the grounded tap does
// (c.pressSpent = 1). The landing branch left the press unspent, so the held button then fired the gravity ring the ball had just left under, one tick later. A custom level, t=1,346 (a mini ball reaches the
// ceiling at 531 on the press at 1,345, held to 1,355): GD taps once (vy -2.695, upright) and flies on; the model tapped, then took the ring at (3,228, 525) on 1,347 (vy +2.515, flipped again) and sat on the
// ceiling while GD fell, 12 px apart at 1,382 and a death at 1,430 that the model did not see.
inline bool g_landTapSpent = true;
// --flyridedown (ON since 2026-10-24; --no-flyridedown is the off arm): a resting flyer is held by a moving face that is falling away only when its foot is not below the face. The support scan of the flight branch (step.hpp, flyRide) took any face within
// 0.6 px + |dcy| of the foot, falling or not, and re-seated the body on it. A custom level, t=8,017: a mini ship leaves a ramp's top end past the tip of a thin wall (top 180.000) that holds it at 189.000; the block
// next to it (30x30, top 180.286 and sinking 0.064 a tick) comes 0.46 px under the ship's side, 0.286 px above its foot, and the model seated the ship on that face (189.286). GD does not: its row is 189.000 whatever the overlap
// (injection, 0 to 2.5 px under the side, with and without the press). GD keeps a rider on a face that fell away only as postCollision's stick re-land does (the rule already above it for a drop of 1.25 px or more); a face
// that stands ABOVE the foot is no stick object of this body. The 0.268 px rode on to the spike 270 ticks later that GD's mini ship hits and the model's clears.
inline bool g_flyRideDown = true;
// --p2soloafter (ON since 2026-10-24; --no-p2soloafter is the off arm): the pass in which the second body's solo (dual-off) portal ends the dual reaches no pad or ring after that portal. toggleDualMode with the second body as the toucher hands player 1 the second body's attributes at that point (copyAttributes: position, gravity, mode, size, vy, +0x985/+0x986), and the object pass goes on in ascending uid acting on a body that is no longer played (the rule the portal pass already had, formerly --p2solo, p2SoloUid in step.hpp). The pad loop and the ring loop ran with no such cut, so a pad or an orb above the portal in uid that the second body overlapped on that tick fired on it, and player 1 inherited the teleport. A custom level, t=7,294: a normal wave pair ends its dual at a solo portal (uid 171) through the second body, which sits 0.11 px inside the spider orb (uid 22,556, the next object in the pass) while the press is fresh; the model teleported the survivor to y 295 on that tick, GD does not: its next press edge, two ticks later (7,296), meets the orb and fires it (a wave's ring fires on a press edge only, GameObject touched-ring list + pushButton). With the flag a section anchored at t=7,200 tracks GD through the death at t=7,299 (130 of 130 ticks; 94 without).
inline bool g_p2SoloAfter = true;
// --flyframesup (ON since 2026-10-24; --no-flyframesup is the off arm): in a rotated gameplay frame (State::frame != 0) a flying body that stands on something is checked against what it stands on. The flying walk looks for its support only when the body
// is flipped or above the level's floor line (y > floorY + pHalf + 0.5), and in a rotated frame the body's y is the frame's cross axis (here -21,795), nowhere near that line, so the scan never ran: a ship seated on a
// column of blocks stayed grounded for ever, after the column ended. A custom level, t=13,817 (gameplay frame 3, a ship against the face of blocks that end at y=1,830, the plan holding nothing): GD lets it fall
// away from the face (x 21,795 -> 21,840 by t=13,893, rotation 0 -> 21.9, accelerating 0.069 a tick); the model held it at 21,795 with vy 0 and rot 0, so the hazard 45 px further along was never reached
// and GD's death at 13,893 (the ship's top 0.1 px into a 120 x 90 thorn) was a plan the model flew on.
inline bool g_flyFrameSup = true;
// --ballorbtap (ON since 2026-10-24; --no-ballorbtap is the off arm): a ball seated by a spider orb that a HELD press fired taps back at once. ringJump's 0x2b branch turns gravity to the orb's facing and runs spiderTestJump (a flipGravity and a vy of +-1); the ball is on its new surface in the same tick, and the game's next row shows gravity as it was and vy +-2 (the buffered-press mirror 0x989 is still up after the orb: spiderTestJumpInternal clears 0x985 only, and pushButton had set both). A custom level, t=6,971: a ball held down since t=6,966 takes a spider orb (uid 170 at (10,875, 165)) from y 199.9; the orb call reads flip 1, vy 1.000 and the row is y 285.000, vy -2.000, flip 0, the next row y 284.521. The model sat on the ceiling (flip 1, vy 0) and fell 0.03 px a tick instead of 0.48, so no search from that state followed the game. Mode 2 only, and only the held press's orb (--spiderorbpass's seat); a cube lands on the next tick (the lv22 orbs). With the flag a section anchored at t=6,900 tracks the game for its whole 400 ticks (72 without).
inline bool g_ballOrbTap = true;
// --wavepadsink (ON since 2026-10-24; --no-wavepadsink is the off arm): a pad that fires on a wave standing on a floor leaves it where its own step put it. The game runs the pads before the solids, and the pad has already turned the wave's
// velocity upward when the solid pass looks, which refuses to land a rising body: the wave that slid on the floor (it moves a step down each tick and the floor pushes it back) is not pushed back on that
// tick and sits one step inside the floor until the next. A custom level t=28,433: a mini wave sliding on a block row meets the left edge of a pink pad; GD's y is 990.907 (993 less the released step
// 2.093) with vy 8.32, back on 993 the tick after; the model pushed it out first and kept 993.
// The same flag releases a mini wave's pad memory with the window it fired by (kWaveHazHalfMini, not 2.0): the pad was forgotten on its own tick and fired a second time (vy 8.32 again, no push-out).
inline bool g_wavePadSink = true;
// --spdseenall: the nodes of State::spdSpill. Appended under a lock (a push is a portal the four places could not hold, rare).
struct SpdSpillNode { int32_t uid; const SpdSpillNode* prev; };
inline std::mutex g_spdSpillMu;
// A node is made once for each (list so far, portal): two states that gave up the same portal on the same list share it, so the pool grows with the lineages that met a crowd of portals and not with the
// states that did. It is NEVER freed: dp calls can run side by side in one process (the lanes), the nodes are reached from states of any of them, and a call that frees "its" nodes at its start left the
// other call's states pointing into freed memory (the first run with the flag on crashed in spdSpillHas on its first search).
inline std::unordered_map<uint64_t, std::vector<SpdSpillNode*>> g_spdSpillPool;
inline const SpdSpillNode* spdSpillPush(const SpdSpillNode* prev, int32_t uid) {
    const uint64_t key = ((uint64_t)(uintptr_t)prev * 0x9E3779B97F4A7C15ull) ^ ((uint64_t)(uint32_t)uid * 0xC2B2AE3D27D4EB4Full);
    std::lock_guard<std::mutex> lk(g_spdSpillMu);
    std::vector<SpdSpillNode*>& bucket = g_spdSpillPool[key];
    for (SpdSpillNode* n : bucket)
        if (n->prev == prev && n->uid == uid) return n;
    SpdSpillNode* n = new SpdSpillNode{uid, prev};
    bucket.push_back(n);
    return n;
}
inline size_t spdSpillCount() {
    std::lock_guard<std::mutex> lk(g_spdSpillMu);
    size_t n = 0;
    for (const auto& kv : g_spdSpillPool) n += kv.second.size();
    return n;
}
inline bool spdSpillHas(const SpdSpillNode* n, int32_t uid) {
    for (; n; n = n->prev) if (n->uid == uid) return true;
    return false;
}
// --seatpast (ON since 2026-10-24; --no-seatpast is the off arm): the second look at a solid after a ramp's seat (--slopeseat: a face the seat crossed pushes the body back) is for a solid the seat leaves the body INSIDE. A ramp that carries a body past a block --
// the seat's box clear of the block's far face -- is not blocked by it: the game's solid pass finds the box touching or above the top (below the underside) and has nothing to push. A custom level's steep ramp (30 rise
// in 15, 2:1) ends against a block whose bottom is 15 above the ramp's low end: the game takes a mini wave that rises into the junction up the ramp's line, and over the block, to the ramp's cap (the block's top + 3.000);
// the model put it back under the block (bottom - 2, then --waveunder's 3). The rest of the junction (a wave a ramp seats inside the block's height) is untouched.
inline bool g_seatPast = true;
// ...and how many times a portal found all six slots full (it then fires on every tick of the overlap, silently): printed in the summary of a search (after capstat:) and of a replay, so a level that
// has a still longer row of portals shows it. Counted over every state the call steps, the search's dead branches included. Only under the flag.
inline std::atomic<long long> g_portSeenOverflow{0};
// Always on (was --waveunderflip): the end-of-tick limit of a wave a ramp lifted under a block (--waveunder: the block's bottom less the half stays the limit after the ramp's seat) holds for a FLIPPED wave
// as well. It was written for the upright one; a block's bottom does not care which way the wave is turned. the long custom level t=28,899: a flipped mini wave carried up a ramp under a block (bottom 990.0, x to 37,943.9)
// is held at 987.000 by the game for two ticks (vy 0), the model's ramp lifted it 2 px a tick past the limit (989.0 at 28,900) and it left the block's x range 2 px high; a 59-tick plan is then killed
// at 28,958 where the model's replay carries on. With the flag the replay follows the game's dump to its death, on the same tick.
// --holdseed2 (default on; --no-holdseed2 is the off arm; two-player levels): an anchor whose second player holds its button (--startact2 1) with GD's second +0x985 at 0 (jumpBuf2 0) starts that body's hold dead,
// as --holdseed does for the first body. The seed read the flag only under a first-player hold; at a custom level's anchors (t=27,945, 27,963) the first player's button is up, the second's is down and dead
// (born holding nothing, never released), and the model took the second body for a held button that jumps again on every ceiling landing, where the game's body stays put and is killed.
inline bool g_holdSeed2 = true;
// --wavepdbg <half> (experiment): the mini wave's contact half in the mode-portal pass; 0 keeps kWaveContactHalfMini.
inline double g_wavePortalHalf = 0.0;
// Always on (was --ceilcrush): an upright full-size ship that a ramp (or any rising face) carries above the band's
// ceiling less 8 px for two ticks in a row is destroyed, with a NULL object (checkCollisions 0x2137f0: the flag it sets on the previous tick
// and the one it sets now). The band clamp pulls a free ship down to ceiling - 15 and nothing kills it there; the solid pass then lifts it
// back, and it is the ride's height that counts. Measured on GD (a custom level, a ship riding a 2:1 ramp under a Static Camera's closing
// band; the ship moved along the ramp by injection, 17 samples of 0.05..0.5 px): flagged iff y > pmax - 15 + 7 (the kill tick moved with
// the 6.90 -> 7.00 px of overshoot, nothing else). Ship only, upright, normal size, Free Mode byte known and clear.
inline constexpr double kShipCeilKill = 8.0;
// Always on (was --boxmoves): a touch box is a trigger OBJECT, and a Move that displaces its group displaces it. A custom level's touch Toggle (uid 43421) is
// dumped 836 px under the floor (editor y -611); an x-crossing Move at x 11,895 lifts its group by 750 (to y 229), and GD fires it when the ball passes x 25,437..25,502 at y 230..300 --
// which switches off the group of a touch gravity trigger 40 px further on (uid 43420), so GD's gravity never changes there (gravityMod 1 in every attempt of the level), where the model, with
// the box where it was dumped, never fired the Toggle and fired the trigger one tick after the overlap (gravity x2: a ball falling 0.259 a tick instead of 0.129, dead 100 px early). Only
// groups a plain translation acts on (no Rotate / Scale / Area effect) and only Moves that have run their course by the box's x. A box that follows a recorded object (touchfollow) is left to it.
// The touch gravity triggers are displaced the same way, and one whose group a touch Toggle switches off (togon 0) is dead once that box has fired (GravTouchBox::offBits). Of the touch boxes
// only those Toggles are displaced: moving every box replaced placements GD had right (the objects a box moves are placed from the recording of the run that entered it or did not).
// --tpskipfly (on by default since 2026-10-14; --no-tpskipfly is the off arm): teleportPlayer (win 0x20fdb0) sets player+0x560 first thing, and checkCollisions (0x2137f0)
// reads and clears it at its top and skips the whole band block when it was set -- so on the tick after a teleport
// no flying mode is clamped to the band. The ground modes had it (State::tpSkip bit 1, --tpbandskip); the wave's
// and the flight loop's band clamps did not. SubZero 4003 t=10,278 / 10,322 (Worker 99): a wave teleported to
// y=159 goes on down to 157.05 under the band floor's 160 and is put back on the tick after; the model clamped at
// once and ran one tick ahead of GD's zig-zag from there.
inline bool g_tpSkipFly = true;
// --balltapexit's re-run (frames.hpp; the top of stepOne): 1 while the nested step runs with the
// ball's fresh-press taps held back, and whether the uphill launch fired in it.
inline thread_local int g_ballTapNested = 0;
inline thread_local int g_ballTapLaunch = 0;
// Ring once (was --ringonce, always on since 2026-09-26): a fired ring never fires again in the
// run, remembered for the last five rings a body fired (usedOrb, usedOrbOld and
// State::usedOrbHist) instead of two. GD refuses a ring it has fired even after other rings in
// between (a custom level t=2,548, the field's note).
// Portal once (was --portalonce, always on since 2026-09-26): a MODE portal fires only on the
// first tick the player overlaps it -- the latch the gravity portals already have (step.hpp,
// portalLatch), for the mode portals. GD
// activates a portal on its first overlap whether or not it changes anything. A custom level t=4,300: a
// cube enters cube portal uid 637 at about t=4,275 (no change), then UFO portal uid 651 at 4,299
// while still inside 637. GD stays a UFO falling at -2.991; the model fired 637 and 651 again on
// every tick inside both, halving vy twice a tick (-2.905 -> -0.748 -> -0.209). The size portals
// the same: a custom level t=16,674, a mini cube inside mini portal uid 2359 enters normal portal 2356
// and GD stays full size at y=135; the model fired 2359 and 2356 again and re-seated to 141. It
// remembers one tick of overlaps (State::portSeen), not a run-long latch, so a portal left and
// re-entered fires again.
// --portseenx: ...but a body still inside the portal's x window keeps it as seen when it leaves the
// box only in y (step.hpp, the portal pass's y gate), so leaving and coming back over the same x
// does not fire it again -- GD's mark is the object's own, per player and attempt. A custom level
// t=9,979..9,982 (dual mini cubes): player 1 takes mini portal uid 24070 at 9,979, its foot rises
// 1.1 px clear of the box on 9,980..9,981 and comes back on 9,982, the tick it also enters the
// normal portal uid 24068 beside it. GD comes out full size; the model fired 24068 and then 24070
// again and stayed mini, which put the dual solo / robot / gravity cluster at x=17,665 three ticks
// late for player 1. On by default since 2026-10-06; --no-portseenx is the off arm. A portal
// re-entered after the body's x has left its window (reverse play) still fires again.
inline bool g_portSeenX = true;
// The --start hold field (its 7th, held_before(plan, t0)); -1 when no --start was given.
inline int g_startHoldField = -1;
// Ring first (was --ringfirst, always on since 2026-09-26):
// a press that finds a ring is the ring's. pushButton fires the touched
// rings and returns; it calls updateJump only when there are none. The model runs the grounded
// jump or tap in its physics branch and the ring loop after it, so a grounded ball pressing inside
// a gravity ring flipped twice. A custom level t=11,833: a ball on the ceiling presses in gravity ring
// uid 1981; GD comes out upright at -3.1304 (the ring alone), the model tapped (flip) and then
// fired the ring (flip back) and stayed on the ceiling at +3.1304. The tap's flip and spin are
// undone when a ring fires on the tick of the press that made them.
// Ring order (was --ringorder, always on since 2026-09-26): which touched ring a press fires is
// GD's m_touchedRings order, carried in State::touchRing, instead of "in contact a tick ago beats
// not, then the lowest uid". The array
// keeps a ring's first-contact position; at a tick's head resetTouchedRings drops the rings the
// tick before did not touch, and the collision pass appends new contacts in ascending uid. One
// tick of history cannot tell apart two rings entered on different earlier ticks: a custom level t=2,741,
// a ship in contact with yellow 447 (entered ~2,735) and gravity 443 / yellow 446 (a tick later,
// one square), and GD fires 447 where the lowest-uid rule fired 443.
// Gravity pad chain (was --gravpadchain, always on since 2026-09-26):
// the gravity pads a tick enters are taken in uid order and every one
// the facing gate lets through flips. collisionCheckObjects (0x2158f9) sets gravity to the pad's
// own direction (!isFacingDown) when it differs, then propellPlayer and flipGravity, pad by pad
// in the bucket's uid order -- so two pads facing opposite ways flip twice. The model's "one flip
// per tick" (gravPadFlipped) is right only for pads that agree, which the gate already reduces to
// one. A custom level t=1,307 enters a wall of sideways blue pads (rot -90, flipY alternating = facing
// alternating); GD comes out upside down at +6.4 where the model stopped after the first flip at
// -6.4 (another custom level t=6,960 and a third custom level t=8,103 are the same wall kind).
// Pad first (was --padfirst, always on since 2026-09-26):
// a pad that fires on the tick of a grounded press leaves the press
// unspent. The pad fires in the collision pass and propellPlayer drops the ground flag; the buttons
// run after it, so updateJump finds the player airborne and does not jump, and +0x986 stays set
// for a ring touched later in the hold. The model jumped in its physics branch (spending the
// press) and let the pad overwrite the velocity. A custom level t=15,114: an upside-down cube presses on
// the tick a blue pad flips it (both give -6.4), and on the next tick GD fires yellow orb uid 1503
// with that press (11.18) while the model, its press spent, did not.
// One-way ceiling (was --onewayceil, always on since 2026-09-26):
// a one-way platform's world underside is a ceiling for the flying
// modes. collidedWithObjectInternal lets ship, UFO, wave and swing into its ceiling resolution
// unconditionally; the cube's head branch is gated (the arms), which is what the "transparent from
// below" measurement saw (lv13, a cube jumping up through type-21 blocks). A custom level t=9,220: an
// upright ship holding up into plate uid 840 (id 143, underside 136) stops at y=121 with vy 0 in
// GD, where the model flew through. It still never kills (the plate's side test stays off).
// Portal flap (was --portalflap, always on since 2026-09-26):
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
inline bool toggleScalesNow(uint8_t mode) {   // ship, UFO, wave, robot, spider
    return mode == 1 || mode == 3 || mode == 4 || mode == 5 || mode == 6;
}
// --forcecircle (default on since 2026-10): a force block 3645 with neither target mode nor range pushes like a 2069 --
// m_force along (flipY ? 180 : 0) + 90 - rot at the mode's unit, a tick after the contact -- and its
// contact is GD's circle test, gated by the collision sections. collisionCheckObjects sends 0x815 (2069)
// and 0xe3d (3645) down one branch (m_stateForce = 2, calculateForceToTarget 0x4c1ec0); the falloff there
// only runs with +0x74d (range) set. An object with m_objectRadius > 0 is met through playerCircleCollision
// (0x211df0): with the level's fixRadiusCollision, centre distance <= player width / 2 + radius * the
// larger scale; without it, the circle's centre inside the player's rect or a corner of the rect inside the
// circle. And the pass reaches the object only from the 3 x 3 cells around its centre's (--bigsection).
// A custom level t=17,650: uid 23888 (radius 15, scale 9 x 1, m_force 1.5, rot 0) under a falling cube;
// GD adds a constant +0.337 per tick from the tick the player's cell reaches the block's, at any height
// up to the circle's edge (a y sweep: 5 of 6 heights start on the same tick, the sixth two ticks later at
// 148.36 / 150.76 from the centre). The model's field started 7 ticks early and weakened with height.
inline bool g_forceCircle = true;
// --forcelive (default on since 2026-10): a constant 3645 (--forcecircle) is met where the moving-geometry recording has it on the
// tick, not where the level put it. A custom level t=21,135..21,167: uid 26764 (rot 180, scale 1 x 4, m_force 5),
// carried by a Lock to Player X Move from x 41,265 and lifted 75 px, pushes a falling cube to vy -16.125 (= the -15
// cap - 5 x 0.225) while its circle (15 + 60) holds the player's centre; the model met it at its load position, 186 px
// behind, and fell at -15, 6 px high by the landing, where it took a jump GD never had. ...and only while the recording
// has it on (modifiers.hpp forceFieldAt has the rows it reads): at t=15,447 a Toggle turns uid 12729 off as the cube it
// follows leaves its floor, and GD pushes once (vy -1.341) and then lets it fall at plain gravity.
inline bool g_forceLive = true;
// --ringpresseat (default on since 2026-10): a cube's or ball's fresh press inside ANY toggle ring (1594) that the recording has on
// goes to the ring -- pushButton hands a press to the touched rings and makes no jump of it -- including the rings the
// activator construction leaves out because their effect is not modelled (--orbspawn's ring takes the press only when
// it is taken). The held button does not jump either (holdDead, as --orbspawn has it). A custom level t=21,176: a cube
// carried on a rising block presses inside spawn ring uid 26528 (on; its Toggles reach a spike, so the model left it
// out); GD's y follows the block with vy 0 through the hold (21,176..21,177) and after, where the model jumped 11.23.
// ...and only a ring THIS body has not fired: its uid goes into the body's ring memory (usedOrbOld / usedOrbHist,
// per body through swapHalves, seeded at an anchor from --spentorb / --spentorb2 in their order; a Multi Activate
// ring never). GD marks a fired ring used per player -- activatedByPlayer (0x1a4a90) sets +0x5b4 for the player
// whose m_uniqueID is 1 and +0x5b5 for the other -- and collisionCheckObjects skips a ring
// hasBeenActivatedByPlayer (0x1a4af0) answers yes for before playerTouchedRing. A custom level's dual
// balls: toggle ring uid 13578 rides beside p2; p1 fired it at t=7,736 and p2 with a tap at 10,625 that shows
// nothing else. GD's p2 then flips on its presses inside the ring at 11,575 and 11,607; with the 10,625 tap taken
// out of the plan, GD's p2 press at 11,575 goes to the ring and the flip comes a tick late.
// The cost: a toggle ring takes a place in the same five-deep memory as the rings proper, so on a level that fires
// toggle rings the oldest ring proper is forgotten one fire sooner. A ring firing twice there is the first thing to
// suspect when this change turns a replay from passing to failing.
inline bool g_ringPressEat = true;
// --spawnringon (default on since 2026-10): the toggle rings (1594) take presses the way the recording says GD's did.
// pushButton hands a press to the rings the previous tick's collisions touched, and a fired ring's Toggles switch
// the next ring on after that, in the row the fire is recorded in (the press step's own K.t). So:
//  - a ring the recording has OFF on row K.t-1 takes no press, as a spawn ring (--orbspawn, --acthazoff) or under
//    --ringpresseat (which read its "on" from row K.t+1);
//  - a spawn ring takes a held press (markTouched) only with the button down on this step and the press not spent
//    (it took the parent's button, a tick after a release, and every overlapping box at once);
//  - its jump (--spawnringjump) is the held press's own, a tick late: given only to a cube that pressed from the
//    ground, and not while another ring the recording has on in the fire's row holds the body.
// A custom level x 41,355 .. 41,520: three rings stacked per spot, A ("A off, B on"), B ("B off, C on") and C (opens
// the spike wall at x 41,685). GD's cube fires A on the press at 21,165 in the air (row 21,166: A off, B on) and B on
// the press at 21,175 on a rising block (row 21,176: B off, C on), never leaves its line, presses no more and dies on
// the wall at 21,225. The model fired all three boxes when it entered them at 21,162, a tick after the release, then C
// on the second press, jumped both times and lived. With the flag the anchored replay agrees with GD on every column
// to the death.
inline bool g_spawnRingOn = true;
// --acthazoff (default on since 2026-10): a spawn ring (--orbspawn) whose spawned Toggles switch hazards OFF is taken as an activator;
// activatorTargetSafe refuses a hazard because switching one ON can kill later, which a Toggle with togon 0 cannot do.
// A custom level x 41,355 .. 41,520: three stacked toggle rings per spot, each press turning the next one on; the third,
// uid 26528, spawns group 317, whose Toggle uid 26957 switches group 340 -- the spike wall at x 41,685 (uids 26960 ..
// 26965) -- off. Left out, the ring could not open the wall, and every search from anchors back to t=19,724 ended on it
// (t=21,224, "doomed") or took the fall route down to the floor.
inline bool g_actHazOff = true;
// --actlookchain (off): a touch toggle block (1594) whose target group holds triggers is not left out ("a chain this does not follow")
// when every trigger it reaches is look-only (a Spawn whose group holds Alpha/Pulse/colour triggers, say), which cannot change what is
// touched. A custom level's block uid 8770 (x 2,714, 50.9 square) toggles group 197, the 129-object platform at x 2,775..2,895; the group
// holds one Spawn (uid 34174) that spawns group 556, two Alpha triggers. Left out, the model read the platform's on/off from the
// recording of an attempt that never entered the box (off), where GD's low route does enter it and rides under the platform's underside:
// t=1,744 GD stands on the underside at y 254.752, the model rises on to a saw at t=1,771.
inline bool g_actLookChain = true;   // on by default (release flags, 2026-10-17); --no-actlookchain is the off arm
// Ground-grow (was --groundgrow, always on since 2026-09-26):
// a cube or ball that a size portal grows while it stands on the WORLD
// ground is lifted to the new seat on the NEXT tick, by the ground clamp, not on the portal's own
// tick. GD's ground plane is enforced in PlayerObject::update, before checkCollisions fires the
// portal, so the grown body spends one tick embedded; a block is resolved in checkCollisions and
// can re-seat it the same tick (lv12 t=3,286, which stays). Old custom level t=17,939: a
// mini ball on the ground (y=99) meets RegularSize portal uid 2257 -- GD reads vsize 1 at y=99
// and 105 only at t=17,940, where the model went to 105 at once. The calibration rig's reading
// (cube next tick, ball and UFO the same tick; the retracted r69 note at the re-seat) is left
// alone for the flying modes, which have their own floor clamp.
// Ring press pin (was --ringpresspin, always on since 2026-09-26):
// a ring taken off a block by a FRESH press leaves y on the block. GD
// reaches a ring by two paths: a button already down fires it from the collision pass
// (playerTouchedRing), before the solid pass puts the body back on the block, so y keeps that
// tick's gravity step; a press fires it from pushButton in the buttons phase, after the solid pass
// has re-seated it. The model released the block pin for every ring. Old custom level
// t=2,126: a mini cube on block uid 204 taps (press line 2,125) inside yellow orb uid 200; GD
// leaves at y=150.000 with 8.944, the model at 149.951. The orb evidence at the release (lv19
// t=254/506, lv16 t=6,404, lv18 t=10,824) has no press at those ticks in today's plans.
// Solid order (was --solidorder, always on since 2026-09-26):
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
// Ball ceiling (was --ballceil, always on since 2026-09-26):
// an upright ball whose head is past a solid's underside, having been no
// more than the landing tolerance past it at the start of the tick and not falling, is put under
// the face with vy 0 -- one-way plates included. collidedWithObjectInternal lets the ball into its
// ceiling resolution unconditionally (the cube's head branch is the gated one), which the
// ground-mode loop had only for a ball entering from the side (lv9 t=18,354). The same custom level t=9,235
// (hbox lines): after slab 4959 lands the ball at y=240, one-way plate 4956 (underside 240,
// 9 px into the head, x overlap 0.28) puts it at y=231; the model passed through the plate.
// Takes GD's solid order (--solidorder) to put the landing before the push.
// The boundary, from injections under normal slab uid2511 (underside 330) in the same level, a mini
// ball at x=6,225: rising at vy +2, a head 9.9 px past the underside at the start of the tick is
// put at y=321 with vy 0 and onGround 1 (and falls from the next tick); 10.1 dies. Falling at vy -1
// it is never pushed -- the head sits 9.4 px inside and the ball falls on, alive -- and 10.3 dies.
// So the tolerance is kLandTol on the tick's start, the gate is "not falling", and the push grounds
// the ball. The falling case's survival is --faceclass's head side.
// Flipped ball ceiling (was --ballceilflip, always on since 2026-09-26):
// --ballceil's push on a flipped ball, whose head side is a solid's
// top: head past the top, no more than kLandTol past it at the start of the tick, not falling (in
// its own gravity) -> y on the top, vy 0, and NOT grounded -- GD's onGround stays 0 on this side.
// Old custom level t=7,686: a gravity portal flips a ball falling at -10.6 (7,683); it
// decelerates to -5.1 and its head reaches block 2274's top (360); GD's row is y=375, vy 0,
// onGround 0, then +0.129 a tick. The model went on into the block.
// Snap past a one-way (was --snaponeway, always on since 2026-09-26):
// the stair snap's nudge is blocked by a one-way (breakable, type 21)
// PREVIOUS snap object only, not by a one-way landing target. checkSnapJumpToObject (0x393cb0)
// skips the nudge when the stored object (+0x888) is missing, is the same uid, or its object type
// (vtable+0x660) is not 0 -- the new object's type is never read. The counted "0 moved / 108 still"
// on calls with a type 21 in them is consistent (lv13 t=2,142 had both one-ways). Old custom
// level t=12,161: a flipped cube snapped on solid 4136 lands on breakable 4127's underside
// (dx 60, dy -60 = the slow `big` stair, mirrored); GD moves x by +1.0 (snapdist 1.2129).
// Flight re-land (was --flyreland, always on since 2026-09-26):
// in the flight loop, once a ceiling has pushed the player down this tick,
// a floor may land it even though it is already grounded. GD resolves each solid on its own; the
// model's gate `!c.grounded` let the ceiling have the last word. Old custom level t=3,536
// (hbox lines), a mini ship squeezed between floors 998/969 (top 224) and ceiling 996 (underside
// 240): 998 lands it at y=233, 996 pushes it down to 231, and 969 lands it at 233 again, every tick,
// so GD stays at 233 on the floor. The model ended on the ceiling at 231. Takes --solidorder for
// GD's order. Opening the gate without the ceiling condition cost lv19 t=19,801 (a grounded UFO
// re-landed on a face 0.002 px off GD's), so the plain case keeps it.
// Pad then land (was --padthenland, always on since 2026-09-26):
// pads that fire on a landing tick, leave gravity where it was and leave
// vy pointing along it do not undo the landing; the player stays on the face with vy 0.
// checkCollisions runs the activation pass before the solid pass (releasePin's note), so the solid
// pass lands the player on the pads' vy. The model runs the pads last and undid the landing for
// any pad. Old custom level t=8,103 (fg and hbox lines): two gravity pads flip the cube
// and back (vy +6.4, then -6.4), then floors 2871 and 2870 land it at y=225 with vy 0. The model
// left it at vy -6.4 for a tick.
// Face class (was --faceclass, always on since 2026-09-26):
// a solid whose face the player's foot (in its own gravity) is inside by
// no more than the landing tolerance -- kShipLandTol for the ship, kLandTol for cube and ball -- is
// a ground contact. Neither the ship's side kill nor the crush applies to it. GD classifies a
// contact by the face before the inner box decides anything. Measured on an old custom level by
// injecting a rising player (vy +2, centre over the block, x fixed) above a top face:
//   full ship  over uid441:  dies at centre 4.487 above the face, lives at 4.507  (inner box 4.5)
//   mini ship  over uid997:  dies at 2.993, lives at 3.003 -- depth 9 - 3.0 = 6.0 = kShipLandTol
//   full cube  over uid4491, full ball over uid3860: die at 3.8, live at 4.8   (inner box 4.5)
//   mini cube  over uid1570, mini ball over uid3980: live at 1.601 and 1.621 (depth 7.4 < kLandTol)
// With the full size the inner box never reaches a face the foot is within tolerance of (15 - 10 =
// 5 > 4.5, 15 - 6 = 9); with the mini size it does, so only the mini bodies see the rule. The crush
// rig's mini cube (kCrushHalf) was measured on the HEAD side, under a ceiling, which this leaves
// alone. A custom level t=3,603: a mini ship rising past uid997's corner, 5.96 into the top face; GD's hbox
// is hit=0 and the model died on fly/solid-side, then on crush.
// The BALL's head side is a face too (it is on GD's unconditional ceiling list): a mini ball
// falling at vy -1 under slab uid2511 of a custom level lives with its head 9.45 px past the underside and
// dies at 10.05 (--ballceil's note). The cube's head side is not (the crush rig).
// Cube, ship and ball only: the wave dies 1.5 px from a face (lv17 t=7,040, lv20 t=839), well
// inside the tolerance, and the UFO (which lands even while rising), robot, spider and swing are
// unmeasured.
// Portal ceiling (was --portalceil, always on since 2026-09-26):
// when a mode portal turns a cube, ball, robot or spider into a flying
// body, the new body meets the ceilings the same tick. It uses the flight loop's ceiling-ride test
// (head no more than kShipLandTol past the underside at the start of the tick, past it now, not
// falling): y goes under the face and vy to 0. GD's activation pass switches the mode before its
// solid pass; the model resolves the solids with the old body and runs the portals afterwards.
// Old custom level t=8,382 (dhh and hbox lines): a cube jumping at 10.99 enters a ship
// portal with its head 7.4 into ceiling 2988 (4.99 at the start of the tick). GD's didHitHead puts
// the ship at y=225, vy 0. The model kept the halved 5.39 and rose inside the ceiling.
// Flip grace (was --flipgrace, always on since 2026-09-26):
// 004b's grace, re-landed narrow. Within kFlipGraceTicks of a gravity flip
// a cube, ball, robot or spider whose inner box would kill it on a static solid (the side test or
// the crush) is put on that solid's face instead -- its head-side face, vy 0 -- provided GD
// classifies the contact as head side (collidedWithObjectInternal: the foot moved 10 px toward the
// head, now or at the previous position, reaches no further than the solid's far edge;
// flipGraceSeat). The withdrawn 004b gate, and this flag until 2026-09-25, used the static proxy
// "centre outside the solid's y span", which also spares side entries GD kills (a custom level t=19,794).
// It is limited to static solids in frame 0: the classifier also widens by the surface's speed.
// Old custom level t=15,121: a blue pad flips the cube upright at 15,114, a yellow orb sends
// it up into the underside of the block at y 300, and on the tick its inner box crosses GD puts it
// at y=285 (head on the face) with vy 0; the model died there. The lv18 probe behind 004b (a block's
// top face, flip age 0/12/24 lives, 25/30/36 dies) is the same window.
// --flipgracedyn: the flip grace for a MOVING solid as well. collidedWithObjectInternal classifies the
// contact the same way for any object -- the foot plus the reach (10) against the solid's MinY, at the
// current and the previous player position, on the solid's current rect -- and widens the reach by the
// solid's speed only when it moved toward the body's head (upright: up); a solid moving the other way,
// or not at all, is classified as a static one. A custom level t=5,296: a blue pad flips a mini cube
// upright at 5,294, and block uid 9160 (30 x 30, coming down 4.69 px a tick to its rest at 945) meets
// its head two ticks later; GD seats it at 921.000 (= the underside 930 - 9) with vy 0 and onGround 1,
// and it falls away from 5,297. The model, whose grace took static solids only, killed it as a crush.
// A solid moving toward the head keeps the old behaviour (its widened reach is not modelled).
// On by default; --no-flipgracedyn is the off arm.
inline bool g_flipGraceDyn = true;
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
// The maps are indexed by uid, one per body (g_halfNow picks the second body's during its half).
inline bool g_spentGlobal = true;
inline std::vector<uint8_t> g_spentMap1, g_spentMap2;
inline void markSpentGlobal(std::vector<uint8_t>& m, int uid) {
    if (uid < 0 || uid > 4000000) return;
    if ((size_t)uid >= m.size()) m.resize((size_t)uid + 1, 0);
    m[(size_t)uid] = 1;
}
// --histstat <file> (print only): after a replay, one line appended to <file> with how full the
// fixed-length histories got and how often one dropped an entry: touchRing
// (4 slots, --ringorder), portSeen (3, --portalonce), usedPad (4) and the fired-ring memory
// (usedOrb + usedOrbOld + usedOrbHist = 5, --ringonce); plus how many ring fires in the replay were
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
inline thread_local std::vector<int> g_histPads;
inline double pinkRingRatio(int mode) {
    return mode == 2 ? 0.77 : mode == 1 ? 0.37 : mode == 3 ? 0.42 : 0.72;
}
inline double rawGravStep(bool ballLike, bool robot, double dx) {
    const double mul = speedMulForDx(dx);
    const double g = mul < 0.8 ? 0.940199 : mul < 1.0 ? 0.958199024 : mul < 1.2 ? 0.957199 : 0.961199;
    if (ballLike) return -0.958199024 * 0.6 * 0.225;
    return -(robot ? 0.9 : 1.0) * g * 0.225;
}
// --offboard <margin> (EXPERIMENT): the repair loop's own definition of a run that has left the
// playfield (repair.hpp offBoardTick: world y more than `margin` outside the recorded band at that
// tick), applied in the search. GD does not kill there; the loop credits the death there instead.
// Without it the search keeps offering arcs through the sky that the loop scores as dead on
// arrival -- lv22 t~2677, 70 rounds with nothing learnt (2026-09-19). 0 = off.
inline double g_offBoardMargin = 0.0;
inline int g_shiftDbgUid = -1;       // --shiftdbg <uid>
inline bool g_shiftDbgDone = false;
// --ceilfarbox (default on; --no-ceilfarbox is the off arm): a flipped cube arriving fast over the far end of a ceiling ramp is taken by it (step.hpp).
inline bool g_ceilFarBox = true;
// Always on (was --landjumpshrink): a cube that lands on the tick it presses jumps with the size it has AFTER that tick's portals. GD
// resolves every object of the collision pass (the solid's landing and a size portal alike) before the buttons phase, whose jump
// reads the scale then; the model wrote the landing's jump at the solid, before the mini portal had been reached, at the full
// 11.18 where GD leaves at 8.944 (the official level 11 at t=15519).
// Always on (was --ufoheldflap): a cube that holds a press nothing spent (it pressed in the air, a tick before the UFO portal, with
// no ground to jump from) is flapped by the UFO on the portal's next tick, as a fresh press on the portal tick is. calib_portalpress:
// cube -> UFO, airborne, press at -1 / 0: the UFO flaps 6.871. A custom level at t=19973 (x=37281.9): the press came with the gravity portal at 19972,
// the UFO portal fired at 19973 and GD's row at 19974 reads vy +6.871 where the model went on at 2.227.
// Always on (was --tpgravhalf): a gravity-switching 747 teleport that carries no force halves the body's vy when it changes gravity, as every other
// gravity switch does (PlayerObject::flipGravity, 0x39a2d4: m_yVelocity *= 0.5). The 747 branch assigned its exit vy (18) or kept the entry vy
// and only the grounded cube halved. A custom level at t=6139 (x=11369.7): a flipped mini cube, vy 1.836, takes a 747 (tpg 1, no force) to y=595 and turns
// upright; GD's row reads vy 1.026 = (1.836 + 0.216) / 2, the model 2.052.
// --waveflipcap (default on; --no-waveflipcap is the off arm): a FLIPPED wave under a PLAIN ceiling ramp is the upright wave's outline -- the perpendicular distance ph, cut off at the box's edge (objMinY - ph) -- not
// the spiked ramp's 5.845 cut off at the line's low end (the rigs measured the 5.845 on spiked units only). A custom level's wave corridor of down-pointing V ceilings (plain ramps):
// GD kills the flipped wave at (4495.04, 298.54), 4.54 px from the descending ramp's line and 1.46 below its tip (line 300), and spares it at 5.47; the model's tip cap let the first
// through a dozen times over, and 5.845 would kill the second.
inline bool g_waveFlipCap = true;
// --ballseatground (default on; --no-ballseatground is the off arm): a ball that the slope law seated on a floor ramp's extrapolated end counts as grounded on the next tick, so a press taps it (see step.hpp, the support scan). A custom level
// t=7,544: a mini ball seated 7.8 px short of a ramp (y 186.18, onGround 1, vy 0) is tapped on the next tick by GD (up 0 -> 1, vy +2.908, onGround 0); the model dropped the press and rode the seat.
inline bool g_ballSeatGround = true;
// --ridesupgrace (default on; --no-ridesupgrace is the off arm): the ramp support window of the next tick's press runs past the ramp's far end by ONE TICK'S MOVEMENT, not a fixed 1.5 px -- the same "r_new <= x1 + dx <=> r_prev <= x1" the ride's own
// window uses (step.hpp, rideGrace). A custom level t=2,756: a flipped cube hangs under a descending ceiling ramp (x1 = 3840, dx 1.614), its contact point 3841.575 on the press tick, 0.04 inside x1 + dx and 2.07
// outside x1 + 1.5; GD jumps on that tick (vy -12.508 = the jump and the ramp's push), the model found no ground and jumped a tick later and a tick of the ramp's growing push stronger.
inline bool g_rideSupGrace = true;
// --robotslopenohover (default on; --no-robotslopenohover is the off arm): a robot's jump taken while the previous tick's ramp contact is alive (State::onSlope, the jump that carries the ramp's push) arms no hover budget. A custom level t=5,080: the
// robot rides a ramp to its top end and jumps from the block beyond it, vy 4.972 (the bare 4.568 + the ramp's 0.404) and then loses 0.194 a tick from the first tick, button held for 15; its jump from flat ground
// at t=4,944 (vy 4.568) hovered for 7 ticks as the model's does.
inline bool g_robotSlopeNoHover = true;
// --waveflipcapfloor (default on; --no-waveflipcapfloor is the off arm): --waveflipcap's capped outline for a flipped wave on a PLAIN FLOOR ramp too (the cap objMaxY + ph - inset, not the line's top). A custom level's flipped wave
// 0.7 px inside the high end of a descending plain ramp (line 601.2, top 601.9): GD kills it down from y 606.1, the line-top cap let it live at 604.7 (bisected by injection: dead at 605.6, alive at 606.6).
inline bool g_waveFlipCapFloor = true;
// --waveflipceilup (default on; --no-waveflipceilup is the off arm): a flipped wave under a plain CEILING ramp that climbs in the travel is killed 1.0 px (vertically) further out, as an upright wave on a floor ramp that descends is
// (the mirror image). A custom level's flipped wave rising under a plain ascending ceiling ramp: injection bracketed the kill at y 628.15 alive / 628.4 dead, 8.13 px under the line (the 7.07 of the
// perpendicular 5.0 plus 1.0); under a descending one the same gap, 7.74, lives.
inline bool g_waveFlipCeilUp = true;
// --rampseatall (default on; --no-rampseatall is the off arm): the same-tick ramp seat of a growing mode/size portal (step.hpp, "rampseat") goes on through the ramps after the first it seats the body on, as GD's one object
// loop does: each ramp whose uid is above the portal's pushes the grown box out in turn, and the window is the lowest point's (the centre may be 3 px short of the span). A custom level t=12,417
// (mini wave -> ball, three ramps of a moving group stacked 6 px apart): GD ends on the highest seat the body still penetrates, 3651.295, the model stopped at the first ramp's, 3639.0.
inline bool g_rampSeatAll = true;
// --portalobbmode (default on; --no-portalobbmode is the off arm): --portalobb's GD box (obb.txt) also for the portals that change the MODE (ball, UFO, wave, robot, spider, swing), whose oriented extents
// are otherwise rebuilt from the bound by a uniform-scale inversion. A mode portal stretched along one axis only (34 x 288.5 at rot 45) came out 45.7 x 115.6 wide, 12.7 px
// off at the end, and the model took it a tick away from GD (t=6085, t=7442).
inline bool g_portalObbMode = true;


// Always on (was --shrinksnap): on the tick a cube turns mini the stair snap is taken by the FIRST solid of the contact order only (step.hpp).
// The later solids of the pass meet the mini box, which no longer reaches them, so they do not become m_objectSnappedTo. Over GD's run of
// the official level 11 the model's order (descending section x, section y, uid; last wins) gives GD's snap object on 24 of the 26 ticks
// that touch two solids -- and the two it misses are both mini-portal ticks (t=3384 and t=15519), where GD's is the first of them.
// Always on (was --ceilfarfree; with --ceilfarbox): the far-box acquisition asks what GD's own test asks (slopeWouldAcquire: the seat target
// short of the FREE y -- the y this tick's move gives the body, a solid already counted), not the y the body had last tick. Without it
// a flipped cube a long way under a ceiling ramp's far end (30 px below its seat, still rising at vy 15) was seated on the ramp.
// Always on (was --ringhalfstep): the late ring loop meets a TURNED ring, from the previous tick's end (--ringrotground), at the midpoint of the
// previous and this tick's positions instead of at the previous end. GD's own sweep on a custom level (a mini cube, 2.4 px a tick, a
// dash ring turned 9 deg): the press fires it up to |dx| 25.60 of the effect tick's row and not from 25.70 -- 24.445 (ring half 15.445 + 9) of
// the row less half a step, where the model's box at the previous end fired it at 25.83.
// ...and not on a tick the body went through a teleport portal (stepOne's teleportedThisTick): the midpoint of the previous end and this tick's place is then a place the body never was. the long custom level t=26,201:
// a cube carried from y 189.5 to 295 on the tick of a press had a turned green orb (rot -33, 49.8 wide, 40 px above-right of the new place) tested at the midpoint y 242, 13 px away, and fired it; GD did not
// (vy -8.676 = the gravity step alone, the model's -8.496), the orb being 40.0 from the new place against a reach of 39.9.
// Always on (was --flipgracehead): the flip-grace seat (--flipgrace, a solid met within 25 ticks of a gravity flip) grounds the body only when
// it was moving ALONG its gravity (a landing). Against it the contact is the head's, GD clamps y and vy and leaves onGround 0. All 62 flipgrace
// seats of the kitref episodes where the model has GD's y and vy, and a long custom level's: moving against gravity -> GD og 0 (spider 13, cube 11), along
// it -> og 1 (ball 6, cube 3). The model's grounded 1 let --lingerhold jump a tick later (a custom level, t=30590, GD does not).

}  // namespace dp
