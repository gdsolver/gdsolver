#pragma once
#include <fstream>
#include <sstream>
#include <string>
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
// `c`: PlayerObject::update (0x388d80) adds dt * 0.9 * m_force_y * c to vy in float, c a per-mode literal it loads from rodata: 0.47 for the ship, 0.58 for the
// UFO, and 0.5875 / 0.725 (= c * 1.25) when the player is not full size. 0 = no such literal (the other modes' pushes ride their gravity, see above).
struct ForceUnit { double v; float c = 0.f; };
// --forceunit1x: the unit's gravity is the 1x one whatever the speed. The
// force's own step is 0.225 x m_force for a cube (the 0.945 note at the force
// application in step.hpp), and lv22 t=20,958..20,960 (mini cube, speed 1.1,
// one 4.2 box) reads +0.730 = 0.945 - 0.215 in GD where the speed's g gives
// 4.2 x 0.2239 = 0.941 and +0.726. On by default since 2026-10 (was opt-in).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
inline ForceUnit forceUnitFor(uint8_t mode, float /*dxF*/) {
    const double g = std::fabs(cubePhysFor(1.29825f).g);
    switch (mode) {
        case 0: return {g / kForceGDiv};                  // cube
        case 1: return {kForceUnitShip, 0.47f};
        case 2: return {g * 0.6 / kForceGDiv};            // ball
        case 3: return {kForceUnitUfo, 0.58f};
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
struct ForceBox {
    double cx, cy, hw, hh;
    double force;   // m_force
    int uid = -1;
    int fid = 0;    // m_forceID (property 530); 0 = none, set by --forceids
    // The push's world-y share: 1 for a box pointing up. --forceboxdir sets it from the box's
    // rotation and flipY (level_loader.hpp, the FORCE BOX branch); off, every box pushes up.
    double dirY = 1.0;
};
inline std::vector<ForceBox> g_forceBoxes;
// --forceids <file>: the forceblocks dump (uid,...,forceid). GD sums the boxes a
// player overlaps, but a box with m_forceID > 0 counts once per tick per ID:
// collisionCheckObjects reads obj+0x750 and skips the box if the ID is already
// in the player's set at +0xb98 (0x215b01-0x215b40), then adds
// calculateForceToTarget into +0xb8c. lv22 t=20,961: uid17041 and uid17184, two
// 4.2 boxes stacked at x=21,555 with forceid 1, push a mini cube +0.730/tick in
// GD where the model, summing both, gave +1.666. The only boxes in lv1-22 with
// an ID. Not given: every fid is 0 and the sum is as before.
inline std::string g_forceIdsPath;
struct ForceField {
    double cx, cy, R;
    int dir;                // -1 = push world-down
    int uid = -1;
    double force = 0.0;     // m_force (objrects' `force` column)
    double dirY = 1.0;      // the y share of its push, as --forceboxdir reads a 2069
    bool constLaw = false;  // forceblocks.txt: neither target mode nor range (--forcecircle)
    int fid = 0;            // its force ID (forceblocks.txt), one push per ID per tick
    // the recording's rows for this uid (--forcelive): tick, centre x, centre y, on
    std::vector<int> liveT;
    std::vector<float> liveX, liveY;
    std::vector<uint8_t> liveOn;
};
inline std::vector<ForceField> g_forceFields;
inline int g_forceCircles = 0;   // the fields with constLaw (applyForceIds)
// --forcecircle (speed.hpp): GD's circle contact for a 3645, behind the collision sections' gate.
inline bool forceCircleTouch(const ForceField& ff, double fx, double fy, double x, double y,
                             double pHalf) {
    auto cell = [](double v) { return v <= 0.0 ? 0 : (int)(v * 0.01); };   // gdSectionCell
    if (std::abs(cell(fx) - cell(x)) > 1) return false;
    if (g_dynamicLevelHeight && std::abs(cell(fy) - cell(y)) > 1) return false;
    const double dx = x - fx, dy = y - fy;
    if (g_fixRadiusCollision) return dx * dx + dy * dy <= (pHalf + ff.R) * (pHalf + ff.R);
    if (std::fabs(dx) <= pHalf && std::fabs(dy) <= pHalf) return true;   // its centre inside the player's rect
    for (int k = 0; k < 4; ++k) {
        const double cx = (k & 1) ? x + pHalf : x - pHalf, cy = (k & 2) ? y + pHalf : y - pHalf;
        if ((cx - fx) * (cx - fx) + (cy - fy) * (cy - fy) < ff.R * ff.R) return true;
    }
    return false;
}
// --forcelive (speed.hpp): where the field is on the recording's row t, and whether it was on at row t - 1; its
// load position, on, without rows. A tick's push comes from the last tick's contact (the counter collisionCheckObjects
// sets), so the caller passes the step's tick - 1: the field's position is that tick's row, and a Toggle that turns it
// off lands after that tick's collision pass, so the switch reads the row before. A custom level t=15,447: a 3645
// following the player is turned off on row 15,447; GD still pushes on 15,448 and not on 15,449.
inline bool forceFieldAt(const ForceField& ff, long long t, double& fx, double& fy) {
    fx = ff.cx;
    fy = ff.cy;
    if (!g_forceLive || t < 0 || ff.liveT.empty()) return true;
    auto rowAt = [&](long long tt) {
        size_t lo = 0, hi = ff.liveT.size();
        while (lo < hi) {
            const size_t m = lo + (hi - lo) / 2;
            if ((long long)ff.liveT[m] <= tt) lo = m + 1; else hi = m;
        }
        return lo;   // one past the row in force at tt (0: none yet)
    };
    const size_t lp = rowAt(t);
    if (lp > 0) {
        fx = (double)ff.liveX[lp - 1];
        fy = (double)ff.liveY[lp - 1];
    }
    const size_t lo = rowAt(t - 1);
    return lo == 0 || ff.liveOn[lo - 1] != 0;
}
inline double forceCircleSum(double x, double y, double pHalf, long long t = -1) {
    double f = 0.0;
    int seen[8];
    int ns = 0;
    for (const auto& ff : g_forceFields) {
        if (!ff.constLaw) continue;
        double fx, fy;
        if (!forceFieldAt(ff, t, fx, fy)) continue;   // switched off on that row
        if (!forceCircleTouch(ff, fx, fy, x, y, pHalf)) continue;
        if (ff.fid > 0) {
            bool dup = false;
            for (int k = 0; k < ns; ++k)
                if (seen[k] == ff.fid) { dup = true; break; }
            if (dup) continue;
            if (ns < 8) seen[ns++] = ff.fid;
        }
        f += ff.force * ff.dirY;
    }
    return f;
}
inline double forceBoxSum(double x, double y, double pHalf) {
    double f = 0.0;
    int seen[8];
    int ns = 0;
    for (const auto& fb : g_forceBoxes)
        if (std::fabs(x - fb.cx) <= fb.hw + pHalf
            && std::fabs(y - fb.cy) <= fb.hh + pHalf) {
            if (fb.fid > 0) {
                bool dup = false;
                for (int k = 0; k < ns; ++k)
                    if (seen[k] == fb.fid) { dup = true; break; }
                if (dup) continue;
                if (ns < 8) seen[ns++] = fb.fid;
            }
            f += fb.force * fb.dirY;
        }
    return f;
}
inline void applyForceIds(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::printf("forceids: cannot read %s\n", path.c_str());
        return;
    }
    std::string line;
    std::getline(in, line);
    std::vector<std::string> hdr;
    {
        std::stringstream ss(line);
        std::string c;
        while (std::getline(ss, c, ',')) hdr.push_back(c);
    }
    int iu = -1, ifd = -1, irel = -1, irng = -1;
    for (int k = 0; k < (int)hdr.size(); ++k) {
        if (hdr[k] == "uid") iu = k;
        if (hdr[k] == "forceid") ifd = k;
        if (hdr[k] == "relative") irel = k;
        if (hdr[k] == "range") irng = k;
    }
    if (iu < 0 || ifd < 0) {
        std::printf("forceids: %s has no uid/forceid column\n", path.c_str());
        return;
    }
    int set = 0;
    while (std::getline(in, line)) {
        std::vector<std::string> f;
        std::stringstream ss(line);
        std::string c;
        while (std::getline(ss, c, ',')) f.push_back(c);
        if ((int)f.size() <= std::max(iu, ifd)) continue;
        const int uid = std::atoi(f[iu].c_str());
        const int fid = std::atoi(f[ifd].c_str());
        for (auto& fb : g_forceBoxes)
            if (fb.uid == uid) { fb.fid = fid; if (fid > 0) ++set; }
        // --forcecircle: a 3645 with neither column set pushes at a constant m_force
        if (irel >= 0 && irng >= 0 && (int)f.size() > std::max(irel, irng))
            for (auto& ff : g_forceFields)
                if (ff.uid == uid) {
                    ff.fid = fid;
                    ff.constLaw = std::atof(f[irel].c_str()) == 0.0 && std::atof(f[irng].c_str()) == 0.0;
                    if (g_forceCircle)
                        std::printf("forcecircle: uid %d R=%.3f m_force=%.3f dirY=%.3f %s\n", uid, ff.R, ff.force,
                                    ff.dirY, ff.constLaw ? "constant push" : "range/target - left to the field");
                }
    }
    g_forceCircles = 0;
    for (const auto& ff : g_forceFields) g_forceCircles += ff.constLaw ? 1 : 0;
    std::printf("forceids: %d box(es) carry a force ID\n", set);
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
// `t` is the step's tick, -1 for none; a moving 3645 is read at the row before it (--forcelive, forceFieldAt).
// --tptrigforce: set for the step after a teleport TRIGGER moved the body from a place no force reached
// (step.hpp, State::tpSkip bit 2). The push PlayerObject::update adds is what the previous tick's
// collisionCheckObjects summed into +0xb8c/+0xb90 (cleared at the end of update), and a crossing or touch
// teleport trigger runs after that pass (checkSpawnObjects, after checkCollisions), so the pass saw the old
// place. The model reads the boxes at the start-of-tick point, which is the target. A custom level t=22,979:
// a spawned teleport trigger puts a cube at a ship portal inside a 3.241 box; GD's first ship tick reads
// 7.442 (no push) where the model gave 7.807. Per thread, and only for the length of that step.
// On by default; --no-tptrigforce is the off arm.
inline bool g_tpTrigForce = true;
inline thread_local bool t_forceMute = false;
// --forceraw (ON since 2026-10-24; --no-forceraw is the off arm): the ship's and the UFO's push is added to the velocity as GD computes it, not rounded to the 0.001 grid on its own first. GD:
// vy += (float)((float)((float)(dt * 0.9f) * F) * c) with F the box's m_force summed (float) and c = 0.47 (ship) / 0.58 (UFO), x 1.25 when mini; the model then snaps the
// sum with the gravity (stepVy + push, qVy). Rounding the push alone first moves a sum that lies on half a grid step: a custom level's box (m_force 2) pushes
// 0.2114999890 in GD (0.225f * 2 * 0.47f in float, just under 0.2115), the model gave q(2 * 0.1058) = 0.212, and the release (+0.103) came out +0.315 a tick where
// GD's vy goes up by 0.314 (ten ticks of 32,603..32,615, a flipped ship) -- a pixel high 250 ticks later, into a hazard (t=32,849). The same law reads the rigs (calib_forcedrop_ship
// f1/f2/f5/f10, GD 2.209 at 1x/3x/4x: +0.003 / +0.108 / +0.426 / +0.955 beside -0.103, one unit 0.10575 in all of them, mini f10 +1.200 beside -0.122 = 0.5875 * 0.225 * 10)
// where the rounded product disagreed with f2 or f10 by one grid step for any single unit.
// The formula and the literals were read from the 2.2081 executable (PlayerObject::update at 0x388d80, setYVelocity at 0x388d10); the rig measurements and the level that
// shows the defect are the 2.209 game (the workers of the package), and the same law reproduces them, so 2.209 is taken to carry the same code here.
inline bool g_forceRaw = true;
inline double forceBoxAcc(double x, double y, double pHalf, ForceUnit unit, long long t = -1) {
    if (t_forceMute) return 0.0;
    if (g_forceRaw && unit.c > 0.f) {
        const double F = forceBoxSum(x, y, pHalf) + (g_forceCircle ? forceCircleSum(x, y, pHalf, t >= 0 ? t - 1 : -1) : 0.0);
        return (double)((float)(0.225f * (float)F) * unit.c);
    }
    return qVy((forceBoxSum(x, y, pHalf)
                + (g_forceCircle ? forceCircleSum(x, y, pHalf, t >= 0 ? t - 1 : -1) : 0.0)) * unit.v);
}
// Is there a push to look up at all: the force boxes, and under --forcecircle the constant 3645s.
inline bool forcePushOn() { return !g_forceBoxes.empty() || (g_forceCircle && g_forceCircles > 0); }
inline double forceFieldAccAt(double x, double y, double pHalf);
inline double forceFieldAcc(double x, double y, double pHalf) {
    return t_forceMute ? 0.0 : forceFieldAccAt(x, y, pHalf);
}
// Does the collision pass at (x, y) leave no push for the next tick -- the boxes, the constant 3645s and the
// fields, read whether or not the current step is muted.
inline bool forceFreeAt(double x, double y, double pHalf, long long t) {
    if (forcePushOn() && forceBoxSum(x, y, pHalf) + (g_forceCircle ? forceCircleSum(x, y, pHalf, t) : 0.0) != 0.0)
        return false;
    return g_forceFields.empty() || forceFieldAccAt(x, y, pHalf) == 0.0;
}
inline double forceFieldAccAt(double x, double y, double pHalf) {
    double a = 0.0;
    for (const auto& ff : g_forceFields) {
        if (g_forceCircle && ff.constLaw) continue;   // forceBoxAcc pushes it (forceCircleSum)
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
// --groundforce: a grounded body in a force box or field whose push outpulls gravity leaves the ground on that
// tick. GD's PlayerObject::update adds the force after updateJump has taken the gravity step (the note at the
// force in step.hpp's air branch), and nothing in that order asks whether the body stands on something; the
// collision pass seats it again only if it has not risen. A custom level: a ball lands at y 1,335 in a row of
// 1.983 boxes and GD reads vy 0.139 on the next tick (0.268 push - 0.129 gravity), onGround 0, y 1,335.031,
// where the model kept it seated at 0. Returns the player-frame vy of the lift-off, or 0 for none. Not while
// dashing (the dash has its own branch).
inline bool g_groundForce = true;
// --forceboost: ...and a push raises the velocity-limit exemption (State::boost, GD's +0x952): right after it
// sets the velocity, PlayerObject::update writes +0x952 whenever the force is not 0 (0x389307-0x389310).
// updateJump clears the byte at its head only for a vy inside (-6.4, 8.0), so a ship or UFO pushed past its
// terminal keeps climbing. A custom level: a ship in a column of 1.983 boxes reads 8.027, 8.134, 8.241 ... in
// GD (0.210 push - 0.103 gravity a tick) where the model clamped at 8 and added the push, 8.210 every tick.
inline bool g_forceBoost = true;
inline double groundLift(double gAcc, double gTerm, double x, double y, double pHalf, uint8_t mode, double dx,
                         double gdSign, double tScale, long long t, bool dashing) {
    if (!g_groundForce || dashing) return 0.0;
    double f = 0.0;
    if (forcePushOn()) f += forceBoxAcc(x, y, pHalf, forceUnitFor(mode, (float)dx), t) * gdSign;
    if (!g_forceFields.empty()) f += forceFieldAcc(x, y, pHalf) * gdSign;
    if (f <= 0.0) return 0.0;
    const double v = qVy(std::max(gAcc * tScale, gTerm)) + f * tScale;
    return v > 0.0 ? v : 0.0;
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
// --warpcap: a warp above 1 leaves the tick alone. GJBaseGameLayer::applyTimeWarp (0x2361a0) writes the value
// to the CCScheduler's time scale as well, so more than 1 runs more ticks a second, not longer ones. A custom
// level ramps its warps 1.1, 1.2, 1.3, 1.4 every 48.6 px (and 1.05 earlier): GD's dx stays 2.4004 a tick (speed
// 1.6) across all of them, where the model went to 3.3604 = x1.4; the 0.2 of lv22 (and 0.5 .. 0.95 on the same
// level) slow it as measured. On by default; --no-warpcap is the off arm.
inline bool g_warpCap = true;
inline double timeWarpAt(double x) {
    double m = 1.0;
    for (const auto& w : g_timeWarps) {
        if (w.cx > x) break;
        m = w.mod;
    }
    return (g_warpCap && m > 1.0) ? 1.0 : m;
}
// The warp in force at x as the SCHEDULER has it, --warpcap not applied: applyTimeWarp (0x2361a0) writes the value to the
// CCScheduler's time scale whatever its size, and the action manager (the dash art's spin, --balldashsprite) runs on that
// scaled dt, a warp above 1 included.
inline double rawTimeWarpAt(double x) {
    double m = 1.0;
    for (const auto& w : g_timeWarps) {
        if (w.cx > x) break;
        m = w.mod;
    }
    return m;
}
// --wavewarp: the wave's line under a warp is the warped x's. Its y step was dx (x2 mini) unscaled while x took the
// warp, so a warped wave climbed and dived at 1 / warp times GD's angle. A custom level's dual wave under a 0.4 warp
// (x=675): GD's rows step x and y by 0.780 a tick (vy shown -7.8 = 1.3 speed, unwarped), the model's by 0.780 and 1.950.
// lv22's warps sit outside its wave sections.
inline bool g_waveWarp = true;
// --warpfly: the ship and the UFO integrate under the warp too (step.hpp, the flight branch). Their
// branch took the warped dx but stepped vy and y at full time. A custom level's warp at x=2,130
// (tw 0.36, an upright ship at speed 0.7): GD's dx drops 1.0464 -> 0.3767 and its vy steps go
// -0.069 -> -0.025 released and +0.108 -> +0.039 held, all 128 ticks inside the warp equal to
// qVy(vy + rung x 0.36), and y follows 0.225 x vy x 0.36. The switch speed between the weak and
// strong rungs is a velocity and stays put: released at vy +0.82 inside the warp, GD steps the weak
// -0.025, not a scaled strong rung. Measured on the upright ship only, so it acts on the ship alone.
// --warpflyufo extends it to the UFO the same way (its flap target is a velocity and is left alone,
// its gravity is a rung). Witness: the warp_fly_rig cells below (a UFO at tw 0.36 / 0.5 / 0.8 is exact to
// 0.000 over ~1,300 ticks each). --warpfly is on by default since 2026-10-06 (--no-warpfly is the off arm);
// --warpflyufo since 2026-10-07 (--no-warpflyufo is the off arm).
inline bool g_warpFly = true;
inline bool g_warpFlyUfo = true;   // on since 2026-10-07 with --warpslowonly (--no-warpflyufo is the off arm)
// --warpslowonly (default off): the flying branch's warp factor is min(tw, 1). Measured on a calibration rig
// (oneoff warp_fly_rig.py: a floor, a Time Warp trigger, a hover plan, GD against leveldp over 1,000-1,300 ticks
// per cell, the control section before the warp exact at 0.000 in every cell): a SHIP at tw 0.36 and 0.7 steps
// scaled (--warpfly right, 0.000 over the warp), but at tw 1.3 it steps UNSCALED (--warpfly 28.5 px out, off
// 0.000); a UFO at tw 0.36 / 0.5 / 0.8 steps scaled (--warpflyufo 0.000), at 1.05 / 1.3 / 2.0 unscaled
// (--warpflyufo up to 50.8 px out, off 0.000). GD time-scales these two modes only for a warp slower than 1.
// (tw 2.0 on the ship is out by up to 4.8 px near the warp's end under either arm; not modelled here.)
inline bool g_warpSlowOnly = true;   // on since 2026-10-07 (--no-warpslowonly is the off arm)
// The velocity a teleport (a portal, the trigger 3022, the orb 3027) gives the body after the move,
// GJBaseGameLayer::teleportPlayer (0x20fdb0, 0x2103e7..0x2105c8), from the objrects columns
// tpfm / tpf / tpang / tprmod / tprmin / tprmax (--tppush). fm bit 0: a static force (345) of
// strength f along the direction ang (degrees, as the export computes it from the exit); bit 1: it
// adds to vy (443) instead of setting it; bit 2: a redirected force (347), which wins -- the body's
// speed turned to ang, times rmod, held between rmin and rmax where they are set
// (PlayerObject::redirectPlayerForce, 0x39fc60). 0 = the teleport leaves the velocity alone. Read
// for the portals and the trigger; the orb's is not measured and the loader only reports it.
struct TpForce {
    uint8_t fm = 0;
    float f = 0.f, ang = 0.f, rmod = 0.f, rmin = 0.f, rmax = 0.f;
    int exitUid = 0;   // --tpexitrot: the exit object the direction was read off (0 = none found)
};
// --tpexitrot: the direction a teleport's force takes is read off its exit's rotation when the teleport happens
// (teleportPlayer 0x210105..0x210173: (portal-like exit ? 180 : 90) - getRotation()), and the export reads it at
// load. An exit a Rotate turns since then points the force elsewhere: tpang - (the exit's turn so far). A custom
// level: teleport portal uid 12 (static force 15) exits at decorations of group 1, which a Rotate at x 1,545 turns
// -45 + 2 x 360 degrees over 1.7 s; GD's cube leaves the exit at vy 10.6066 (= 15 sin 45), the model at 0
// (tpang 360 from the load), and fell where GD climbed. The exit is found by its position (tpex / tpey) among the
// objects a Rotate turns; its Rotates (g_rotTailParts, not lockrot) give the turn by their fire ticks.
inline bool g_tpExitRot = true;
struct TpExitRot { int a; double total, durT, erate; int ease; };
inline std::unordered_map<int, std::vector<TpExitRot>> g_tpExitRots;   // exit uid -> its Rotates (step.hpp tpExitTurn)
// --tpexitmove: the target is the exit's real position when the teleport happens (teleportPlayer 0x20ff6a), and the
// export reads it at load. An exit a Move carries since then is somewhere else: tpex / tpey + the Moves' offset so
// far. A custom level: teleport portal uid 11494 exits at uid 11496 (groups 51 and 52), which a Move at x 6,465
// lowers by 150 over 1 s; GD's cube lands at 3,285, the model at the load's 3,435, and left the level from there.
// The exit is found by its position among the objects an x-crossing Move carries (g_autoTrig's ctl rows).
// On by default; --no-tpexitmove is the off arm.
inline bool g_tpExitMove = true;
struct TpExitMove { int a; float dx, dy; double durT; int ease; double erate; };
inline std::unordered_map<int, std::vector<TpExitMove>> g_tpExitMoves;   // teleport uid -> its exit's Moves
// TELEPORT TRIGGER (object id 3022, --teleporttrig). EffectGameObject::triggerObject (0x4a5f30)
// sends it to GJBaseGameLayer::teleportPlayer (0x20fdb0) with no player, which takes m_player1
// (layer+0xda0) and moves it to the target group's object -- the objrects tpex/tpey, the same
// columns a teleport portal's exit uses. Measured on the calib_teleporttrig rig, every mode, no
// input: it fires on the tick whose move carries the player's x past the trigger, and at the end
// of that tick the player stands at the target -- y exactly, and x exactly too unless m_ignoreX
// (352) keeps it, so a trigger with its target straight above still pulls x back by what the
// tick overshot (720.533 -> 720.000). vy is untouched (falling at -6.480 it carries on to -6.696);
// m_ignoreY (353) keeps y; m_gravityMode (354) sets the gravity as a teleport portal's does.
// m_saveOffset (351) is not in the dump; the static / redirected forces (345 / 347) are read
// under --tppush (force).
struct TpTrig {
    double cx, tx, ty;
    uint8_t ignX, ignY, grav;
    int uid;
    TpForce force{};
    int backIdx = -1;   // --tpback: its entry in g_tpBackHi when it moves x back (cli.hpp), else -1
};
// --tpback: a teleport whose exit lies BEHIND moves x back too -- a teleport portal of the 2902 family, a
// teleport trigger by its crossing, its touch or its chain. GD's crossing queue does not follow:
// GJBaseGameLayer::checkSpawnObjects (0x21a8f0) walks each channel's triggers in x order from an index that
// only moves forward, firing one once the player's x reaches its start x (+0x708). So a trigger already
// passed does not fire again, and the next one fires once x is past the farthest x reached. The model reads
// every crossing (the teleport and gravity triggers, the time warps, the camera mode) at crossX: the state's
// x, or that farthest x while it is behind it (State::tpBack, an index into g_tpBackHi). The farthest x of a
// jump is its source's: a trigger's cx, a box's or a portal's left edge -- fixed per source, so the table is
// built at load and the search only reads it. An anchor after such a jump is seeded from the mod's
// --startxhi (the farthest x of the attempt). One body in frame 0.
inline bool g_tpBack = true;
// --tptrigonce (ON since 2026-10-24; --no-tptrigonce is the off arm): every crossing teleport trigger carries the farthest-x entry (--tpback) of its own x, so it fires once.
// GD's crossing queue (checkSpawnObjects 0x21a8f0) walks a channel from an index that only moves forward: a trigger it has fired is
// not fired again whatever the player's x does afterwards. The model has no such mark; it reads a crossing as `cx > x_before` and
// `cx <= x_after`, and a trigger that moves x back by what the tick overshot is only kept from firing again when it has a
// farthest-x entry, which the loader gave to the triggers whose target lies more than 0.5 px behind. A trigger whose target is its own
// position has none: the dump writes cx and the target with four decimals, the float x the teleport leaves is a hair below cx
// (cx 18074.8008, float 18074.80078125), and the trigger fires again on every tick -- y put back to the target, x held, vy running down
// to -15. Measured on a custom level at x=18,074.8, t=10,141: GD's row 10,141 is the target and 10,142 runs on at +1.95; the model's replay
// of the same plan held (18074.80078, 330.5) from 10,141 to the end of the run, and every route over the spot was judged on a fall
// that GD never makes (the deaths of x 18,123-18,185 at vy -15). The entry only takes effect on a firing that leaves x behind its
// own position (State::tpBack is set when the target is behind the x the tick reached), so a trigger whose target lies ahead is
// unchanged. Hangs on --tpback.
inline bool g_tpTrigOnce = true;
// --tpjumpnext: ...and a teleport that moves x FORWARD (a portal's exit, an orb's, a trigger's by its crossing or
// its touch) leaves the x crossings it passed to the NEXT tick. Measured on a rig of its own (not shipped:
// a cube at 1x jumped from about 578 to 720): gravity triggers inside the jump (690) and on its target (720) fire
// on the tick after the orb's, and the same after a teleport trigger's; a 2900 there turns the frame on that tick
// too (vy 5.193); an ordinary crossing fires on its own tick. So a tick's crossings end where the jump left from
// and the next tick's start there (State::tpFromX, step.hpp crossBaseX). A custom level t=1,158: an orb puts a
// robot on a 2900 at (1605, 570); the search turned a tick early and the replay never did. Gravity and time
// warps were already right (read by position). One body in frame 0. On by default; --no-tpjumpnext is the off arm.
inline bool g_tpJumpNext = true;
// --tptrigoff (cli.hpp, after the load): a crossing teleport trigger that a Toggle may have switched off by the time
// the player reaches it is left out. A custom level: an Item Compare at x 5 spawns a Toggle-off for the trigger at
// x 31 (and a Toggle-on for the one at x 11); GD takes the one at 11 to x 2,087 and never the one at 31, which
// --tpjumpnext would otherwise fire the tick after that jump. On by default; --no-tptrigoff is the off arm.
inline bool g_tpTrigOff = true;
inline std::vector<double> g_tpBackHi;
inline double g_startXhi = std::nan("");   // --startxhi; NaN = not said
inline double crossXOf(double x, uint8_t back) {
    return (back && back <= g_tpBackHi.size()) ? std::max(x, g_tpBackHi[back - 1]) : x;
}
// the entry a jump back from a source leaves: the farther of the state's and the source's
inline uint8_t tpBackAfter(uint8_t back, int idx) {
    if (idx < 0 || idx + 1 > 255) return back;
    if (back && back <= g_tpBackHi.size() && g_tpBackHi[back - 1] >= g_tpBackHi[(size_t)idx]) return back;
    return (uint8_t)(idx + 1);
}
// ...and the portals' and orbs' entries, by uid (cli.hpp); a trigger or a box carries its own
inline std::unordered_map<int, int> g_tpBackObj;
inline int tpBackObjIdx(int uid) {
    const auto it = g_tpBackObj.find(uid);
    return it == g_tpBackObj.end() ? -1 : it->second;
}
// at load: the entry for a source's farthest x (one per distinct value)
inline int tpBackIdx(double hi) {
    for (size_t i = 0; i < g_tpBackHi.size(); ++i)
        if (g_tpBackHi[i] == hi) return (int)i;
    if (g_tpBackHi.size() >= 254) return -1;
    g_tpBackHi.push_back(hi);
    return (int)g_tpBackHi.size() - 1;
}
inline std::vector<TpTrig> g_tpTrigs;   // sorted by cx at load
// --tptrigspawn: a SPAWNED teleport trigger (spawn=1, not touch) goes on the crossing list at the x where
// an x-crossing chain fires it (triggers.hpp spawnFiresOf: an x-crossing Spawn 1268 or a Toggle 1049 that
// turns its group on, through spawn-triggered Spawns and Toggles, with the chain's spawn delays). 16 of the
// cov126 sample's 127 levels hold one whose chain starts at an x crossing (11 with no delay). A custom level
// sends the cube to y 1,695 from a Spawn at x 9,369: the model, which left the trigger out, ran on below GD
// from there to the end; with it the replay of that attempt meets GD again. The loader
// keeps the candidates (cx = the trigger's own x); cli.hpp resolves them after the load. A target behind
// the x it fires at stays out, as a crossing trigger's does.
inline bool g_tpTrigSpawn = true;
inline std::vector<TpTrig> g_tpSpawned;
// --tptouch: a teleport trigger set off by TOUCH -- its own box (touch=1), or the box of a touch-triggered Spawn
// or Toggle-on whose chain spawns it with no delay -- moves the state whose box overlaps it in the collision
// pass, at the end of that tick as the crossing trigger does (step.hpp). State::tpTouch keeps the last box
// fired (index + 1), so a target inside its own box does not fire it again. A target behind the box stays out.
// --gravtouch takes the gravity triggers such a chain spawns the same way. One body in frame 0.
inline bool g_tpTouch = true;
struct TpTouchBox {
    double cx, cy, hw, hh;   // the box the player overlaps (the trigger's, or the chain root's)
    TpTrig t;                // what it does; t.uid is the teleport trigger
    int boxUid;              // the box's own uid (the trigger, or the chain's root)
    bool event = false;      // --eventtrig: fired by an event, never by an overlap (cx is where it listens from)
};
inline std::vector<TpTouchBox> g_tpTouchBoxes;   // sorted by cx; the event entries are appended after
// --eventtrig: an Event trigger (3604) crossed on the queue registers a listener (activateEventTrigger 0x232110);
// when one of its events happens, gameEventTriggered (0x231ff0) spawns its target group on the spot. The model
// takes the events a state's own pads and orbs raise -- PadActivated (9) and OrbActivated (8) for any, and the
// one each kind has (YellowPad 45 .. SpiderPad 49, YellowOrb 34 .. TeleportOrb 44: GJGameEvent) -- on the tick
// the pad or orb fires (State::usedPad / usedOrb gaining it), once the state's crossing x is past the trigger's.
// What the group spawns with no delay: a gravity trigger takes State::gravTouch and a teleport trigger the
// touch teleport's end-of-tick move, each through an entry no overlap fires (gravity uid -1, teleport event).
// An Event trigger with an extra ID (447 / 525) filters on more than the event and stays out. A custom level: a
// listener for RedPad (47) at x 29,145 spawns a 0.32 gravity trigger; GD's gravityMod goes 1 -> 0.32 on the
// tick its cube takes a red pad (vy 20), and the model, without it, rose at the full gravity from there.
// One body in frame 0.
inline bool g_eventTrig = true;
struct EventFire {
    std::vector<int> events;
    double listenX;
    uint8_t grav;   // 1: g_gravTouchBoxes[idx], 0: g_tpTouchBoxes[idx]
    int idx;
};
inline std::vector<EventFire> g_eventFires;
inline int gdEventOfPad(int id) {
    switch (id) {
        case 35: return 45;     // YellowPad
        case 140: return 46;    // PinkPad
        case 1332: return 47;   // RedPad
        case 67: return 48;     // GravityPad
        case 3005: return 49;   // SpiderPad
        default: return 0;
    }
}
inline int gdEventOfOrb(int id) {
    switch (id) {
        case 36: return 34;     // YellowOrb
        case 141: return 35;    // PinkOrb
        case 1333: return 36;   // RedOrb
        case 84: return 37;     // GravityOrb
        case 1022: return 38;   // GreenOrb
        case 1330: return 39;   // DropOrb (black)
        case 1594: return 40;   // CustomOrb (toggle)
        case 1704: return 41;   // DashOrb
        case 1751: return 42;   // GravityDashOrb
        case 3004: return 43;   // SpiderOrb
        case 3027: return 44;   // TeleportOrb
        default: return 0;
    }
}
// GRAVITY TRIGGER (object id 2066, --gravtrig). EffectGameObject::triggerObject (0x4a5f30, case
// 0x4a6e1c) writes the trigger's m_gravityValue (148, the objrects `gmod`) into the player's
// m_gravityMod (+0xb84), and PlayerObject::updateJump (0x38b900) multiplies the gravity it
// integrates by it -- the cube's m_gravity and the other modes' 0.958199 alike. Measured on the
// calib_gravtrig / calib_gravtrig2 rigs, every mode:
//   - the dump's gravityMod changes on the tick whose move crosses the trigger, and the vy step
//     takes the new value from the NEXT tick (in the air at 0.5: -0.216, then -0.108) -- the time
//     warp's rule, so the value in force over a tick is the one at the tick's starting x;
//   - the step is the UNROUNDED gravity times the value, then the 0.001 grid: a cube at 2 falls by
//     0.431 a tick (2 x 0.215595), not 2 x 0.216; at 0.1 by 0.022;
//   - only gravity moves: the ship's thrust is the same at 0.5 as at 1, and no impulse changes
//     (the cube's 11.180, the ball's 3.354, the robot's 5.590, the UFO's flap).
// A trigger for player 2 only (gpl bit 1) leaves player 1 alone; m_followCPP (bit 2) is player 1
// in a run with one body.
struct GravTrig {
    double cx, mod;
    int uid;
};
inline std::vector<GravTrig> g_gravTrigs;   // sorted by cx at load
// --gravspawn: a SPAWNED gravity trigger (spawn=1, not touch, not player 2 only) fired by an x-crossing
// Spawn trigger (1268, no touch, no spawn, delay 0) whose target group holds it takes effect at that Spawn's
// x, by the crossing rule above. A custom level: an x-crossing 2066 sets 0.57, a Spawn at x 23,355 fires a
// spawned 2066 at 1.0 in its group, and GD's gravityMod goes 0.57 -> 1 on the tick whose move crosses
// 23,355 in all three attempts that got there (the model, without it, kept 0.57 and fell at -0.123 where
// GD fell at -0.216). A Spawn with a delay, a touch or a spawned Spawn is not followed. The loader keeps
// the candidates; cli.hpp resolves them against triggers.txt / objgroups.txt after the load.
inline bool g_gravSpawn = true;
// --gravchain: ...and the Spawn is found by the chain walk --tptrigspawn uses (spawnFiresOf): a spawned
// Spawn or a Toggle that turns the group on is followed back to its x crossing, and a delay moves the
// x it takes effect at. Off: only an x-crossing Spawn with delay 0 that targets the trigger's group.
inline bool g_gravChain = true;
struct GravSpawned {
    double mod;
    int uid;
    double cx = 0.0;   // the trigger's own x (an ordered Spawn's queue reads it)
};
inline std::vector<GravSpawned> g_gravSpawned;
inline double gravModAt(double x) {
    double m = 1.0;
    for (const auto& g : g_gravTrigs) {
        if (g.cx > x) break;
        m = g.mod;
    }
    return m;
}
// --gravtouch: a gravity trigger set off by TOUCH (touch=1, not for player 2 only) is fired by the state whose
// box overlaps the trigger's rect in the collision pass -- plain rect overlap, closed edges, as every touch
// trigger (triggers.hpp TouchTrig, --ctrltouch) -- and State::gravTouch keeps the last one (index + 1 into
// g_gravTouchBoxes). Its value is in force from the next tick, the crossing rule's +1, until an x-crossing
// trigger past the box's left edge takes over (gravModOf). 28 of the cov126 sample's 127 levels hold one. An
// anchor is seeded from GD's own m_gravityMod there (--startgmod, the mod writes it): where it differs from
// what the crossing triggers give, an entry at the anchor's x that no box overlap reaches stands for it.
// One body in frame 0, as --ctrltouch.
inline bool g_gravTouch = true;
struct GravTouchBox {
    double cx, cy, hw, hh, mod;
    int uid;   // -1: the anchor's seed (--startgmod), never overlapped
    // --boxmoves: the touch boxes (g_touch indices) whose Toggle switches this trigger's group OFF; the trigger fires nothing once one of them has fired.
    std::vector<int> offBits;
};
inline std::vector<GravTouchBox> g_gravTouchBoxes;   // sorted by cx at load; a seed is appended after
inline double g_startGmod = std::nan("");   // --startgmod; NaN = not said
inline double gravModOf(double x, uint8_t touch) {
    double m = 1.0, last = -1e300;
    for (const auto& g : g_gravTrigs) {
        if (g.cx > x) break;
        m = g.mod;
        last = g.cx;
    }
    if (touch && touch <= g_gravTouchBoxes.size()) {
        const GravTouchBox& b = g_gravTouchBoxes[touch - 1];
        if (last < b.cx - b.hw) m = b.mod;
    }
    return m;
}
// FOLLOW PLAYER Y (object id 1814, --followy). GJBaseGameLayer::processPlayerFollowActions
// (0x22ded0), which processMoveActionsStep calls once a tick before the player moves, adds to every
// object of the target group
//     clip(k * (P + offset - y), +-maxStep),   k = clamp(dt*60*speed, 0, 1),  maxStep = dt*60*max
// (no cut when max is 0), where P is player 1's y as the previous tick left it and dt*60 is 0.25
// at 240 ticks a second. Measured on the calib_followy rig, every unit within the recorder's 0.05:
// speed 1 under a jumping cube and a flying ship, 0.5 with an offset of 30, a max of 1 (0.25 a tick
// exactly), 10 (k = 1, the player's own y), and two objects of one group starting 240 px apart,
// each closing on the player by itself. The first move is on the tick after the one whose move
// crosses the trigger's x. Each object closes on the same target, so every object of the group is
//     y = F + (y0 - y0ref) * (1 - k)^n
// with F the reference object's own y (State::followF, started at y0ref on the crossing tick) and n
// the ticks since -- exact without a max; with one the objects that start away from the reference
// are approximate for as long as the cut holds. A delay (property 91) reads P from a 200-slot ring
// of past y the update loop advances every 0.01 s (the rig's delay unit matches that too), which a
// state cannot carry: such a trigger is not loaded. Neither is a touch- or spawn-triggered one.
// A state holds kFollowYSlots values; a trigger takes a slot whose last trigger's run has ended by
// its x (FollowY::endX, at the level's fastest speed, so a slot is handed on late rather than
// early), and one that finds none is not loaded. The objects of a trigger whose slot has been
// handed on stay where the recording puts them.
constexpr int kFollowYSlots = 2;
struct FollowY {
    double cx;            // fires on the tick whose move crosses it
    int uid, group;
    double k, offset, maxStep;
    double durTicks;      // the command's duration in ticks (dur * 240)
    double endX;          // the x its run is over by, at the level's fastest speed
    int slot;             // which State::followF it lives in
    double y0ref;         // the reference object's load-time y: F's start
    int refUid;
    long long tAct = -1;  // the crossing tick, once a walk has seen it (model.hpp prepareGroup)
    // --followtouch: started by the player touching the trigger's own box (touch=1), per state. `cx` is then where the trigger becomes the
    // slot's owner (the box's left edge less the player's reach); tcx / tcy / thw / thh are the box.
    bool touch = false;
    double tcx = 0.0, tcy = 0.0, thw = 15.0, thh = 15.0;
    // The group's objects the model collides with, recorded or static: (uid, load-time y). The
    // first is the reference.
    std::vector<std::pair<int, double>> objs;
};
inline std::vector<FollowY> g_followYs;   // sorted by cx
// The trigger that holds `slot` at x: the last one in that slot at or behind x, -1 for none.
inline int followOwner(int slot, double x) {
    int o = -1;
    for (size_t i = 0; i < g_followYs.size(); ++i) {
        if (g_followYs[i].cx > x) break;
        if (g_followYs[i].slot == slot) o = (int)i;
    }
    return o;
}
// One object of a follow group in a step's windows: which window (0 near, 1 ports, 2 pads, 3 orbs,
// 4 slopes, 5 speeds), where in it, and the trigger and load-time y that place it.
struct FollowRef {
    uint8_t win;
    uint32_t idx;
    uint16_t cmd;
    double y0;
};
// --followsection: the objects of a Follow Player Y group that the model does NOT place (its trigger
// is not loaded -- delayed, touch- or spawn-triggered, out of slots), as (uid, load-time y). They stay
// where the recording puts them, but the game still reaches them only through their load-time
// section cell (step.hpp, the follow copies), so they ride in the windows' follow references under
// kFollowSecCmd, which places nothing. Filled only under the flag.
inline std::vector<std::pair<int, double>> g_followSecObjs;
constexpr uint16_t kFollowSecCmd = 0xFFFF;
// --bigsection (constants.hpp): the static colliders big enough to reach the player from a section cell
// outside the 3x3 the collision pass walks, as (uid, load-time y). They ride in the windows' follow
// references under kBigSecCmd, which places nothing. Filled only under the flag.
inline std::vector<std::pair<int, double>> g_bigSecObjs;
constexpr uint16_t kBigSecCmd = 0xFFFE;
// ...and the grouped ones (Dynamics), under kBigSecDynCmd: the game re-files a moved object where the
// move takes it, so their cell is read from the window's own placement, not from the load-time y.
inline std::vector<std::pair<int, double>> g_bigSecDyn;
constexpr uint16_t kBigSecDynCmd = 0xFFFD;
// Half extent past which a cell two away can reach the player: the near edge of such a cell is 100
// units from the player's own cell, less the player's half (15) and the margins.
constexpr double kBigSecHalf = 70.0;
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
// t: the tick of the row at x, for --zoomchain (-1 = not known: the reading below)
inline double camScaleAt(double x, double dx, long long t = -1);     // defined below gdEase
inline std::unique_ptr<std::atomic<long long>[]> g_zoomCrossT;   // per g_zoomTrigs entry
inline size_t g_zoomCrossN = 0;
constexpr long long kZoomCrossUnknown = -(1LL << 62);
inline void zoomCrossReset(size_t n) {
    g_zoomCrossT.reset(n ? new std::atomic<long long>[n] : nullptr);
    g_zoomCrossN = n;
    for (size_t i = 0; i < n; ++i) g_zoomCrossT[i].store(kZoomCrossUnknown, std::memory_order_relaxed);
}
// One row of a modifier box's recorded motion. Its own type rather than
// DynSample so that this header keeps depending on nothing: the loader copies
// the three fields across.
struct ModRow { int t; float cx, cy; };
struct FlipHeadBox {
    double cx, cy, hw, hh;      // LOAD-TIME position
    int uid = -1;
    // --fgarmlive: the recording's rows for this uid, ascending in t. Empty
    // when the box never moves (or when no recording was given), which reads
    // as "the load-time position is the live one".
    std::vector<ModRow> live;
    // --ridebox: the box is carried by a Move locked to the player on BOTH axes
    // (an autonomous one, from the level data), and the recording says over which
    // ticks it rode: rideT0 = its first moving row, rideT1 = its last (INT_MAX when
    // the recording ends inside the ride). -1/-1 = no ride known in this call.
    bool rides = false;
    int rideT0 = -1, rideT1 = -1;
};
inline std::vector<FlipHeadBox> g_flipHeadBoxes;
// --fgarmlive: arm from the box's LIVE position and let the arm DECAY, instead
// of arming once from the parked box and staying armed for the level.
//
// GD's side is read from the binary: collisionCheckObjects writes 2 into
// m_stateFlipGravity (player+0xb80) on contact (0x215ba1, the only setter) and
// PlayerObject::update decrements it every tick (0x389f40). So the arm lasts
// the contact tick and the one after -- kArmTicks, the same shape the id-1859
// ceiling arm already uses.
//
// What made this look like a long-lived latch for a year: lv22's only 2866,
// uid2860, RIDES THE PLAYER. Across 650 ticks of the recording it stays 1.19 px
// from the player in x -- a 36x36 box around a half-9 cube -- so GD re-arms on
// every one of them and the counter never gets to expire. The three flips GD
// makes (t=2,749 / 2,890 / 2,961) are all inside that one contact, which is why
// "active for at least 254 ticks" looked like a decay constant and was not.
//
// The two halves ship together on purpose. With the decay alone the model loses
// those three flips, because the parked box at (3735,255) is 400+ px behind by
// then; with the live lookup alone nothing changes, because the latch never
// clears anyway. They were each other's cover.
inline bool g_fgArmLive = false;
// Where the box is at tick t: the last recorded row at or before t, and the
// load-time position before the first row. A binary search rather than a
// cursor, because the frame slices each hold their own Level and would
// otherwise share one mutable position.
inline void flipHeadAt(const FlipHeadBox& b, int t, double& cx, double& cy) {
    cx = b.cx;
    cy = b.cy;
    if (b.live.empty()) return;
    size_t lo = 0, hi = b.live.size();
    while (lo < hi) {
        const size_t m = lo + (hi - lo) / 2;
        if (b.live[m].t <= t) lo = m + 1; else hi = m;
    }
    if (lo == 0) return;
    cx = (double)b.live[lo - 1].cx;
    cy = (double)b.live[lo - 1].cy;
}
inline bool flipHeadArms(double x, double y, double pHalf, int t) {
    for (const auto& b : g_flipHeadBoxes) {
        double bx = b.cx, by = b.cy;
        if (g_fgArmLive) flipHeadAt(b, t, bx, by);
        if (std::fabs(x - bx) <= b.hw + pHalf
            && std::fabs(y - by) <= b.hh + pHalf)
            return true;
    }
    return false;
}
// --ridebox (on by default since 2026-10): an arm box that RIDES THE PLAYER is not read at the
// position a recording gives it. It rode the RECORDED run's player, so for any other plan -- every
// candidate of a search that leaves the recorded flight -- that position is wrong by the
// whole difference between the two paths. Measured on lv22's cold of 2026-09-30 (bundle
// v5): the t=2,076 search was given a recording whose uid2860 had followed a run that left
// the playfield (y 757 at t=2,850 against the candidates' 290-330), so under --fgarmlive
// no low candidate was ever armed, all of them died at x~4,030-4,105 on the first ceiling
// or side contact, and the only survivor was the arc into the sky. Same flags without
// --fgarmlive crossed that wall from the same anchor and inputs.
//
// WHAT GD DOES, read from the binary (2.2081):
//   - the arm is renewed on every tick of contact: collisionCheckObjects' per-object loop
//     writes 2 into m_stateFlipGravity (player+0xb80) for an id-2866 (0x215ba1, on
//     `cmp ecx,0xb32`) and into m_stateHitHead (+0xb7c) for an id-1859 (0x215ac6, on
//     `cmp ecx,0x743`); PlayerObject::update decrements both every tick (0x389f40 /
//     0x389f47). So: armed on the contact tick and the one after (kArmTicks).
//   - a Move locked to the player moves its group by the player's own displacement:
//     GJBaseGameLayer::update stores player.position - player.m_lastPosition into the
//     effect manager (+0x800 x, +0x804 y; 0x238222-0x2382b2, with a y-only teleport
//     guard at 0x23823f-0x238266), and GJEffectManager::prepareMoveActions, for a move
//     command with m_lockToPlayerX (+0x73) / m_lockToPlayerY (+0x74), takes that times
//     the move's mod as the step's delta. So the group keeps a FIXED offset from the
//     player for the whole ride -- the offset it had when the Move fired.
//   - measured against the trunk clear of 2026-09-30 (lv22 attempt 52): recorded box(t)
//     minus the player's position at t-1 is constant to 0.001 px over the whole ride --
//     uid2860 (0.765, 6.000) over 648 ticks, uid2152 (0.901, 6.000) over 387.
//
// SO THE ARM DURING A RIDE IS A PROPERTY OF THE STATE, NOT OF THE RECORDING: with the
// offset fixed, the player overlaps the box on every tick of the ride exactly when it
// overlapped it on the tick the ride began -- which is the tick before the box's first
// moving row, where the box is still at its load-time place. The model therefore takes
// only the WINDOW from the recording (rideT0..rideT1) and, inside it, renews the arm
// exactly when it was renewed on the previous tick (the counter at its fresh value);
// before the window the load-time box is tested as usual. Nothing reads the recorded
// position of a riding box.
//
// Scope and the holes it leaves, all written down rather than guessed at:
//   - both axes locked (lockx && locky on an autonomous Move reaching the box). A box
//     locked on one axis keeps the pre-flag reading and the loader says so.
//   - the window comes from the recording; a recording without the ride (the flight
//     died before it) gives no window, and the box is then read as parked.
//   - after the ride the box stands wherever it stopped, which depends on the state's
//     own path again; nothing renews the arm there (it lapses in kArmTicks). GD keeps
//     renewing while the player still overlaps the stopped box.
//   - the offset is exact only while the player's half-size does not change inside
//     the ride; a growing half cannot lose the overlap, a shrinking one could.
//   - "renewed last tick" is one counter per arm kind, so two riding boxes of the same
//     kind would share it (lv22 has one of each).
//   - GD's y guard on a teleport (above) breaks the fixed offset; not modelled.
// Implies --fgarmlive's counter (decays, kArmTicks) for the 2866; the 1859 already had
// it. With both flags, --ridebox decides for riding boxes and --fgarmlive's live reading
// is kept for 2866 boxes that do not ride.
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// 0 before the box's ride, 1 inside it, 2 after it (and 0 when no ride is known).
template <class Box> inline int rideStage(const Box& b, int t) {
    if (!b.rides || b.rideT0 < 0) return 0;
    if (t < b.rideT0) return 0;
    return t <= b.rideT1 ? 1 : 2;
}
// --ridebox's renewal test for the 2866 arm: true when the arm is renewed on tick t.
// `freshPrev` = the counter was at its fresh value after the previous tick.
inline bool flipHeadArmsRide(double x, double y, double pHalf, int t, bool freshPrev) {
    for (const auto& b : g_flipHeadBoxes) {
        const int st = rideStage(b, t);
        if (st == 1) {
            if (freshPrev) return true;
            continue;
        }
        if (st == 2) continue;
        double bx = b.cx, by = b.cy;
        if (!b.rides) flipHeadAt(b, t, bx, by);   // a box that does not ride: as --fgarmlive
        if (std::fabs(x - bx) <= b.hw + pHalf
            && std::fabs(y - by) <= b.hh + pHalf)
            return true;
    }
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
//
// [2026-09-26] THE BOX IS READ AT ITS LIVE POSITION, like the 2866's under
// --fgarmlive, and for the same reason: lv22's switch-band 1859 (uid2152, group
// 21) RIDES THE PLAYER. A move trigger locked to the player's x carries it from
// t=2,445 for 1.61 s, 1.95 px a tick, so GD re-arms on every tick of the ride.
// Read at the parked (3,195,255) the model lets the arm lapse two ticks after
// the player walks off the parked box, and a mini cube that GD clamps under
// uid2182 at t=2,484 (y 261.000, vy 0) dies on the block's side at 2,487.
// Measured in the game (worker 99, 5f13395's lv22 plan): GD clamps with the arm
// last renewed 27, 31, 39, 67, 147, 87, 177 or 277 ticks earlier by the parked
// box's reckoning (the player held at x 3,221.5 or 3,240 by injection), and
// kills once the attempt never overlaps the box -- teleported from x 3,164 or
// 3,168 to 3,221.5, so the box rides 26 px behind. The counter's two-tick life
// is right; the box was in the wrong place.
struct ArmBox {
    double cx, cy, hw, hh;      // LOAD-TIME position
    int uid = -1;
    std::vector<ModRow> live;   // the recording's rows for this uid (see FlipHeadBox)
    bool rides = false;         // --ridebox, as FlipHeadBox
    int rideT0 = -1, rideT1 = -1;
};
inline std::vector<ArmBox> g_armBoxes;
inline void armBoxAt(const ArmBox& b, int t, double& bx, double& by) {
    bx = b.cx;
    by = b.cy;
    if (b.live.empty()) return;
    size_t lo = 0, hi = b.live.size();
    while (lo < hi) {
        const size_t m = lo + (hi - lo) / 2;
        if (b.live[m].t <= t) lo = m + 1; else hi = m;
    }
    if (lo > 0) {
        bx = (double)b.live[lo - 1].cx;
        by = (double)b.live[lo - 1].cy;
    }
}
inline bool armBoxTouch(double x, double y, double pHalf, int t) {
    for (const auto& b : g_armBoxes) {
        double bx, by;
        armBoxAt(b, t, bx, by);
        if (std::fabs(x - bx) <= b.hw + pHalf
            && std::fabs(y - by) <= b.hh + pHalf)
            return true;
    }
    return false;
}
// --ridebox's renewal test for the 1859 arm (see g_rideBox): a riding box renews inside
// its ride exactly when it renewed on the previous tick, is read at its load-time place
// before the ride and renews nothing after it; a box that does not ride is read live, as
// armBoxTouch does.
inline bool armBoxTouchRide(double x, double y, double pHalf, int t, bool freshPrev) {
    for (const auto& b : g_armBoxes) {
        const int st = rideStage(b, t);
        if (st == 1) {
            if (freshPrev) return true;
            continue;
        }
        if (st == 2) continue;
        double bx = b.cx, by = b.cy;
        if (!b.rides) armBoxAt(b, t, bx, by);
        if (std::fabs(x - bx) <= b.hw + pHalf
            && std::fabs(y - by) <= b.hh + pHalf)
            return true;
    }
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
struct SlideBox {
    double cx, cy, hw, hh;      // LOAD-TIME position
    int uid = -1;
    std::vector<ModRow> live;   // the recording's rows for this uid (--slidelive)
    std::vector<uint8_t> liveOn;   // ...and each row's on column (--slideon)
};
// --slideon: ...and a box its recorded row has switched off arms nothing. The 1755 is a type-40 object outside the
// moving geometry's on/off, so a box whose group a touch chain toggles off kept arming the wave in the model. SubZero
// 4003 t=12,239: slide boxes uid 5632-5634, switched off (with the ramps beside them) by the turned touch Spawn uid
// 8888 -- the recording has them off from its touch on -- and GD's wave dies on the block there while the model's
// slid on. The row read is the one BEFORE the tick's own: a custom level's wave crosses the toggle (a plain x-crossing
// 1049) that switches the two 1755s riding beside it off, and GD seats it on them that same tick (y 1,170.00, vy 0)
// -- the crossing fires after checkCollisions, and the tick's row already says off. On by default; --no-slideon is
// the off arm.
inline bool g_slideOn = true;
inline std::vector<SlideBox> g_slideBoxes;
// --slidelive (default off): read a box at its recorded position on this tick, as armBoxAt
// does for the ceiling arm. A 1755 that rides a moving row was tested where it was built, so a
// wave on the moving ceiling and floor of a custom level (t=13,930: rows rising 0.856 and
// sinking 0.571 px/tick, a 1755 in each) lost its arm once the rows had moved: GD keeps both waves
// seated (g=1, vy 0, carried by the rows) and the model flew them at +-4.186, 0.19 px off, which the
// recorder answered with four more fixups every round without the plan ever changing.
// On by default since 2026-10-07; --no-slidelive is the off arm.
inline bool g_slideLive = true;
// --waveflipramp (off): ...and an UPSIDE-DOWN armed wave rides a ceiling ramp from below, by the same law mirrored. The note above had no
// upside-down sample; a custom level's dual has one: the second wave, flipped, rising at 1.047 a tick beside a 45-degree line, meets the
// m=2 ceiling ramp uid 166235 (rotated 90) under a 1755 and goes up it at 2.094 a tick (rows 30,767-30,769: 877.17, 879.27, 881.36) where
// the model, which only lets an upright wave ride, called it inside the ramp and killed it. --no-waveflipramp is the off arm.
inline bool g_waveFlipRamp = true;   // on by default (release flags, 2026-10-17); --no-waveflipramp is the off arm
// --ringpresseat (speed.hpp): every toggle ring (1594) in the level, with its recorded rows.
struct PressRing {
    double cx, cy, hw, hh;      // LOAD-TIME box
    int uid = -1;
    uint8_t multiAct = 0;       // Obj::multiAct: GD never marks it used (--multiorb)
    std::vector<int> liveT;     // the recording's rows: tick, centre, on
    std::vector<float> liveX, liveY;
    std::vector<uint8_t> liveOn;
};
inline std::vector<PressRing> g_pressRings;
// Has the body whose ring memory is `old` / `hist` (State::usedOrbOld / usedOrbHist, where step.hpp's
// notePressRingFired files a toggle ring by uid) fired the ring with this uid?
inline bool pressRingFired(int uid, int32_t old, const int32_t* hist) {
    if (uid < 0) return false;
    if (old == uid) return true;
    for (int k = 0; k < 3; ++k)
        if (hist[k] == uid) return true;
    return false;
}
// The ring's last recorded row at or before t, or -1.
inline long pressRingRow(const PressRing& r, long long t) {
    size_t lo = 0, hi = r.liveT.size();
    while (lo < hi) {
        const size_t m = lo + (hi - lo) / 2;
        if ((long long)r.liveT[m] <= t) lo = m + 1; else hi = m;
    }
    return (long)lo - 1;
}
// Which ring that is on at row onT, and that this body has not fired, holds (x, y) where row t has it? Its index,
// or -1. The load box and "on" without rows.
inline int pressRingHolding(double x, double y, double pHalf, long long t, long long onT, int32_t old,
                            const int32_t* hist) {
    for (size_t i = 0; i < g_pressRings.size(); ++i) {
        const auto& r = g_pressRings[i];
        if (pressRingFired(r.uid, old, hist)) continue;
        double rx = r.cx, ry = r.cy;
        bool on = true;
        if (!r.liveT.empty()) {
            const long k = pressRingRow(r, t);
            if (k >= 0) {
                rx = (double)r.liveX[(size_t)k];
                ry = (double)r.liveY[(size_t)k];
            }
            const long ko = onT == t ? k : pressRingRow(r, onT);
            if (ko >= 0) on = r.liveOn[(size_t)ko] != 0;
        }
        if (on && std::fabs(x - rx) < r.hw + pHalf && std::fabs(y - ry) < r.hh + pHalf) return (int)i;
    }
    return -1;
}
// --spawnringon (speed.hpp): is the toggle ring with this uid on at row t? A uid with no rows (or no toggle ring)
// answers yes, as it did before the flag.
inline bool pressRingOnAt(int uid, long long t) {
    if (uid < 0) return true;
    for (const auto& r : g_pressRings) {
        if (r.uid != uid) continue;
        if (r.liveT.empty()) return true;
        const long k = pressRingRow(r, t);
        return k < 0 || r.liveOn[(size_t)k] != 0;
    }
    return true;
}
inline bool slideBoxTouch(double x, double y, double pHalf, int t = -1) {
    for (const auto& b : g_slideBoxes) {
        double bx = b.cx, by = b.cy;
        if (g_slideLive && t >= 0 && !b.live.empty()) {
            size_t lo = 0, hi = b.live.size();
            while (lo < hi) {
                const size_t m = lo + (hi - lo) / 2;
                if (b.live[m].t <= t) lo = m + 1; else hi = m;
            }
            if (lo > 0) {
                bx = (double)b.live[lo - 1].cx;
                by = (double)b.live[lo - 1].cy;
            }
            // --slideon: the on the box had when this tick's collision pass met it, which is the row BEFORE
            // this tick's: a crossing toggle fires after checkCollisions, and the recorder writes the tick's
            // row after both
            size_t ko = lo;
            while (ko > 0 && b.live[ko - 1].t >= t) --ko;
            if (g_slideOn && ko > 0 && ko - 1 < b.liveOn.size() && !b.liveOn[ko - 1]) continue;
        }
        if (std::fabs(x - bx) <= b.hw + pHalf
            && std::fabs(y - by) <= b.hh + pHalf)
            return true;
    }
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
struct CtrlTouchBox { double cx, cy, hw, hh; int uid; uint8_t on; uint8_t holdOff = 0; };
inline std::vector<CtrlTouchBox> g_ctrlTouchBoxes;
// --stopjump (speed.hpp): the Player Control triggers (1932, Stop Jump on, player 1 targeted or both). The
// crossing queue's, sorted by x, and the touch-started ones' boxes (State::pcTouch is the last one fired).
struct PcTrig { double cx; int uid; };
struct PcTouchBox { double cx, cy, hw, hh; int uid; };
inline std::vector<PcTrig> g_pcTrigs;
inline std::vector<PcTouchBox> g_pcTouchBoxes;
// --reversetrig (speed.hpp): the Reverse triggers (1917) on the crossing queue, sorted by x.
struct RevTrig { double cx; int uid; };
inline std::vector<RevTrig> g_revTrigs;
// Is x over a box a held button switches off, or 30 px short of one? There the button matters in
// the air: the search expands an airborne cube's pressed child (search_dp.hpp) and keys the button
// (search_key.hpp keyOf), or the held way through is merged into the released one before it gets there.
inline bool nearCtrlHoldBox(double x) {
    for (const CtrlTouchBox& b : g_ctrlTouchBoxes)
        if (b.holdOff && x >= b.cx - b.hw - 30.0 && x <= b.cx + b.hw + 15.0) return true;
    return false;
}
inline bool dashStopAt(double x, double y, double pHalf) {
    for (const auto& b : g_dashStopBoxes)
        if (std::fabs(x - b.cx) <= b.hw + pHalf
            && std::fabs(y - b.cy) <= b.hh + pHalf)
            return true;
    return false;
}

}  // namespace dp
