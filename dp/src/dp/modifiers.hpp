#pragma once
#include "dp/speed.hpp"

namespace dp {

// --deadband x0,x1[,mode[,y0,y1]] (repeatable). The note is at the top of stepOne.
// With mode: "in this band everything except mode dies" = forces the portal that
// must be taken. With y0,y1: only branches whose y is also in that range are killed
// (default: all y). lv22's "gravity-flipped ball climbing out of the field and
// advancing in x through the sky above" cannot be told apart from the floor route
// by x alone (the floor passes the same x band at y=250, the arc at y=600+), so the
// band carries a height.
// mode semantics: -1 = all modes die; >= 0 = a FORCING ("only this mode lives");
// <= -2 = the inverse, "only mode (-2 - mode) dies here". The third form is what a
// refutation honestly supports: the phantom veto knows "a SOLVED tail in mode M was
// killed by the game here", which says nothing about the other modes -- lv22's shaft
// wedge point is crossed by the ship (which wedges) AND by the reference robot
// (which climbs through), so a mode -1 box there closes the truth along with the lie.
struct DeadBand { double x0, x1; int mode; double y0, y1; };
inline std::vector<DeadBand> g_deadBands;
// FORCE FIELD (object id 3645, type 40). lv22 x=2,587 is the only instance in
// lv1-22, placed at rot=180 with raw radius 15 scaled x1.78 -> 26.7. Measured
// on worker 98 (2026-08-12) with the run's own plan plus two injected variants
// (falling, rising, entering from above): while the player and field CIRCLES
// overlap at the START of a tick, the next vy integration gains a world-frame
// downward term
//   -0.270 * clamp01((y - (cy - R)) / (2R))
// -- a linear ramp across the field's vertical diameter, zero at the rotated
// bottom edge, the full 0.270 at the top edge and clamped there for anyone
// above it. ~80 airborne samples across the three runs reproduce GD's vy on
// its 0.001 grid with k = 0.270 / 53.4 = 0.005056. The "decaying gravity" the
// death run showed (dvy -0.300 .. -0.129 over 40 ticks, runO4 iter 14..26,
// t=2091..2131) was just this ramp shrinking as the ball fell through it.
// Both vy signs behave identically, so it is position-only, and it also runs
// on the tick an orb FIRES (the model's 0.115 px "dash started early" residue
// at t=2092 was this term missing from the firing tick's integration).
// The gate is circle-vs-circle, NOT the AABB: entering from above, the AABB
// overlap at t=2104-2105 did not fire it and the circle contact at 2106 did
// (effect from 2107, one tick later). All six measured entry/exit edges
// bracket r_p + R to (41.3, 42.4); 15 + 26.7 = 41.7 is what runs here.
// During a DASH nothing is applied (y sat frozen to the last digit for all 10
// dash ticks) -- the dash branch bypassing this term is the measured behavior.
// OPEN: rot 0 is modelled as the mirror (push up, ramp from the top edge) but
// only 180 is measured; sideways rotations are refused at load; the player
// radius follows pHalf so mini shrinks it (unmeasured); whether the 0.270 cap
// scales with the object is unknown (single instance).
constexpr double kFF3645Max = 0.270;
// FORCE BOX (id 2069, type 40): the same m_stateForce family as 3645 (as the 2866
// note below says, 2069/3645 share one counter). lv22 has 13, all rot=0 (pointing
// up). Measured (uid14472 (18,975,645) 30x30, one ship pass, 2026-08-16):
//   - contact is the AABB (radius=0, there is no circle), player half-width 15;
//     contact at t=15,817 -> effect at 15,818, the +1 tick convention matches exactly
//   - the push is a flat +0.159/tick inside the box (world-up): implemented and run
//     side by side, the difference is a constant +0.159 both while pressed (model
//     +0.108) and while released (model -0.103, the ship gravity at spd1.3). No
//     position-linear ramp like 3645's is observed (change <0.001 across the 10px
//     from y 631->641)
// OPEN: a fit to one pass of one instance. Scale dependence of the strength, the
// mirror for rot!=0, and the application to swing/wave are unmeasured.
//   [2026-08-30] Two of those three are closed. Scale does NOT enter (uid17701
//   is 5.1x3.9 and agrees with the scale-1 boxes, and the forcecal rig varies
//   scale 1/3/5 at fixed m_force), and swing and wave are both measured on
//   calib_forcedrop -- swing 0.0900 per unit of m_force, wave exactly zero. The
//   rot!=0 mirror is STILL unmeasured; GD takes the direction from the object's
//   rotation as a full angle (calculateForceToTarget, win 0x4c1ec0) so there is
//   a shape to test against, but no rig has been run rotated.
constexpr double kFF2069 = 0.159;
// ...and the push strength is **per-instance**. Measured on the large uid17701
// (154.47x116.91, (20,085,675)) (the CLEARED run's fall, t=16,367-370): GD's dvy is
// +0.135 = -0.195 + **0.330**. Left at 0.159 the model sinks at -0.036, lands on the
// corner (20,145,675) at the shaft entrance and never reaches the 2900 window
// (y 693..753). With only two points no scaling law is set up; it is held as a
// measured table by uid (OPEN: 17041/17184/11412 are unmeasured, default 0.159).
//   [2026-08-30] CLOSED, and it was the expensive one. 17041/17184 carry
//   m_force=4.2 and were running at 0.159 against a true 0.850 for a robot --
//   5.3x low, at lv22 (21555,2634) and (21555,2694), for as long as this note
//   stood. There is no table any more, so there is no entry to leave empty.
// [2026-08-21] ...and **the same box has a different strength per mode**. uid14472
// gave +0.159 for the ship's pass, but a **robot** passing the same box gets +0.304.
// Measured lv22 t=15,879-15,888 (sp1.3, no input, box uid14472 (18,975,645) 30x30):
//   GD's dvy is exactly +0.109/tick for 9 ticks and returns to -0.195 (the robot's
//   bare gravity) at 15,889, once out of the box. +0.109 = -0.195 + **0.304**
//   The model was still at 0.159, so it sank at -0.036 and the census showed
//   `m5/mini0/g0/gdg0/sp1.4/air` edvy +0.145
// 0.304/0.159 = 1.912 is close to the robot/ship gravity ratio 0.195/0.103 = 1.893,
// which suggests the reading "box strength x that mode's gravity", but **dividing
// the other measurements in the table (11412=0.283 robot / 17701=0.330 robot /
// carpet=0.108 swing) gives coefficients scattered over 1.25..1.69**, so no scaling
// law is set up and it stays a **measured table of uid x mode** (same policy as
// before).
//
// [2026-08-30] THE PARAGRAPH ABOVE IS KEPT BECAUSE IT WAS RIGHT AND WAS THROWN
// OUT ANYWAY. The law is exactly the one it proposes:
//
//     dvy = m_force * |g(mode)| / kForceGDiv
//
// The division that produced 1.25..1.69 left out m_force, the per-instance
// strength ForceBlockGameObject carries and nothing could read until the
// exporter emitted it (objrects' `force` column). Put it back and the scatter
// collapses onto one number.
//
// The FIRST attempt at that number was 1/0.9581990, the dt at kRobotHoverTicks,
// picked because dividing the four corpus values by the QUANTISED gravity gives
// 1.042/1.045/1.043/1.047 and 1.04362 sits in the middle. It was wrong twice
// over -- the divisor is 0.96 and the gravity is the unquantised one -- and it
// cost three regressions, because "a constant we already have is close to the
// answer" is a coincidence, not a derivation. The ladder below is what settled
// it, and it settled it by making the measurement precise enough that the two
// candidates could not both fit.
// 0.9601, not 0.96. Solve each measurement for the interval of divisors whose
// ROUNDED result reproduces it (GD's vy is on the 0.001 grid, so that is the
// only thing a reading pins) and intersect the eight:
//     cube_f10    2.160 /D -> 2.250     D in (0.95979, 0.96021]
//     robot_f10   1.944 /D -> 2.025     D in (0.95952, 0.96020]
//     ball_f10    1.296 /D -> 1.350     D in (0.95964, 0.96035]
//     swing_f10   0.864 /D -> 0.900     D in (0.95947, 0.96053]
//     11412 robot 0.27216/D -> 0.283    D in (0.96003, 0.96339]
//     14472 robot 0.29160/D -> 0.304    D in (0.95764, 0.96079]
//     17701 robot 0.31687/D -> 0.330    D in (0.95877, 0.96168]
//     carpet swng 0.10368/D -> 0.108    D in (0.95558, 0.96447]
//                                       ------------------------
//                              all eight D in (0.96003, 0.96020]
// The interval is not empty, which is the check: four rigs and four corpus
// entries, measured years apart by different means, agree on a 0.00017-wide
// window. A round 0.96 sits 0.00003 BELOW it -- close enough to look right and
// wrong at uid11412, whose product lands exactly on a rounding boundary
// (1.4 x 0.2025 = 0.28350) and is where lv22 lost tracking three times.
constexpr double kForceGDiv = 0.9601;
// HOW IT WAS MEASURED. calib_forcedrop_<mode>[_f<n>]: the player WALKS into a
// 300x300 box (injected probes read a force of exactly ZERO -- the push is
// applied inside checkCollisions' object loop at 0x215b4e, so a candidate list
// built before a 2,370 px teleport does not contain the box), and the fall
// right after it is read in the SAME run, so the gravity subtracted is the
// gravity that was acting.
//
// At m_force=1 the push is 0.09..0.225 and GD's 0.001 grid puts 0.5-1% on it --
// the entire width of the divisor the corpus backs out. The ladder fixes that:
//     rig             m_force   push     push/m_force
//     cube            1         0.225      0.2250
//     cube_f3         3         0.675      0.2250      <- linear
//     cube_f10       10         2.250      0.2250
//     robot_f10      10         2.025      0.2025
//     ball_f10       10         1.350      0.1350
//     spider_f10     10         1.350      0.1350
//     swing          1          0.090      0.0900
//     swing_f10      10         0.900      0.0900      <- linear
// The push per unit of m_force divided by the cube's is 1 / 0.9 / 0.6 / 0.6 /
// 0.4 -- GD'S OWN MODE GRAVITY SCALES, exactly. So the force rides the same
// scale as gravity, and with the UNQUANTISED gravity every one is exact:
//     cube    0.216   / 0.96 = 0.2250      robot  0.1944 / 0.96 = 0.2025
//     ball    0.1296  / 0.96 = 0.1350      swing  0.0864 / 0.96 = 0.0900
// USE cubePhysFor(dx).g, NOT gAcc. gAcc is rounded to the 0.001 grid (0.194 for
// the robot, not 0.1944) and rounding before the division is what made the
// first two attempts miss lv22's table by a grid step at a time.
// wave has no gravity, so the law predicts NO response, and the wave reads
// exactly zero dvy inside the box as well as outside. That is the law's best
// evidence and it was a prediction, not a fit.
//
// SHIP AND UFO ARE MEASURED, NOT DERIVED. Their gravity switches between a weak
// and a strong value at accelSwitchVy, so "inside minus after" straddles two
// regimes and the rig's own subtraction is invalid for them:
//   ship  rig f10 gives 1.024 against a weak -0.069, but the run is on the
//         STRONG side inside the box, so the push is 1.024 + 0.103 = 1.058 ->
//         0.1058 per unit. lv22 wants 0.159/1.5 = 0.1060 and 0.172/1.63 =
//         0.1055 -- and 0.1058 is the only value that rounds to BOTH.
//   ufo   1.176 + 0.129 = 1.305 -> 0.1305 per unit. One rig, no corpus check.
// Neither divides cleanly by 0.96, which is why they sit in the table below
// rather than being pretended into the law.
constexpr double kForceUnitShip = 0.1058;
constexpr double kForceUnitUfo = 0.1305;
// The push per unit of m_force. Speed rides along for the five derived modes
// because cubePhysFor carries it; for the ship and UFO it is unmeasured, and
// lv22 crosses uid14472 at speed 1.3 with the same 0.1058 the 1x rig gives, so
// a constant is what the evidence supports.
// MINI, measured 2026-08-30 on calib_forcedrop_<mode>_mini_f10:
//   cube  push 2.250, |g| 0.216 -- IDENTICAL to full size
//   ball  push 1.350, |g| 0.129 -- IDENTICAL to full size
// which is what the law predicts: neither mode's gravity changes with size, so
// neither does its push. forceUnitFor therefore does not take a size, and for
// six of the eight modes that is measured rather than assumed.
//
// THE SWING IS THE EXCEPTION AND IT DOES NOT FIT. Its gravity DOES change with
// size (kSwingGMini 0.129 against kSwingG 0.086, confirmed here by the fall
// after the box), so the law predicts 10 * 0.129/kForceGDiv - 0.129 = +1.221
// net. GD gives +1.2560/tick, held for seven ticks -- 2.9% out, far outside the
// 0.001 grid. Two things confound the reading and neither is resolved:
//   - GD reports onGround=1 for the whole climb, at vy up to 9. If gravity is
//     not being applied at all there, the push is 0.1256 per unit, which is not
//     a clean scale either.
//   - the mini swing clamps at vy 9.385 while the full one does not clamp at
//     all (see kSwingTerm), so the window is short.
// Left as the full-size value: no level in lv1-22 puts a mini swing in a force
// box, so the model cannot be measured against the difference, and 0.1256 vs
// 0.1385 cannot be told apart without settling the gravity question first.
// A TYPE, not a double, and that is the whole point. This function replaced one
// that took a gravity, six call sites had to change, three of them did, and
// because both were `double` IT COMPILED AND RAN. The sites still passing a
// gravity gave 1.4 x 0.195 = 0.273 where GD gives 0.283 at uid11412 -- and five
// rebuilds went hunting for that 0.010 in the law, in the divisor and in the
// rounding before anyone looked at the call sites. Make the mistake impossible
// rather than remembering not to make it.
struct ForceUnit { double v; };
inline ForceUnit forceUnitFor(uint8_t mode, float dxF) {
    const double g = std::fabs(cubePhysFor(dxF).g);
    switch (mode) {
        case 0: return {g / kForceGDiv};                  // cube
        case 1: return {kForceUnitShip};
        case 2: return {g * 0.6 / kForceGDiv};            // ball
        case 3: return {kForceUnitUfo};
        case 4: return {0.0};                             // wave: measured zero
        // The scales are spelled out rather than taken from kRobotGScale /
        // kSpiderGScale in constants.hpp: this header is below that one in the
        // include order, and these are the numbers the ladder MEASURED. If the
        // two ever disagree, the disagreement is the finding.
        case 5: return {g * 0.9 / kForceGDiv};            // robot
        case 6: return {g * 0.6 / kForceGDiv};            // spider
        case 7: return {g * 0.4 / kForceGDiv};            // swing
        default: return {0.0};
    }
}
struct ForceBox { double cx, cy, hw, hh; double force; };   // force = m_force
inline std::vector<ForceBox> g_forceBoxes;
inline double forceBoxSum(double x, double y, double pHalf) {
    double f = 0.0;
    for (const auto& fb : g_forceBoxes)
        if (std::fabs(x - fb.cx) <= fb.hw + pHalf
            && std::fabs(y - fb.cy) <= fb.hh + pHalf)
            f += fb.force;
    return f;
}
// `unit` is forceUnitFor(mode, dx) -- the push one unit of m_force gives THIS
// player. Quantised the way GD quantises vy: every number the old uid table
// held was a 0.001-grid observation, so the law has to be rounded the same way
// before it can be compared with them.
//     14472 robot   1.5  x 0.2025 = 0.30375  -> 0.304   (table 0.304)
//     17701 robot   1.63 x 0.2025 = 0.33008  -> 0.330   (table 0.330)
//     carpet swing  1.2  x 0.0900 = 0.10800  -> 0.108   (table 0.108, exact)
//     11412 robot   1.4  x 0.2025 = 0.28350  -> lands ON the rounding boundary
//                                               (table 0.283)
//     14472 ship    1.5  x 0.1058 = 0.15870  -> 0.159   (table 0.159)
//     17701 ship    1.63 x 0.1058 = 0.17245  -> 0.172   (table 0.172)
inline double forceBoxAcc(double x, double y, double pHalf, ForceUnit unit) {
    return qVy(forceBoxSum(x, y, pHalf) * unit.v);
}
struct ForceField { double cx, cy, R; int dir; };  // dir -1 = push world-down
inline std::vector<ForceField> g_forceFields;
inline double forceFieldAcc(double x, double y, double pHalf) {
    double a = 0.0;
    for (const auto& ff : g_forceFields) {
        const double dx = x - ff.cx, dy = y - ff.cy;
        const double rr = ff.R + pHalf;
        if (dx * dx + dy * dy > rr * rr) continue;
        double frac = (ff.dir < 0) ? (y - (ff.cy - ff.R)) / (2.0 * ff.R)
                                   : ((ff.cy + ff.R) - y) / (2.0 * ff.R);
        if (frac < 0.0) frac = 0.0;
        if (frac > 1.0) frac = 1.0;
        a += (double)ff.dir * kFF3645Max * frac;
    }
    return a;
}
// FLIP-ON-HEAD-HIT (object id 2866). Not a collider and not a portal: it is
// one of GD's "gameplay modifier" objects, picked by id in
// GJBaseGameLayer::collisionCheckObjects (+0x1146..+0x1196), which set a
// per-player counter that PlayerObject::update decrements every tick:
//   1813 m_stateNoAutoJump   1755 m_stateDartSlide   1859 m_stateHitHead
//   2866 m_stateFlipGravity  2069/3645 m_stateForce
// The only reader of m_stateFlipGravity is PlayerObject::didHitHead (0x393c30):
//   if (m_stateFlipGravity > 0) {
//       flipGravity(!m_isUpsideDown, true);
//       setYVelocity(m_isUpsideDown ? +2 : -2);   // the value AFTER the flip
//       if (m_stateNoAutoJump > 0) <clear the held-button state>;
//   }
// and didHitHead is called from collidedWithObjectInternal, i.e. when the
// player's HEAD reaches a solid. So an armed player does not stop under a
// ceiling -- it turns over and stands on it.
//
// Measured on lv22 (the only level in lv1-22 with a 2866; it has exactly one,
// a 36x36 at (3735,255)):
//   t=2,707 x=3,710.8 -- box overlap begins, the counter goes positive
//   t=2,748 x=3,790.8 y=320.771 vy=10.816, ceiling face 330.008
//   t=2,749 upsideDown 0->1, onGround=1, y=321.008 = 330.008 - 9 (mini half)
// ~~ARMING IS STICKY: the counter stays positive for 500+ ticks after the box is
// behind the player (GD re-sets it every tick), and in a full pass it went off
// again 638 px past the object. The mechanism behind that range is not
// identified, so this models the arm as PERMANENT -- inside the measured span
// it is exact, and no other level owns a 2866, so nothing already green can
// move. If lv22 later diverges under a ceiling far past x=3,735, this is the
// first thing to look at.~~
//
// [2026-09-07 AUDIT] **lv22 DID later diverge under a ceiling far past x=3,735,
// exactly as the last sentence predicted -- at t=11,342, x=16,114, and it is
// lv22's whole-run killer.** Four claims in the paragraph above are refuted.
// The full record, the positive control and the bracket live at State::fgArm;
// what belongs HERE is which of them were wrong and why, because the four sort
// cleanly by provenance:
//
//   REFUTED, and all four rest on ONE behavioural observation of ONE lv22 pass:
//     "stays positive for 500+ ticks"   -- the firings this corpus shows are at
//                                          +42, +183 and +254 ticks, not 500+.
//                                          (+254 was found after this was
//                                          written; it does not change the
//                                          verdict, which turns on the corpus
//                                          having no firing anywhere near 500.)
//     "went off again 638 px past"      -- no firing in the current reference is
//                                          at 638 px; the observation is from an
//                                          older solution (its coordinates aged
//                                          with the route, cf. the corridor ticks
//                                          in step.hpp's acquireFlip note).
//     "GD re-sets it every tick"        -- true only WHILE THE MATCH RUNS. There
//                                          is exactly one setter in the whole
//                                          .text (0x215ba1) and it needs a match.
//     "models the arm as PERMANENT"     -- GD arms at ~2,707 and does NOT flip at
//                                          11,342 in the same replay.
//
//   VERIFIED, and every one of these came from the DISASSEMBLY rather than from
//   that pass -- which is the audit's actual finding, since the file's readings
//   held and its one behavioural generalisation did not:
//     the counter is set to a small immediate and decremented every tick
//       -- 0x215ba1 `mov [r14+0xb80], 2`, 0x389f40 `dec [r15+0xb80]`, the latter
//          straight-line in update whose 915-instruction prefix has exactly one
//          exit (m_isDead) skipping it.
//     didHitHead's body, line for line -- 0x393c30, including the +-2 and the
//       m_stateNoAutoJump clear.
//     the 1859 sibling's "set to 2 by the touch and stepped down every tick"
//       (line ~360) -- 0x215ac6 sets 0xb7c to 2, 0x389f47 decrements it. The
//       SAME claim shape as the refuted one, from the same reading, and correct.
//
//   IMPRECISE: the range "+0x1146..+0x1196" does not contain 2866's branch. That
//     range holds 0xb74/0xb78/0xb7c/0xb88; `cmp ecx, 0xb32` jumps OUT of it to
//     0x215ba1 (+0x1241).
//
//   AND THE ARM HAS A SECOND CONSUMER, so "the only reader ... is didHitHead"
//     below is wrong -- and wrong the day it was written, not stale:
//     collidedWithObjectInternal, which the very next sentence names as the
//     caller, tests it itself at 0x391e4b (+0x3db, `> 0`) to compute a local
//     boolean, four instructions after m_stateHitHead and the platformer flag.
//     GD uses the arm as a COLLISION MODIFIER with two consumers; the model
//     treats it as a flip enable.
//
// WHAT IS STILL OPEN: the binary says 2 ticks and GD flips 42 ticks after
// arming. Nothing here closes that -- see State::fgArm.
// TIME WARP (object id 1935, EffectGameObject::m_timeWarpTimeMod, dumped as the
// objrects `tw` column). GD scales TIME, so within one tick the x advance, the
// y integration and the velocity increment all shrink by the same factor --
// unlike a speed portal, which changes dx and the gravity table together and
// shows up in the dump's `speed` column (that column does not move here).
// Measured on lv22 x=4,535 (the only level in lv1-22 with any; it has two):
//   dx  1.95019 -> 0.39014      (x0.20005)
//   dy  3.37500 -> 0.67500      (x0.2, vy pinned at terminal 15)
//   dvy 0.215   -> 0.043        (injection probe at t=3,200, vy 0 -> 0.043 ->
//                                0.086 -> 0.129 ...; 0.216*0.2 = 0.0432 and the
//                                0.001 grid is applied to each STEP, not to the
//                                accumulated exact value)
// Fires on the player's centre crossing the trigger's cx, effect from the next
// tick -- the same rule as every other autotrigger. The partner at x=4,615
// carries tw=1.0 and restores it.
struct TimeWarp { double cx, mod; };
inline std::vector<TimeWarp> g_timeWarps;   // sorted by cx at load
inline double timeWarpAt(double x) {
    double m = 1.0;
    for (const auto& w : g_timeWarps) {
        if (w.cx > x) break;
        m = w.mod;
    }
    return m;
}
// CAMERA ZOOM (object id 1913) and the INVISIBLE CEILING.
//
// The flying band's HEIGHT is not a per-mode constant. Measured with the MOD's
// `camscale` column (= the divisor in GJBaseGameLayer::getMaxPortalY, win
// 0x213770, whose body is `getMinPortalY() + [+0x2ec] / [+0x1a8]`):
//
//   tick  mode    camscale   pmax-pmin   product
//    900  spider  0.909091     297.000   270.000
//   1200  cube    0.949295     284.422   270.000
//   3600  swing   0.833333     324.000   270.000
//   4000  swing   0.666667     405.000   270.000
//   5000  cube    0.909091     297.000   270.000
//
// **270.000 exactly, in every mode.** So the height is 270 / camScale, and the
// old kBandFly/kBandOther pair was only ever right where camScale == 1.
// lv22's swing section zooms out to 1/1.5 and the real ceiling is 495 where the
// model was clamping at 390 -- 105 px of reach it was denying itself.
//
// The zoom itself comes from id 1913, dumped as the objrects `zoom` column with
// its `zdur` (seconds), `zease` and `zrate`. The dumped values match the
// measured camscale exactly (x=915 -> 0.909091, x=4,617 -> 0.833333,
// x=5,205 -> 0.666667, x=6,285 -> back to 0.909091).
//
// The trigger fires on the x crossing and then EASES over time. The state does
// not carry the crossing tick (a float in the dedupe key would be very
// expensive), so the progress is mapped through x using the current dx. That is
// exact while the speed is constant, which is the case across every zoom in
// lv22; it drifts only if a speed portal or a time warp lands inside an ease.
// lv1-21 have no id 1913 at all, so nothing already green can move.
constexpr double kBandBase = 270.0;
struct ZoomTrig { double cx, target, durTicks, rate; int ease; };
inline std::vector<ZoomTrig> g_zoomTrigs;   // sorted by cx at load
inline double camScaleAt(double x, double dx);     // defined below gdEase
struct FlipHeadBox { double cx, cy, hw, hh; };
inline std::vector<FlipHeadBox> g_flipHeadBoxes;
inline bool flipHeadArms(double x, double y, double pHalf) {
    for (const auto& b : g_flipHeadBoxes)
        if (std::fabs(x - b.cx) <= b.hw + pHalf
            && std::fabs(y - b.cy) <= b.hh + pHalf)
            return true;
    return false;
}
// CEILING ARM (id 1859, GameObjectType 40). Touching one is what puts the
// classic ground modes -- cube, robot, spider -- into the ceiling RESOLUTION
// arm instead of the kill: collidedWithObjectInternal :739-750 lets
// ship/UFO/wave/swing/ball through unconditionally and everything else only
// with `[0xb7c] > 0`, the counter this object sets. GD writes 2 into it on the
// touch and decays it every tick, so the window is the touch tick and the one
// after it (kArmTicks).
//
// MEASURED, three points, 2026-09-01 (worker 98):
//   lv22 (22,365): five 1859 at cy=2,055 under a ceiling whose underside is
//        2,070. A FULL-SIZE cube injected at y=2,035 rising at vy=20 stops dead
//        at y=2,055.000 with vy=0 and falls away -- the bonk.
//   lv18 (24,345): the same full-size cube into a plain ceiling (underside 240,
//        no modifier objects anywhere near) DIES.
//   lv11 (4,500):  a MINI cube into a plain continuous ceiling DIES.
// So the discriminant is neither the size nor the mode: ~~it is this object~~.
// The model's old `mode == robot || (cube && mini)` was its proxy, which is why
// f3ca55d had to limit the bonk to mini to stop it firing where GD kills.
//
// [2026-09-07] **"IT IS THIS OBJECT" IS WRONG -- 1859 IS ONE OF THREE INPUTS.**
// Read at the gate itself (collidedWithObjectInternal +0x3ce..+0x3e2): the local
// boolean that selects resolution over kill is set by
//     m_stateHitHead (0xb7c) > 0   OR   platformer (0xb70)   OR
//     m_stateFlipGravity (0xb80) > 0
// -- the last being the id-2866 arm, four instructions below the other two. So a
// live 2866 also puts a classic ground mode into the resolution arm, and this
// sentence would tell anyone reasoning about ceiling resolution the wrong thing.
// Marked here rather than folded into State::fgArm's record because that is
// where this claim is met.
//
// INDEPENDENT of everything about the 2866 arm's LIFETIME: this is about what
// FEEDS the gate, not how long the counter lives. The model's bonk gate reads
// only armT (the 1859 side).
//
// AND THE FAILURE MODE IS THE SAME ONE AS THE ceilramp NOTE'S: the paragraph
// this sits in was written about 1859 and is silently wrong about MEMBERSHIP --
// right about what it measured, silent about the axis it never varied. That is
// the second time today a note's blind spot rather than its content was the
// defect (the 2866 paragraph below was written about permanence and was silently
// wrong about the reader set). Worth treating as a pattern, not a tally.
//
// WHAT WOULD MAKE THIS WRONG: a fourth input to that boolean, or the 2866 branch
// being unreachable for the classic ground modes. Look at
// collidedWithObjectInternal +0x3ce..+0x3e2 and at what [rsp+0x35] gates
// (0x392474 and 0x3928ed).
// The armed-mini case (lv22's switch band, 1859 at (3,195,255)) is covered by
// the verified solutions that run through it.
struct ArmBox { double cx, cy, hw, hh; };
inline std::vector<ArmBox> g_armBoxes;
inline bool armBoxTouch(double x, double y, double pHalf) {
    for (const auto& b : g_armBoxes)
        if (std::fabs(x - b.cx) <= b.hw + pHalf
            && std::fabs(y - b.cy) <= b.hh + pHalf)
            return true;
    return false;
}
// DART SLIDE ARM (id 1755, GameObjectType 40 -- m_stateDartSlide in the counter
// list above). This is what lets a WAVE stand on a solid instead of ignoring it.
//
// MEASURED by an entry sweep (2026-09-02, wave-seat-entry-sweep note), after
// three geometric readings of the same landing were each refuted by the corpus:
//   - a wave is armed if it touched a 1755 box on this tick or the one before
//   - while armed, ANY contact with a solid's top (d >= 0, no depth threshold,
//     no horizontal threshold, no phase condition) puts it at top + wave half.
//     It is a PUSH-OUT, not a landing: a player injected 1 to 6 px INTO the
//     block is lifted back out to the same line
//   - unarmed, the same face does nothing at all
//
// That is why no geometry worked. 1755 exists on lv21 and lv22 only -- lv17 has
// ZERO -- so lv17's wave passing 0.47 px through a block's seat line and flying
// on is correct behaviour, and every rule that made it stop was wrong for the
// same reason.
//
// The box is the SCALED one: the dumps carry w,h = 120x120 for these (w0,h0 =
// 30x30), and the raw size does not reach the player on the arming tick.
//
// NOT MEASURED, and so not implemented: mini (zero samples in the sweep -- no
// 0.6 scaling is invented here), faces other than the top, and how the 27-tick
// slide is maintained (consistent with re-arming every tick inside the box, but
// the sweep only pins the arm's own lifetime).
struct SlideBox { double cx, cy, hw, hh; };
inline std::vector<SlideBox> g_slideBoxes;
inline bool slideBoxTouch(double x, double y, double pHalf) {
    for (const auto& b : g_slideBoxes)
        if (std::fabs(x - b.cx) <= b.hw + pHalf
            && std::fabs(y - b.cy) <= b.hh + pHalf)
            return true;
    return false;
}
// DASH STOP (id 1829, GameObjectType 40). Touching one while a dash ring's
// dash is active ENDS the dash: vy restarts from 0 under ordinary gravity on
// the next tick. Measured on lv22 (dash from ring 1704 at x=13,333, held
// throughout): the player's head reaches 1,019.989 at t=9,409 (0.011 px under
// the box bottom 1,020.0 of the 1829s at cy=1,035) and 1,021.603 at t=9,410;
// GD's dash still moves the 9,410 step diagonally and vy shows -0.215 at
// t=9,411 -- so the cancel reads the START-of-tick position, same convention
// as the force field and the 2866 arming below. lv21 has 12 of these but its
// verified plan never overlaps one mid-dash (quick_regress must stay identical).
// UNVERIFIED: any effect outside an active dash (lv22 uid 1805 at x=2,745
// sits in the ball section where no dash exists; modelled as inert there).
struct DashStopBox { double cx, cy, hw, hh; };
inline std::vector<DashStopBox> g_dashStopBoxes;
inline bool dashStopAt(double x, double y, double pHalf) {
    for (const auto& b : g_dashStopBoxes)
        if (std::fabs(x - b.cx) <= b.hw + pHalf
            && std::fabs(y - b.cy) <= b.hh + pHalf)
            return true;
    return false;
}

}  // namespace dp
