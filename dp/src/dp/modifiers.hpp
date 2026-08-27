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
constexpr double kFF2069 = 0.159;
// ...and the push strength is **per-instance**. Measured on the large uid17701
// (154.47x116.91, (20,085,675)) (the CLEARED run's fall, t=16,367-370): GD's dvy is
// +0.135 = -0.195 + **0.330**. Left at 0.159 the model sinks at -0.036, lands on the
// corner (20,145,675) at the shaft entrance and never reaches the 2900 window
// (y 693..753). With only two points no scaling law is set up; it is held as a
// measured table by uid (OPEN: 17041/17184/11412 are unmeasured, default 0.159).
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
struct ForceBox { double cx, cy, hw, hh; double k; double kRobot = 0.0; };
inline std::vector<ForceBox> g_forceBoxes;
inline double forceBoxAcc(double x, double y, double pHalf,
                          uint8_t mode = 255) {
    double a = 0.0;
    for (const auto& fb : g_forceBoxes)
        if (std::fabs(x - fb.cx) <= fb.hw + pHalf
            && std::fabs(y - fb.cy) <= fb.hh + pHalf)
            a += (mode == 5 && fb.kRobot > 0.0) ? fb.kRobot : fb.k;
    return a;
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
// ARMING IS STICKY: the counter stays positive for 500+ ticks after the box is
// behind the player (GD re-sets it every tick), and in a full pass it went off
// again 638 px past the object. The mechanism behind that range is not
// identified, so this models the arm as PERMANENT -- inside the measured span
// it is exact, and no other level owns a 2866, so nothing already green can
// move. If lv22 later diverges under a ceiling far past x=3,735, this is the
// first thing to look at.
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
