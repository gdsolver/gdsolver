#pragma once
#include "dp/modifiers.hpp"

namespace dp {

constexpr double kFloorY = 105.0;   // cube rest height on the ground line
// ...which is a REST HEIGHT, not the ground. The ground plane is at 90 and the
// rest height is 90 + the player's half, so a mini player rests at 99, not 105.
// Measured on lv11 t=9793: an inverted mini ship descending at vy = -3.424 is
// stopped dead at y = 99.000 and held there. The code below therefore uses
// kGroundY + pHalf everywhere it used to use kFloorY.
constexpr double kGroundY = kFloorY - 15.0;   // 90.0
constexpr double kCubeHalf = 15.0;  // outer box (landing)
// Mini. Measured on lv11: the cube crosses the mini portal (rect x[4409.5,
// 4440.5], so the boxes touch at x = 4394.5) at t=3384 and immediately starts
// falling from its resting y=165, then settles at **y=159.000** on the same
// continuous platform whose top is 150. 159 - 150 = 9, i.e. the half size goes
// 15 -> 9 (0.6x). Without this the model kept standing at 165 on a surface GD
// had already dropped away from.
constexpr double kMiniHalf = 9.0;
constexpr double kMiniScale = 0.6;
// Mini jumps LOWER. Measured on lv11 t=3528: same tick, same x, both grounded
// at y=159, GD leaves with vy = 8.9440 and the model left with 11.1800.
// 8.944 / 11.18 = 0.800 exactly. Gravity is NOT scaled -- both sides step
// -0.2160 per tick from there, and the traces stay exactly 2.2360 apart in vy
// until the landing. Without this the model floats 40 px above GD through the
// whole mini section and every hazard test after it is answered for the wrong
// player.
// UNVERIFIED for mini: terminal velocity, and the orb/pad impulses. Measure
// them the same way when a plan first depends on one.
constexpr double kCubeJumpMini = 8.944;
// ...and the same 0.800 applies to the launch impulses. Measured by injecting
// the mini cube onto lv11's yellow pad (x[7888.5,7913.5], t=6076): GD returned
// vy = 12.800, and the full-size pad is 16.0 -- 12.800 / 16.0 = 0.800 exactly,
// the same factor the jump showed. Two independent measurements agreeing to
// four digits is a rule, so it is applied to the orbs too.
// NOTE the orb side is INFERRED from those two, not measured directly. lv11's
// yellow orb at (8055,255) is the first plan that will depend on it: if the
// factor is wrong there, the GD replay diverges at that orb and says so.
// (This is separate from the per-MODE ratios -- ball is 0.600/0.700 -- which
// really do differ per constant and must stay measured one by one.)
constexpr double kMiniImpulse = 0.800;
constexpr double kCubeInner = 5.0;  // side/ceiling kill box
// ---- ROBOT (mode 5) -------------------------------------------------------
// Read out of PlayerObject::updateJump (win RVA 0x38b900), not fitted. The
// robot is a cube with three differences, all of which are literal branches in
// that function:
//
//  1. GRAVITY x0.9. updateJump picks a per-mode scale (xmm7) right after the
//     mode flags: ball 0.6, spider 0.6, ROBOT 0.9, everything else 1.0, and it
//     multiplies the `dt * gravity` term that is subtracted from m_yVelocity.
//     It is NOT applied to the jump.
//  2. JUMP x0.5. `if (m_isRobot) jumpHeight *= 0.5` sits immediately after the
//     jump height is read (m_gravity's sibling double at this+0x7c0), before
//     the mini factor. So a robot tap leaves at half a cube's jump for the
//     same speed, and mini still multiplies by 0.800 on top.
//  3. HOLD = HOVER, not thrust. While the button stays held after the jump,
//     updateJump ADDS exactly the same `sign * dt * gravity * 0.9` it is about
//     to subtract, so vy is constant -- the robot floats. The budget is a
//     double at this+0x830 that is zeroed when the jump fires and grows by
//     `dt * 0.1` per tick while the hover runs; the hover stops once it
//     reaches 1.5. With dt = kCubeG / 0.9581990 = 0.225422 (the same dt that
//     makes the cube's gravity -0.216) that is 1.5 / (0.1 * 0.225422) = 66.5,
//     i.e. 67 ticks.
//     Releasing the button sets a flag (this+0x99c, cleared only by the next
//     jump), so a release ENDS the hover permanently -- re-pressing in mid-air
//     does not resume it.
//
// Also from the same function: the robot's jump needs m_jumpBuffered as well as
// the hold, i.e. a fresh press. Holding through a landing does NOT re-jump.
// The model's cube already tests the rising edge (`input && !s.action`), so
// that comes for free.
constexpr double kRobotGScale = 0.9;
constexpr double kRobotJumpScale = 0.5;
constexpr int kRobotHoverTicks = 67;
// ---- SPIDER (mode 6) ------------------------------------------------------
// Same shape as the robot: the mode table in updateJump gives it the BALL's
// gravity scale (0.6), and its action is not a jump at all -- it is
// PlayerObject::spiderTestJump, a teleport to the surface on the other side.
//
// Measured on lv22 (injected onto the spider section at x~1,010, full size):
//   gravity  -0.129 / tick, i.e. 0.216 x 0.600 -- the ball's number to three
//            digits, on 13 consecutive airborne ticks
//   rest     y = 163.500 on a floor whose top is 150, and y = 286.500 under a
//            ceiling whose underside is 300. Both are 13.5 off the surface,
//            not the 15 every other ground mode uses.
//   teleport t=262 y=163.5 upright -> t=263 y=286.5 upsideDown, one tick, and
//            the gravity is flipped. The target was the block at (1035,315),
//            whose x range starts 9.5 px to the RIGHT of the player, so the
//            search is at least as wide as the player's own box.
//   gate     a tap in MID-AIR does nothing (13 ticks of undisturbed free fall
//            after one). GD only calls spiderTestJump from the grounded branch
//            of updateJump, same as the cube's jump.
// A/B switches for bisecting a regression. Not knobs: both default to OFF and
// exist so that "which of today's changes broke lv18" can be answered by two
// runs instead of two rebuilds.
inline bool g_noMiniWave = false;   // --no-miniwave
inline bool g_oldLatency = false;   // --old-latency
// --old-slope: A/B escape hatch (same convention as --old-latency). Restores
// the pre-2026-08-04 slope exit: ball = tap-anchored line / mini x0.625, and
// NO ride-time ramp. See slopeExitVy / slopeRampFactor for why the new form
// is the disassembled truth.
inline bool g_oldSlope = false;
// A jump taken while still ON a ramp adds this FRACTION of the ramp's own exit
// velocity (slopeExitVy's cube row), on top of the mode's jump. Measured on the
// `rampjump` calibration map -- see the jump branch for the 12-unit table.
constexpr double kSlopeJumpBonus = 0.25;
// The bonus is capped at 0.4 x that mode's bare jump value (rampjump rig 12/12,
// measured in the note at the jump branch).
constexpr double kSlopeJumpBonusCap = 0.4;
constexpr double kSpiderGScale = 0.6;
constexpr double kSpiderHalf = 13.5;
constexpr double kSpiderHalfMini = 8.1;   // 13.5 * 0.6, UNVERIFIED
// ...and the teleport SEARCH is 1.0 px wider in x than that. The lv22 note above
// could only say "at least as wide as the player's own box"; lv21 pins it.
// Measured 2026-08-10 on lv21's flipped spider resting at y=406, tapping next to
// the block (11791,375) whose top is 390 (so the near target is y=403.5 and the
// far one is y=223.5, 180 px apart -- the level's whole route turns on which).
// Seven injections of the player's x through the MCP, reading GD's landing:
//   |x-cx| = 28.799  29.299  29.499 -> 403.5 (near)
//   |x-cx| = 29.510  29.650  29.799  29.999 -> 223.5 (far)
// so the boundary is 29.5 to within 0.011 px, i.e. hw + 14.5 for a 30-wide
// block. The model was using hw + kSpiderHalf = 28.5 and took the far target
// on every tick in the 28.5..29.5 band.
// NOT SEPARABLE from this one site: "the spider's x half is 14.5" and "the box
// is 15 wide and GD wants more than 0.5 px of overlap" are the same formula
// (hw + 14.5) for every object. Only the vertical half is independently pinned
// at 13.5, by the rest positions (403.5 = 390 + 13.5, 223.5 = 210 + 13.5).
// Applied to the teleport search ONLY, which is where it was measured. The
// support test a few lines up has the same shape and may well want the same
// number, but that is a second variable and lv1-20 cannot referee it (nothing
// before lv21 has a spider).
constexpr double kSpiderSearchHalfX = 14.5;
constexpr double kSpiderSearchHalfXMini = 8.7;   // 14.5 * 0.6, UNVERIFIED
// The wave has TWO different sizes, and they are not the same number.
//
// The BAND clamp keeps its centre 10 off the surface: measured on lv17 t=3565,
// it comes down the diagonal and stops dead at y=100.000 (vy=0, onGround=1)
// on a band whose floor is 90, then slides for 30+ ticks. That 10 is the
// clamp's own constant (the same slot that holds 15 for everyone else), not a
// hitbox.
//
// Its HITBOX is far smaller -- about 5. Two independent brackets on lv17:
//   * the ball portal at (11055,197) w=34 fires at x=11033.47, and the tick
//     before is 11032.17, so half is in [4.53, 5.83)
//   * the ship portal at (8865,195) h=86 spans y=[152,238] and the wave rides
//     past it at y=140.25/141.54 WITHOUT switching, so half < 10.46
// 5.0 is the only round value in both. This half is what portal, pad and orb
// contact use as well -- with 15 the model entered that ship portal four ticks
// early and came out in the wrong mode while GD was still a wave.
// The mini values carry the established 0.600 ratio and are UNVERIFIED.
// CONFIRMED against GD 2026-08-06: injecting the player at y=600 in lv20's wave
// section and reading where GD puts it back gives y=380.000 at three x well
// apart (4,550 / 5,121 / 5,199). The band there is the x=867 wave portal's
// (cy=239 -> floor 90, ceil 390) -- the second wave portal at x=4,445 is INERT
// because the player is already a wave, so it never rewrites the band. 390-10
// = 380 exactly. 10 it is; pHalf (5) would give 385 and 15*vsize would give 375.
constexpr double kWaveClamp = 10.0;
constexpr double kWaveClampMini = 6.0;
constexpr double kWaveHalf = 5.0;
// ...and the MINI one is NOT 0.600 of it. That carry-over was flagged
// UNVERIFIED above and this session refuted the same 0.600 twice already (the
// kill box does not scale at all, the ramp box scales by ~0.70). GD brackets it
// on lv20's 1.1x speed portal uid4303 (rot 41, real box 51 x 56 -- obb and
// w0,h0 agree, so the oriented shape is not in question). Writing the fire test
// on the portal's own long axis, the threshold is `25.5 + 1.41077 * half`:
//   x=6128.008 y=298.316  |lx| = 28.979  GD does NOT fire  -> half <  2.466
//   x=6131.903 y=300.912  |lx| = 27.743  GD DOES fire      -> half >= 1.590
// (both read off a plain replay's dump, the ticks GD's `speed` column steps
// from 0.9 to 1.1; the model and GD are bit-identical up to the first of them).
// 3.0 fires three ticks early, and after that the run carries 0.316 px of extra
// x per tick for the rest of the level -- the `dy is 0 and only dx grows`
// signature. 2.0 sits in the middle of the bracket; it is BRACKETED, not
// measured, so tighten it before deriving anything from it.
constexpr double kWaveHalfMini = 2.0;
// ...and the HAZARD stage-1 box is a third one again: GD tests the AABB of
// `player->getObjectRect()`, which cfg `hitboxtrace=1` reads as 10x10 full and
// **6x6 mini** -- half 5.0 and 3.0. The 2.0 above is a bracket off a speed
// portal's firing, a different code path, and it is 1 px too small here.
// Bisected live in GD on both faces, two levels, two speeds; see the table at
// `wHazHalf` in stepOne. 3.000 exactly, touching counts, not speed-dependent.
constexpr double kWaveHazHalfMini = 3.0;
// ...and the PORTAL contact box is that same 6x6 rect, so it is 3.0 too.
// Kept as its own name because the two are measured separately and the SPEED
// portal still wants the 2.0 bracket above (see the portal loop in stepOne).
constexpr double kWaveContactHalfMini = 3.0;
// [2026-08-19 night 3] **The mini's portal contact half-width is smaller than the
// collision's 9.** Lining up the ramps calibration rig tick by tick between GD and
// the model (py/calib_diff.py), **only the mini->normal size portal has the model
// 1 tick early, 8/8** (norm->mini is the same tick 8/8, i.e. the normal size's 15
// is right). Bracket: against portal id99 (cx=4470, w=31 -> hw 15.5)
//   GD does not fire at |x-cx| = 23.342  -> half < 7.842
//   GD fires at        |x-cx| = 22.044  -> half >= 6.544
// Same family as the wave's mini being 3.0 (6x6 box). 7.5 is 12.5x0.6, the middle
// of the bracket. **All 8 units share the same geometry, so it cannot be narrowed
// further** -- tighten it with a rig that varies the phase (shifts the portal x).
constexpr double kMiniContactHalf = 7.5;
// ...and SQUARE. The `portwave` rig (2026-08-18) read b ~= 11.5 on the y side
// at portal rots 20/43 and the tall box was implemented and REVERTED the next
// day: the rig put every portal centre at GROUND_TOP+15 while the zero-input
// wave slides at ~+6, and that constant dy of ~-9 enters the SAT's u-axis
// inequality with exactly the |sin| weight the y half has -- (a,b)=(3,11.5)
// at dy=0 and (3,3) at dy=-9 predict the SAME brackets at both angles, so the
// rig cannot tell them apart. A rig for b must VARY the portal heights.
// The square box is the one with in-level evidence: lv20's rot-53 stack
// (uid527, census t0=600) rejects at |ly|=50.89 (t=849) and fires at 49.07
// (t=850) against ohh 45 -- a y half of 2.77..5.80, which is 3.0 and rules
// out 11.5 (that fires at t=846, and the census following shrank 400->247).
// It also agrees with GD's own getObjectRect (6x6 mini).
// ...and the box that KILLS the wave on a solid is a different, much smaller
// one. The 5.0/3.0 above are the CONTACT half (portal, pad, orb) and match
// GD's own getObjectRect for the wave exactly -- 10x10 full, 6x6 mini
// (cfg `hitboxtrace=1`, lv20 t=645/t=654). The rect is not what a solid is
// tested against.
//
// Bisected live (gd_run + inject, boundary to 0.01 px), two levels, two object
// ids, both sizes, and BOTH faces:
//   lv17 t=7040 FULL, block id173 (9135,165) top 180: y=181.510 lives,
//                                                     y=181.500 dies
//   lv17 t=7040 FULL, same wall's left face x=9090:   x=9088.4  lives,
//                                                     x=9088.5  dies
//   lv20 t=839  MINI, block id94  (1093,125) top 140: y=141.510 lives,
//                                                     y=141.500 dies
// Every one of those brackets closes on 1.5, and the exactly-1.5 cases die, so
// touching counts as a hit. The value does NOT scale with vsize -- the mini and
// the full wave share it -- which is why deriving it from the rect (5.0, and
// 0.6x of that) was wrong in both sizes.
//
// This was costing whole routes, not pixels: replaying lv20's own best plan,
// the model tracked GD to 0.3 px for 786 ticks and then killed the player at
// t=786 on id94 (1005,195) at a corner overlap of 1.1 x 3.4 px that GD flies
// straight through.
// SOLIDS ONLY. The hazard test keeps the contact half -- measured, see the
// note at the wave's hazard branch.
constexpr double kWaveKillHalf = 1.5;
// The dart's sprite rotation eases toward its travel angle by this fraction of
// the remaining gap per tick. Measured, see the wave branch in stepOne.
constexpr double kWaveRotK = 0.0625;
constexpr double kWaveRotKMini = 0.10;
constexpr double kRadToDeg = 57.29577951308232;
// ...but the RAMP test uses its own box, and it is NOT centred on the player.
// Point-probed on lv20's zig-zag wave corridor (t=4042, one tick, x and y both
// injected, boundary bisected to 0.004 px) against the two ceiling ramps
// (5625,295) 310->280 and (5655,295) 280->310 and the two floor ramps under
// them. Every one of the 26 boundaries is reproduced by ONE rectangle:
//   corner sampled at x + 3.072 when the wedge thickens to the RIGHT
//                     x - 4.068 when it thickens to the LEFT
//   surface offset by 4.0 vertically, and the sample x clamped to the ramp box
// e.g. ceiling line - 7.072 in the far field, flat at corner - 4.0 across the
// 3 px left / 4 px right of the two ceilings' shared corner (that flat is the
// clamp), floor line + 8.072 on the mirrored side -- and the floor pair has NO
// flat, because there the sample points away from their shared corner.
// The old single 8.0 is that rectangle read at 45 degrees from
// one side only (lv18's downhill ramp at (19125,195): kill at line + 8.0 +-
// 0.4, and this box says 8.070 there).
// Only |m| = 1 is measured; the split into "sample x" and "offset y" is what
// separates it from a plain half extent, and it is what makes the corner flat.
// The mini values carry the established 0.600 ratio and are UNVERIFIED.
constexpr double kWaveSlopeDxR = 3.072;
constexpr double kWaveSlopeDxL = 4.068;
constexpr double kWaveSlopeDy = 4.0;
// ...and the mini ratio is BRACKETED, not carried over. The old 0.600 was
// flagged UNVERIFIED and lv20 refutes it; 1.000 is refuted the other way. Two
// GD verdicts in the same corridor pin it, writing the box as `s` x the
// measured full-size one (3.072 / 4.068 / 4.0):
//   LIVES  lv20 t=3691, mini wave (4790.70, 236.210), ramp uid3321 (4785,225)
//          sy0=210 sy1=240 (m=+1, floor half, sample to the RIGHT)
//          survival needs 236.210 >= 230.70 + s*(3.072 + 4.0)  ->  s <= 0.779
//   DIES   lv20 t=3712, mini wave (4819.28, 225.825), ramp uid3383 (4815,225)
//          sy0=240 sy1=210 (m=-1, floor half, sample to the LEFT)
//          the kill needs 225.825 <  220.72 + s*(4.068 + 4.0)  ->  s >  0.633
// so s is in (0.633, 0.779] and 0.700 sits in the middle of it. 0.600 misses
// GD's kill by 0.26 px -- and that one corridor cost the cold run ~40
// iterations of 5 px each, because every tick GD killed and the model did not
// came back as a `kill` fixup instead of as a rule.
// Two brackets, one constant: TIGHTEN IT with py/hitbox_sweep.py before
// deriving anything from the value.
constexpr double kWaveSlopeMiniScale = 0.700;
constexpr double kWaveSlopeDxRMini = kWaveSlopeDxR * kWaveSlopeMiniScale;
constexpr double kWaveSlopeDxLMini = kWaveSlopeDxL * kWaveSlopeMiniScale;
constexpr double kWaveSlopeDyMini = kWaveSlopeDy * kWaveSlopeMiniScale;
constexpr double kShipHalf = 15.0;  // ship outer box (any contact excludes)
// player-vs-hazard half: the OUTER 30x30 box. Measured on lv1: spike (525,105)
// hitbox 6x12 kills at |dx|=17.4 -> 3+15. (rectcorridor's 10 was a safe
// under-estimate for blocking, not the true kill range.)
constexpr double kHazHalf = 15.0;
// Planning margin: GD checks collisions at sub-tick positions and sampling only
// the tick endpoints can miss a corner graze, so plans are kept this far off
// hazard edges.
//
// 1.5 with 4 samples was too blunt for tight corridors. Measured on lv10's ball
// zone: GD carried the ball ALIVE past a spike at a gap of 15.107, while the
// model kills at kHazHalf + margin = 16.5 -- so every branch through that
// corridor died in the model and the frontier collapsed to a single state by
// x=3244. The margin exists to cover the gap BETWEEN samples, so pay for it
// with samples instead: 8 sub-steps put consecutive samples ~0.4 px apart at
// cube speed, and 0.5 covers that with room to spare.
// Even 0.5 was too much. The lv10 measurement pins it: GD ran the ball past a
// spike of half-width 2.79 at a horizontal distance of 17.90 and it LIVED --
// 17.90 is barely outside the exact box contact at 2.79 + 15 = 17.79, so any
// margin at all kills a case GD survives. With 8 sub-steps the sampling gap is
// ~0.4 px and the endpoints are exact, so the margin has nothing left to buy.
// A plan that grazes now shows up as a death in the driver's GD replay, which
// re-anchors -- that is the right place to catch it, not a blanket inflation of
// every hazard in the level.
// Hazards are sampled at this many points between the previous and the new
// position. A tick IS one GD physics step, so GD only ever tests the END of the
// tick -- every intermediate sample is a position the player never occupied,
// and killing on one would be killing on a fiction, which is what makes the
// model miss frame-perfect gaps like lv11's three spikes at x=24,883.
// TRIED AND REVERTED (2026-07-31): kSubSteps = 1, endpoints only. lv1-10 all
// still cleared, but lv11 went the WRONG way -- the GD replay fell from 24,874
// back to 11,914, i.e. the model started emitting plans GD kills. So the
// intermediate samples are catching something real; GD's collision pass is
// evidently finer than the per-tick state dump exposes. The under-kill is the
// dangerous direction, so this stays at 8 until it can be measured directly
// instead of argued from the tick rate.
constexpr int kSubSteps = 8;
constexpr double kHazMargin = 0.0;
// Cube: how far below a surface the foot may already be and still be pushed up.
// 3.0 was a guess and it was too small: lv6 t=13606 has GD landing the cube on
// the ledge at x=17700 (top y=180) with the foot 3.29 px below it, and the model
// missed by 0.29 px and fell past.
// Swept at that ledge (inject y in 0.5 steps, read land/die):
//   penetration <= 10.50 -> lands,  >= 11.00 -> dies
// so the true limit is ~10.5-11. 6.0 is used instead: it is the ship's exactly
// measured boundary (kShipLandTol) and it covers everything observed in play,
// while a 10 px upward teleport would be a large licence to hand the planner on
// the strength of one sweep whose contact had already begun at injection time.
// Redo as a contact-onset sweep (like the ship's) before raising it further.
// TRIED AND REVERTED (2026-08-03): 10.5 (the swept value quoted above), to see
// whether lv18's frontier could then reach the ledge 269 (24,675,255) top 270
// that it tops out 8.35 px below. It changed NOTHING -- the frontier's y range,
// alive counts and death tick were bit-identical. kLandTol only forgives a foot
// that is ALREADY within N px BELOW the face; at y=262 the foot is at 253, i.e.
// 17 px below 270, which no tolerance in this range reaches. lv18's wall is not
// the landing tolerance.
// [2026-08-18] 6.0 -> 10.5 (the measured value the sweep above produced, as is).
// The landing at lv19 t=9,451 has a 6.05px penetration and fell **0.05px outside
// the threshold**. Once the robot's g was corrected to the measured -0.194 the
// player's y matched GD, and as a result the penetration went from 5.958 to 6.047
// and the landing was dropped (GD does land). Restoring the constant to the
// measured value passes both. --landtol A/Bs it.
// [2026-08-18, revised the same day] 10.5 -> 10.0. 10.5 made lv9 STUCK in the same
// day's cold run: the gravity-flipped ball at x=11,265 "landed" from **10.2125px**
// past the underside (180) of the slab (11285,187) and burrowed in, whereas GD does
// not land it and it dies on the wall with an x penetration of 10.9
// (gd_death_context names it). So the top face of the same slab was re-swept finely
// with worker-98 injections (x centred / entering sideways from non-overlapping x,
// vy -2 / 0):
//   d=10.0 (centred)       lands
//   d~10.05 (sideways)     refused -> floats 7 ticks, then side death at xpen~11
//                          (same as the ball)
//   d=10.2125 (real run)   refused (lv9's ball itself)
//   d=10.5 (centred)       dies on the injection tick (the crush side fires first)
//   d=6.047 (real run)     lands (lv19 t=9,451 -- the reason for raising to 10.5;
//                          holds at 10.0 too)
// The boundary is a single one at [10.0, 10.05]; no sideways/centred distinction is
// needed. Yesterday's 10.5 was the value of lv6's 0.5px-step sweep, whose own note
// warned of contamination with "contact had already begun at injection time"
// ([[gd-injection-resolution]]). The side where the penetration keeps deepening
// while refused is killed by cube/solid-side at the same xpen ~11 as GD (lv9's
// replay is confirmed to match on all 8,672 ticks).
inline double kLandTol = 10.0;
// The "gap between the extrapolated seat and the player's y" that ride continuation
// (stickHere) allows. **A constant in px** -- measured on the seam rig at 3 speeds
// (see the stickHere note). --stickgap A/Bs it.
// The value is the midpoint of the range [4.0307, 4.0371) that satisfies **130** of
// the 132 units (seam/seam07/seam11 x cube/ball x |m|{1,0.5} x 12 phases). The 2
// outliers are one-offs (seam unit8 wants >= 4.087, seam11 unit43 wants < 4.026),
// 1 tick at the edge of the phase -- the remaining systematic error is under 0.05px.
inline double kStickGap = 4.034;
// How far off a rotation trigger (id 2900) the player may be on the PERPENDICULAR axis and still
// fire it. Bracketed by GD's own gframe changes across two full lv22 runs -- fires at |dv| up to
// 116.7, skips at 274.4 and 657.8 -- so anything in that gap fits every measurement; 150 sits in
// it with margin either side. The reasoning, and why the old 30 was falsified, is at the gate in
// step.hpp's applyRotation. --rotperp A/Bs it.
inline double kRotPerpWin = 150.0;
// How deep a top face may sit (how far below the line) and still be grabbed as a
// "step" while riding. Midpoint of the measured boundary (3.100, 3.200] on the
// ridestep calibration rig. --stepdepth A/Bs it.
// [2026-08-21 diagnostic] A/B switch that cuts the ship ladder of hanging rides
// (r88). Default false = rule on. Exists only to measure where a cold run's cost
// is attributed.
inline bool g_noHangLadder = false;
// [2026-08-22 r106] Inner-box half-size for the crush (squeeze). Measured on the
// calib_crush calibration rig: "instant death when the inner box (centre +-4.5)
// touches the interior of a solid". The boundaries are
// cube (19.5,19.6] / robot (19.5,19.75] / ball (19.5,20.0] /
// mini cube (13.5,13.6] -- all consistent with "dies when ceiling underside -
// centre <= 4.5", and **still 4.5 for mini** (the same size-independent family as
// the wave's solid=1.5). Injecting into the interior of a corridor also dies on
// the injection tick = an interior intersection, not an entry condition.
// Independent corroboration: the kLandTol sweep's "d=10.5 (centred) dies on the
// injection tick (the crush side fires first)" = the centre is the same 4.5 from the
// slab's top face. The existing cube/solid-side killing at xpen ~11 (= 15 - 4.5 +
// alpha) is the same box seen from the side. --nocrush A/Bs it.
inline bool g_noCrush = false;
inline double kCrushHalf = 4.5;
inline double kStepDepth = 3.15;
// Ship: MEASURED, not guessed. State-injection sweep at lv4's ledge x=17250
// (top y=150), stepping the injected y by 0.1 and reading the outcome:
//   penetration <= 5.95  -> lands (GD pushes the ship up onto the ledge)
//   6.05 .. 9.15         -> passes straight through, no landing and no death
//   >= 10.15             -> dies
// so the landing limit is exactly 6.0, twice the cube's 3.0. Using the cube's
// value here made the model fall past ledges GD had landed on (lv4 t=13257,
// penetration 5.67 -- it missed by 0.02 px and cost the whole level).
// The 6..10 "passes through" band is GD having a smaller inner box for the
// kill test than the outer box it rides on; the model kills there instead,
// which is conservative (it never plans a route that GD would end).
constexpr double kShipLandTol = 6.0;
// How deep the horizontal overlap must be before GD resolves a solid it did not
// see the ship fly into, as a multiple of the player's half width. Bracketed by
// the two 0->1 transitions of the hooked collidedWithObject return on lv11:
// mini (half 9) 14.670 -> 15.970, normal (half 15) 25.930 -> 27.230, i.e.
// 1.630..1.774 and 1.729..1.815. 1.75 is the only clean value in both.
constexpr double kSolidResolveX = 1.75;
// The plain (unarmed) cube CEILING STOP is OFF. [2026-08-16]
// It was fitted to two samples: lv1's false stop (xPen 6.67, full size) and
// lv22 t=2,749 (xPen 19.78, mini) -- but t=2,749 is the 2866 FLIPHEAD event,
// which `acquireFlip` handles on its own. A third sample, measured 2026-08-16
// by sweeping GD directly (gd_run inject at lv22 t=11,385, mini), has GD NOT
// pinning at xPen 15.957 -- and not at the block's dead centre either
// (xPen 24.0, the deepest possible), so no threshold on xPen can separate it
// from t=2,749. GD's unarmed didHitHead does nothing: the player keeps moving
// into the block and dies of the overlap, which is exactly what it did.
// --ceilpin restores it for A/B. `held` reads ceilPin, which only this branch
// sets, so it is gone too.
inline bool g_ceilPin = false;
// Ship zones have an invisible ceiling. It was modelled as "ship portal cy +
// 120" from a single lv1 measurement (portal cy 255, ceiling 375.000) -- but
// that was a coincidence. lv4's ship portal sits at cy = 233, so the rule
// predicts 353, and GD's measured ceiling in that zone is again 375.00 -- the
// model clamped the ship 22 px low and it could never reach the route above.
// Two zones with different portal heights, same ceiling: it is ABSOLUTE.
// ...and 375 is not universal either. lv10 t=17836 has GD flying the ship at
// y=427 and still climbing, so a hard clamp at 375 blocked that route outright.
// maxY does not explain it (lv1 maxY=540 -> 375, lv2 maxY=390 -> 375,
// lv10 maxY=630 -> above 427), so there is no level-derived rule in hand.
// TRIED AND REVERTED: disabling the clamp entirely (relying on g_yBound alone).
// It let lv10 through to t=18,743 but **lv2 went from cleared to stuck** at
// t=10,569 and lv1 needed 5 iterations instead of 2 (2.7 min vs 0.3). The clamp
// is load-bearing where it is right, so it stays at the value that is right for
// lv1/lv2/lv4.
// What it actually is: a SEARCH BOUND, not physics. GD exceeds it (lv10 y=427)
// and no level-derived rule reproduces it -- neither maxY nor the ship zone's
// own highest surface (lv1 zone 450 -> 375, lv2 zone 390 -> 375). But lv1/lv2
// need it, because without it the DP spends its frontier on high routes that
// GD then refuses. So it is a knob with a default, not a constant: `--shipceil`
// raises it for levels whose ship really does fly higher.
inline double g_shipCeil = 375.0;
// ...and the flying modes have a hard FLOOR too, which is not the world's.
// Measured on lv12's UFO section through the gd MCP: injecting the UFO at y=60
// at t=5400 / 5500 / 5640 (x = 7010 .. 7321) has GD put it back at y=195.000
// with onGround=1 every time, and injecting at y=700 clamps it down to 465.
// So the band is [180, 480] for the player's surfaces -- exactly 300 tall --
// while there is NOT ONE object under it (checked: zero type-0 in y[140,220]
// across x[7000,7160]). A cube injected the same way at t=5300 just dies, so
// the band belongs to the flying modes, not to the level's geometry.
// The model had the ground pinned at 90 and simply fell through, which is why
// the DP planned a route GD never lets it take.
// Same status as g_shipCeil: measured per level, not derived. 0 = no floor.
// The band is per SECTION, not per level: forcing lv12's UFO band (180/465)
// onto its ship sections killed the search inside the first one immediately.
// So these two apply to the UFO portal (type 19) only, and a cube or ball
// portal clears them again.
// ...and for the UFO the band is DERIVED from the portal, not a knob at all.
// lv12 has two UFO sections at wildly different heights and one number cannot
// serve both, so the portal's own cy was checked against the measurements:
//   portal (6945, 345)   -> floor 180 measured, 345 - 165 = 180   ceiling 480
//                           measured, 345 + 135 = 480
//   portal (16665, 1095) -> floor 930 measured, 1095 - 165 = 930
// Two sections, three bounds, exact on all of them. The same arithmetic also
// reproduces the ship bound that was tuned by hand long ago: lv1's ship portal
// sits at cy = 255, and 255 + 135 - 15 = 375 -- exactly g_shipCeil's default.
// The knobs below stay as overrides (0 = derive).
constexpr double kFlyBandBelow = 165.0;
constexpr double kFlyBandAbove = 135.0;
inline double g_flyFloor = 0.0;
inline double g_ufoCeil = 0.0;   // 0 = fall back to g_shipCeil

}  // namespace dp
