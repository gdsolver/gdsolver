#pragma once
#include "dp/object.hpp"

namespace dp {

// ---- MOVING GEOMETRY ------------------------------------------------------
//
// Everything above is a STATIC grid: the objrects dump is one snapshot taken at
// level entry, and the whole model treats it as the truth for all 30,000 ticks.
// That holds for lv1-18, which contain exactly ZERO moving objects
// (scripts/mechanic_census.ps1, "movable" column), and it collapses at lv19,
// where the same column reads 820 / 1,015 / 1,731 / 761 for lv19-22.
//
// The failure is not subtle. lv19's cold DP dies at x=1,459 because the static
// grid has nothing to stand on there -- while GD, replaying a working plan, has
// the player GROUNDED at (1,352, 131.6). The thing it is standing on is
// `id 470 type 0 groups=1`, which the dump lists at (1,365, **45**), i.e. below
// the floor: it rises into place before the player arrives. lv22 is the same
// shape at x=525 and it is what stops that level at x=604.
//
// Rather than implement GD's trigger system (move / rotate / toggle / spawn,
// with easing), the MOD records what actually happened: `grouptrace=1` writes
// `tick,uid,cx,cy,w,h` for every grouped object, every tick it changes. That
// makes rotation and scaling fall out for free -- they change the rect, and the
// rect is all the model ever asks for.
//
// Between two samples an object holds its last rect, and past the final sample
// it holds it forever. That is exactly right for a platform that has finished
// moving, which is the common case: the trigger fires when the object comes on
// SCREEN, so a platform is normally already in place by the time the player
// reaches it.
// `on` is GD's own toggle state (GameObject::m_isGroupDisabled). A toggle does
// NOT move the object, so a recorder that watched positions alone would report
// a wall that never changes -- which is exactly what lv19 x=27,705 looked like:
// two 30x30 blocks filling the only 60 px gap in a wall the player flies
// straight through.
// ---- ROTATED GAMEPLAY (id 2900) --------------------------------------------
//
// GD 2.2 turns the whole gameplay frame: the world does not move, the player's
// travel direction and gravity do. lv22 has 20 of these (no other level in the
// suite has any, so all of this is inert there).
//
// Measured on lv22 with the reference replay (findings.md 2026-08-13):
//   - `rot` is the ABSOLUTE screen angle, not a delta. Eight of lv22's twenty
//     are rot=0, i.e. "turn it back".
//   - travel = R(-rot) * xhat:  0 -> +X,  90 -> -Y,  180 -> -X,  270 -> +Y.
//     uid 1343 (2265,316, rot=90): from t=1,747 x freezes at 2,266.5 and y
//     falls 1.2982/tick (= the section's dx). uid 6286 (16125,489, rot=-90):
//     x freezes at 16,131 and y RISES 1.298/tick.
//   - it fires on the tick AFTER the player's forward coordinate crosses the
//     object's -- the same crossing+1 rule every autonomous trigger uses. The
//     boxes overlap for ~15 ticks before that and nothing happens.
//   - the world VELOCITY carries over: the forward speed becomes the new
//     perpendicular one, and the old perpendicular is dropped (the forward
//     speed is the section's, not a free variable).
//
// The model keeps its state in the CURRENT frame's coordinates, so every
// physics rule stays written as "x is the clock, y is height"; what changes is
// the geometry handed to it. (u,v) = R(-rot)*(X,Y), which for 90-degree
// multiples is a pure index swap with signs -- an AABB stays an AABB with
// hw/hh swapped.
struct RotTrig {
    double cx, cy;     // WORLD position of the trigger
    int frame;         // rot/90 mod 4
    // ONE SHOT, like every other trigger GD activates from the level: the tick
    // it fired, -1 while it is still live. Measured on lv22 (2026-08-13): the
    // player leaves the first rotated section at world x=2,143.5 heading +X and
    // re-crosses uid 1343 (2265,316) at t=1,911 -- injected back up to y=315.9,
    // i.e. INSIDE the trigger's own box -- and GD does nothing at all.
    // Without this the model turns again, exits at 2,143.5, walks back to
    // 2,265, turns again: a closed loop the frontier can never leave (x pinned
    // at <=2,266 for 1,000 ticks). It only looked like progress before because
    // the broken frame floor dropped the player 700 px out of the section.
    int firedT = -1;
    // A separate one-shot for the reverse run (a 2900 pointing at the same
    // frame). **Must NOT be shared with firedT**: if shared, a trigger consumed
    // at the entrance as a reverse-run toggle can no longer be used later as its
    // real frame change. Measured 2026-08-15: lv22's cold run could no longer
    // get out of x=2,343 (the old exe was at 2,441 by iter 11).
    int revT = -1;
    // The gravity direction this trigger sets (from mvdir), in the model's local
    // convention. -1 = an old export without the column, in which case flip is
    // carried over as before.
    int setFlip = -1;
    // The reverse run this trigger sets (from gnddir). -1 = an old export
    // without the column; only then does it fall back to the old "toggle if it
    // is the same frame" rule.
    int setRev = -1;
    // objrects' uid. Used only to match --spentrot (the driver naming the 2900s
    // that had already fired before the anchor).
    int uid = -1;
    // Effective multiplier on vy [2026-08-19]: m_velocityModY(583, member
    // default 0.0) if m_editVelocity(169) is set, 1.0 (pass-through) if not.
    // Folded at parse time. An old export (no column) falls back to 1.0 -- lv22
    // is the only level with a 2900, and its objrects was re-dumped the same day.
    float vmodY = 1.0f;
    // m_overrideVelocity(584). When set, vmodY is an absolute assignment, not a
    // multiplier. lv22 has no instance (carried only).
    uint8_t ovrVel = 0;
};
inline std::vector<RotTrig> g_rotTrig;
// --spentrot uid,uid,...: the uids of the 2900s this run had already fired
// before the anchor (--start's t0). The one-shot (firedT) cannot be carried
// through --start, so re-anchoring behind the maze re-fires a spent trigger at
// the next crossing and only the model turns (lv22 t=14,321 uid6337: GD had
// consumed it at t=12,795 and passes straight through; the model turned into
// frame 3 and the whole world broke. A fixup cannot carry the frame, so the
// driver re-stacked the same fixup for 30 iterations and marked time). The
// driver derives it from the gframe transitions (t<t0) in the GD dump and
// passes it in.
inline std::vector<int> g_spentRot;
// A/B switch that turns off the reverse-run toggle (a 2900 of the same frame)
// (--norevtoggle).
inline bool g_revToggle = true;

// ---- CONTROL-DISABLED WINDOWS (--ctrlwin t0:t1,...) -------------------------
//
// **id 2899 is not a reverse run but an Options trigger** (GD's
// GameOptionsTrigger). All 10 of lv22's raise/lower m_disableP1Controls, and
// while it is raised **the button is ignored entirely** -- neither a press nor
// the cube's held-button re-jump happens. The windows measured in GD
// (2026-08-18, cfg endtrace=1) are four:
//   t=13,461..14,224 / 18,259..18,612 / 19,152..19,576 / 20,110..20,487
// 1,921 ticks in total. The model thought it could jump there, and this was the
// identity of fixcensus's `m0/mini0/g1/gdg1/sp0.9/air/in1` family (edy=0.000 /
// edvy=-11.180, only the model jumps).
//
// **Why not derive it from the triggers ourselves**: which x crossing makes GD
// raise it is still unsolved. Crossing the same x of the same trigger many times
// fires it only once, and that once is not necessarily the first crossing
// (uid 18316 on the 5th, uid 18086 on the 3rd). Nor is it the distance to the
// player's y (no fire at Δ=1,236, fires at Δ=1,324). The gate is GD's camera or
// something like it, which the current model does not have. **Firing on a guess
// does more harm than good** (burning uid 18315 at its first crossing t=10,673
// creates 393 ticks of control loss that GD does not have, and on top of that
// misses the real window). So the windows are passed only "when GD knows them":
// section-anchor replays (fixcensus / quick_regress / the driver's re-anchor)
// already receive gframe and pmin/pmax from the GD dump, so this fits the same
// scheme. A cold run from the head has no such information and the model jumps
// as before (an unresolved hole).
inline std::vector<std::pair<long long, long long>> g_ctrlWin;

inline bool ctrlOffAt(long long t) {
    for (const auto& w : g_ctrlWin)
        if (t >= w.first && t <= w.second) return true;
    return false;
}
// Jump ticks that come from a window's re-push (= w1+1). GD's buffered jump has
// holding already at 0 when it fires, so **no hover starts** -- the model side,
// which mirrors it with a synthetic press, would create s.action=1, so the jump
// on exactly this tick does not accumulate rHover. Filled only when a replay is
// loaded (always empty during the search).
inline std::vector<long long> g_winRePushJump;
inline bool rePushNoHoverAt(long long t) {
    for (long long v : g_winRePushJump)
        if (t == v) return true;
    return false;
}

inline void toFrame(int f, double X, double Y, double& u, double& v) {
    switch (f & 3) {
        case 0:  u =  X; v =  Y; break;
        case 1:  u = -Y; v =  X; break;   // rot 90:  travel -Y
        case 2:  u = -X; v = -Y; break;   // rot 180: travel -X (the reverse runs)
        default: u =  Y; v = -X; break;   // rot 270 (= -90): travel +Y
    }
}
inline void fromFrame(int f, double u, double v, double& X, double& Y) {
    switch (f & 3) {
        case 0:  X =  u; Y =  v; break;
        case 1:  X =  v; Y = -u; break;
        case 2:  X = -u; Y = -v; break;
        default: X = -v; Y =  u; break;
    }
}
// The travel coordinate of a world point in this frame (what `cx` becomes).
inline double frameU(int f, double X, double Y) {
    double u, v;
    toFrame(f, X, Y, u, v);
    return u;
}
// ...and the PERPENDICULAR one (what `cy` becomes). A 2900 fires only when the
// player's box overlaps it on this axis -- see the gate in applyRotation.
inline double frameV(int f, double X, double Y) {
    double u, v;
    toFrame(f, X, Y, u, v);
    return v;
}

// ---- turning one object into a frame's coordinates -------------------------
//
// What has to come along:
//   - the box: centre through toFrame, hw/hh swapped on the odd frames
//   - `rot`: the object's own turn is measured against the screen, so it moves
//     with the frame (used by the oriented rings and the dash angle)
//   - the slope line (sy0/sy1 are absolute heights at the box's left/right
//     edge): a turned slope is a different line, so the two endpoints are
//     carried as points and re-read
//   - tpY (an absolute target y for a teleport) becomes the target's v
inline void turnObj(Obj& o, int f) {
    if ((f & 3) == 0) return;
    const double cx = o.cx, cy = o.cy;
    double u, v;
    toFrame(f, cx, cy, u, v);
    // the slope's two surface points, in world, before the box moves
    const double sxL = cx - o.hw, sxR = cx + o.hw;
    const double syL = o.sy0, syR = o.sy1;
    o.cx = u; o.cy = v;
    if (f & 1) std::swap(o.hw, o.hh);
    o.rot += 90.0 * (f & 3);
    if (o.slope) {
        double uL, vL, uR, vR;
        toFrame(f, sxL, syL, uL, vL);
        toFrame(f, sxR, syR, uR, vR);
        if (uL <= uR) { o.sy0 = vL; o.sy1 = vR; }
        else          { o.sy0 = vR; o.sy1 = vL; }
    }
    if (o.tpY != 0.0) {
        double tu, tv;
        toFrame(f, cx, o.tpY, tu, tv);
        o.tpY = tv;
    }
    // the exit half is a POINT of its own (its x is not the portal's cx), so
    // it turns as one -- the teleport block reads its v as the target
    if (o.tpEx != 0.0 || o.tpEy != 0.0) {
        double eu, ev;
        toFrame(f, o.tpEx, o.tpEy, eu, ev);
        o.tpEx = eu; o.tpEy = ev;
    }
}

}  // namespace dp
