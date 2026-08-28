#pragma once
#include "dp/slopes.hpp"

namespace dp {

struct StepCtx {
    double x, xPrev;
    float dxF;         // this layer's per-tick x advance (see advanceX)
    long long t;
    // (the flying band lives in State now -- see State::bandFloor)
    const std::vector<const Obj*>* near;
    const std::vector<const Obj*>* ports;
    const std::vector<const Obj*>* pads;
    const std::vector<const Obj*>* orbs;
    const std::vector<const Obj*>* slopes;
    const std::vector<const Obj*>* speeds;
    // Both sizes. The ship used ShipParams::normal() unconditionally, so a mini
    // ship climbed at the full-size 0.086/tick where GD gives 0.101 -- measured
    // on lv11 t=8395..8411, a steady 0.0150/tick of drift that put the model
    // 3.6 px below GD within 17 ticks and killed the whole ship section.
    // ShipParams::mini() already held the right numbers; nothing selected them.
    const gdapprox::ShipParams* SP;
    const gdapprox::ShipParams* SPmini;
    const gdapprox::UfoParams* UP;
    const gdapprox::UfoParams* UPmini;
    // Touch-trigger boxes overlapping this group's x window, with the bit each
    // one owns in State::trig. Null on every level that has none.
    const std::vector<std::pair<const TouchTrig*, uint32_t>>* trigs = nullptr;
};

// Why the last `dead = true` fired, and on what. "The frontier died at x=N" on
// its own never says whether the model is over-killing or under-killing, and
// every wall so far has needed that answer before anything else -- lv16's ramp
// ridge and lv18's spiked tunnel both read as "the search ran out" until the
// object was named. Written unconditionally (a store of two words per kill) and
// only ever READ by --dbg, so the search itself is unaffected.
// thread_local because the layer expansion runs on several threads (see
// ThreadPool): every worker kills its own children and would otherwise stamp on
// the others' reason. --dbg reads them on the same thread that stepped the
// child, so the reason is still the right one there.
inline thread_local const char* g_deadWhy = "";
inline thread_local const Obj* g_deadObj = nullptr;
// Whether something that can deliver an impulse (orb / pad) was within reach on this
// tick. **"Was present", not "fired"**. An axis for diagnosing missed impulses; it is
// written to the trace and goes into the driver's cause signature.
inline thread_local uint8_t g_nearOrb = 0;
// The vy that GD's dump prints on the tick a dash ring **engages**. The model's
// internal value is already 0 there (no vy is held while dashing), but GD leaves
// "the value with gravity added once more" in that tick's dump, and it only becomes
// 0 from the next tick on.
// Measured (lv21; y matches exactly on both sides = same trajectory):
//   t=1,894  type37  -10.120 -> GD -10.336 (= -10.120 + g), model 0
//   t=18,955 type37   -5.452 -> GD  -5.668 (= -5.452 + g)
//   t=4,511  type38  -14.256 -> GD  -7.236 (= (-14.256 + g) / 2)
//   t=5,555  type38   -8.908 -> GD  -4.562 (= (-8.908 + g) / 2)
// So **37 passes it through, 38 (with gravity flip) halves it**. y is already frozen
// on the engage tick, so it never shows in the trajectory, and GD also restarts the
// fall from 0 after the dash ends ([[gd-dash-stop-1829]]) -- a purely
// **display-only** difference.
// It is fixed anyway because fixcensus / quick_regress / the driver's fixup all look
// at the trace: left alone, a non-real divergence gets recorded once every time, and
// at worst a fixup gets written back on top of correct physics.
// Not kept in the state (a non-zero vy would split the dedupe key per branch).
inline thread_local double g_dashVy = 0.0;
inline thread_local uint8_t g_dashVySet = 0;
// Why vy was set to 0 on this tick. **Kept the same way as the cause of death.**
// A wrong clamp shows up as "only the model loses velocity", but which clamp fired
// cannot be worked back from outside (it was happening where there is not a single
// static object). "" = not clamped.
inline thread_local const char* g_clampWhy = "";
// What the clamp hit. Chasing a false firing needs "which object was taken as the
// ceiling / floor" (measured: not a single static object at that coordinate = it was
// looking at a moving object).
inline thread_local int g_clampUid = -1;
inline thread_local float g_clampCx = 0.f, g_clampCy = 0.f;
#define CLAMP0(why) do { g_clampWhy = (why); } while (0)
#define CLAMP0O(why, ob) do { \
    g_clampWhy = (why); g_clampUid = (ob)->uid; \
    g_clampCx = (float)(ob)->cx; g_clampCy = (float)(ob)->cy; } while (0)
// ...and WHERE the killer was AT THE MOMENT it killed. Reading cx/cy off the
// pointer after the run is not the same number for a moving object, and that
// difference is exactly what a stale-geometry bug looks like.
inline thread_local float g_deadCx = 0.f, g_deadCy = 0.f;
#define DIE(why, obj) do { dead = true; g_deadWhy = (why); \
    const Obj* dobj_ = (obj); g_deadObj = dobj_; \
    if (dobj_) { g_deadCx = (float)dobj_->cx; g_deadCy = (float)dobj_->cy; } } while (0)

// The ship's accel-switch threshold is per SPEED (see ShipParams::withSpeed).
// The base sets are built once at start-up and never carried a speed, so a
// 0.7 section ran with the 0.9/1.1 threshold and switched to the weak accel one
// tick late -- 0.026 vy, which on lv18 grew to 43 px and a different route.
// Only the slow variant differs, so it is two extra statics rather than a copy
// per step.
inline const gdapprox::ShipParams& shipParamsFor(float dxF, bool mini,
                                                 const StepCtx& K) {
    static const gdapprox::ShipParams kSlow =
        gdapprox::ShipParams::normal().withSpeed(0.7);
    static const gdapprox::ShipParams kSlowMini =
        gdapprox::ShipParams::mini().withSpeed(0.7);
    // [2026-08-21 r54] **1.1 is a separate table too**. `K.SP` is the raw params
    // that never went through `withSpeed`, so until now 0.9 and 1.1 used the same
    // `accelSwitchVy` (1.9165). gdref measured 1.1 at **(1.912, 1.915)**, and
    // 1.9165 is outside that interval (see the note on accelSwitchVyForSpeed).
    // Per-tick advance: 0.7->1.0465 / 0.9->1.29825 / 1.1->1.6143 /
    // 1.3->1.9096 / 1.6->2.3438.
    static const gdapprox::ShipParams kFast =
        gdapprox::ShipParams::normal().withSpeed(1.1);
    static const gdapprox::ShipParams kFastMini =
        gdapprox::ShipParams::mini().withSpeed(1.1);
    // 0.7's per-tick advance is 251.16/240 = 1.0465; the next one up is 1.29825
    if (dxF < 1.2f) return mini ? kSlowMini : kSlow;
    if (dxF >= 1.45f && dxF < 1.8f) return mini ? kFastMini : kFast;
    return mini ? *K.SPmini : *K.SP;
}
// [2026-08-21 r57] **The UFO too: only the mini at sp1.1 has a different threshold.**
// Measured by sorting gdref's UFO samples by "vp in the player frame" against "dvy"
// (weak=-0.101 / strong=-0.152):
//   mini sp0.7  weak <= 1.784 / strong >= 1.936 -> contains 1.9165
//   mini sp0.9  weak <= 1.891 / strong >= 1.936 -> contains 1.9165
//   **mini sp1.1 weak <= 1.905 / strong >= 1.915 -> 1.9165 is outside the interval**
//   normal size at every speed: weak <= 1.840 / strong >= 1.969 (all contain 1.9165)
//   flipped side: normal sp0.9/1.1 and mini sp1.1 all contain -1.9165
// Take the midpoint 1.910. Real damage: lv20 t=6,303 (mini UFO sp1.1), census
// `m3/mini1/g0/gdg0/sp1.1/air` edvy -0.051.
// [2026-08-21 r85] **The normal-size UFO is per-speed too**. Bracketing the switch
// vy with calibration-rig measurements, ceilrel x 3 speeds (0.7/0.9/1.1, 4 units
// each):
//   sp0.9  T in [1.8380, 1.9670)   contains 1.9165
//   sp0.7  T in [1.7720, 1.9010)   **1.9165 is outside**
//   sp1.1  T in [1.7940, 1.9230)   contains 1.9165
// All three contain the ship's per-speed table values (1.885 / 1.9165 / 1.9135) =
// **the UFO uses the same table as the ship**. 1.9165 at 0.7 became a switch one
// tick early, and 0.043/tick was accumulating (ceilrel07's 4 UFO units, dvy +0.044).
inline const gdapprox::UfoParams& ufoParamsFor(float dxF, bool mini,
                                               const StepCtx& K) {
    static const gdapprox::UfoParams kUfoFastMini = [] {
        gdapprox::UfoParams p = gdapprox::UfoParams::mini();
        p.accelSwitchVy = 1.910;   // (1.905, 1.915)
        return p;
    }();
    static const gdapprox::UfoParams kUfoSlow = [] {
        gdapprox::UfoParams p = gdapprox::UfoParams::normal();
        p.accelSwitchVy = 1.885;   // same value as the ship's 0.7 (inside the rig's interval)
        return p;
    }();
    static const gdapprox::UfoParams kUfoSlowMini = [] {
        gdapprox::UfoParams p = gdapprox::UfoParams::mini();
        p.accelSwitchVy = 1.885;
        return p;
    }();
    if (dxF < 1.2f) return mini ? kUfoSlowMini : kUfoSlow;
    if (mini && dxF >= 1.45f && dxF < 1.8f) return kUfoFastMini;
    return mini ? *K.UPmini : *K.UP;
}

// GD's spider surface search: "the face on the other side, travelling against
// the current gravity". Used by the SPIDER's own tap (inline in stepOne, which
// is where every constant here was measured) and by the SPIDER ORB (type 43),
// which runs the same search from any mode and in mid-air.
// Returns false when there is nothing to land on -- GD's spiderTestJump finds
// no surface then, and the caller must do nothing rather than guess.
// `outHaz` (when given) comes back true if the thing the search stopped on is a
// HAZARD. GD's search does not skip them: measured on lv22 t=1,737 with a hook
// on PlayerObject::spiderTestJump itself (the raw accumulator fields are reset
// every tick, so only a hook inside the call sees the answer) -- the spider taps
// off the ceiling at y=316.500 and GD puts it at **213.250**, which is exactly
// the cy of the id-392 spike row (uid 18201..18206 / 17571, 2.6x4.8, all at
// cy=213.25), and kills it there. The model skipped `type != 0`, walked past the
// row and landed on the block below at y=163.500 -- 50 px lower, alive, and the
// DP then planned a route out of a section GD does not let you leave.
// Eight teleports were captured in that one replay; the other seven land on
// blocks at block-derived heights (223.5 / 163.5 / 316.5 / ~286-291), so the
// hazard case is the only one whose landing is the object's OWN centre. That
// offset is ONE sample -- but the player dies on the same tick, so only the
// stopping matters, not the height.
// `frame` is the gameplay frame the search runs in; it only matters for the
// transverse window (see the note at the window test).
inline bool spiderTargetY(const StepCtx& K, double x, double fromY, bool flip,
                          bool mini, double pHalf, double& outY,
                          bool worldFloor = true, bool* outHaz = nullptr,
                          int frame = 0) {
    const double gs = flip ? -1.0 : 1.0;
    // 1.0 px wider in x than the spider's own half -- measured, see
    // kSpiderSearchHalfX. This picks WHICH surface it lands on.
    const double searchHalfX = mini ? kSpiderSearchHalfXMini : kSpiderSearchHalfX;
    double bestY = 0.0;
    bool found = false, bestHaz = false;
    // HAZARDS are searched too, but through their OWN rect. Straight off
    // spiderTestJumpInternal (2.2081, 0x3943f0 -- note the version, see the
    // note on kSpiderSearchHalfX): the function builds a rect, extends it to
    // getMinPortalY/getMaxPortalY, narrows it in x, and then asks the layer
    // TWICE with two different rects:
    //     r15 = staticObjectsInRect (rect A)      <- solids
    //     r13 = damagingObjectsInRect (rect B)    <- hazards
    // Using ONE rect for both is what broke the first attempt at this: with the
    // solid column's half (14.5) the spike uid5356 at |dx| = 15.93 becomes a
    // candidate and the model died at t=819, where GD sails past it
    // (163.500 -> 290.860). With the player's own half (13.5) that spike is
    // 15.93 > 13.5 + 2.376 = 15.876 -- OUT by 0.05 px -- while the one that
    // does kill, uid18205 at |dx| = 0.74, is well inside 13.5 + 1.3.
    // The margin is thin, but the two rects are the binary's own structure and
    // the sign of the split is not a free parameter.
    for (const Obj* o : *K.near) {
        const bool haz = (o->type != 0);
        if (haz && o->radius > 0.0) continue;   // saws are circles, not faces
        const double halfX = haz ? pHalf : searchHalfX;
        // ...and in a TURNED frame that half is on the WRONG AXIS. The rect
        // spiderTestJumpInternal builds is narrowed in the player's own x and
        // EXTENDED to getMinPortalY/getMaxPortalY in world Y. In frame 0 the
        // search's transverse axis IS world x, so `halfX` is the narrowing and
        // everything lines up; in an odd frame the transverse axis is world Y --
        // the EXTENDED one -- and narrowing it to 14.5 throws away exactly the
        // objects GD reaches.
        // Measured on lv22's rotated section (2026-08-16, cold run, t=1,813):
        // the spider taps at world (2,266.5, 229.5) and GD lands it at world
        // x=2,173.5 = the right edge of uid1204 (2,152.5,202.5, 15x15) plus the
        // spider's 13.5. That block is 27.0 px away in world Y and the window
        // was 7.5 + 14.5 = 22.0, so the model skipped it and took uid1212/1214
        // at world x=2,115 instead -- landing 30 px further, and every tick
        // after that belongs to a different run.
        // (uid1201/1202 sit at the same world Y but have MOVED to y=84 by then,
        // so exactly one candidate is left at that height and the
        // nearest-ahead rule then picks GD's.)
        // ...and only the SOLID rect is the extended one. The two rects are
        // already known to differ (see the note above this loop); a hazard found
        // with the band's reach is one GD never sees. Measured on the same tick:
        // with the band applied to both, the model stopped on a hazard at world
        // x=2,257.5 -- nearer than uid1204 in the travel axis, so it won the
        // nearest-ahead rule -- and died, where GD sails past it to 2,173.5.
        // [2026-08-26] In a TURNED frame the window is not centred on the
        // player at all. Hooked spiderTestJumpInternal's two queries (the
        // srect:/drect: lines, cfg hitboxtrace=1) on lv22's t=1,813 tap:
        //   srect: o=(0.000,201.517) s=(2255.000,14.500)   <- solids
        //   drect: o=(0.000,225.517) s=(2255.000, 8.000)   <- hazards
        // with the player at world y 230.815 scrolling 1.298/tick. In the
        // model's progress coordinate both anchor at the POST-MOVE centre
        // xF = x + dxF:
        //   solids : [xF + pHalf, xF + 2*pHalf + 1]  (the strip hangs off the
        //            LEADING edge; 14.5 = size*0.5 + 1, the 0x3943f0 constant)
        //   hazards: [xF - 4, xF + 4]                (the +4.0 constant; the
        //            old +-(hw+pHalf) window was ~4x wider than GD's band and
        //            invented the tp-hazard kill at 2,257.5 that doomed every
        //            anchor on the cold run's prefix)
        // This one form reproduces every measurement on both worldlines: the
        // natural tap (lands 2,173.5 on uid1204, a 6px GAP from the player's
        // box -- inside the strip), the y=240/244 injection boundary (strip
        // overlap 0.6px / miss 3.2px), and the 8/16 landings at 2,143.5
        // (uid1204 out of the strip at that worldline's y, the moved-in
        // uid1212/1214 inside it).
        if (frame % 2 != 0) {
            const double xF = x + (double)K.dxF;
            if (haz) {
                if (o->cx + o->hw < xF - 4.0 || o->cx - o->hw > xF + 4.0)
                    continue;
            } else {
                if (o->cx + o->hw < xF + pHalf
                    || o->cx - o->hw > xF + 2.0 * pHalf + 1.0)
                    continue;
            }
        } else if (true) {
            if (std::fabs(x - o->cx) > o->hw + halfX) continue;
        } else {
            // the portal band in this frame's travel coordinate. GD's own
            // limits come from the anchor's dump (--startband); with none, a
            // window wide enough for the 270 px band.
            double bLo = x - 150.0, bHi = x + 150.0;
            if (g_startBandSet && g_startBandCeil < 1e8) {
                const double u0 = frameU(frame, 0.0, g_startBandFloor);
                const double u1 = frameU(frame, 0.0, g_startBandCeil);
                bLo = std::min(u0, u1); bHi = std::max(u0, u1);
            }
            if (o->cx + o->hw < bLo || o->cx - o->hw > bHi) continue;
        }
        // the face that matters is the one the player comes to rest ON: the
        // UNDERSIDE of a solid when travelling up, its TOP when travelling down
        const double face = flip ? (o->cy + o->hh) : (o->cy - o->hh);
        // a hazard is not landed ON -- GD puts the player at its centre and
        // kills it there (lv22 t=1,737: 316.500 -> 213.250, the id392 row's cy)
        const double rest = haz ? o->cy : (face - gs * pHalf);
        if ((rest - fromY) * gs <= 0.5) continue;   // not ahead of us
        if (!found || (rest - fromY) * gs < (bestY - fromY) * gs) {
            bestY = rest;
            bestHaz = haz;
            found = true;
        }
    }
    if (outHaz) *outHaz = found && bestHaz;
    // ...and the world's own floor counts when travelling down with no block
    // under us. NOT in a turned frame: there `v` is a world X, so kGroundY is a
    // vertical line at world x=90 -- a floor that does not exist. Measured on
    // lv22 (findings.md 2026-08-13): the tap sent the model to world x=1,459
    // and then it slid off the level, where GD lands at x=2,143.5 every time.
    // GD does have a ground in the turned frame (the spider bounces between
    // 2,143.5 and 2,266.5 with no object at the former), but its position is
    // not derived yet -- and a missing floor kills the branch, which is the
    // safe direction, while a phantom one invents a route.
    if (flip && worldFloor) {
        const double rest = kGroundY + pHalf;
        if (fromY - rest > 0.5 && (!found || rest > bestY)) {
            bestY = rest;
            found = true;
        }
    }
    outY = bestY;
    return found;
}

// Turn the state if this tick's advance crossed a rotation object (id 2900).
// `uPrev` is where it was BEFORE the move, in its own frame. Returns the new
// frame, or -1 when nothing changed.
//
// The world POINT carries over (it is one player in one world) and so does the
// world VELOCITY: the forward speed becomes the new perpendicular one, and the
// old perpendicular is dropped because the forward speed is the section's, not
// a free variable. Everything that names a surface or a band is cleared -- the
// band is a pair of world-Y limits and would clamp the player into a wall.
// `t` is this tick, and it is READ ONLY here: a trigger already spent on an
// earlier tick is skipped, one spent on THIS tick still fires (the whole group
// crosses together, and the marking is done once per group in the layer
// prologue -- this runs on the worker threads).
// `input` / `wasGrounded` describe the tick's BUTTON, which GD processes after
// the rotation has already turned the world (see the re-tap at the end).
inline int applyRotation(State& c, double uPrev, double dxUsed, long long t,
                         int input = 0, bool wasGrounded = false,
                         double sPrevY = 0.0, bool havePrevY = false) {
    if (g_rotTrig.empty()) return -1;
    const int f0 = (int)c.frame;
    const int rev0 = (int)c.rev;   // reverse travel is a 180-degree heading = true frame +2
    int nf = -1;
    int nflip = -1;
    const RotTrig* best = nullptr;   // the matching trigger nearest in the perpendicular axis
    double bestDv = 1e18;
    float rotModY = 1.0f;          // vy multiplier of the adopted trigger (RotTrig::vmodY)
    uint8_t rotOvr = 0;
    bool toggleRev = false;
    for (const RotTrig& r : g_rotTrig) {
        // A 2900 that points to the current frame is a **reverse-travel toggle**.
        // GD measurement (2026-08-15, lv22's 14 firings tabulated by frame and
        // travel before/after):
        //   t=18,382 (22575,1755) f0=0 -> 0, dx +1.613 -> **-1.613**, player 4px
        //   from the obj. Neither ug nor the y progression changes, so it is not
        //   a rotation
        //   t=14,489 (16365,1275) f0=0 -> 0 but travel unchanged -- here the player
        //   is **543px** from the obj's cy, a firing that fails the proximity test
        // The other 12 all actually change the frame. So it is not "points to the
        // same frame = do nothing" but "reverse the travel direction".
        // In an export that has gnddir, the frame and rev this trigger sets are
        // known, so "same frame means toggle" is not needed. If the heading is
        // already the same it is naturally a no-op (lv22's uid11177 is exactly that).
        const bool same = (r.setRev < 0) ? (r.frame == f0)
                                         : (r.frame == f0
                                            && r.setRev == (int)c.rev);
        const int shot = same ? r.revT : r.firedT;
        if (shot >= 0 && shot != (int)t) continue;
        const double ru = frameU(f0, r.cx, r.cy);
        // In reverse travel (State::rev) the travel coordinate **decreases**, so a
        // forward-only crossing test can never step on it. [2026-08-16] lv22
        // t=11,965: GD turns to frame 1 on uid6307 (15,405,859), but the model only
        // saw u drop 15,405.5 -> 15,404.2, never once judged it "crossed", and
        // dropped the rotation entirely.
        // The DP side's group latch already looks at grev (the layer prologue), so
        // only this spot had been left behind.
        const bool crossed = c.rev
            ? (uPrev > ru && (double)c.xAbs <= ru)
            : (uPrev < ru && (double)c.xAbs >= ru);
        if (!crossed) continue;
        // ...and the player has to be AT the trigger, not merely past its
        // travel coordinate. `frameU` is the trigger's world Y once f0 is odd,
        // so without this a trigger ANYWHERE in the level matches: lv22's
        // uid5957 sits at world (8,297,423) -- 1,548 px away -- its frame-3 u
        // is 423, and the model turned back to frame 0 at t=4,688 on it and
        // flipped its gravity, which GD never does.
        // GD's own firings (hooked GJBaseGameLayer::rotateGameplay, `rot:`):
        //   t=1,745 uid1343 obj(2265,316)   player(2265.4,316.5)
        //   t=1,816 uid1215 obj(2143.5,225) player(2143.5,224.3)
        //   t=4,664 uid4355 obj(6735,399)   player(6735.3,399.0)
        //   t=5,120 uid4467 obj(6875,885)   player(6878.3,885.3)
        //   t=5,968 uid5809 obj(8247,963)   player(8247.2,961.0)
        // In all five the player is ON the trigger in both axes within 3.3 px,
        // which is why this used to test both. **BOTH HALVES ARE WRONG.**
        // Falsified 2026-08-15 on lv22's own cold run, twice, from GD's dump
        // for the plan the DP built (model and GD bit-identical either side):
        //   t=1,746  frame 0 -> 1 on uid1343 (2,265, 316) with the player at
        //            (2,265.4, **1,306.2**) -- 990 px ABOVE it in y
        //   t=2,579  frame 1 -> 0 on uid1215 (2,143.5, 225) with the player at
        //            (**5,027.8**, 224.8) -- 2,884 px away in x
        // The travel axis is the only one that matters: in frame 0 the player
        // crossed the trigger's world X, in frame 1 it crossed its world Y, and
        // the other coordinate was arbitrary both times. Those five samples
        // were routes that happened to pass over their own trigger.
        // Without this the model plans the whole rotated section in frame 0 --
        // lv22's cold run flew 990 px over the entrance, GD turned, and it died
        // 900 ticks later at x=5,112 with the death looking like a spike.
        //
        // What DOES gate it is the level's own x window: an object is not live
        // until the level has scrolled past it. Dropping the test entirely is
        // wrong in the other direction -- the same replay then turns back to
        // frame 0 at t=1,771 on uid11177 (16,365, 1,275), whose frame-1 travel
        // coordinate the player crosses while it is still at x=2,310, i.e.
        // **14,055 px before that part of the level exists**.
        // `r.cx <= wxP` fits all seven observations at once:
        //   FIRE   uid1343 (2,265)   player x 2,265.4     (frame 0, crossing)
        //   FIRE   uid1215 (2,143.5) player x 5,027.8     (frame 1, 2,884 back)
        //   FIRE   uid4355 (6,735)   player x 6,735.3
        //   FIRE   uid4467 (6,875)   player x 6,878.3
        //   FIRE   uid5809 (8,247)   player x 8,247.2
        //   SKIP   uid11177 (16,365) player x 2,310.5     (14,055 ahead)
        //   SKIP   uid5957 (8,297)   player x ~6,735      (1,562 ahead)
        // In an even frame the crossing already implies it, so this only bites
        // in a turned one -- which is exactly where `frameU` stops meaning
        // "world x" and a trigger anywhere in the level can match by y.
        // The 30 is one tick of slack for the crossing tick itself.
        // OPEN: "has ever reached" would be the literal reading of scrolling
        // and would differ from "is at" for a player that drifted back left
        // inside a turned frame. lv22 never does, so the cheaper form stands
        // (the alternative costs a State field and a dedupe-key bit).
        double wxP, wyP;
        fromFrame(f0, (double)c.xAbs, (double)c.y, wxP, wyP);
        if (r.cx > wxP + 30.0) continue;
        // ...and the PERPENDICULAR axis has to overlap. [2026-08-16 revived]
        // The note below says both halves are wrong; the perpendicular half is
        // NOT. GD was asked directly (cfg hitboxtrace=1 -> `rot:` lines) on the
        // x=15,656 plan and named its six firings:
        //   t=1745 (2265,316)   player(2265.4, 316.5)   du 0.4  dv 0.5
        //   t=1816 (2143.5,225) player(2143.5, 224.3)   du 0.0  dv 0.7
        //   t=4435 (6735,399)   player(6735.4, 406.5)   du 0.4  dv 7.5
        //   t=4893 (6875,885)   player(6849.6, 885.8)   du 25.4 dv 0.8
        //   t=5759 (8247,963)   player(8247.0, 961.0)   du 0.0  dv 2.0
        //   t=6093 (8297,423)   player(8275.9, 421.9)   du 21.1 dv 1.1
        // Every |dv| is under 8, and the two it did NOT fire on that same run
        // are uid6308 (15,399,615) |dv|=89.4 and uid6307 (15,405,859)
        // |dv|=330.9 -- the player crossed both travel coordinates.
        // The level's own layout settles it: lv22's 2900s past x=15,000 come in
        // PAIRS 6 px apart in x and 126-366 px apart in y, each half setting the
        // OPPOSITE direction (15,399 gnd=4 / 15,405 gnd=2; 16,005 gnd=1 /
        // 16,011 gnd=3; 16,125 gnd=1 / 16,131 gnd=3). Only the player's height
        // can pick between them, so a travel-axis-only rule cannot be right.
        // Without this the model turned at x=15,405 where GD's gframe stays 0,
        // and planned the rest of the level in a frame GD does not have.
        // The travel-axis CROSSING above stays: it is what stops a fast tick
        // from stepping over the box, and dropping it is the 2026-08-15 fix.
        //
        // [2026-08-24] **30 IS FALSIFIED, and the window was doing the pair's job.**
        // Read off GD's own gframe changes in two full lv22 runs, measured on the
        // tick BEFORE the change (once gframe moves the reported x/y are in the new
        // frame, and comparing those to the trigger's world position is nonsense --
        // that mistake is what made this look settled):
        //   FIRES  0, 0, 0.5, 1.2, 1.3, 2.0, 3.7, 5.6, 6.0, 9.3, 12.0, 14.1, 14.7,
        //          24.4, 25.6, 29.3, and **116.7**
        //   SKIPS  274.4 (uid16911), 657.8 (uid11177)
        // The 116.7 is uid11342 (20115,723) on lv22's cold route -- THE SAME TRIGGER
        // the known-good route fires at 29.3. One trigger, two heights, both fire, so
        // no window of 30 can be right. lv22's 84% wall is exactly this: the model
        // dropped that firing, planned the rest of the level in frame 0, and every
        // route it found there is in a world the game does not have.
        //
        // What the window was really for is the PAIRS (the note above): two triggers
        // 6px apart setting opposite directions, where "only the player's height can
        // pick between them". That is a CHOICE, not a range -- so the choice is made
        // explicitly below (nearest |dv| wins) and the window goes back to being what
        // it says it is, "the player is at this trigger rather than somewhere else in
        // the level". Bracketed by the measurements above at (116.7, 274.4); 150 sits
        // in that gap with ~30% margin either side. The older note's non-firings at
        // 89.4/330.9 are from a lineage this could not re-measure -- if 89.4 is real
        // and not a pair losing to its partner, this is too wide and the regression is
        // where that shows up. ([[gd-constant-justification-goes-stale]])
        const double dv = std::fabs((double)c.y - frameV(f0, r.cx, r.cy));
        if (dv > kRotPerpWin) continue;
        if (same) { if (g_revToggle && r.setRev < 0) toggleRev = true; continue; }
        // NEAREST WINS. The loop used to let the last match overwrite the earlier
        // ones, which is only safe while the window is too narrow for two to match at
        // once -- i.e. the tight window was hiding this. With the pairs 6px apart in
        // travel and 126-366 apart perpendicular, both halves now match and the height
        // has to choose, which is what the note above says GD does.
        // ...and the winner is adopted **inside the loop**, exactly where the old
        // code adopted the last match. That is not a style point: `c.rev` is read
        // by the `same` test at the top of this loop, so a trigger adopted here
        // decides whether the NEXT one counts as a rotation or as a reverse-travel
        // toggle. Deferring the write until after the loop -- which is how the
        // nearest-wins choice was first written -- classifies every trigger against
        // the ORIGINAL heading, and that is a different rule even on a tick where
        // only one trigger ends up winning.
        //
        // Bisected 2026-08-25, in-process cold lv22, same binary either way:
        //   db91ab5 (adopt in the loop)      x=20,135  84%, first veto x=8,294
        //   0a1e8b6 (adopt after the loop)   x=2,266   stuck at the level's FIRST
        //                                             2900, first veto x=2,266
        //   0a1e8b6 with --rotperp 30        x=2,266   -- so it was never the
        //                                             window that did it
        // The pair choice the rewrite was for is kept: a strictly nearer match
        // still overwrites a farther one, and with one match this is the old code.
        // --rotlast drops even that and restores the plain LAST-MATCH-WINS rule,
        // so the whole pre-0a1e8b6 function can be put back with one flag (plus
        // --rotperp 30) and A/B'd against everything that was built on top of it.
        if (g_rotLast || dv < bestDv) {
            bestDv = dv;
            best = &r;
            if (r.setRev >= 0) c.rev = (uint8_t)r.setRev;   // absolute value
            nf = r.frame;
            nflip = r.setFlip;
            rotModY = r.vmodY;
            rotOvr = r.ovrVel;
        }
    }
    if (toggleRev) c.rev = (uint8_t)!c.rev;
    if (nf < 0) return -1;
    // A same-frame firing (one that sets reverse travel as an absolute value via
    // gnddir) changes **only the heading**. Coordinates and velocity stay as they
    // are -- running it through the re-basing below (fromFrame/toFrame and
    // kRotCarry) would replace the vy of an ongoing jump arc with the dx-derived
    // carry. Measured (t=18,382 from the note at the head of this function,
    // uid16659): dx +1.613 -> -1.613 with "the y progression does not change" --
    // GD's vy simply decays normally, 9.598 -> 9.404, across the turn tick.
    // [2026-08-18]
    if (nf == f0) {
        if (nflip >= 0) c.flip = (uint8_t)nflip;
        return nf;
    }
    // The point that carries over is the one GD's collision pass sees: AFTER the
    // move, BEFORE the button (see the re-tap below). stepOne already applied
    // the tap, so for the one case where that moves the player discontinuously
    // -- a grounded spider -- the tap's y is undone here and redone in the new
    // frame. Measured on lv22 t=1,745: GD's dump reads y=316.500 on the rotation
    // tick, i.e. the parent's y untouched, while the model had already dropped
    // it to 313.500 with the old frame's tap.
    const bool reTap = (input == 1 && c.mode == 6 && wasGrounded);
    if (reTap && havePrevY) c.y = (float)sPrevY;
    double X, Y;
    fromFrame(f0, (double)c.xAbs, (double)c.y, X, Y);
    double nu, nv;
    toFrame(nf, X, Y, nu, nv);            // no origin correction is needed
    c.xAbs = (float)nu;
    c.y = (float)nv;
    // ---- vy hand-over [2026-08-19 settled -- replaces the whole old kRotCarry set] --
    // Cross-checking the disassembly of PlayerObject::rotateGameplay (0x399d50)
    // against lv22's raw level data (keys 169/582/583/584 of the 2900s) closed all
    // 15 of gdref's transitions, all 15 exactly (findings.md 2026-08-19, item 5):
    //   The velocity swap runs **only when** the vertical/horizontal flag
    //     (mvdir 3/4 = vertical) changes: yvel := travel speed (= |dx|/0.25, always
    //     positive) x modY. The old vy is discarded (outside platformer mode the
    //     old-vy component is thrown away wholesale).
    //   modY is per trigger instance: if m_editVelocity(169) is set, then
    //   m_velocityModY(583, member default 0.0); otherwise pass-through (x1).
    //   m_overrideVelocity(584) is an absolute assignment, not a multiply (no
    //   example in lv22).
    //   vertical/horizontal = parity of the true frame. A transition that keeps the
    //   parity (true 2->0, gdref t=13,646) runs no swap at all and leaves vy as
    //   is -- in the model's representation that is same frame + rev, so the
    //   nf==f0 early return above already behaves that way.
    // Every constant and exception of the old implementation was a misreading of
    // this law:
    //   kRotCarry 0.90 = (dx/0.25)/(dx/0.225). The ball's 0.99 was modY 1.1
    //   misread (6.457x1.1 = 7.175x0.990, indistinguishable from one sample).
    //   t=4,665's "starts from rest" = editVelocity=1 with 583 missing (x0).
    //   t=6,094's "mystery of the ball's magnitude" = modY -1.21. t=16,956's
    //   "robot has no mirror" = modY +1 (the paired trigger at t=4,894 is -1 -- it
    //   was the trigger's sign, not a mode mirror).
    // Mapping onto the internal convention: the model's frame 3 has +v=-X (the
    // parser's kUpFor correspondence), opposite to GD's local up (+X), so only
    // enf==3 flips the sign. gdref measurements, 3 points:
    //   t=11,344 (0->3)                     GD +5.193 -> internal -5.192
    //   t=11,626 (true 3->2 = frame0+rev1)  GD +5.193 -> internal +5.192
    //   t=1,817  (1->0)                     GD -5.193 -> internal -5.193
    // dxUsed arrives **negative** in reverse travel, so only its magnitude is used
    // (measured lv22 t=11,965, f0=0 rev0=1 -> nf=1: with the sign included the
    // world-x direction is opposite to GD's).
    const int ef0 = (f0 + 2 * rev0) & 3;
    const int enf = (nf + 2 * (int)c.rev) & 3;
    if (((ef0 ^ enf) & 1) != 0) {
        double vyGd = (double)rotModY * std::fabs(dxUsed) / 0.25;
        if (rotOvr) vyGd = (double)rotModY;   // absolute assignment (no example in lv22)
        c.vy = (float)(vyGd * (enf == 3 ? -1.0 : 1.0));
    }
    // [2026-08-21 r52] Carry "the tick the frame changed" one tick forward
    // (see the note on State::frameChg. The rotation hits **after** stepOne, so an
    //  assignment on the stepOne side alone never sets it -- that misfired once).
    if ((uint8_t)nf != c.frame) c.frameChg = 1;
    c.frame = (uint8_t)nf;
    // ...and the trigger SETS the gravity direction too (m_moveDirection, the
    // `mvdir` column). The model used to carry `flip` across a rotation, which
    // is only right when the trigger happens to agree. Measured 2026-08-15 on
    // lv22's cold run, GD's dump against the trace's own `flip` column:
    //   t=1,746  uid1343 mvdir=4 (right), nf=1: GD upsideDown stays 1
    //   t=2,579  uid1215 mvdir=2 (down),  nf=0: GD upsideDown **1 -> 0**
    // The carry got the first one right by luck and the second one wrong, and
    // from that tick the model's gravity stepped +0.216/tick where GD stepped
    // -0.216 -- the whole post-rotation section solved upside down.
    if (nflip >= 0) c.flip = (uint8_t)nflip;
    // Entering frame 3 flips `upsideDown`. Measured directly with the trace's
    // own `flip`/`frame` columns against GD's dump: the two agree on every tick
    // up to t=4,664 and disagree from t=4,665 -- the rotation tick itself --
    // where GD goes up 0 -> 1 and the model stays 0. The frame-1 rotation
    // (t=1,745) does NOT flip it, which is the odd frames' opposite handedness
    // showing up (`toFrame` gives frame 1 v = +X, frame 3 v = -X).
    // Without this a type-4 portal asks `c.flip != 0`, reads 0, and is skipped
    // as inert -- GD fires it at t=5,110 and halves vy (12.824 -> 6.413).
    // (the frame-3 mirror is handled by gdUpOf, where GD's statement is read --
    // flipping it HERE puts the sign in the physics and makes the player fall
    // the wrong way in world; tried and reverted)
    c.grounded = 0;
    c.snapObj = nullptr;
    c.snapDist = 0.f;
    c.onSlope = 0;
    c.slopeM = 0.f;
    c.usedOrb = nullptr;
    c.bandFloor = 0.f;
    c.bandCeil = 1e9f;
    c.bandRefY = 0.f;
    // ---- the tick's BUTTON, run again in the NEW frame -----------------------
    //
    // GD's per-tick order is update -> checkCollisions -> buttons. The rotation
    // trigger fires in the collision pass, so a tap on the SAME tick is served
    // by a world that has already turned. stepOne applied it in the old frame
    // and everything above then overwrote x, y and vy, so the tap vanished.
    //
    // Measured on lv22's cold run (2026-08-16), the entrance to the first
    // rotated section, model and GD bit-identical up to the tick:
    //   t=1,744  both (2,264.850, 316.500) vy=0.000 frame 0
    //   t=1,745  GD (**2,143.500**, 316.500) vy=**-1.000** frame 1
    //            model (2,266.149, 313.500) vy=+5.193 frame 1
    // -1.000 is the teleport's own signature (see the spider tap in stepOne), and
    // 2,143.5 is a landing along the NEW frame's vertical (world X). The model
    // kept the rotation's carry and never teleported, and every tick after that
    // belongs to a different run -- the whole reason lv22's cold stalled at
    // x=2,275 with a frontier that otherwise reaches x=23,513 of 24,085.
    //
    // Only the SPIDER needs this: it is the one mode whose button moves the
    // player discontinuously along the gameplay vertical, which is the axis the
    // rotation just redefined. A cube/ball/ship impulse is a velocity, and the
    // rotation sets the velocity itself from the carry.
    if (reTap) {
        Level* nlv = levelOfFrame(nf);
        if (nlv) {
            const double pH = c.mini ? kSpiderHalfMini : kSpiderHalf;
            // the new frame's near list around the landing column
            std::vector<const Obj*> nr;
            const double lo = (double)c.xAbs - 40.0, hi = (double)c.xAbs + 40.0;
            auto lb = std::lower_bound(
                nlv->objs.begin(), nlv->objs.end(), lo - 80.0,
                [](const Obj& o, double v) { return o.cx < v; });
            for (auto it = lb; it != nlv->objs.end() && it->cx <= hi + 80.0; ++it)
                nr.push_back(&*it);
            nlv->dyn.collect(Dynamics::NEAR, lo, hi, nr);
            StepCtx NK{};
            NK.near = &nr;
            NK.t = t;
            double bestY = 0.0;
            bool hazTgt = false;
            if (spiderTargetY(NK, (double)c.xAbs, (double)c.y, c.flip != 0,
                              c.mini != 0, pH, bestY, nf == 0, &hazTgt, nf)
                && !hazTgt) {
                const double gs = c.flip ? -1.0 : 1.0;
                c.y = (float)bestY;
                c.flip = c.flip ? 0 : 1;
                c.vy = (float)(1.0 * gs);   // the teleport's own +-1.000
                c.grounded = 1;
                c.snapObj = nullptr;
            }
        }
    }
    return nf;
}

inline State stepOne(const State& s, int input, const StepCtx& K, bool& dead) {
    State c = s;
    // The no-control window (id 2899 / GameOptionsTrigger; history at the
    // declaration of g_ctrlWin). **Only the button fails to reach the physics**;
    // the held-button bookkeeping stays alive: the caller writes `c.action = curIn`
    // from the raw input, so a hand kept pressed across the window counts as a
    // continuation from the tick the window lifts (GD measurement: on lv22, holding
    // from t=14,100 onward makes the ship climb from the lift at t=14,225).
    if (!g_ctrlWin.empty() && ctrlOffAt(K.t)) input = 0;
    c.pFlap = 0;   // 1-tick lifetime (see the note on State::pFlap)
    c.pBallOff = 0;   // same (State::pBallOff)
    c.pExitVy = 0.f;  // same (State::pExitVy, r93)
    c.pNoTerm = 0;    // same (State::pNoTerm, r102)
    // The velocity-limit exemption is swing-scoped for now (State::boost):
    // outside mode 7 nothing maintains it, so drop it rather than let a stale
    // bit cross a mode portal.
    if (c.mode != 7) c.boost = 0;
    // ONE float x per state, advanced and snapped exactly as GD does it.
    // It used to be `shared double base + per-state xAdj`, which cannot be exact:
    // GD writes the stair snap straight into the same float it accumulates into,
    // so after a snap its rounding follows a different path than base+offset.
    // That was the last 0.003 px, and 0.003 px is not academic -- lv5 t=8482 sat
    // exactly on |x - cx| = 30.0, where the model let go of the ledge and GD did
    // not.
    // This state's own speed (see State::dx); 0 = never set, use the layer's.
    // EVERYTHING speed-dependent below reads it -- the x advance, the stair
    // table, the cube's jump/gravity, the ball's flip value, the ship params
    // and the ring scale. They used to read the LAYER's dxF, which is the same
    // number only while the whole frontier shares one speed. On lv18 it does
    // not: the layer accumulator picked up every speed portal ANY state could
    // touch and ran up to 1,700 px ahead of the plan's own x (measured, see
    // docs/findings.md), so the search planned against displaced geometry and
    // handed 1.1's constants to states that were still at 0.7.
    const float useDx = (s.dx > 0.f) ? s.dx : K.dxF;
    c.dx = useDx;
    // TIME WARP (id 1935). A pure TIMESCALE, not a speed: the x advance, the y
    // integration and the velocity increment are all multiplied by the same
    // factor, while the dump's `speed` column does not move at all. Read off x
    // rather than carried in the state -- it is a monotone function of x and
    // the level's warps sit outside the rotated section, so there is nothing to
    // remember. `xPrev` because the effect starts the tick AFTER the crossing,
    // the same +1 every autotrigger has (measured on lv22: the player crosses
    // cx=4,535 during t=3,131 and t=3,132 is the first slowed tick; the
    // partner at cx=4,615 is crossed during t=3,335 and t=3,336 is normal
    // again).
    const double tScale = timeWarpAt((double)s.xAbs);
    // [2026-08-21 r44] The free-integration y from **before** the ride overwrites
    // y. Written just before the slope block, read on the portal side (a different
    // scope).
    float yFreeBeforeSlope = 0.f;
    // [2026-08-27] **The y the UPDATE phase wrote, before any collision clamp.**
    // GD's tick is update -> collisions -> buttons, and the collision pass reads
    // the player rect it snapshotted at entry (disassembly 0x2149b8; the long
    // note at `yColl`). Every clamp, seat, ride and push-out below happens
    // INSIDE that pass, so none of them is visible to it -- which makes this,
    // not `c.y`, the position the pass works from.
    // Only the DUAL BIRTH reads it so far (`c.y2` in the type-23 branch);
    // `yColl` and `yPort` still carry their own approximations of the same
    // quantity and are left alone on purpose (they decide portal timing on
    // every level, this decides where a second body appears).
    // Written next to each mode branch's own integration, never next to a
    // clamp. `s.y` is the right value for a branch that integrates nothing:
    // a body at rest moves by kYScale * 0 = 0.
    float yFree = s.y;
    // [2026-08-21 r52] Whether a "gravity portal right after a frame change" was
    // suppressed on this tick. Needed so State::frameChg is not lowered while the
    // suppression continues.
    bool gravHoldOver = false;
    // [2026-08-21 r61] Whether this tick's ride was a "push-out with gravity still
    // pointing the other way" (the branch that left vy and grounding in place).
    // Used only when a gravity portal at the end of the tick returns to upright,
    // to apply GD's landing on the spot.
    bool rodeFlipped = false;
    // [2026-08-21 r66] Whether a ceiling-ramp push-down (upceil) took effect on
    // this tick. Read by the release test at the end of the tick (ceil/release).
    bool ceilPressedNow = false;
    double ceilPressM = 0.0;
    // Reverse travel (State::rev) decreases x. `useDx` is a speed (positive), so
    // the sign is applied to the state-side advance as well. Drop this and only the
    // replay's local `x` goes negative while the state's xAbs keeps growing: it
    // matches GD on the 1st tick only and diverges from the 2nd.
    c.xAbs = advanceX(s.xAbs,
                      (float)(useDx * tScale * (s.rev ? -1.0 : 1.0)));
    const double x = (double)c.xAbs, xPrev = (double)s.xAbs;
    // The start-of-tick point in WORLD coordinates, for the modifier boxes.
    //
    // `frameLevel` turns the colliders, portals, pads, orbs, speeds, slopes and the
    // moving geometry into each rotated frame -- but NOT the modifier lists (force
    // boxes 2069, force fields 3645, the head-flip box 2866, the dash stop 1829).
    // Those are loaded once, in world coordinates, and State::xAbs/y are always the
    // CURRENT frame's coordinates, so a rotated frame was asking them about a place
    // the level does not have. The box does not misfire, it silently stops applying.
    //
    // Measured, lv22 t=16,462-16,471 (robot in frame 3, inside uid17701 -- whose
    // robot strength 0.330 is already in the table in modifiers.hpp, fitted at
    // t=16,367 in frame 0). GD's dvy is +0.330 for exactly the ten ticks the
    // player's lower edge is still below the box's top edge at y=733.5, and +0.000
    // from the tick it is not. The model held +0.000 throughout -- the robot hover's
    // own shape with the field missing -- which is 10px off GD's arc by t=16,480,
    // and it then planned the rest of the shaft in a place GD is not.
    //
    // Nothing about the DIRECTION needs turning: `acc += forceBoxAcc(...) * gsign`
    // and `c.vy = vp * gsign` cancel, so the exported dvy is +k whatever the frame
    // and whatever the flip. That is what GD does -- the same +0.330 appears at
    // t=16,367 (frame 0, upright) and at t=16,462 (frame 3, flipped) -- and it acts
    // on the frame's vertical axis rather than on world y: the travel advance
    // through those ten ticks stays +1.95/tick to the last digit.
    //
    // The time warp above stays on the frame's own x: the level's warps sit outside
    // the rotated sections (see tScale). The deadbands below get the same world
    // treatment, for the same reason -- they are authored in world coordinates.
    double modX = xPrev, modY = (double)s.y;
    if (s.frame & 3) fromFrame((int)s.frame, xPrev, (double)s.y, modX, modY);
    // ...and the sign that family converts with. The strengths were fitted against
    // GD's dump, whose "up" in frame 3 is the MIRROR of the model's internal one --
    // the same statement gdUpOf makes about lip and the trace writer makes about
    // vy. Identical to gsign in frames 0/1/2. (History: the modifier-boxes commit.)
    const double gdSign = gdUpOf(s) ? -1.0 : 1.0;
    // [2026-08-25] A "recorded band ceiling holds in turned frames" kill lived here
    // for half a day and was measured FALSE both ways: the reference run spends
    // 1,282 of its 3,260 turned-frame ticks above pmax-15 (up to +1,069px -- the
    // tower at t=13,33x), and inside the DP the kill pruned exactly the
    // reference-shaped climb (the west weave whose jump apexes cross pmax-13,
    // killed at t=16,554 on a bit-exact replay), leaving only the eastern dead-end
    // branches that die under the roof at (20,175,1,215). The ship wedge at
    // y = pmax-15 exactly is real, but it is a property of that worldline's frozen
    // camera, not a law of the ceiling; the wedge machinery (stall guard, wedge
    // credit, mode-specific veto) owns that case now.    // --deadband x0,x1 (repeatable): discard branches that enter this x band.
    //
    // Why it is needed: a deepest-first driver keeps choosing "dead ends that are
    // merely deep".
    // lv22's fast lane jams at x=5,097 (confirmed by measurement: GD section solver,
    // 1,743 layers + 4 entry steps, all EXHAUSTED), but the branches that reach it are
    // the deepest, so the correct slow lane is never chosen. A point DOOMED is not
    // enough; the **whole band** is closed to force the split earlier. Like needtrig
    // this is search guidance, not a seed (the band comes from a measured jam; it does
    // not violate CLAUDE.md's cold rule).
    // ...and a band is WORLD-COORDINATE guidance, so it is tested against the world
    // point, not the frame's own. Every band that exists is authored that way: the
    // phantom veto draws its box around GD's dump state (repair.hpp, checkPhantom) and
    // a hand-written --deadband is read off GD too. Tested in frame coordinates a band
    // in a turned stretch cannot match anything -- lv22's cold run vetoed the wall at
    // (2266,229) twice while the search was in frame 1, where the travel coordinate is
    // about -229 and the band asks for 2262..2270, and then spent 114 rounds walking
    // back into the box the veto had already closed.
    if (!g_deadBands.empty()) {
        double dbX = x, dbY = (double)s.y;
        if (s.frame & 3) fromFrame((int)s.frame, x, (double)s.y, dbX, dbY);
        for (const auto& db : g_deadBands) {
            if (dbX < db.x0 || dbX > db.x1 || dbY < db.y0 || dbY > db.y1)
                continue;
            // mode semantics at the struct: -1 all die, >=0 forcing, <=-2 only
            // that one mode dies (what a refutation honestly supports).
            const bool hit = (db.mode == -1)
                || (db.mode >= 0 && (int)s.mode != db.mode)
                || (db.mode <= -2 && (int)s.mode == -2 - db.mode);
            if (hit) {
                dead = true; g_deadWhy = "deadband"; g_deadObj = nullptr;
                return c;
            }
        }
    }
    // GD's MAX GAMEPLAY Y (--maxplayy; the disassembly is at g_maxPlayY's
    // declaration). World y above the bound is out of bounds; GD destroys with
    // a NULL object after two consecutive out-of-bounds ticks. Checking the
    // INCOMING state's world y lands the model's death on the same tick as
    // GD's latch for a state that stays out; one that pokes above for exactly
    // one tick dies here but survives in GD -- the bound sits 390px above the
    // level's top structure, nothing playable does that. This is what closes
    // the sky-escape phantom: lv22's cold run was pinned at 41% planning a
    // free ball climb to y=3,600+ that GD kills (fresh world at the bound
    // 3,675; the loop's own dirty worlds even earlier). Rotated frames map
    // through fromFrame the same way the deadbands above do. The mini bound
    // adjustment (checkCollisions adds (1-sizeFactor)*vehicleSize*k) is left
    // off: it only RAISES the bound, so full-size is exact and mini errs a few
    // px early at an altitude nothing reaches legitimately.
    if (g_maxPlayY < 1e17) {
        double wy = (double)s.y;
        if (s.frame & 3) {
            double wx;
            fromFrame((int)s.frame, x, (double)s.y, wx, wy);
        }
        if (wy > g_maxPlayY) {
            dead = true; g_deadWhy = "maxplayy"; g_deadObj = nullptr;
            return c;
        }
    }
    // Player half sizes for THIS tick. Mini shrinks every box, so the rest of
    // the body reads them from here instead of from the constants. The size
    // portal is resolved at the end of the tick, so a change only takes effect
    // on the next one -- which is what GD does (lv11: the portal box is entered
    // at t=3384 and the player first loses support at t=3385).
    // ...and the WAVE's is 10, not 15 (see kWaveHalf). This has to be the
    // half used for PORTAL, pad and orb contact too, not just for collisions:
    // with 15 the model entered lv17's ship portal at (8865,195) four ticks
    // early (it fires at |x - cx| <= hw + half, so 8833 instead of 8838) and
    // came out halved and in the wrong mode while GD was still a wave.
    // `s.mode` and not `c.mode`: on the tick a portal switches modes, the
    // contact test that switched it belongs to the OLD body.
    const double pHalf = playerHalf(s.mode, c.mini != 0);
    // pInner is NOT scaled by mini. Bracketed on lv12's block at x[5820,5850]
    // y[180,210], where a mini cube falls past the left face:
    //   t=4479  |dx| 21.19  |dy| 11.17  GD does NOT kill  -> pInner <= 6.19
    //   t=4480  |dx| 19.89  |dy| 14.39  GD KILLS           -> pInner >  4.89
    // so pInner is in (4.89, 6.19] and the full-size 5.0 sits inside it, while
    // the scaled 3.0 does not. With 3.0 the model never killed at all: by the
    // time |dx| dropped under 18 the cube had fallen out from under the block,
    // and the DP happily planned straight through a wall GD stops it at.
    const double pInner = kCubeInner;
    dead = false;
    // Was the cube touching a solid from below at THIS tick's x, before the
    // input was applied? The stair snap needs this rather than c.grounded: on
    // the tick the player jumps, c.grounded goes 0, but GD still resolves the
    // bottom collision and still refreshes m_snapDistance. Gating on c.grounded
    // dropped exactly that one tick, leaving every snapDistance 1.308 px (one
    // tick of x) short -- measured against GD on lv4, every snap event.
    bool cubeContact = false;
    // Whether this is a tick on which GD called `runNormalRotation` (= the tick
    // that rewrites the rotation's sign). The callers are **take-off and
    // rings/orbs/pads**; **portals do not call it**.
    // Measured (lv20): the green ring at t=7,154 (flip 0->1) reverses the sign of
    // rot, but the gravity portal at the teleport destination at t=7,282
    // (flip 1->0) does not, and the next reversal was the yellow pad at t=7,286
    // (flip stays 0, sign +). "Rewrite whenever flip changes" would wrongly
    // rewrite at 7,282.
    bool rotWrite = false;
    // ...and whether that call came from a PAD, which additionally turns the
    // sprite one full step on the tick itself (measurements at the use site).
    bool rotPadSpin = false;
    // ...and the sign written is "the gravity **at the moment of the call**".
    // Inside ringJump the call site comes after the green ring's (29) flip
    // (0x3993bf) and before the gravity orb's (13) flip (0x399608). The
    // measurements mirror that order exactly:
    //   lv20 t=7,154 green ring  flip 0->1 -> rot **does reverse** (new gravity)
    //   lv22 t=685   gravity orb flip 0->1 -> rot **does not reverse** (old gravity)
    // So "the new one whenever flip changes" breaks lv22 (measured: tracking fell
    // from 400 -> 181 ticks). Default to the value from before entering the
    // branch, and override for the green ring only.
    uint8_t rotWriteFlip = 0;
    // The surface a standing player is being CARRIED by this tick, if it moved
    // (see the ride block near the stair snap). Only a MOVING support sets it,
    // so a static floor leaves everything bit-identical.
    bool rideOn = false;
    double rideFace = 0.0;
    // ...and how far that face moved THIS tick. The ride carries the player by
    // the face's delta rather than re-seating it on the face, so an offset the
    // state came in with survives. It only differs after a RE-ANCHOR: from the
    // head the player is exactly on the face and the two are the same number.
    // Measured on lv22 (2026-08-14): the ladder's anchor at t=5,769 sits in the
    // middle of a sinking-floor ride in the ball corridor, and the re-seat
    // dragged the player 2.559 px down on the very first tick
    // (`first divergence t=5,770 dy=-2.5590 dvy=+0.0000`), so every anchor taken in
    // that corridor started 2.5 px below GD.
    double rideDy = 0.0;
    double rideGap = 0.0;   // how far the chosen face is from the foot
    // A pad sitting ON a block fires in the same tick the cube would land on
    // that block's corner. GD resolves both in one pass and the pad's upward
    // velocity wins: the player never rests on the block. The model landed
    // first, so its y was snapped to the surface and ran 0.71 px above GD from
    // there on (measured on lv6, t=4502, yellow pad on the block at x=5864).
    bool landedThisTick = false;
    // [2026-08-21 r94] **When several faces satisfy the landing condition on the
    // same tick, the face that is highest "up" along the gravity direction wins.**
    // Not scan order. Measured lv16 t=17,627 (a cube falling at the end lands on
    // the corner at x=27,150): overlapping there are
    //   uid8869 (30x30 block, top face **600.000**)
    //   uid8868 (30x1.5 slab, top face **599.95**)
    // and GD landed at 615.000 (=600+15), the model at 614.950 -- the loop's
    // **last-one-wins** let the lower slab overwrite. The flight branch's support
    // scan has long taken "the face that actually supports = highest along the
    // gravity direction" (see the note at lv19 t=21,746). Same rule for landing.
    double landFaceBest = 0.0;
    float preLandY = 0.f;
    // ...and the SAME start-of-tick problem hits the stair snap. `cubeContact`
    // above is the support the state came in with, so it covers "was standing,
    // then jumped" but NOT "landed and jumped on the same tick" -- there
    // s.grounded is 0 (airborne) and c.grounded is set back to 0 by the jump
    // (see the cube branch by `impulsedThisTick`), so the snap block never ran.
    // GD calls checkSnapJumpToObject from the landing branch of
    // collidedWithObjectInternal, i.e. on the collision itself, before the
    // buttons: measured on lv21 t=1172 (cfg snaptrace=1) GD logs
    //   prev=567,(1425,165) obj=664,(1515,225) dx=90 dy=60
    //   x=1521.5475->1522.5475 d=+1.0000 sd=13.4615->7.5475
    // -- the `big` pattern, off a buffered press. The model held the identical
    // snapObj and snapDistance (13.4615) and simply did not make the call, so it
    // ran exactly 1 px behind GD from t=1173 to the end of the level (18,700
    // ticks), which is the whole of lv21's x divergence.
    // Kept separate from `landedThisTick` because `releasePin` clears that one,
    // and GD's call happens whether or not a pad later undoes the landing.
    bool cubeLandedThisTick = false;
    // [2026-08-21 r84] The tick on which, mid-ride, the player moved onto a "step
    // deeper than the line". The ride ends there (on a tick with this flag set both
    // the stick and the seat-priority are stopped -- otherwise the slope pulls the
    // player straight back to the seat and the step is as if it never existed).
    bool stepLandedThisTick = false;
    // The same thing for a player that was ALREADY resting on a block: it also
    // carries this tick's gravity step until something re-seats it. Filled in
    // by the cube branch (see the long note at `pinnedOnBlock`), consumed by
    // whatever launches the player out of the pin.
    //
    // `pinnedOnBlock` is tested BEFORE `landedThisTick` and not after: a ramp
    // ride sets BOTH (the slope code books a landing on every riding tick), and
    // taking the landing branch there puts the player straight back on the
    // ramp's surface -- which is exactly the 0.0486 px this is here to keep.
    // They are otherwise mutually exclusive (landing from the air means the
    // state came in with grounded = 0, so nothing was pinned).
    bool pinnedOnBlock = false;
    float prePinY = 0.f;
    // Whether this tick jumped through a teleport portal (pads/orbs at the landing site
    // count from the next tick).
    bool teleportedThisTick = false;
    // ...and **the uid of the teleport that fired on that tick**. GD processes the
    // objects of one section in uid order, and a teleport does not change x
    // (ignoreX/Y=0), so the landing site falls in the **same section**. That is:
    //   uid > teleport's uid -> processed after the teleport = **fires on the
    //   landing tick**
    //   uid < teleport's uid -> already processed = judged at the old position
    //   (usually does not fire)
    // Measured (GD, cfg padtrace=1/hitboxtrace=1):
    //   lv20 dump t=7,282: teleport uid7021 -> yellow pad uid7025 fires on the
    //     **same tick**, vy 15.000 -> -16.000 (p=(10830.065,161.000) = post-landing
    //     coordinates), then uid7026 re-fires with the same value, and finally the
    //     gravity portal uid7028 flips + halves to -8.000. All three uids are
    //     greater than 7021
    //   lv21 dump t=4,710: teleport uid5090 -> gravity portal uid5050 fires on the
    //     **next tick**. Smaller uid
    // A blanket "landing site counts from the next tick" breaks lv20; a blanket
    // "same tick" breaks lv21. uid order explains both.
    int teleUid = -1;
    // The SPIDER's tap moves y discontinuously, and GD's collision pass never
    // looks at the path it did not travel. Set where the teleport lands, read
    // by the hazard sweep below (which otherwise samples s.y -> c.y and kills
    // on anything in between).
    bool spiderWarpedThisTick = false;
    auto releasePin = [&] {
        if (pinnedOnBlock) {
            c.y = prePinY;
            pinnedOnBlock = false;
        } else if (landedThisTick) {
            c.y = preLandY;
            landedThisTick = false;
        }
    };
    // Did this tick consume a discrete impulse (cube jump / ball flip / UFO
    // flap)? GD's per-tick order is update -> checkCollisions -> buttons, so an
    // impulse on the same tick as a MODE portal belongs to the NEW mode.
    // Measured on lv12 t=16112: a grounded UFO enters the cube portal at
    // x=20919 and GD reports vy = 11.1800, a plain cube jump, on that tick --
    // not the UFO's flap 6.871, and not that flap halved by the portal (3.4355,
    // which is what the model produced). The DP was buying a boost GD refuses.
    bool impulsedThisTick = false;
    const bool wasGrounded = (s.grounded != 0);
    // A pad also beats the ball's tap-flip. Measured on lv9 t=9126: a flipped
    // ball on a ceiling taps while touching the pad under it, and GD comes out
    // STILL flipped with the pad's velocity -- the flip never happens. The model
    // flipped first, which then made its own pad test look at the wrong side and
    // skip the pad entirely, so it read -3.354 where GD had -9.600.
    bool ballFlippedThisTick = false;
    // mode 0 = cube, 1 = ship, 2 = ball. Cube and ball share this branch: the
    // ball is a cube with a weaker gravity whose tap flips gravity instead of
    // jumping. Everything else (support, landing, hazards) is identical.
    // mode 3 = UFO. It shares the ship's whole contact story (rides floors,
    // lands from above, inner-box side kills, ceiling ride) and differs only in
    // how vy advances, so it goes down the ship branch rather than getting a
    // third copy of the collision code that would drift out of sync.
    // mode 4 = WAVE. It is not a variant of anything else: there is no gravity,
    // no impulse and no landing -- the player travels on a fixed diagonal whose
    // direction is the input, and the only thing that ever happens to it is
    // dying. Measured on lv17 t=3396..3412 (speed 0.9, full size), holding
    // nothing: x advances 1.29825 and y falls 1.29825 EVERY tick, i.e. exactly
    // 45 degrees, and the dump reports a constant yvel of -5.193 = 4 * dx.
    // (The reported velocity is carried so the dump and the dedupe key stay
    // meaningful; nothing reads it as an integrator.)
    // Contact is the ship's old under-approximation: ANY solid overlap ends the
    // branch. GD lets a wave graze some surfaces, so this is conservative --
    // it can only refuse routes, never invent one.
    if (s.mode == 4) {
        // The wave's box is SMALLER than everyone else's. Measured on lv17
        // t=3564..3565: it comes down the diagonal and stops dead at y=100.000
        // with vy=0 and onGround=1, then slides. The band's floor there is 90,
        // so the half extent is 10, not the 15 every other mode uses.
        const double wHalf = (c.mini ? kWaveHalfMini : kWaveHalf);
        // ...and the HAZARD half is a different one again for MINI. The
        // disassembly note at the hazard branch below says stage 1 is the AABB
        // of `player->getObjectRect()`, and that rect is 10x10 full / **6x6
        // mini** (cfg `hitboxtrace=1`) -- half 5.0 and 3.0. kWaveHalf is already
        // 5.0, but the mini value here (kWaveHalfMini = 2.0) is a bracket taken
        // off a SPEED PORTAL's firing, which is a different code path.
        // Bisected live in GD on both sides of the box, two levels, two speeds:
        //   lv21 t=14411 sp0.9, uid18870 top 124.988, read at x=18713.57
        //     centre 2.762 / 2.962 above -> dies    3.012 / 3.512 -> lives
        //   lv18 t=19426 sp0.7, uid13692 (26958,214) 6x7.2, top 217.6
        //     centre 2.95 above -> dies             3.05 -> lives
        //   lv18 t=19421 sp0.7, same spike, SIDEWAYS (y held at 216, inside the
        //     band): right edge to x=26,952.05 -> dies, 26,951.95 -> lives
        // so the box is square and the boundary is 3.000 on every face, with
        // touching counting as a hit. It is NOT speed-dependent.
        const double wHazHalf = (c.mini ? kWaveHazHalfMini : kWaveHalf);
        // The direction is the input the state came in WITH, not this tick's --
        // GD's per-tick order is update -> checkCollisions -> buttons, so a
        // press lands one tick later. Measured on lv17: the wave is pinned on
        // the floor at y=100 through t=3619 and only starts climbing at t=3620,
        // while the model turned at 3619 and ran a tick ahead from there on.
        // ...and a MINI wave climbs and dives at TWICE the angle. Measured on
        // lv20 t=654, the tick the size portal fires: y steps by 1.29825 while
        // vsize is 1.0 and by 2.59650 from the tick vsize reads 0.6 -- exactly
        // 2x, four digits, with the speed and everything else unchanged.
        // The REPORTED velocity does not double: GD prints -5.193 (= 4 * dx)
        // on both sides of the portal, so `c.vy` keeps the un-doubled value.
        // That matters because the driver re-anchors from GD's yvel column and
        // reads the wave's direction back out of its sign.
        // Without this the model held a 45-degree line where GD dived at 2:1
        // and was 8 px above it within six ticks; lv20's cold run sat at
        // x=907..956 for a dozen iterations.
        const double waveSlope = (c.mini && !g_noMiniWave) ? 2.0 : 1.0;
        const double dyDir = (s.held ? 1.0 : -1.0) * (s.flip ? -1.0 : 1.0);
        const double dy = dyDir * useDx * waveSlope;
        c.y = (float)((double)s.y + dy);
        yFree = c.y;
        c.vy = (float)(dyDir * useDx * 4.0);
        c.held = (uint8_t)input;
        c.grounded = 0;
        // SPRITE ROTATION. The dart eases toward the angle it is travelling at,
        // by a FIXED fraction of the remaining gap each tick:
        //     rot += kWaveRotK * (target - rot)
        // Fitted from GD's own OBB corners (cfg hitboxtrace=1, `pobb`), lv20's
        // whole run, grouped by size and speed:
        //     full  k = 0.06250  (781 samples at dx=1.950, and 0.0624/0.0627 at
        //                         dx=1.298 / 1.614 -- speed-independent)
        //     mini  k = 0.10000  (110 samples)
        // and the target is the travel angle itself, +45/+63.435 going DOWN and
        // -45/-63.435 going up (cocos rotation is clockwise-positive), i.e.
        // atan(slope) with slope 1 full / 2 mini. The two k are MEASURED, not
        // derived -- nothing yet explains why mini eases 1.6x faster.
        {
            const double target = kRadToDeg * std::atan(waveSlope)
                                * (dy < 0.0 ? 1.0 : -1.0);
            const double k = c.mini ? kWaveRotKMini : kWaveRotK;
            c.rot = (float)((double)s.rot + k * (target - (double)s.rot));
        }
        // ...and it RIDES the band rather than dying on it (the t=3565 slide
        // above runs for 30+ ticks with y pinned at 100.000).
        const double wClamp = (c.mini ? kWaveClampMini : kWaveClamp);
        // The recorded band, if any, takes priority over the frozen band (history at
        // the declaration of g_bandTrack). But **only while the current y is inside
        // the recorded band**: in a section flying outside the band GD's pmin/pmax
        // are merely camera state, not a clamp (same reasoning as --startband's
        // OUTSIDE gate; measured 2026-08-16, when letting the track pass through
        // wiped out every anchor in the ball section).
        double btF = (double)s.bandFloor, btC = (double)s.bandCeil;
        if (s.frame == 0) {
            double f_ = btF, c_ = btC;
            if (bandTrackAt((long long)K.t, f_, c_)
                && (double)s.y >= f_ - 1.0 && (double)s.y <= c_ + 1.0) {
                btF = f_; btC = c_;
            }
        }
        const double bFloor = std::max(btF, kGroundY);
        const double yMaxW = btC - wClamp;
        if (g_bandDbg && (double)c.y > yMaxW - 8.0)
            std::printf("banddbg t=%lld x=%.1f y=%.2f band=[%.0f,%.0f] yMaxW=%.1f\n",
                        (long long)K.t, x, (double)c.y, (double)s.bandFloor,
                        (double)s.bandCeil, yMaxW);
        if ((double)c.y > yMaxW) {
            c.y = (float)yMaxW; c.vy = 0; c.grounded = 1; CLAMP0("wave/ceil");
        }
        if ((double)c.y < bFloor + wClamp) {
            c.y = (float)(bFloor + wClamp); c.vy = 0; c.grounded = 1;
            CLAMP0("wave/floor");
        }
        if (c.y > g_yBound) DIE("wave/out-of-play", nullptr);
        for (const Obj* o : *K.near) {
            if (dead) break;
            for (int si = 0; si <= kSubSteps && !dead; ++si) {
                const double f = si / (double)kSubSteps;
                const double sx = xPrev + (x - xPrev) * f;
                const double sy = (double)s.y + ((double)c.y - (double)s.y) * f;
                if (o->type == 0) {
                    // <= and not <: exactly-touching dies (measured, see
                    // kWaveKillHalf).
                    if (std::fabs(sy - o->cy) <= o->hh + kWaveKillHalf
                        && std::fabs(sx - o->cx) <= o->hw + kWaveKillHalf)
                        DIE("wave/solid", o);
                    // The HAZARD still runs on the contact half, and that is a
                    // PLACEHOLDER, not a measurement. What is measured is that
                    // both shapes on offer are wrong, so there is nothing to
                    // switch to yet (2026-08-09, lv20 spike id667 rot -63 at
                    // (1057.19,167.595), the kill boundary swept in x and
                    // bisected in y to 0.014 px, one tick, 33 positions):
                    //
                    //   x <= 1054.0   rising, slope +1.969
                    //   1054.25..1057.00  FLAT at y = 175.964   (width 3.00)
                    //   1057.25..1060.25  slope -0.314
                    //   x >= 1060.5   slope -0.503
                    //
                    // Four straight pieces. A turned RECTANGLE dilated by an
                    // axis-aligned player box can only produce three (its two
                    // edges plus the player's flat top), so the tested shape is
                    // not o->hw/hh (the bound: refuted twice over) and not the
                    // recorded OBB either. Its own two edges do show up as the
                    // +1.969 and -0.503 ends; the -0.314 in between belongs to
                    // something neither shape has.
                    //
                    // The flat is worth reading: its width is 3.00 = 2 * 1.5,
                    // which is the SOLID half, so the player's box is very
                    // likely 1.5 here too and the earlier "hazards use the
                    // contact half (2.96..2.98)" was an artefact of assuming
                    // the hazard was its OBB. Do not act on that until the
                    // shape is known -- the two are not separable from one
                    // boundary.
                    // ...and the path was found, by disassembly. GD's hazards
                    // never go through collidedWithObject at all: there is a
                    // dedicated loop in GJBaseGameLayer::checkCollisions
                    // (win 0x2137f0, head 0x2147C0) that ends in
                    // destroyPlayer(player, obj), and it is TWO tests, not one:
                    //   1. AABB of player->getObjectRect() vs the object's rect
                    //   2. if obj->m_shouldUseOuterOb, the two ORIENTED boxes
                    // The four-piece envelope above is those two conditions and
                    // not one exotic shape -- the flat is stage 1 (it sits at
                    // objMaxY + 3.0 on both spikes measured), the slopes are
                    // stage 2. That is why no single box ever fitted.
                } else if (hazardHit(o, sx, sy, wHazHalf, kHazMargin,
                                     kSawMargin, 4, c.mini)
                           // Stage 2. `s.rot` and not `c.rot`: the collision
                           // pass runs BEFORE the rotation is updated for the
                           // tick. Measured both ways on lv20 t=816 -- this
                           // tick's 22.113 puts the boundary at 175.586 and
                           // misses by 0.05, the previous tick's 17.522 gives
                           // 175.632 and lands inside the 0.014 px bracket.
                           && (!o->obbOk || s.mode != 4
                               || obbOverlap(*o, sx, sy, wHazHalf, (double)s.rot))) {
                    DIE("wave/hazard", o);
                }
            }
        }
        // ...and RAMPS, which live in their own array and so were invisible to
        // the loop above. A plain ramp lifts a cube or a ball out (see the
        // slope block below), but the wave has no such resolution -- it just
        // dies. Measured on lv17 t=3699: zig-zagging at y=190.9/192.2 the wave
        // runs into the 30x30 ramp at (4815,195) and GD destroys it, while the
        // model flew on and the loop sat at x=4,805 for 40 iterations.
        // A ramp's solid half is on ONE side of its line, and which side is
        // slopeDir's business, not the line's (slopeIsCeiling). Testing every
        // ramp as if the solid were "box floor up to the line" kills the wave
        // in the EMPTY half of a ceiling ramp: lv20's zig-zag corridor is a
        // floor ramp with a mirrored ceiling ramp 100 px above it, and the
        // model died 8 px below a ceiling it never touched, on the rising tick
        // only, from t=4043 (x=5,640) on -- the whole x=6,342 wall.
        if (K.slopes) {
            const double wsR = (c.mini ? kWaveSlopeDxRMini : kWaveSlopeDxR);
            const double wsL = (c.mini ? kWaveSlopeDxLMini : kWaveSlopeDxL);
            const double wsY = (c.mini ? kWaveSlopeDyMini : kWaveSlopeDy);
            for (const Obj* sp : *K.slopes) {
                if (dead) break;
                const double sx0 = sp->cx - sp->hw, sx1 = sp->cx + sp->hw;
                if (x + wHalf <= sx0 || x - wHalf >= sx1) continue;
                const double sm = (sp->sy1 - sp->sy0) / (sx1 - sx0);
                const bool ceilRamp = slopeIsCeiling(sp->slopeDir);
                // Sample where the wedge is THICKEST -- the corner of the box
                // that reaches deepest into the player. For a ceiling that is
                // where the line is lowest, for a floor where it is highest,
                // which comes out as the same end of the object either way.
                // The clamp to the box is what makes the boundary go flat
                // around a corner instead of following the line out of it.
                const bool toRight = ceilRamp ? (sm < 0.0) : (sm > 0.0);
                const double sxc = std::min(std::max(x + (toRight ? wsR : -wsL),
                                                     sx0), sx1);
                const double surf = sp->sy0 + sm * (sxc - sx0);
                if (ceilRamp) {
                    // above the box entirely: clear of it (measured -- the
                    // sweep comes back alive at y >= 315 over a 280..310 box)
                    if ((double)c.y > surf - wsY
                        && (double)c.y - wsY < sp->cy + sp->hh)
                        DIE("wave/slope", sp);
                } else {
                    if ((double)c.y < surf + wsY
                        && (double)c.y + wsY > sp->cy - sp->hh)
                        DIE("wave/slope", sp);
                }
            }
        }
    } else if (s.mode != 1 && s.mode != 3 && s.mode != 7) {
        const bool isBall = (s.mode == 2);
        // The ROBOT rides this branch too: it is a cube with a 0.9 gravity, a
        // half jump and a hover while the button is held (see kRobotGScale).
        // Everything else about it -- support, landing, stair snaps, hazards,
        // orbs, pads, slopes -- is the cube's, so it must not get a second copy
        // of that code.
        const bool isRobot = (s.mode == 5);
        // ...and so does the SPIDER, whose only difference from a ball is that
        // its tap teleports instead of flipping in place (see kSpiderGScale).
        const bool isSpider = (s.mode == 6);
        // The band clamps the BALL as well as the flying modes (GD's `flyish`
        // is m_isShip|m_isBird|m_isBall|m_isDart|m_isSpider|m_isSwing) and does
        // NOT clamp the cube. This is what every hand-placed `--ceil` was:
        // lv9's 330, lv14's 420@13100:13600 and lv15's 330@16300:17800 all come
        // straight out of their ball portal's cy. The knob stays as an override
        // and wins where it is given.
        // ...and the SPIDER is in GD's `flyish` list too (m_isShip | m_isBird |
        // m_isBall | m_isDart | m_isSpider | m_isSwing), so the band clamps it
        // exactly like the ball.
        // [CAUTION 2026-08-16] Do not apply bandtrack to the ball/spider. Once it
        // was applied, every anchor on the high route at lv22 t=5,6xx (ball at
        // y=983, recorded band 90..387) became PARTIAL t=0. There are two
        // measurements of GD's ball flying through while ignoring pmin/pmax (y=983
        // and y=3,677), while on lv9/14/15 there are also measurements of the ball
        // stopping at the band -- the condition is unresolved. Left as the frozen
        // band (the previous behaviour).
        // [2026-08-20 trial] When the band is **not on the grid** it is not a
        // physical ceiling. GD's pmax is a multiple of 30 when it comes from a
        // portal, but on lv22 it is the camera-derived value 386.999969, and GD's
        // ball punches through it at vy=6.5 (t=6,736..6,745, up to y=387.684). On
        // lv9 on the other hand pmax=330 exactly, and **even charging in at
        // vy=+15.000 (terminal velocity)** it stops at y=315.000 (t=7,312). Not the
        // speed but where the value comes from.
        // [2026-08-21 r81] The recorded band (--bandtrack), if any, takes priority
        // over the frozen band -- the same wiring as the wave/flight band clamps
        // (5803/7103) was missing from the ball/spider's ceilHere. Measured lv22
        // t=8,458 (spider warp): the model cuts the target to 586.5 with the
        // spider-portal-derived band 600, but GD's getMaxPortalY is camera-driven
        // at 755 (the dump's pmax says so directly) -- the recorded band is off-grid
        // so bandUsable drops out, and the warp reaches the actual object at 646.5.
        // The "only while the current y is inside the recorded band" gate is the
        // same (see the note at 5803).
        double bandCT = (double)s.bandCeil, bandFT = (double)s.bandFloor;
        if (s.frame == 0) {
            double f_ = bandFT, c_ = bandCT;
            if (bandTrackAt((long long)K.t, f_, c_)
                && (double)s.y >= f_ - 1.0 && (double)s.y <= c_ + 1.0) {
                bandFT = f_; bandCT = c_;
            }
        }
        const double bandC = bandCT;
        // ...and being on the grid is NOT enough: a CAMERA-driven band rests on
        // the grid whenever the camera is pinned at the floor, and GD's ball
        // flies straight through it. bandTrackIsCamera() carries the
        // measurements -- lv9's portal band blocks, lv22's camera band does not.
        // Only the whole recording can tell them apart, so a run without one
        // keeps exactly the old behaviour.
        const bool bandUsable =
            bandC < 1e8
            && std::fabs(bandC - std::round(bandC / 30.0) * 30.0) < 0.01
            && !bandTrackIsCamera();
        const double ceilHere = std::min(
            playerCeilAt(x),
            ((isBall || isSpider) && bandUsable) ? bandC : 1e9);
        const CubePhys cph = cubePhysFor(useDx);
        // The SPIDER takes the ball's LITERAL, not the cube's g scaled by 0.6.
        // The lv22 note above already measured "-0.129 / tick ... the ball's
        // number to three digits" and then wrote it as 0.216 * 0.600 = 0.1296,
        // which qVy rounds to 0.130 -- 0.001 too much on every airborne tick.
        // GD's own lv21 replay steps by exactly 0.129, twelve consecutive ticks
        // across both orientations (t=9303.. +9.600 9.471 9.342 9.213 9.084
        // 8.955 8.826, and t=8470.. the same with the sign flipped).
        // It matters because it accumulates: 0.001 per tick is ~1 px of y after
        // 100 airborne ticks and ~4.5 px after 200, which is a different landing.
        // OPEN: measured at speed 0.9 only. The cube's g is speed-dependent
        // (-0.216 / -0.215 / -0.212) while the ball's is one flat literal; the
        // spider is given the ball's treatment because that is what it matched
        // here, not because the other speeds were checked.
        // The robot's g splits by speed band: in the fast band (dx>1.78, lv22's
        // sp1.4 plateau) GD measures **exactly -0.195** every tick (CLEARED
        // t=16,970-17,016, vy column 8.944->8.749->8.554...). With the rounded
        // total of cph.g*0.9=0.1944 (-0.194), the 0.001/tick accumulation turned
        // into a landing-phase shift on the plateau's bounces and SOLVED plans were
        // failing in GD (same family as the spider taking the ball's literal 0.129).
        // The slow band stays as before (-0.194): setting every speed to -0.195
        // turned lv19/20/21 red (10/2/4 sections) = the measurements there are
        // correct at -0.194. The cube's 1.4 row falls back, unmeasured, to 1.3's
        // {11.230, -0.216} (the jump side is unverified).
        // [2026-08-18] The robot's g is now a **table of literals** too (not 0.9x
        // the cube's). Measured by counting gdref's free-fall ticks across all
        // levels by speed and size (only ticks that are not pressed, not grounded
        // and not at terminal):
        //   robot 0.9  vsize1.0  n=2,305  -0.194 x2,304
        //   robot 0.9  vsize0.6  n=933    -0.194 x933
        //   robot 1.1  vsize1.0  n=2,424  -0.194 x2,422
        //   robot 1.1  vsize0.6  n=1,776  -0.194 x1,774
        //   robot 1.3  vsize1.0  n=767    -0.195 x694
        // cph.g * 0.9 comes to 0.1935 in the 1.1 band, and rounding to the 0.001
        // grid alternates 0.193/0.194. GD is exactly 0.194 on all 2,400 ticks. This
        // is the origin of fixcensus's largest family (lv19x5 + lv20x4, which the
        // landing clamp had turned into edy=+0.088).
        // ...and the BALL's 0.129 was confirmed **flat across all speeds and
        // sizes** (mode 0.129 in all 8 combinations of 0.7/0.9/1.1/1.3 x 1.0/0.6,
        // n=21,900). The "OPEN: measured at speed 0.9 only" above is resolved.
        const double gAcc =
            isBall     ? kBallG
            : isRobot  ? ((useDx > 1.78) ? -0.195 : -0.194)
            : isSpider ? kBallG
                       : cph.g;
        const double gTerm = isBall ? kBallTerm : kCubeTerm;
        // cube. GD re-evaluates support at THIS tick's x before the input:
        // walking off the edge and pressing on the same tick does NOT jump
        // (measured at x=1546, lv1). All vertical reasoning is in the PLAYER
        // frame so flipped gravity is the same code with mirrored signs.
        const double gsign = s.flip ? -1.0 : 1.0;
        const double vp = (double)s.vy * gsign;
        auto faceOf = [&](const Obj* o) {
            return s.flip ? (o->cy - o->hh) : (o->cy + o->hh);
        };
        bool groundedNow = s.grounded != 0;
        // `s.y <= kGroundY + pHalf` is "standing on the world ground plane", so
        // the support scan is skipped and `grounded` is kept for free. That is
        // only true in the UNTURNED frame: in a turned one `y` is a world X, so
        // the test reads as "the player is left of world x=105" -- true for the
        // whole rotated section -- and the model stays grounded on nothing.
        // Measured on lv22 t=4,723: GD leaves the surface and accelerates away
        // at +0.212/tick while the model sat pinned at x=6,801.00 with
        // grounded=1 for 167 ticks; the support loop never even ran (no `supp`
        // line). Same class as the note at spiderTargetY's `worldFloor` gate --
        // a world floor does not exist in a turned frame.
        if (groundedNow
            && (s.flip || s.frame != 0 || s.y > (kGroundY + pHalf) + 0.5)) {
            bool sup = false;
            // [2026-08-22 r103] **In a rotated frame the player is not carried by a
            // "receding face".** GD's hitboxes (`hbox:` lines with cfg
            // hitboxtrace=1) named it: in lv22's rotated section, at t=6,128 (MOD
            // tick) alone **not a single collidedWithObject line appears** = zero
            // contact on that tick; GD sets og=0, accumulates one gravity step (the
            // ball's -0.129), and recovers on the next tick by penetrating the wall
            // at vy=-0.258. The wall (moving solid uid5825) is receding at
            // **0.045px/tick** at that point, and the player's one tick of fall,
            // **0.225x0.129 = 0.029px**, cannot keep up. Once the floor slows to
            // 0.02/tick the contact holds from then on (og drops on this 1 tick
            // only).
            // **Frame 0 is the opposite** -- in the same lv22's non-rotated corridor
            // at t=5,930, even with a floor sinking 0.838px/tick, GD carries the
            // player with onGround=1 (the measurement in the note just below). So
            // GD's "carry" exists only along world vertical; in a rotated frame the
            // face escapes in world x, so the carry has no effect.
            // [Rejected (2026-08-22 r103)] Tried narrowing this to "in a rotated
            // frame, follow a receding face by at most one tick of fall (0.225*g)",
            // but **the recording's resolution is insufficient**. In GD's
            // measurement (hbox: lines) the wall recedes smoothly at 0.045px/tick
            // and contact breaks on just the 1 tick where the gap crosses
            // 0.03->0.04. The model's moving geometry, with grouptrace's 0.05px
            // threshold, **moves only once every 2 ticks**, so the gap oscillates
            // 0/0.09, and with the same rule it also drops at 6,126 / 6,128 / 6,133
            // (GD does not). Closing it would need both "linear interpolation of
            // the recording" + "rotated-frame contact tolerance ~0.035" -- a change
            // that moves the moving geometry on every level, for a 1-tick vy blip
            // (zero effect on y), so it is shelved.
            // [Abandoned (2026-08-22 r105 / final)] lv22@6,129 **cannot be closed in
            // principle within this model's abstraction**. Laying out the window
            // (t=6,118..6,136) tick by tick after adding r104's interpolation:
            //
            //   The player's descent is **perfectly uniform at 1.6145 px/tick**
            //   (spread 0.001 from print rounding only). GD's y **matches the model
            //   on every tick**, and t=6,129 sits exactly on the carry line too.
            //   Only GD reports that 1 tick as vy=-0.129 / og=0, and returns to 0
            //   on the next tick.
            //
            // So it is **a reporting difference with zero effect on y that
            // self-heals in 1 tick**. Moreover the geometry the model sees carries
            // no signal whatsoever that distinguishes that tick from the others
            // (the 1.615 steps appear at 6,121 / 6,125 / 6,129 / 6,133, a rounding
            // pattern every 4 ticks, and og drops only at 6,129). The signal exists
            // only inside the float accumulation of GD's own resolution loop, so
            // **no function** of the model state can pick out this tick. That is
            // why r103 (narrowing the tolerance) and r105 (narrowing by one tick of
            // fall) both over-fired on the neighbouring 4 ticks. Adding a fake
            // "move vy only" rule would merely add a phantom distinction to the
            // dedupe key and make the search more expensive, so it is **not added**.
            for (const Obj* o : *K.near)
                if (o->type == 0
                    && std::fabs(x - o->cx) <= o->hw + pHalf + kContactEps
                    && std::fabs((double)s.y - gsign * pHalf - faceOf(o))
                           < kSupportTol + std::fabs(o->dcy)) {
                    sup = true;
                    // ...and if that face MOVED this tick, the player goes with
                    // it (see the ride block by the stair snap).
                    // The `+ |dcy|` in the test above is what lets that happen:
                    // `s.y` is LAST tick's player y and `faceOf(o)` is THIS
                    // tick's face, so a surface that dropped more than 0.6 px in
                    // one tick read as "no longer under him" and the rider was
                    // dropped on the very tick the ride was supposed to start.
                    // Measured on lv22 t=5,930 (ball corridor, x=8,180): the
                    // floor's first step is 0.838 px (eased Move, 12 px over
                    // 0.5 s), GD keeps onGround=1 and carries the ball down
                    // 975.000 -> 972.020 over 11 ticks while the model let go
                    // and free-fell. That lost the ball's tap-flip at t=5,940
                    // (a ball only flips gravity while it is ON something) and
                    // the whole corridor after it.
                    // ...and when more than one moving face qualifies, ride the
                    // one the foot is actually on. The loop used to keep the
                    // LAST match, so a stacked corridor could re-seat the player
                    // onto a face a couple of pixels away. Measured on lv22's
                    // ball corridor (2026-08-14): the anchored resim followed
                    // for 178 ticks and then jumped 2.21 px in one tick with
                    // dvy=0.0000 -- a re-seat, not physics.
                    if (o->dcy != 0.0) {
                        const double foot = (double)s.y - gsign * pHalf;
                        const double gap = std::fabs(foot - faceOf(o));
                        if (!rideOn || gap < rideGap) {
                            rideOn = true;
                            rideGap = gap;
                            rideFace = faceOf(o);
                            rideDy = o->dcy;
                        }
                    }
                }
            // --slopedbg: WHAT is holding the player up. A model that stays
            // grounded where GD falls looks identical to a physics bug until
            // the supporting object is named (lv22 t=4,723: 167 ticks pinned at
            // x=6,801 while GD accelerates away in the rotated frame).
            if (g_slopeDbg) {
                std::printf("supp t=%lld x=%.2f y=%.2f flip=%d sup=%d uids=",
                            (long long)K.t, x, (double)s.y, (int)s.flip,
                            sup ? 1 : 0);
                for (const Obj* o : *K.near)
                    if (o->type == 0
                        && std::fabs(x - o->cx) <= o->hw + pHalf + kContactEps
                        && std::fabs((double)s.y - gsign * pHalf - faceOf(o))
                               < kSupportTol + std::fabs(o->dcy))
                        std::printf("%d(%.1f,%.1f) ", o->uid, o->cx, o->cy);
                std::printf("\n");
            }
            // the invisible ceiling supports too. Without this a ball resting on
            // it lost `grounded` on the very next tick and could never tap back
            // off -- and tapping off is the only way down. lv9 rides that
            // ceiling over one wall and has to drop before the next one.
            if (!sup && s.flip
                && std::fabs((double)s.y + pHalf - ceilHere) < 0.6)
                sup = true;
            // ...and so does a RAMP. Slopes are a separate list, so this loop
            // never saw them: a ball riding a ramp lost `grounded` on the tick
            // after it landed and could never tap off it. The tap is the ball's
            // only action, so the whole ramp was a forced no-input corridor --
            // the frontier ran at alive=1 for 70 ticks on lv16 and then died on
            // the saw at x=6,849 that one tap anywhere in a 55-tick window
            // clears (measured in GD: tap at any t in 4676..4730 reaches
            // x=7,047, and the level's own route is to flip up onto the ceiling
            // blocks at y=440 and ride them over the saw).
            // The window is the ride's own (see the slope block), so a ramp
            // that has run out this tick correctly stops supporting.
            // ...and a FLIPPED player standing under a CEILING ramp is on one
            // too. Kept to that pair on purpose: the blanket `s.flip` relaxation
            // is what took lv16 from CLEARED to STUCK once (see the ceiling
            // branch's own note), and the case measured here is exactly
            // "flipped + the ramp whose solid side is up".
            // lv22 t=4,636 (cube, one tick after the swing->cube portal, ramp
            // uid4359 (6705,435) m=-1): the push-out already puts the model at
            // GD's 413.802, GD reads onGround=1 and jumps off at vy=-2.390 the
            // next tick, and without this the flag is gone by then.
            if (!sup && s.onSlope && K.slopes) {
                for (const Obj* sp : *K.slopes) {
                    // upright: any ramp. flipped: ONLY one whose solid side is
                    // up, and then the contact offset mirrors. The blanket
                    // `s.flip` relaxation is what took lv16 from CLEARED to
                    // STUCK once (see the ceiling branch's note), so this stays
                    // on the pair that was measured.
                    if (s.flip && !slopeIsCeiling(sp->slopeDir)) continue;
                    const double sx0 = sp->cx - sp->hw, sx1 = sp->cx + sp->hw;
                    const double m = (sp->sy1 - sp->sy0) / (sx1 - sx0);
                    if (m == 0.0) continue;
                    // the contact point mirrors for a flipped player, the same
                    // way the ceiling branch's `rc` does
                    const double off = (m > 0) ? slopeXOffset(m, pHalf)
                                               : -slopeXOffset(m, pHalf);
                    const double xr0 = x + (s.flip ? -off : off);
                    // [2026-08-19] A cube's continued ride downhill (in the travel
                    // direction) stays on the extrapolated line past the low end
                    // too (sampleAt's extrapolation branch). Support extends to the
                    // same window -- with the old 1.5px, a press during an
                    // extrapolated ride was refused with groundedNow=0 and only GD
                    // jumped (lv16 t=6,322, y matches exactly, vy alone differs by
                    // 11.42).
                    double loB = sx0, hiB = sx1 + 1.5;
                    if (!s.flip && s.mode == 0) {
                        const bool revS = ((double)K.dxF < 0.0 || s.rev != 0);
                        const double extC = pHalf * std::sqrt(1.0 + m * m)
                                            / std::fabs(m);
                        if (!revS && m < -0.01) hiB = sx1 + extC;
                        else if (revS && m > 0.01) loB = sx0 - extC;
                    }
                    if (xr0 < loB || xr0 > hiB) continue;
                    sup = true;
                    break;
                }
            }
            groundedNow = sup;
        }
        cubeContact = groundedNow;
        // the ceiling pin is re-earned every tick (see "cube/ceilstop")
        c.ceilPin = 0;
        // A player standing on a BLOCK carries that tick's gravity step in y.
        // GD's tick is update(gravity + move) -> collisions -> buttons, and only
        // the ordinary jump puts the player back on the surface; anything else
        // that launches it leaves the moved y in place. Measured over the 19
        // verified replays (scratchpad/pad16.py, impulse.py -- HANDOFF update 30):
        //   plain jump (11.180 etc.)          y unchanged   832 + 151 + 56 + 45
        //   pad / orb while on a BLOCK        y moves       9 of 9
        //   pad / orb while on the GROUND     y unchanged   12 of 12
        // The yellow pad's 21 firings split 9/12 on exactly that line with no
        // exception, which is why the ground test below is the discriminator
        // and not the pad's value or the mode.
        //
        // It matters because the offset does not decay: the model drew its whole
        // arc 0.0486 px high and LANDED A TICK LATE. lv19 t=290 is that landing,
        // and everything after it (2,818 divergent ticks) hangs off it.
        pinnedOnBlock =
            groundedNow && (s.flip || (double)s.y > (kGroundY + pHalf) + 0.5);
        prePinY = (float)((double)s.y + kYScale * qVy(gAcc) * gsign);
        double vpNew = vp;
        bool ballFlipped = false;
        // A gravity portal firing on this tick EATS the cube's jump. GD flips
        // first (flipGravity clears m_isOnGround) and only then handles the
        // button, so the press finds nothing to jump off.
        // Measured on lv16 x=27,315 (portal id 11, cy=665), press one tick
        // apart with everything else identical:
        //   press t=17,439 -> t=17,440 vy = 11.42 (jump), then the portal
        //                     halves it at 17,441 to (11.42-0.215)/2 = 5.6025
        //   press t=17,440 -> t=17,441 vy = -0.1075 = (0 - 0.215)/2, NO jump
        // The model jumped anyway and left with 5.710 = 11.42/2, i.e. 6 px
        // above GD within a tick; the plan then walked into the spike at
        // (27,333,609) that GD's flat trajectory passes under.
        // Cube only -- the ball's tap on a portal tick has not been measured.
        bool gravPortalThisTick = false;
        if (!isBall) {
            for (const Obj* p : *K.ports) {
                if (p->type != 3 && p->type != 4) continue;
                const uint8_t wantFlip = (p->type == 3) ? 1 : 0;
                if (s.flip == wantFlip) continue;   // no change -> does not fire
                if (p->oriented && !orientedHit(*p, x, (double)s.y, pHalf)) continue;
                if (std::fabs(x - p->cx) <= p->hw + pHalf
                    && std::fabs((double)s.y - p->cy) < p->hh + pHalf) {
                    gravPortalThisTick = true;
                    break;
                }
            }
        }
        if (groundedNow && input && !s.action && !gravPortalThisTick
            && isSpider) {
            // SPIDER: teleport to the surface on the other side and flip.
            // The search is the mirror of the support test above -- the same
            // x-overlap rule, and the face that matters is the one the player
            // will come to rest ON: the UNDERSIDE of a solid when travelling
            // up, its TOP when travelling down.
            // A tap with nothing to land on is dropped rather than guessed at:
            // refusing the branch can only cost routes, while inventing a
            // landing invents a plan GD will not follow.
            double bestY = 0.0;
            bool hazTgt = false;
            // (the search itself is spiderTargetY, shared with the SPIDER ORB)
            const bool found = spiderTargetY(K, x, (double)s.y, s.flip != 0,
                                             c.mini != 0, pHalf, bestY,
                                             s.frame == 0, &hazTgt,
                                             (int)s.frame);
            if (!found) DIE("spider/no-target", nullptr);
            // ...and the search STOPS on a hazard (see spiderTargetY's note):
            // GD lands the spider on it and kills it on the same tick.
            if (hazTgt) DIE("spider/tp-hazard", nullptr);
            impulsedThisTick = true;
            spiderWarpedThisTick = true;
            // ...and the block-pin has to let go. `pinnedOnBlock` was armed
            // above for a player standing on a solid, and `releasePin` puts y
            // back to `prePinY` -- which on a teleport tick drags the player
            // back onto the block it just left. Measured on lv22 t=2,023: the
            // tap fires, the model teleports to y=163.5 and the ball portal
            // uid1590 fires there (both exactly as GD does), and then the pin
            // pulls y back to 316.53 -- 153 px above GD for the whole section,
            // and straight into the solid uid1616 (2415,345) 5 ticks later.
            pinnedOnBlock = false;
            c.y = (float)bestY;
            yFree = c.y;
            // GD leaves 1.0 of velocity pointing the way the teleport went, not
            // zero. Straight off its own dumps (lv21, cfg gatetrace):
            //   t=9057  y 406 -> 223.5 (down), yvel = -1, onGround 0
            //   t=9058  y 223.5,             yvel =  0, onGround 1
            // and the four teleport fixups the driver recorded all read
            // edvy = +1 going up / -1 going down, with edy = 0 -- i.e. the
            // landing height was already right and only this was missing.
            // `gs` is +1 for an upright spider (which teleports UP) and -1 for a
            // flipped one, so it IS the direction of travel.
            // Not scaled by anything: all four records are sp0.9 full size and
            // read exactly 1.000.
            //
            // [2026-08-18] grounded is **0** (matching GD's onGround=0). This used
            // to be 1, held back with "the next tick zeroes vy and grounds the
            // player either way, so changing the flag too would move two
            // variables". **A sample that separates the two has appeared**: lv22
            // t=2,044 fires an orb on the tick after the teleport. Treated as
            // grounded, the flight branch's vp is crushed to 0, the carried -1.000
            // vanishes, and y moves only 0.225 px (GD moves 0.254 px at -1.129).
            // Treated as airborne, the landing test re-seats the player on the
            // face on the next tick, so routes without an orb are unchanged.
            //
            // It has to go in vpNew, NOT in c.vy. The branch ends with
            // `c.vy = vpNew * gsign` (and gsign is built from s.flip, the OLD
            // gravity), so anything written to c.vy here is dead. vp is the
            // velocity in the player's own pre-teleport frame, and +1 there is
            // "moving the way the teleport went" for both orientations:
            //   upright  gsign=+1 -> c.vy = +1 (teleported up)
            //   flipped  gsign=-1 -> c.vy = -1 (teleported down)
            // Setting c.vy alone looked right in a y-only spot check and changed
            // nothing at all; what caught it was the driver still recording the
            // same edvy=+-1 fixups on the next cold run.
            c.flip = s.flip ? 0 : 1;
            c.grounded = 0;
            c.snapObj = nullptr;
            vpNew = 1.0;
            // [2026-08-21 r93] **When a warp interrupts a ride, the ramp's
            // slope-exit launch pulse appears on the next tick.** Measured lv21
            // t=16,713/16,714 (spider, rode an |m|=2 uphill for 3 ticks, then
            // warped by tap):
            //   16,713 warp (y 161.5 -> 226.5, flip 0->1, vy=+1.000 = the signature)
            //   16,714 GD alone **vy=+5.226**, og=0 (y does not move)
            //   16,715 pushed back onto the ceiling, 0.000 / og=1
            // 5.226 = slopeExitVy(2, spider, sp1.1) 13.064 x the ramp factor 0.400
            // for a 3-tick ride, exactly. The sign is **the pre-warp orientation**
            // (an uphill launch is + when upright) -- not the orientation after the
            // warp flipped it.
            // A pending carried for 1 tick (same shape as pBallOff; also in the key).
            if (s.onSlope && (double)s.slopeM != 0.0) {
                const double mTravW =
                    (double)s.slopeM
                    * (((double)K.dxF < 0.0 || s.rev != 0) ? -1.0 : 1.0);
                const bool upW = s.flip ? (mTravW < 0.0) : (mTravW > 0.0);
                if (upW)
                    c.pExitVy = (float)((s.flip ? -1.0 : 1.0)
                                        * slopeExitVy(
                                              std::fabs((double)s.slopeM),
                                              c.mode, useDx, c.mini != 0)
                                        * slopeRampFactor((int)s.slopeT + 1));
            }
        // The CUBE re-jumps while the button is simply HELD -- it does not need
        // a fresh press. Measured on lv1 with a single press at t=60 and no
        // release at all: jump at t=61 (vy=11.18), land at t=164, and GD jumps
        // AGAIN at t=165 with the same 11.18. The model's `!s.action` edge gate
        // refused that, which is why the seeded lv22 clear diverged at t=10,556
        // (GD jumps off the ceiling lane on a hold started 4 ticks earlier,
        // model sat still) -- and why every plan that holds through a landing
        // came back as a divergence the driver had to paper over.
        // The cube -- and the ROBOT too (the 2026-08-17 measurement overrides the
        // reading of the disassembly). The note at kRobotGScale read it as
        // "m_jumpBuffered is also needed = fresh press", but lv22 t=15,878: a robot
        // with the button held (the edge 30+ ticks earlier) lands, and on the next
        // tick GD re-jumps with vy=+5.615 (= this speed's jump 11.23 x 0.5). The
        // reading "buffered stays set while held" reconciles both. ball / spider /
        // UFO remain edge-triggered, unmeasured.
        // ...and NOT when this same press already spent itself on a ring:
        // lv22 t=10,352 presses, the spider orb fires at 10,353, the player is
        // grounded at 10,354 with the button still down through 10,356 -- and
        // GD does NOT jump. One press activates one thing; the re-jump needs
        // the press back. `ringHold` is exactly "this press has fired a ring"
        // and is cleared on release, so it is the gate.
        } else if (groundedNow && input && !gravPortalThisTick
                   && (!s.action
                       || ((s.mode == 0 || s.mode == 5) && !s.ringHold))) {
            // TRIED AND REVERTED (2026-08-26): delaying the grounded BALL's
            // tap-flip by one tick. The lv22 ball-corridor fixups show GD
            // firing the flip one transition late there (paired records:
            // in=1 edvy=-3.426, then in=0 edvy=+3.426, at three heights),
            // but the delay is NOT a function of anything the model can see:
            // unconditional, it broke every static-floor ball level at once
            // (lv9-15/18, all by exactly -ballFlipFor on the edge tick);
            // gated on rideOn (a moving support), quick_regress still broke
            // lv19 t=20,600 (400->80), lv20 t=5,400 and SIX lv22 segments
            // including the corridor itself (t=5,800 400->23) -- on the very
            // reference plan the records came from, GD fires edge-tick flips
            // on movers too. Whether the buttons phase sees the re-seated
            // grounded that tick is decided inside GD's collision pass, below
            // the model's resolution (the gd-shrinking-gap class). Per-run
            // fixup records are the only honest carrier for it.
            impulsedThisTick = true;
            if (isBall) {
                // the tap flips gravity and drops toward the NEW floor; the
                // player-frame velocity is expressed against the new sign, so
                // this cannot go through the shared `vpNew * gsign` below
                c.flip = c.flip ? 0 : 1;
                // the ball's tap scales with size like every other impulse.
                // Measured on lv11 t=8398 (mini ball): GD leaves with 2.6832
                // where the model produced 3.3540, and 2.6832 / 3.354 = 0.800.
                // ...and with the section's speed, by the same factor as the
                // rings: measured on lv15 t=10685, GD taps out at 3.426 where
                // the 0.9 constant gives 3.354, and 3.426 / 3.354 = 1.0214669,
                // the jump's ratio at 1.1 to seven digits.
                // tapping UP off a slope adds the measured bonus (see above)
                // The bonus scales with the gradient's exit ratio (the measurement
                // site of 0.2796, lv16 t=4701, is uid1715 = w60 h30 with m=0.5.
                // lv20 t=5,270's corner (m=1) is 0.5178 = 0.2796 x
                // exit(1)/exit(0.5)).
                // [2026-08-21 r92] The gate is **the same "uphill in the player's
                // own frame" as the cube's bonus** (mirrored when flipped). The old
                // `c.flip` (whether the tap went to the flipped side) was a
                // restatement of one corpus sample, "an upright ball jumping uphill
                // on a floor", and dropped the hang (a flipped ball sliding along a
                // ceiling ramp tapping downward) -- the rig's 6 ball cells are on
                // that side.
                const double mTravB =
                    (double)s.slopeM
                    * (((double)K.dxF < 0.0 || s.rev != 0) ? -1.0 : 1.0);
                const bool bUphill =
                    s.onSlope
                    && (s.flip ? (mTravB < 0.0) : (mTravB > 0.0));
                // ...**the bonus does not scale with mini** (same ordering as the
                // cube side: the mini ratio applies to the tap value only, and the
                // bonus sits raw on top). The rig's 3 mini cells gave the same
                // 0.225/0.417/0.591 as normal size.
                const double bBonus =
                    bUphill ? kBallSlopeExitBonus
                                  * slopeExitVy(std::fabs((double)s.slopeM),
                                                2, useDx, c.mini != 0)
                            : 0.0;
                const double bTap =
                    ballFlipFor(useDx) * (c.mini ? kMiniImpulse : 1.0)
                    + bBonus;
                c.vy = (float)(-bTap * (c.flip ? -1.0 : 1.0));
                ballFlipped = true;
                ballFlippedThisTick = true;
            } else {
                // jump edge: vy set, y frozen this tick
                // mini keeps the measured 0.800 ratio against this speed's jump
                vpNew = c.mini ? (cph.jump * (kCubeJumpMini / kCubeJump))
                               : cph.jump;
                // ...and the robot's is half of it, with the hover budget
                // armed. GD zeroes the budget (this+0x830) in the same block
                // that sets the jump velocity.
                if (isRobot) {
                    vpNew *= kRobotJumpScale;
                    c.rHover = rePushNoHoverAt(K.t)
                                   ? (uint8_t)0
                                   : (uint8_t)kRobotHoverTicks;
                }
                // A jump taken WHILE STILL ON A RAMP carries the ramp's own
                // push on top of the jump. The ramp-exit rule next door only
                // fires when the player actually leaves the ramp window
                // (`!rampWindowHere`), so a mid-ramp jump got the bare jump
                // and ran a whole velocity unit slow for the rest of the arc.
                //
                // Structure from the disassembly (updateJump 0x38bc77): the
                // term is gated on the on-slope flag (this+0xb58), it is added
                // AFTER the robot's x0.5 (0x38bc44) so it is not scaled by it,
                // and it is a ratio `|double@0xaf8| / (float@0x9f4 *
                // double@0x7b8)`.
                //
                // Corpus (all three exact, and all |m| = 0.5):
                //   lv18 t=11,030  sp1.0 ride 6    11.180 -> 11.580
                //   lv18 t=10,938  sp1.0 ride24+   11.180 -> 12.180
                //   lv16 t=16,810  sp1.1 ride24+            +1.243
                // so the speed factor is sp exactly and the ride uses the same
                // ramp the exit does (0.400 is that ramp's 0.4 floor).
                //
                // The GRADIENT was measured on a purpose-built map
                // (py/mklevel.py `rampjump`, 2026-08-18: cube/robot x |m| in
                // {0.5,1,2} x normal/mini, one press 70% along a 3-ramp chain,
                // 12 units in one run). Bonus, by |m|:
                //   0.5 -> 1.000   1.0 -> 1.851   2.0 -> 2.627
                // which is slopeExitVy's own cube row divided by 3.999:
                //   3.999/3.999   7.405/3.999    10.507/3.999
                //   = 1.0000      1.8517         2.6274
                // i.e. the bonus is a QUARTER of the ramp's own exit. It is
                // size-independent (the six mini units give the identical
                // bonus on top of the mini jump) and the full-size ROBOT gives
                // the cube's bonus on top of its halved jump, which is what
                // the disassembly's ordering says.
                // OPEN: the MINI ROBOT at |m| = 1 and 2 both come out at
                // 6.261 (= base 4.472 + 1.789) where the law wants 6.323 and
                // 7.099. Two units out of twelve, both the same number, so
                // something caps there; unexplained and unmodelled.
                // [2026-08-19] The bonus applies **only to a ride that is uphill in
                // the travel direction**. The rampjump rig was measured on uphill
                // chains only, and the downhill measurement (lv16 t=5,991, press
                // from a downhill ride at m=-0.5) had GD at exactly the raw jump
                // 11.42 = bonus 0.
                // [2026-08-20] The OPEN above ("the mini robot at |m|=1 and 2 both
                // come out at 6.261, something caps there") is closed. **The bonus
                // is capped at 0.4x the player's own jump value.** 12 units
                // re-measured on the rampjump rig with its header fixed:
                //   cube  base 11.180  cap 4.472 ... 0.5/1/2 all pass the raw 1.000/
                //         1.851/2.627 (12.180 / 13.031 / 13.807)
                //   robot base  5.590  cap **2.236** ... 0.5->1.000 and 1->1.851
                //         pass, but **2->2.236 hits the cap** (GD 7.826; the
                //         uncapped model gave 8.217)
                //   mini robot base 4.472  cap **1.789** ... |m|=1 and 2 are both
                //         6.261 = 4.472+1.789 (the old note's "2 unexplained units")
                // The mini cube, base 8.944, cap 3.578, passes the raw values.
                // 12/12 explained by this one rule.
                // [2026-08-21 r91] **Uphill mirrors when flipped.** For a hanging
                // player (flipped, sliding along the underside of a ceiling ramp),
                // a line descending in the world is uphill in its own frame, and GD
                // produces the same bonus. The calibration rig ceilhold's 6 cube
                // cells and 6 robot cells are **identical, without exception, to
                // the rampjump rig's (floor-side) table**:
                //   cube  |m| 0.5/1/2 -> +1.000 / +1.851 / +2.627 (normal and mini)
                //   robot same -> +1.000 / +1.851 / **+2.236 (cap)**,
                //          mini up to +1.789 (cap) at both |m|=1,2
                // The exit gate (uphillExit) already has the same mirror.
                const double mTravJ =
                    (double)s.slopeM
                    * (((double)K.dxF < 0.0 || s.rev != 0) ? -1.0 : 1.0);
                if (s.onSlope
                    && (s.flip ? (mTravJ < 0.0) : (mTravJ > 0.0))) {
                    const double bonus =
                        kSlopeJumpBonus
                        * slopeExitVy(std::fabs((double)s.slopeM), 0,
                                      useDx, c.mini != 0)
                        * slopeRampFactor((int)s.slopeT + 1);
                    vpNew += std::min(bonus,
                                      kSlopeJumpBonusCap * std::fabs(vpNew));
                }
                // On the fresh-jump tick of a **downhill** ride, y first drops by
                // one more step of the seat and then jumps (collisions before
                // buttons, and a falling seat does not cancel the jump). Measured
                // lv16 t=5,991: GD y=232.217 = the seat's next step, vy=11.42 = the
                // raw jump value. Asymmetric with the uphill freeze (the note just
                // below). Measured for the cube only.
                else if (s.onSlope && c.mode == 0) {
                    c.y = (float)((double)c.y
                                  + (double)s.slopeM
                                        * std::fabs((double)useDx)
                                        * (((double)K.dxF < 0.0
                                            || s.rev != 0) ? -1.0 : 1.0));
                    yFree = c.y;
                }
            }
            c.grounded = 0;
            // ...and the HELD re-jump also MOVES on this tick, where a fresh
            // press does not. GD's order says why: a press is consumed at the
            // end of its tick (after the move), so its velocity first shows in
            // the next one, while the hold is read in updateJump at the top of
            // the tick and the move that follows already carries it.
            // Both halves measured on lv1 with one press at t=60, never
            // released:
            //   fresh: t=61 vy=11.18, y=105.000 (frozen)  -> t=62 y=107.4669
            //   held : t=164 landed, t=165 vy=11.18 AND y=105 -> 107.5155
            //          (+2.5155 = 11.18 * kYScale, in the same tick)
            // lv22 t=10,556 is the same thing upside down: GD leaves the
            // ceiling at 1035 -> 1032.431 = -11.42 * kYScale on the jump tick.
            // Without this the model runs exactly one tick behind for the rest
            // of the level (a flat 2.5695 px on lv22).
            if (s.action && s.mode == 0) {
                c.y = (float)((double)s.y + kYScale * vpNew * gsign * tScale);
                yFree = c.y;
            }
        } else if (groundedNow) {
            vpNew = 0;
            c.grounded = 1;
            c.rHover = 0;
            // ...but GD's update phase still took this tick's gravity step
            // before the collision pass put the body back on the surface --
            // that step is what `prePinY` holds (the 9-of-9 pad/orb split in
            // the note at `pinnedOnBlock` measures exactly this quantity), so
            // it, and not the seat, is the pre-collision position.
            yFree = prePinY;
        } else {
            c.grounded = 0;
            // ROBOT hover: while the button is STILL held from the jump and the
            // budget lasts, GD adds back exactly the gravity step it is about
            // to subtract, so vy does not change at all.
            // The hold that counts is `s.action`, the input this state came in
            // WITH, not this tick's: GD's per-tick order is update -> collisions
            // -> buttons, so the button state updateJump sees is the one set at
            // the end of the previous tick. (Same reason the wave steers on
            // s.held.) A release therefore still hovers on the release tick and
            // stops on the next one, which is what GD does.
            if (s.dashing && s.action
                && (g_dashStopBoxes.empty()
                    || !dashStopAt(modX, modY, pHalf))) {
                // DASH: no gravity at all, a straight line at the ring's own
                // angle. Same "the hold that counts is s.action" rule as the
                // robot's hover and the wave's steering.
                // A DASH STOP box (id 1829) at the START-of-tick position
                // drops the dash into the gravity branch below: vp is 0 while
                // dashing, so the fall restarts from 0 exactly like GD's
                // measured -0.215 on the cancel tick (see DashStopBox).
                c.dashing = 1;
                c.rHover = 0;
                vpNew = 0.0;
                c.y = (float)((double)s.y + (double)s.dashSlope * useDx);
                yFree = c.y;
            } else if (isRobot && s.rHover && s.action) {
                vpNew = vp;
                // FORCE BOX (id 2069): the field applies during hover too, at the
                // ROBOT's strength -- this branch is gated on isRobot, so `s.mode`
                // is 5 by construction and the mode-less lookup could only ever
                // read the wrong column of a per-uid x mode table.
                // Measured lv22 t=16,462-16,471 (hover held, inside uid17701):
                // GD's dvy is +0.330 = that box's kRobot, not its 0.172.
                // The note that used to stand here read a ship's dvy at
                // t=15,846-848 as "a robot hover, +0.159 minus the hover's own
                // small decay". The mode there is ship, which never reaches this
                // branch, and the hover's decay is 0.000 in 23 of the 24 held robot
                // jumps in the corpus -- so neither half of that was measuring this.
                // The dash branch stays pass-through, as measured for 3645.
                if (!g_forceBoxes.empty())
                    vpNew += forceBoxAcc(modX, modY, pHalf, s.mode) * gdSign;
                c.rHover = (uint8_t)(s.rHover - 1);
                c.y = (float)((double)s.y + kYScale * vpNew * gsign * tScale);
                yFree = c.y;
            } else {
                c.dashing = 0;
                double acc = gAcc;
                // FORCE FIELD: world-frame push, converted to the player frame
                // by gsign like every other world force. Position is the
                // START-of-tick one (xPrev, s.y): the effect shows up one tick
                // after the overlap in all six measured entry/exit edges.
                if (!g_forceFields.empty())
                    acc += forceFieldAcc(modX, modY, pHalf) * gdSign;
                // FORCE BOX (id 2069): same convention (start-of-tick position,
                // world->player via gsign). Measurements at the declaration of kFF2069
                if (!g_forceBoxes.empty())
                    acc += forceBoxAcc(modX, modY, pHalf, s.mode) * gdSign;
                // The TIME WARP scales the increment, not the terminal: the cap
                // is a velocity. The 0.001 grid is applied to each step (GD's
                // vy went 0 -> 0.043 -> 0.086 -> 0.129 under a 0.0432 step; the
                // exact accumulation would have given 0.130 on the third).
                // r102: the tick after hitting a black orb on a fast climb has no terminal
                vpNew = s.pNoTerm ? qVy(vp + acc * tScale)
                                  : qVy(std::max(vp + acc * tScale, gTerm));
                if (isRobot) c.rHover = 0;
                c.y = (float)((double)s.y + kYScale * vpNew * gsign * tScale);
                yFree = c.y;
            }
        }
        if (!ballFlipped) c.vy = (float)(vpNew * gsign);
        // [2026-08-19 D9] **In a rotated frame, vy := -/+1.000 on the tick after
        // leaving the face**. The point is to write it **after** the position
        // update (above) -- GD's world x moves only 0.040 on this tick (0.225 if
        // it followed the ladder), and from the next tick on it sits on the
        // regular ladder at 1.129x0.225 = 0.254.
        // The measured pair (both ball, leaving the face by gravity flip; lv22's
        // flip source is gravity portal uid13833 type=4 -- slopedbg's portfire
        // names it):
        //   lv16 t=4,237 (gf=0): next tick -0.129, then -0.258/-0.387 ...
        //   lv22 t=6,307 (gf=1): next tick **-1.000**, then -1.129/-1.258 ...
        // The discriminator is gf (rotation frame). No sample on the gf=3 side yet.
        if (s.pBallOff && c.mode == 2) c.vy = (float)(-1.0 * gsign);
        // FLIP-ON-HEAD-HIT arming (id 2866). Same START-of-tick position the
        // force field uses -- measured on lv22: GD's counter goes positive on
        // the tick whose PRE-move x first overlaps the box (t=2,707, pre-move
        // x=3,708.886 against the box's left edge 3,717 and a 9 px half), not
        // on the tick whose post-move x does (t=2,706). Sticky once set.
        if (!c.fgArm && !g_flipHeadBoxes.empty()
            && flipHeadArms(modX, modY, pHalf))
            c.fgArm = 1;
        // Dual anti-collision bounce. Measured on lv16's second dual (mini
        // ball, 11 point probes, findings-lv16-dual.md): a body moving
        // TOWARD its partner that ends the tick within 23 px of it while the
        // partner is GROUNDED is flipped away with vy = +/-2.000 EXACTLY
        // (not mini-scaled), y not clamped. Input-independent; inactive
        // while the partner is airborne (fires the tick after it lands);
        // a body moving AWAY sails through the zone untouched.
        // 23 = 2*half + 5 is a guess at the shape -- only the mini sum
        // (9+9+5) is measured, and only for the ball. The partner side here
        // is the START-of-tick y2/grounded2, which matches the measured
        // "fires the tick AFTER the partner lands".
        if (s.dual && isBall && !c.grounded && s.grounded2) {
            const double sep = (double)c.y - (double)s.y2;
            const double moved = (double)c.y - (double)s.y;
            if (std::fabs(sep) < 2.0 * pHalf + 5.0 && moved * sep < 0.0) {
                c.flip = (sep > 0.0) ? 1 : 0;
                c.vy = (sep > 0.0) ? 2.0f : -2.0f;
                c.grounded = 0;
            }
        }
        // invisible ceiling: a flipped ball rides it exactly like ground
        const double pCeil = ceilHere;
        // r75: whether this tick's grounding comes from the "invisible ceiling" (no
        // actual object supporting). Read by the reach-back gate in the landing
        // loop below.
        bool groundedInvCeil = false;
        if (c.flip && (double)c.y + pHalf >= pCeil) {
            c.y = (float)(pCeil - pHalf);
            c.vy = 0; CLAMP0("ground/flipceil");
            c.grounded = 1;
            groundedInvCeil = true;
            // GD runs buttons AFTER the collision pass, so a ball that
            // ARRIVES at the ceiling on the press tick flips on that same
            // tick. The input gate above ran on the pre-clamp position and
            // rightly said "airborne", so the edge would otherwise be spent
            // by the time the ride grounds it -- measured on lv16's second
            // dual t=12,494: GD clamps p2 to 621 and it leaves with the
            // mirrored tap (-2.7408) in the SAME tick; the model froze it
            // at 621 with the plan holding, and the pair never re-mirrored.
            if (isBall && input && !s.action) {
                impulsedThisTick = true;
                ballFlippedThisTick = true;
                c.flip = 0;
                c.vy = (float)(-ballFlipFor(useDx)
                               * (c.mini ? kMiniImpulse : 1.0));
                c.grounded = 0;
            }
        }
        // [2026-08-20 rig ramps] The ceiling **blocks from above regardless of the
        // gravity direction**. An upward-moving player is merely stopped, not
        // seated (not set grounded). Measured (rig ramps t=17,347, ball non-mini,
        // sp0.9, band [90,330]):
        //   GD  t=17,346 y=314.336 vy=+5.300 -> t=17,347 y=315.000 vy=0.000
        //       onGround=0 throughout, then free fall at -0.129/tick
        //   the model passed straight through and climbed to y=334 (divergence
        //   167 ticks / max 139px)
        // The exact mirror of the flipped-side branch just above and of the
        // "ground blocking from below" (kGroundY) just below. `c.vy > 0` is the
        // gate limiting it to **only the tick it pushed up from below**: at lv22
        // t=5,683.. an upward ball at y=1,035 (above the actual ceiling) comes down
        // far above band 387, and without this it gets sucked to 372.
        else if (!c.flip && (double)c.y + pHalf >= pCeil && c.vy > 0) {
            c.y = (float)(pCeil - pHalf);
            c.vy = 0; CLAMP0("ground/ceilblock");
        }
        // GD's ground plane blocks the player from BELOW whatever its gravity is:
        // an inverted mini ship flown downward is stopped at 99.000 and held there
        // (lv11 t=9793). The old test had !c.flip and let it sink through the world.
        // Skipped in a TURNED frame: `y` is a world X there, so this plane is a
        // wall at world x=90 that does not exist (see spiderTargetY's note).
        // [2026-08-20 r38] **The band's floor catches the ball/spider too.** The
        // ceiling side (pCeil) was in but the floor side was not, so the model let
        // a ball fall straight through where GD stops it at pmin+pH. The flight
        // branch already does this (`floorY = max(btFlyF, kGroundY)`) -- the same
        // picture on the cube-family side.
        // Measured lv20 t=9,055 (mini ball, sp0.9, band [270,510], with a press):
        //   GD  t=9,054 y=280.263 vy=-9.128 -> t=9,055 **y=279.000 exactly**
        //       (=270+9); the flip tap rides on the same tick, upsideDown 0->1,
        //       vy=+2.683 = ballFlipFor(0.9) x kMiniImpulse exactly
        //   the model stayed in free fall at 278.18 (census edy +0.820 /
        //   edvy +11.940)
        // The gate is the same as the ceiling's: "the band value sits on the 30
        // grid" (see the note on bandUsable).
        const double bandF = bandFT;   // r81: recorded band first (see the ceilHere note)
        const bool bandFUsable =
            bandF > kGroundY + 0.01 && bandF < 1e8
            && std::fabs(bandF - std::round(bandF / 30.0) * 30.0) < 0.01;
        const double floorHere =
            ((isBall || isSpider) && bandFUsable) ? bandF : kGroundY;
        // [2026-08-21 r90] **The ground does not catch on a tick the player
        // warped.** Calibration rig ceilhold's 6 spider cells (pressing while hung
        // from a ceiling ramp = teleport to the opposite face, landing on this
        // world ground): GD **stays vy=-1.000 / onGround=0 on that tick** and seats
        // on the next tick (the teleport's signature -/+1.000 is as measured in the
        // note at 6291). The model's teleport puts y exactly at the seat, so this
        // caught it on the same tick and erased the -1.000 (all 6 cells edvy
        // exactly 1.000). The other loops (10435 / 11351 / 11611) already have the
        // same gate.
        if (c.frame == 0 && !c.grounded && !spiderWarpedThisTick
            && c.y <= floorHere + pHalf
            && c.vy <= 0) {
            // ...but an INVERTED CUBE is not blocked here: GD kills it.
            //
            // The block above reads "whatever its gravity is", and its evidence is one
            // measurement -- an inverted mini SHIP held at 99.000 on lv11. Carried to the
            // cube it invents a floor GD does not have. Measured on custom level 1474319
            // (2026-08-27), twice, on two different plans:
            //   plain plan  t=425 y=105.262 (bottom 90.262) alive
            //               t=426 y=104.982 (bottom 89.982) DEAD, obj=NULL
            //   +tap plan   t=379 bottom 91.522 alive -> t=380 bottom 89.735 DEAD, obj=NULL
            // The kill is on the crossing tick, with no object and no two-tick latch, and
            // there is no collider anywhere near y=90 in that stretch -- it is the world
            // plane itself. The model instead landed the cube at y=105 exactly (bottom
            // 90.000, vy=0) and flew the rest of the level from a state GD had ended, which
            // is why every plan it built there died in the game at the same three x's.
            // Confirmed on an OFFICIAL level, so the rule does not rest on levels this
            // repository does not carry: lv4 t=8,358 x=10,863, where the player is
            // genuinely an inverted cube and the nearest object of any kind is 210 px away
            // in y (nothing within 120 px of that x below y=200). Injected at y=120 with
            // vy=-14.5, GD ran it down 120 -> 116.79 -> 113.62 -> 110.50 -> 107.44 ->
            // 104.42 and fired destroyPlayer on that last tick -- the tick, and the only
            // tick, on which the box bottom went under 90 (92.44 -> 89.42). Every suspect
            // came back with POSITIVE clearance, the nearest at +180.6 px. Two controls at
            // the same tick: vy=0 floats up and lives, and vy=-2 (which descends to a
            // bottom of ~100 and reverses) lives 42 ticks -- so it is the CROSSING of 90
            // that kills, not the descending. The plane is not level-specific, which is
            // what makes lv4 an answer to the question lv20 could not host (every one of
            // lv20's 872 inverted-cube ticks has geometry in the y=70..122 band, so any
            // death there is an object's, and the state cannot be injected either: mode
            // and gravity are not injectable and lv20 is a WAVE at the tick in question).
            // Ground modes other than the cube are left alone: the ball's catch at lv20
            // t=9,055 is measured UPRIGHT (the flip tap lands on the same tick), so nothing
            // in the corpus says what an inverted one does, and inventing it would be the
            // same mistake in the other direction.
            // The rule was for a day a switch, off by default, because it cost lv20 its
            // cold clear -- not on physics but on the CAP: the route survives the rule and
            // the first solve is PARTIAL at x=28,823 at cap 2000 and 4000 and SOLVED at cap
            // 8000, both frontiers identical to t=2,500 and parting at the rule's first
            // firing (t=2,528, an inverted cube descending at vy=-14.5), then sitting at
            // the cap with 7.3M states dropped. What the loop actually could not get past
            // was x=25,760.9, where GD kills the dual's SECOND body in the corridor mode
            // portal uid13881 opens; three measured holes fed it (a mode portal did not end
            // a ceiling press the way it ends a slope ride, a ceiling release fired on the
            // tick the body came to rest instead of the tick after, and the two bodies
            // shared one `mode` and one press counter although each is tested at its own
            // y). With those closed the suite clears cold with the rule unconditional --
            // 22/22, net -29 iterations -- so there is nothing left for a switch to select.
            if (c.mode == 0 && c.flip) {
                DIE("ground/floor-inverted-cube", nullptr);
                return c;
            }
            c.y = (float)(floorHere + pHalf);
            c.vy = 0; CLAMP0("ground/floor");
            // only an upright player RESTS on it; a flipped one is merely
            // blocked and drifts back up under its own gravity, which is what
            // GD's two ticks of vy = 0 then +0.122 show.
            if (!c.flip) c.grounded = 1;
            // ROBOT: a press buffered while airborne jumps on the LANDING tick,
            // on the ground plane exactly as on a block (the cube/ball branches
            // of the solids loop below; the ground landing had none at all).
            // Measured on lv22 t=19,835 (mini, sp1.1): press t=19,834 airborne
            // (vy=-5.326), GD's landing tick carries BOTH y=99 (=90+9, the
            // ground) and vy=+4.568 (=11.42 x 0.8 mini x 0.5 robot), onGround
            // 0, never a vy=0 tick. The model landed and jumped one tick late
            // -- fixcensus edvy=+4.568 / edy=0.000 exactly.
            // ROBOT ONLY: the cube's ground-plane case is unmeasured (its
            // block-landing branch below IS measured, lv20 t=1,224).
            if (c.mode == 5 && !c.flip && input && !s.action
                && !impulsedThisTick) {
                impulsedThisTick = true;
                const CubePhys jph = cubePhysFor(useDx);
                c.vy = (float)(jph.jump
                               * (c.mini ? (kCubeJumpMini / kCubeJump) : 1.0)
                               * kRobotJumpScale);
                c.rHover = (uint8_t)kRobotHoverTicks;
                c.grounded = 0;
            }
            // ...and a BALL that ARRIVES here on the press tick flips on that
            // same tick -- GD runs buttons AFTER the collision pass, so the
            // input gate further up ran on the pre-clamp position and rightly
            // said "airborne". This is the **mirror** of the ceiling side (the
            // same-tick tap inside `ground/flipceil`), which was measured at lv16
            // t=12,494. The floor-side measurement is lv20 t=9,055 (note above): GD
            // emits both upsideDown 0->1 and vy=+2.683 on the same tick. The
            // impulse is in **the new gravity's direction**, so upward +ballFlipFor
            // (the ceiling side is -).
            if (isBall && !c.flip && input && !s.action && !impulsedThisTick) {
                impulsedThisTick = true;
                ballFlippedThisTick = true;
                c.flip = 1;
                c.vy = (float)(ballFlipFor(useDx)
                               * (c.mini ? kMiniImpulse : 1.0));
                c.grounded = 0;
            }
        }
        for (const Obj* o : *K.near) {
            // inclusive: touching edges count as overlapping (see kSupportReach)
            if (std::fabs(x - o->cx) > o->hw + pHalf + kContactEps) continue;
            if (o->type != 0) {  // hazard (sub-tick sampled, planning margin)
                if (K.t > 14) {
                    // TRIED AND REVERTED (2026-08-03): sampling SAWS at the tick
                    // END only (nSub = 1, si from 1), on the theory that the
                    // intermediate samples eat lv18's sub-pixel clearance past
                    // the saw at (24,571.7,375.5). The frontier was UNCHANGED --
                    // same alive counts, same y ranges, same death tick.
                    // ...but a spider TELEPORT is not travel. GD puts the
                    // player on the other surface in one step and never looks
                    // at the gap. Measured on lv22 t=1,910: GD warps
                    // y 316.5 -> 163.5 and lives, while the model swept the
                    // 153 px segment and died on the spike uid17571 (2.6x4.8 at
                    // 2272.5,213.25) sitting 50 px above the landing -- the
                    // only thing stopping a from-the-head replay of the r24
                    // plan, which otherwise matched GD's dump on every tick.
                    // ...and the LANDING is not where GD looks either. The
                    // per-tick order is update -> checkCollisions -> buttons,
                    // so the collision pass of the warp tick runs at the
                    // position the player had BEFORE the tap: the new x, the
                    // OLD y. Measured on lv22 2026-08-15 (spliced plan, GD
                    // worker 98, the event list): with the press at t=1,969 the
                    // warp lands on t=1,970 and the run reaches x=4,824; with
                    // the press at t=1,970 the warp would land on the same tick
                    // x reaches 2,343.444, and GD prints
                    //   checkCollisions
                    //   destroyPlayer 2343.44 163.5     <- the OLD y
                    //   INJECT_handleButton 1970 1      <- the tap, too late
                    // The model sampled the landing (2,343.444, 316.5), found
                    // it clear, and returned SOLVED on a plan GD kills -- the
                    // whole lv22 wall at x=2,343.
                    // The landing is not lost by skipping it here: the NEXT
                    // tick's si=0 sample is exactly (this tick's x, the landing
                    // y), which is where GD's next collision pass runs.
                    const bool warped = spiderWarpedThisTick;
                    const int subLo = warped ? kSubSteps : 0;
                    for (int si = subLo; si <= kSubSteps && !dead; ++si) {
                        const double f = si / (double)kSubSteps;
                        const double sx = xPrev + (x - xPrev) * f;
                        const double sy = warped
                            ? (double)s.y
                            : (double)s.y + ((double)c.y - (double)s.y) * f;
                        if (hazardHit(o, sx, sy, pHalf, kHazMargin,
                                      sawMarginFor(s.mode), s.mode, c.mini)
                            && (!g_obbAll || !o->obbOk
                                || obbOverlap(*o, sx, sy, pHalf, (double)s.rot)))
                            DIE("cube/hazard", o);
                    }
                }
                continue;
            }
            const double face = s.flip ? (o->cy - o->hh) : (o->cy + o->hh);
            const double prevFoot = (double)s.y - gsign * pHalf;
            const double newFoot = (double)c.y - gsign * pHalf;
            const double vpNow = (double)c.vy * gsign;
            // While riding a slope the ordinary landing test must be strict.
            // kLandTol lets a player "reach up" 6 px onto a face it never
            // crossed, and lv19's first slope has a 1.5 px slab (id 468) at its
            // top: climbing past it, the cube's foot is 5.5 px below the slab,
            // inside the tolerance, so the model teleported onto the slab at
            // t=245 while GD stayed on the slope until t=253.
            //
            // TRIED AND REVERTED (2026-08-18, same session): capping the
            // reach-back by the X penetration (min(kLandTol, xpen)), on an
            // axis-resolution theory. It refused the everyday landing where
            // the foot sinks past the face plane BESIDE the block and the
            // block then arrives (depth <= ~4, xpen ~1.3 on the entry tick)
            // -- 14 new census families across 13 levels, all gdg1. The
            // worker-98 sweep that killed the theory is in kLandTol's own
            // comment: at the SAME depth 10, an x-centred inject lands and a
            // side entry refuses -- but at depth 9.5 the side entry lands
            // too, so the discriminator is the DEPTH boundary (~10.0), not
            // the entry direction. The flat kLandTol stays; only its value
            // moved (10.5 -> 10.0, see the declaration).
            double landTol = c.onSlope ? 0.001 : kLandTol;
            bool stepCandidate = false;
            // [2026-08-21 r84] **Even mid-ride, a "top face deeper below the line"
            // is grabbed as a step.** The 0.001 above was a guess to reject lv19
            // t=245 (a 1.5px slab sitting on the ramp's top end = **above** the
            // line), and it blocked the opposite side (a shelf below the line) too.
            // Calibration rig ridestep (a top face placed beside an uphill chain,
            // sweeping the **depth below the line** in 0.05px steps): GD splits at
            //   depth <= 3.100 -> passes / depth >= 3.200 -> mounts
            // with **the same boundary at |m|=1 and 0.5** (phase/reach do not split
            // it). ball/robot the same (2 points each). **Only the spider does not
            // mount even at depth 3.6**, so it is excluded. Consistent with both
            // corpus samples:
            //   lv16 t=5,378 (block uid1935 top=326, line 334 -> depth 8) mounts
            //   lv19 t=245  (slab above the line -> depth < 0)            does not
            if (c.onSlope && s.onSlope && !c.flip && c.mode != 6
                && (double)s.slopeM != 0.0) {
                const double mR = (double)s.slopeM;
                const double xoffR = slopeXOffset(mR, pHalf);
                // Foot and contact point are both taken at the **previous tick**
                // (s.y is the previous tick's ride y = the line at the previous
                // tick's contact point. Extrapolating at this tick's x
                // underestimates by m*dx and shifts the boundary by 1 tick)
                const double rR = xPrev + (mR > 0 ? xoffR : -xoffR);
                const double footR = (double)s.y - pHalf;
                // The point where depth is measured is "the contact point clamped
                // to the object's span". Measuring at the near end in the travel
                // direction, a player **already inside the object's span** gets
                // extrapolated back to the far end and the depth jumps (lv19
                // t=15,515: going back 44px to the left end of a 60-wide 1.5px slab
                // read depth 43.7, and it wrongly moved onto the step from a
                // downhill ramp GD keeps riding -- exposed as family +1).
                const double nearX = std::min(std::max(rR, o->cx - o->hw),
                                              o->cx + o->hw);
                const double lineAt = footR + mR * (nearX - rR);
                if (lineAt - face >= kStepDepth) {
                    landTol = kLandTol;
                    stepCandidate = true;
                }
            }
            if (g_dynDbg >= 0 && o->uid == g_dynDbg)
                std::printf("land? t=%lld uid=%d frame=%d flip=%d gs=%.0f "
                            "cy=%.3f hh=%.3f face=%.3f prevFoot=%.3f "
                            "newFoot=%.3f vpNow=%.3f -> %s\n",
                            (long long)K.t, o->uid, (int)c.frame, (int)s.flip,
                            gsign, o->cy, o->hh, face, prevFoot, newFoot, vpNow,
                            (vpNow <= 0 && (prevFoot - face) * gsign >= -landTol
                             && (newFoot - face) * gsign <= 0) ? "LAND" : "no");
            // [2026-08-19 the ball's corner landing is pen>1.0 (lv20 t=5,269)] When
            // a falling ball's centre is past the high end of a slope and the
            // landing face is at the same height as that corner, GD does not land
            // until the tick the box bottom breaks 1.0px below the corner (worker98
            // bisection 0.9955/1.0055, the same constant as preSlopeCollision's end
            // strip 1px inside). The site is the 1.5-wide bar uid5263, but the
            // edgeland rig measured "a plain plate catches even at 12.3 overhang",
            // so it is the neighbouring slope hijacking the test, not the bar's
            // width. The corner's gradient is also needed for the same-tick tap
            // bonus (below), so it is recorded.
            double ballCornerM = 0.0;
            if (c.mode == 2 && !c.flip && vpNow <= 0 && K.slopes
                && (newFoot - face) * gsign <= 0
                && (prevFoot - face) * gsign >= -landTol) {
                for (const Obj* spc : *K.slopes) {
                    if (spc->slopeHazard) continue;
                    if (slopeIsCeiling(spc->slopeDir)) continue;
                    const double sx0 = spc->cx - spc->hw;
                    const double sx1 = spc->cx + spc->hw;
                    const double sm = (spc->sy1 - spc->sy0) / (sx1 - sx0);
                    if (sm == 0.0) continue;
                    const double cxE = (sm > 0) ? sx1 : sx0;
                    const double cyE = (sm > 0) ? spc->sy1 : spc->sy0;
                    if (std::fabs(cyE - face) > 0.01) continue;
                    const bool past = (sm > 0)
                        ? (x > cxE && x - pHalf <= cxE + kContactEps)
                        : (x < cxE && x + pHalf >= cxE - kContactEps);
                    if (!past) continue;
                    ballCornerM = std::fabs(sm);
                    break;
                }
            }
            if (ballCornerM != 0.0 && (face - newFoot) * gsign < 1.0)
                continue;
            // [2026-08-21 r75] **A tick grounded on the invisible ceiling does not
            // grab a reach-back (a landing where the foot did not cross the
            // face).** kLandTol's boundary sweep (d=10.0 centre = lands) is entirely
            // measurements of a ball **injected in mid-air**. Calibration rig
            // ceilramp unit16 (flipped ball, hang-riding the band ceiling 330 with
            // og=1): the underside 320 of backfill block uid869 is 10.0px beyond
            // the foot (330) = just inside the tolerance, and only the model
            // "landed" at 305 on t=20554 and jumped 10px. GD does not grab even as
            // the x overlap grows; at t=20555 the ceiling ramp's line (313.033 =
            // ceilLimAt) picks it up, and from then on onSlope's strict tol (0.001)
            // guards it.
            // The gate is **groundedInvCeil only**: applying it to all of c.grounded
            // at first also dropped legitimate reach-back re-seats while grounded on
            // an actual object (the flipped cube's 0.05px / the flipped robot's
            // 0.75px, lv16/lv20) and families went +2 -- GD does grab the small
            // re-seats of a player standing on an actual object, even while
            // grounded.
            const double dHangL = (prevFoot - face) * gsign;
            if (vpNow <= 0 && dHangL >= -landTol
                && (dHangL >= 0.0 || !groundedInvCeil)
                && (newFoot - face) * gsign <= 0) {
                // kept in case a pad fires this same tick -- and only the FIRST
                // landing of the tick may record it. A 30 px box straddles two
                // 30 px blocks whenever it is not exactly aligned, so the loop
                // lands twice: the second pass then saved the y the first pass
                // had already snapped, and the pad "undid" the snap back to the
                // snap. Measured on lv16 t=4783: a flipped ball rising at
                // vy=10.285 into the ceiling blocks at x=6855 and x=6885 hits
                // the yellow pad at (6885,438) on the same tick, and GD leaves
                // it at the free-flight 426.6987 with vy=-9.6 -- it never rests
                // on the face. The model reported 425.000 and stayed 1.70 px low
                // for the rest of the section.
                if (!landedThisTick) preLandY = c.y;
                // r94: only the face highest along gravity writes y (measured, see decl.)
                const bool higherFace =
                    !landedThisTick || (face - landFaceBest) * gsign > 0.0;
                landedThisTick = true;
                cubeLandedThisTick = true;
                // r84: moved onto a step mid-ride (stops the slope loop below)
                if (stepCandidate && dHangL < -0.001)
                    stepLandedThisTick = true;
                if (higherFace) {
                    landFaceBest = face;
                    c.y = (float)(face + gsign * pHalf);
                }
                c.vy = 0; CLAMP0("fly/land");
                c.grounded = 1;
                // GD runs buttons AFTER the collision pass, so a ball that
                // LANDS on the press tick flips on that very tick. The input
                // gate above saw the pre-landing position and said airborne,
                // and the edge would be spent by the next tick. Measured on
                // lv16's second dual t=12,495: p2 (flipped, rising 9.93)
                // reaches the ceiling blocks at 630, GD reports y=621 AND the
                // mirrored tap (-2.7408) on the same tick; the model parked
                // it at 621 and the pair never re-mirrored. Ball only: the
                // cube's same-tick jump is unmeasured and the last "one
                // measurement" cube landing rule cost 7 levels (see below).
                if (isBall && input && !s.action && !ballFlippedThisTick) {
                    impulsedThisTick = true;
                    ballFlippedThisTick = true;
                    c.flip = c.flip ? 0 : 1;
                    // A tap on the tick of a corner landing carries the slope
                    // bonus. The value scales with the gradient's exit ratio (lv20
                    // t=5,270 measured: 3.9438 = 3.426 + 0.5178, 0.5178 = 0.2796 x
                    // exit(1)/exit(0.5) = x1.8517. Both samples are sp1.1, so it is
                    // written as a ratio that makes no claim about speed scaling).
                    const double cornerBonus = (ballCornerM != 0.0)
                        ? kBallSlopeTapBonus
                              * (slopeExitVy(ballCornerM, 2, useDx,
                                             c.mini != 0)
                                 / slopeExitVy(0.5, 2, useDx, c.mini != 0))
                        : 0.0;
                    c.vy = (float)(-(ballFlipFor(useDx) + cornerBonus)
                                   * (c.mini ? kMiniImpulse : 1.0)
                                   * (c.flip ? -1.0 : 1.0));
                    c.grounded = 0;
                }
                // ...and the CUBE does the same thing, which the note above
                // called unmeasured. MEASURED 2026-08-09 on lv20 t=1224 (cfg
                // orbtrace=1 for the event list, so the press tick is read and
                // not inferred): the plan's edge is `INJECT_handleButton 1223
                // 1`, the cube is still airborne at t=1223 (y=256.500,
                // vy=-8.576), and on t=1224 GD's dump has BOTH the landing
                // (y=255.250, the slab id664 top 240.3 plus half 15) and the
                // jump (vy=+11.180). The model landed and sat there: its
                // `groundedNow` is the support at the START of the tick, so the
                // edge was spent while airborne and the next tick had no edge
                // left. It stayed 22 px/tick behind for the rest of the level.
                // The EDGE is kept (`!s.action`) -- this only moves WHEN the
                // support is read, exactly as the ball's case above does. The
                // rule that cost 7 levels was a different one: it let a cube
                // jump off a face it had merely touched, without the edge.
                // ...and the ROBOT too (2026-08-18): the buffered press works
                // exactly like the cube's. Measured on lv22 t=19,835 (mini,
                // sp1.1): the plan presses at t=19,834 while airborne
                // (vy=-5.326), the robot lands the next tick, and GD's dump has
                // BOTH the landing (y=99, the surface) and the jump
                // (vy=+4.568 = 11.42 x 0.8 mini x 0.5 robot) on t=19,835 --
                // never a vy=0 tick. onGround stays 0. The model landed, sat
                // one tick, and jumped at 19,836: the fixcensus signature is
                // edvy=+4.568 with edy=0.000 exactly.
                // Held-through (edge 30+ ticks old) is DIFFERENT and stays as
                // is: lv22 t=15,878 measured GD re-jumping the tick AFTER the
                // landing (the kRobotGScale note), which the main grounded
                // branch already does. The edge here is what distinguishes the
                // two -- same as the cube.
                else if ((c.mode == 0 || c.mode == 5) && input && !s.action
                         && !impulsedThisTick) {
                    impulsedThisTick = true;
                    const CubePhys jph = cubePhysFor(useDx);
                    double vj = jph.jump
                                * (c.mini ? (kCubeJumpMini / kCubeJump) : 1.0);
                    if (c.mode == 5) {
                        // half impulse + the hover budget, exactly as the
                        // ordinary robot jump arms it (GD zeroes this+0x830 in
                        // the same block that writes the jump velocity)
                        vj *= kRobotJumpScale;
                        c.rHover = (uint8_t)kRobotHoverTicks;
                    }
                    c.vy = (float)(vj * (c.flip ? -1.0 : 1.0));
                    c.grounded = 0;
                }
            } else if (!o->oneway) {
                // CEILING STOP. A cube whose HEAD reaches a solid's near face is
                // not killed -- GD pins it there, zeroes vy and sets onGround,
                // so it can even jump off again.
                // Measured on lv22 (runR5's plan, cfg hitboxtrace=1, so the
                // rects are GD's own):
                //   t=2,748  y=320.771 vy=10.816 onGround=0 (head 329.77,
                //                                            face 330.01)
                //   t=2,749  y=321.008 vy=0      onGround=1
                //
                // Two gates, both needed -- a version with neither made 13 of
                // the 21 levels worse, and a version with only the first still
                // did:
                //   * ACQUIRING it needs the same depth test the ship's ceiling
                //     ride has. Instrumented lv1's false stop against lv22's
                //     real one: xPen 6.67 (full size, threshold 26.25) vs 19.78
                //     (mini, 15.75), so `xPen > kSolidResolveX * pHalf` splits
                //     them exactly.
                //   * HOLDING it reads `ceilPin`, not `grounded`. lv22 rides
                //     this face for 60+ ticks while the slab itself creeps
                //     upward, so the pin has to survive; but `grounded` is also
                //     true for a player walking the floor, and then any block
                //     whose underside passes within a pixel of its head grabs
                //     it (lv1 t=5,800).
                //   * the tolerance is 0.001, NOT kLandTol. That 6 px is the
                //     landing's "reach up onto a face" slack; reused on the head
                //     it grabs anything whose underside passes within 6 px.
                //     This one gate took the damage from 13 levels to 2.
                //   * UPRIGHT only. A flipped cube descending onto a block's top
                //     is the same geometry mirrored, and GD does NOT stop it:
                //     lv8 t=1,263 has the flipped cube 0.2 px into uid79 and it
                //     keeps falling (134.80, 134.23, 133.71 ...) with
                //     onGround=0 the whole way. With `!s.flip` the whole suite
                //     is byte-identical; without it lv8 loses 137 ticks.
                // [2026-08-19] The ball's head bonk has an "entering from the side"
                // path. Measured lv9 t=18,354 (uid3976, 30x30, underside 270): on
                // the tick the right edge of a rising ball's box (vy +2.279,
                // decaying arc) crosses the left edge 23,850, GD **pushes y down**
                // to 255 (=underside-pH), vy=0, not grounded (g=0), free fall from
                // the next tick (-0.129/tick). yPen 2.94 / xPen 0.99 -- a
                // **crossing in the opposite sense** from the cube bonk rejected on
                // 2026-08-18 (the grazing arc with yPen<=xPen); the gate is "x
                // entered on this tick". The cube stays as is, per the rejection's
                // measurement; the robot is covered by the bonk above. The flipped
                // side and the upper bound of yPen (vertical clip) are unmeasured
                // -- provisionally pH/2.
                if (c.mode == 2 && !s.flip && vpNow > 0.001) {
                    const double headB = o->cy - o->hh;
                    const double yPenB = ((double)c.y + pHalf) - headB;
                    const bool xEntered =
                        std::fabs(x - o->cx) <= o->hw + pHalf
                        && std::fabs(xPrev - o->cx) > o->hw + pHalf;
                    if (xEntered && yPenB > 0.0 && yPenB < pHalf * 0.5) {
                        c.y = (float)(headB - pHalf);
                        c.vy = 0; CLAMP0O("ball/headbonk", o);
                        c.grounded = 0;
                        continue;
                    }
                }
                // [2026-08-18] cube only -> robot too. The head-bonk measurement is
                // a mini robot (lv22 t=18,600, the bonk note below). The robot
                // shares every other rule of this branch as "a cube with 0.9
                // gravity", yet only the head side passed straight through.
                // ball/spider are kept outside, unmeasured (the ball's ceiling has
                // the separate flipceil rule).
                if (c.mode == 0 || c.mode == 5) {
                    const double head = s.flip ? (o->cy + o->hh)
                                               : (o->cy - o->hh);
                    const double prevHead = (double)s.y + gsign * pHalf;
                    const double newHead = (double)c.y + gsign * pHalf;
                    const double xPenC = (o->hw + pHalf) - std::fabs(x - o->cx);
                    // 1.0, not a hair: lv22's slab is itself climbing, and by
                    // t=2,771 it moves 0.35 px in a tick -- a tolerance that
                    // tight drops the pin mid-ride and the model peels away.
                    const bool held = s.ceilPin != 0
                                      && std::fabs(prevHead - head) <= 1.0;
                    // The PIN is upright-only (the lv8 measurement above). The
                    // ARMED flip is not: GD's didHitHead has no orientation
                    // gate, it runs off the head-collision branch, and the head
                    // follows the player's own gravity. Measured on lv22's
                    // reference replay, x=3,774 -> 4,395 is a CORRIDOR the cube
                    // crosses by flipping back and forth off both surfaces:
                    //   t=2,991 up 0->1 (y=321)   t=3,207 up 1->0 (y=293)
                    //   t=3,301 up 0->1 (y=321)   t=3,421 up 1->0 (y=291)
                    // With `!s.flip` on the armed path the model could only ever
                    // make the FIRST of those, so from x=3,790 it had no way
                    // down: every route it found flew off the top of the level
                    // and died to GD's out-of-bounds kill at y=3,684.
                    const bool acquireBase = vpNow > 0.001
                                             && (prevHead - head) * gsign <= 0.001
                                             && (newHead - head) * gsign >= 0;
                    const double yPenC = (newHead - head) * gsign;
                    // The ARMED flip does NOT get the depth gate. That gate is a
                    // stand-in for "GD resolved this collision vertically", and
                    // 1.75*pHalf is far too strict: measured on lv22 t=2,961 the
                    // cube flips off uid3094 (a static 30x30 at 4,185,345) while
                    // its centre is at x=4,204.19 -- 4.19 px PAST the slab's
                    // right edge, so xPen is 4.81 against a threshold of 15.75.
                    // GD picks the axis with the SMALLER penetration, and the
                    // vertical one there is 0.6 px. That is the test used here.
                    // Without this the model could not make the corridor's
                    // second flip and every route it found flew off the level.
                    const bool acquire = acquireBase && !s.flip
                                         && xPenC > kSolidResolveX * pHalf
                                         && g_ceilPin;
                    const bool acquireFlip = acquireBase && s.fgArm
                                             && yPenC <= xPenC;
                    // [2026-08-18] The plain **head bonk**. What g_ceilPin's note
                    // rejected was "grab and ride (pin: running along the underside
                    // with grounded=1)", which does not mean GD does nothing -- on
                    // the tick a rising head crosses the underside with
                    // **yPen <= xPen** (the shallow axis), GD puts y on the face,
                    // vy=0, **does not ground it**, and free-falls it from the next
                    // tick. Measured lv22 t=18,600 (slope-exit arc off the
                    // reverse-travel staircase, mini robot, underside 2070,
                    // yPen 1.25 / xPen 14.44):
                    //   GD: y=2061 (=2070-9) vy=0 -> -0.194/tick from t=18,601
                    //   model: passed straight through, kept climbing (nearceil
                    //   family)
                    // Consistent with the 2026-08-16 sweep ("no pin" at xPen
                    // 15.957 / 24.0): that was the rejection of the pin; the bonk
                    // only erases vy.
                    // **ROBOT ONLY.** Applied to the cube too, it over-fired on 14
                    // levels / 28 families and was rejected at once (verify
                    // 2026-08-18): GD's cube passes straight through a yPen<=xPen
                    // crossing where the jump arc grazes a corner, with no bonk. As
                    // the neighbouring note says, "one point is not a rule" -- the
                    // cube's head side (lv16's nearceil family) awaits a separate
                    // measurement.
                    // [2026-08-26] ...and the MINI cube gets it too. The switch
                    // band's punch IS this bonk: lv22's certified tap sequence,
                    // four contacts of four, has GD clamp a mini cube rising
                    // +7.65 into a plain block's underside to y=261.000 =
                    // underside 270 - pHalf 9, vy=0, grounded=0, falling away
                    // from the next tick. The model killed it as
                    // cube/solid-side, a rule for LATERAL entry (lv9, xpen~11).
                    //
                    // FULL SIZE MUST NOT, and that is measured, not assumed.
                    // The first cut gated on "x already overlapped at tick
                    // start" -- which reads identically on both sizes -- and it
                    // cost eleven levels their fidelity (quick_regress, the
                    // whole cube suite) plus lv18's cold clear: the model
                    // clamped a full cube grazing 0.175 px into uid12842
                    // (30x30 at 25,095/315) at t=18,252 and lost the only
                    // corridor, while GD flies straight through -- an injection
                    // sweep of that contact passes at every penetration from
                    // -0.5 to +5.25 px with vy untouched. lv13's three contacts
                    // and lv21's one are the same story from the other side:
                    // with the bonk on they diverge, with it off they are
                    // bit-identical.
                    //
                    // What separates the sizes is NOT in this branch's
                    // geometry. Refuted by measurement: object id (lv18's
                    // uid12830, same id 269, same pad beneath, CLAMPS a full
                    // cube at dx=+1.1 where uid12842 passes at dx=+18.6), a
                    // pad flush under the block (both have one), the
                    // neighbourhood (identical but for decorations), and every
                    // threshold on |dx| / xPen / xPen-over-pHalf (mini clamps
                    // at 4.65 px of overlap where full passes at 11.41).
                    // So the rule stands only where it was measured. The next
                    // instrument is the disassembly of the head branch, not
                    // another fitted constant.
                    const bool bonk = (c.mode == 5 || (c.mode == 0 && c.mini))
                                      && acquireBase && !s.flip && !s.fgArm
                                      && yPenC <= xPenC;
                    if (std::fabs(x - o->cx) <= o->hw + pInner
                        && (held || acquire || acquireFlip || bonk)) {
                        // ARMED (id 2866 touched): PlayerObject::didHitHead runs
                        // its body instead of doing nothing, so the player turns
                        // over and stands on the face rather than hanging under
                        // it. The resting y is the same either way (head - pHalf
                        // upright == head + gsign*pHalf flipped), which is why
                        // the pin looked right when it was first fitted to this
                        // very event -- everything downstream is different.
                        // GD also sets vy to +/-2 here, but the landing that
                        // follows in the same tick zeroes it (measured: t=2,749
                        // has onGround=1, yvel=0).
                        if (acquireFlip) {
                            c.flip = (uint8_t)!s.flip;
                            const double g2 = c.flip ? -1.0 : 1.0;
                            c.y = (float)(head + g2 * pHalf);
                            c.vy = 0; CLAMP0O("cube/fliphead", o);
                            c.grounded = 1;
                            c.ceilPin = 0;
                            // every gravity flip negates the cube's spin
                            c.rotNeg = c.flip;
                            continue;
                        }
                        // plain bonk: stops but does not grab (note at the declaration above)
                        if (bonk && !held && !acquire) {
                            c.y = (float)(head - gsign * pHalf);
                            c.vy = 0; CLAMP0O("cube/headbonk", o);
                            c.grounded = 0;
                            c.ceilPin = 0;
                            continue;
                        }
                        c.y = (float)(head - gsign * pHalf);
                        c.vy = 0; CLAMP0O("cube/ceilstop", o);
                        c.grounded = 1;
                        c.ceilPin = 1;
                        continue;
                    }
                }
                // one-way platforms never kill.
                //
                // OPEN BUG (2026-08-03): this ALSO kills on the block a PAD just
                // launched the player off, and GD does not. Measured on lv18
                // from `18000,24372.438,275,0,0,0,0,0,1,...`: the blue gravity
                // pad at (24,375,273) flips the player and gives vy = 5.12,
                // model and GD agreeing exactly at t=18,001. On the NEXT tick
                // the player is at y=276.15 and still overlaps the block the pad
                // sits on (274 at (24,375,255), top 270) by 2.85 px -- GD lets
                // it go because it is leaving, the model kills it on the side
                // test. Every branch that uses that pad dies one tick later.
                // TRIED: gating on `!impulsedThisTick`. It does NOT work --
                // the pad branch never sets that flag (only the cube jump, the
                // ball tap and the UFO flap do), so nothing changed. The gate
                // has to key off the PAD firing: set a local flag in the
                // `c.usedPad[slot] = pd` branch and skip the side test for the
                // rest of this tick, or better, skip only when the player is
                // moving AWAY from this object's face in its own gravity frame.
                // ...unless the player is SEPARATING from it: centre moving away
                // from the block's centre means it is leaving, not running in.
                // This is what a pad/orb launch off the top of a block looks
                // like for the tick after it fires.
                // ...and only when it was ALREADY over/under the block at the
                // start of the tick. Without that x test the guard also spared
                // a player entering through the SIDE while drifting down past
                // the block's centre, which GD does kill (lv18 t=18,175:
                // (24,654.8, 252.0) vy=-4.19 into 269 (24,675,255), xPrev is
                // still 21.85 px away = not yet over it).
                const bool separating =
                    ((double)c.y - o->cy) * (double)c.vy > 0.0
                    && std::fabs(xPrev - o->cx) < o->hw + pInner;
                for (int si = 0; si <= kSubSteps && !dead && !separating; ++si) {
                    const double f = si / (double)kSubSteps;
                    const double sx = xPrev + (x - xPrev) * f;
                    const double sy = (double)s.y + ((double)c.y - (double)s.y) * f;
                    if (std::fabs(sy - o->cy) < o->hh + pInner + kHazMargin
                        && std::fabs(sx - o->cx) < o->hw + pInner + kHazMargin)
                        DIE("cube/solid-side", o);
                }
                // TRIED AND REVERTED (2026-07-31): a cube "face snap" here --
                // when the box overlaps a solid too shallowly to kill and the
                // player is moving into the face with its centre still outside
                // it, put the player on the face (c.y = face + gsign*pHalf,
                // vy = 0, grounded). It reproduces lv14 t=7078 exactly, where a
                // flipped cube rising at vy=10.936 first overlaps the block at
                // (9225,751) by 1.24 px in x and GD pins it at y = 721.000 --
                // but the COLD REGRESSION went 13/13 -> 6/13: lv1, lv3, lv6,
                // lv8, lv9, lv10 and lv12 all went stuck. The centre test was
                // supposed to keep it from becoming a step climb and does not.
                // One measurement is not a rule (this is the seventh time).
                // Whatever GD does at that tick, it is narrower than this.
            }
        }
    } else {
        // ship: rides floors, lands from above, inner-box side/ceiling kills.
        //
        // The whole branch used to run in the WORLD frame with no notion of
        // flipped gravity -- only the cube branch mirrored. An inverted ship was
        // therefore simulated as an upright one, and since the ship's fall and
        // thrust accelerations are not symmetric the vy sequence came out wrong
        // in both magnitude and sign pattern: measured on lv7 t=5810 (ship with
        // upsideDown = 1) GD alternated -0.086/+0.103 per tick while the model
        // alternated -0.069/+0.108. Everything vertical is now done in the
        // PLAYER frame, the same way the cube branch does it.
        const double gsign = s.flip ? -1.0 : 1.0;
        // The flying band's floor, where the section has one, replaces the
        // world's ground line entirely: on lv12 the UFO is put back at y=195
        // (floor 180) with nothing underneath, and 90 never comes into it.
        // The recorded band, if any, takes priority over the frozen band (history at
        // the declaration of g_bandTrack). Zoom and camera movement are both folded
        // into the recording, so the zoom correction below is skipped. But **only
        // while the current y is inside the recorded band** (same gate as the wave
        // side: in a section flying outside the band, pmin/pmax are camera state
        // and not a clamp).
        double btFlyF = (double)s.bandFloor, btFlyC = (double)s.bandCeil;
        bool btFlyOn = false;
        if (s.frame == 0) {
            double f_ = btFlyF, c_ = btFlyC;
            if (bandTrackAt((long long)K.t, f_, c_)
                && (double)s.y >= f_ - 1.0 && (double)s.y <= c_ + 1.0) {
                btFlyF = f_; btFlyC = c_; btFlyOn = true;
                // The FLOOR side reads the previous row (the pan lands after the
                // clamp -- measurement at bandTrackFloorAt). The ceiling keeps the
                // r107 phase it was calibrated with.
                double ff = f_, fc = c_;
                if (bandTrackFloorAt((long long)K.t, ff, fc)) btFlyF = ff;
            }
        }
        const double floorY = std::max(btFlyF, kGroundY);
        bool groundedNow = s.grounded != 0;
        // A grounded SHIP on a MOVING face rides it, exactly like the cube
        // family's support loop (the `+ |dcy|` widening and the ride re-seat
        // by the stair snap). This loop had the plain 0.6 and no ride, so a
        // floor rising 0.2616/tick under a resting mini ship walked into it
        // for 3 ticks and the model re-landed 0.785 higher -- a staircase
        // where GD glides (lv19 t=21,73x, uid 15031's lift; the two
        // edy=+0.2616 census items). The ride gate is dcy != 0, so a static
        // floor is bit-identical.
        bool flyRide = false;
        double flyRideFace = 0.0, flyRideGap = 0.0, flyRideDcy = 0.0;
        if (groundedNow && (s.flip || s.y > (floorY + pHalf) + 0.5)) {
            bool sup = false;
            for (const Obj* o : *K.near) {
                if (o->type != 0) continue;
                const double face = s.flip ? (o->cy - o->hh) : (o->cy + o->hh);
                const double gap =
                    std::fabs((double)s.y - gsign * pHalf - face);
                if (std::fabs(x - o->cx) <= o->hw + pHalf + kContactEps
                    && gap < 0.6 + std::fabs(o->dcy)) {
                    sup = true;
                    // When TWO moving faces qualify, GD stands on the one that
                    // actually supports -- the highest along gravity -- not
                    // the one nearest the (stale) foot. Measured on lv19
                    // t=21,746: the thin platform uid 15033 enters the x-range
                    // 0.047 px BELOW the lift uid 15031 the ship rides;
                    // nearest-gap hopped to it and ran 0.0506 low from there.
                    if (o->dcy != 0.0
                        && (!flyRide
                            || face * gsign > flyRideFace * gsign)) {
                        flyRide = true;
                        flyRideGap = gap;
                        flyRideFace = face;
                        flyRideDcy = (double)o->dcy;
                    }
                }
            }
            groundedNow = sup;
        }
        // [2026-08-21 r88/r89] **A "carried vy" is not the resting slide.** The
        // convention written for the rising-floor catch (flyRide) (the note below:
        // only the vy==0 rest re-accumulates from 0) applies just as well to a
        // **hanging ride** (s.onSlope, sliding along the underside of a ceiling
        // ramp). The calibration rig ceilhold exposed both spots:
        //   ship held: on the tick the support scan re-raised groundedNow, the
        //     ladder re-accumulated from 0 (unit0 -3.376 -> -0.086)
        //   UFO after a jump: the UFO's act is an edge, so even while held it fell
        //     into the "rest short-circuit" from the next tick and vy became 0
        //     (unit7 -3.912 -> 0.000, GD keeps decaying at -3.760)
        // [2026-08-21 r96] **Narrowed to the hang (s.flip) only.** The rig
        // ceilhold's cells are all flip=1; there is no evidence about upright
        // rides. Applied to every ride, lv16's cold run swelled from 22 iterations
        // -> 56+ (census stayed green = the [[gd-census-green-cold-broken]] pattern).
        const bool ridingCarry =
            (flyRide || (s.onSlope && s.grounded && s.flip)) && s.vy != 0.0f;
        c.held = (uint8_t)input;
        // The UFO acts on the EDGE, the ship on the level. One press is one
        // flap and holding does not repeat it -- measured on lv12: pressed at
        // t=170 and held to 195, GD flapped once (vy := 6.871 at t=172, the
        // usual 2-tick input latency) and then decayed at the plain gravity
        // -0.129 for the whole hold. So a held button is "thrust" for a ship and
        // "nothing" for a UFO, and the grounded test below has to agree.
        const bool isUfo = (c.mode == 3);
        // SWING rides this branch too (see the note on kSwingG): airborne with a
        // TOGGLED gravity. Support / band / hazards / solids are the ship's.
        const bool isSwing = (c.mode == 7);
        // s.pFlap: a fresh edge on the same tick as a ship->UFO portal was eaten by
        // the old mode (thrust = the edge is not consumed). GD buffers it and emits
        // the flap on the next tick (lv19 t=13,892 portal -> 13,893 vy=+6.871). But
        // **only if still pressed on the next tick** -- lv14 t=14,193, pressed and
        // released within the same tick, does not fire in GD either (14,194
        // vy=1.619, plain decay).
        const bool act = (isUfo || isSwing)
            ? ((input && !s.action) || (isUfo && s.pFlap != 0 && input))
            : (input != 0);
        if (isUfo && act) impulsedThisTick = true;
        // ...but a PENDING SWING FLIP must not be swallowed by the grounded
        // short-circuit. The tap sets the pending bit and is then RELEASED, so
        // on the tick the flip is due `act` is 0 and this branch used to eat it
        // -- the model only left the surface on the NEXT press, one tick late
        // (lv22 t=3,630: GD is already airborne at vy=-1.000 while the model
        // was still grounded at 0).
        // A ship CARRIED by a rising floor is not the resting slide: its vy
        // holds the floor's own velocity (see the catch below), so a carried
        // state with vy != 0 goes through the airborne integration + catch
        // even without a press. The resting slide (vy == 0) keeps the hold.
        // FORCE BOX (id 2069) LIFTS a grounded swing off the floor: the seat
        // does not hold against a net upward push. Measured on lv22
        // t=3,479-3,488 (landed on the band floor y=105 inside the cy=105
        // carpet): GD's vy climbs 0 -> 0.13 -> ... -> 0.65 (+0.130/tick, two
        // boxes) and the y follows it up off the floor -- then the step drops
        // to +0.022/tick (= 0.108 - 0.086, ONE box) the tick after x passes
        // the carpet edge at 4,845+30, which is what pins the per-box k at
        // 0.108. The gate converts the world-up push into the player frame,
        // so a FLIPPED swing under a ceiling is pushed INTO its seat and
        // stays put (unmeasured, but the sign has nowhere else to go).
        const bool swingLift =
            isSwing && !g_forceBoxes.empty()
            && forceBoxAcc(modX, modY, pHalf) * gdSign > kSwingG;
        if (groundedNow && !act && !(isSwing && s.rHover) && !swingLift
            && !ridingCarry) {
            c.vy = 0;
            c.grounded = 1;
            // the ride: seat on the moved face (cube family's rule, same
            // ordering -- the carry happens in the collision pass)
            if (flyRide)
                c.y = (float)(flyRideFace + gsign * pHalf);
            // [2026-08-21 r86] **A resting flyer also holds "this tick's half
            // gravity step"** (same shape as the cube's pinnedOnBlock). This
            // short-circuit moves neither vy nor y, so left alone the model's y
            // stays one step high on the tick it launches off the ramp's top end.
            // Measured, calibration rig ceilrel07: on the launch tick GD first drops
            // y by 0.019 (= 0.225 x the UFO's weak gravity 0.086) and then writes
            // the launch value. The ship side has the same shape, 0.020. Consumed by
            // releasePin (slope-exit launch, pad, orb). The swing has its own branch
            // so it is left alone.
            // The swing is the same (the rig ceilrel07's 3 swing units: the sink is
            // 0.0193 = 0.225 x kSwingG, same shape as ship/UFO).
            if (!pinnedOnBlock
                && (s.flip || (double)s.y > (floorY + pHalf) + 0.5)) {
                const bool thrFlipRest = s.dual ? true : (s.flip != 0);
                const double vpRest = qVy(
                    isSwing ? -kSwingG
                    : isUfo ? gdapprox::UfoModel::stepVy(
                                0.0, false, ufoParamsFor(useDx, c.mini, K),
                                thrFlipRest)
                            : gdapprox::ShipModel::stepVy(
                                0.0, false, shipParamsFor(useDx, c.mini, K),
                                thrFlipRest));
                pinnedOnBlock = true;
                prePinY = (float)((double)c.y + kYScale * vpRest * gsign);
                // the same statement as the cube's grounded branch: the update
                // phase's step happened, the seat is the collision pass undoing it
                yFree = prePinY;
            }
        } else if (isSwing) {
            // The toggle lands ONE TICK AFTER the press edge: on the edge tick
            // GD still steps with the old gravity (t=3701 vy=-6.424, one plain
            // gravity step past -6.338), and the flip+damp appear on the next
            // (t=3702 vy=-5.053 = -(-0.8*(-6.424) - 0.086)). A same-tick flip
            // reproduced the shape but ran 0.155 high on every tick after.
            // The pending bit rides rHover, which is robot-only and therefore
            // free in mode 7 -- and it MUST be in the state (two states that
            // differ only in the pending flip diverge next tick).
            c.grounded = 0;
            // (the take-off special case that skips the clamp is now the
            // ceiling half only -- see ceilTakeoff below)
            double vpS = groundedNow ? 0.0 : (double)s.vy * gsign;
            double gsS = gsign;
            // GD's velocity-limit exemption (State::boost) clears at the HEAD
            // of updateJump's flying branch, on the INCOMING gravity-frame vy,
            // before the press flip and the gravity step
            // (0x38c527..0x38c59e). The band is (-6.4, +8.0)/chi: the
            // fall-side edge is 6.4, NOT the clamp's 8, which is exactly what
            // lets a slope exit at 6.4..8 keep the flag alive while the fall
            // accelerates past the terminal (lv22 x=5,391). chi is keyed on
            // the same [player+0x9f0]!=1 test that switches the swing gravity
            // 0.4->0.6 (the model's kSwingG->kSwingGMini), i.e. mini; no mini
            // swing exists in the corpus, so the divisor rides the
            // disassembly alone.
            {
                const double bandChi = (c.mini != 0) ? 0.85 : 1.0;
                if (vpS > -6.4 / bandChi && vpS < 8.0 / bandChi) c.boost = 0;
            }
            if (s.rHover) {                    // apply the previous tick's edge now
                c.rHover = 0;
                c.flip = s.flip ? 0 : 1;
                gsS = c.flip ? -1.0 : 1.0;
                vpS = -kSwingFlipDamp * vpS;   // vp in the new frame (sign mapped too)
            }
            if (act) c.rHover = 1;             // takes effect on the next tick (measured)
            // RETRACTED (2026-08-18): "taking off from a surface gets a fixed
            // unit push (-1.000) and no gravity step".
            //
            // It was written from lv22 t=3,630 ("grounded swing at y=327.875,
            // vy=-1.000 exactly"). **That tick does not exist in the current
            // reference** -- lv22 t=3,630 is now an AIRBORNE swing at y=262.07
            // with vy=+1.063, so the constant's justification was a stale
            // worldline ([[gd-constant-justification-goes-stale]]).
            //
            // The current reference contradicts it directly. lv22 t=4,621
            // (swing resting on the flat floor at y=405, tap at 4,620):
            //   GD      vy=+0.086   y=405.019348 (= 405 + 0.225*0.086)
            //   -1.000  vy=+1.000   y=405        (frozen)
            // +0.086 is exactly the ordinary path: vpS is 0 for a resting
            // state, the damp multiplies it to 0, and one kSwingG step in the
            // NEW frame is all that is left. The "the damp leaves the player
            // stuck" worry the rule was built on is unfounded -- the gravity
            // step below is what unsticks it.
            //
            // Retracted only for the case that was actually measured: a swing
            // leaving a FLOOR (s.flip == 0). The stale note's case was the
            // mirror (leaving a CEILING) and there is no swing-on-a-ceiling
            // take-off anywhere in the current corpus to measure, so that half
            // keeps the old behaviour -- and it is load-bearing: dropping it
            // too takes reach_check's lv22 t=3,300 swing case from SOLVED to
            // PARTIAL. Measure it (a ceiling swing + a tap, injected) before
            // touching this half.
            const bool ceilTakeoff = s.rHover && groundedNow && s.flip;
            if (ceilTakeoff) {
                vpS = -1.0;                    // no gravity step on this tick
                c.vy = (float)(qVy(vpS) * gsS);
                c.y = s.y;
                yFree = c.y;
            } else {
                // The TIME WARP scales the swing's step too. It was missing
                // here because this branch does its own integration: measured
                // on lv22 t=3,324 inside the 0.2x warp, GD steps vy by -0.017
                // and y by -0.089 where the model stepped -0.086 and -0.905
                // (5x and 10x). The terminal is a velocity, so it is NOT
                // scaled.
                vpS -= swingG(c.mini != 0) * tScale;
            }
            if (!ceilTakeoff) {
                // The cap is SYMMETRIC in the WORLD frame: |vy| <= 8, a hard
                // clamp, and y integrates with the CLAMPED value. Probed
                // directly (2026-08-17, lv22 free swing at t~4,300, up-accel
                // phase, no taps):
                //   - approach from below: 7.939 -> 8.000 exactly (7.939 +
                //     0.086 = 8.025 clamped), then dvy=0 for 27 ticks
                //   - inject vy=+11 -> next tick 8.000, y steps 1.8 (the
                //     clamped 8, not 11)
                //   - inject vy=-11 -> next tick -8.000, y steps -1.8, then
                //     the ordinary +0.086/tick decay
                // The one-sided version this replaces was generalized from a
                // dump statistic (solution_lv22r24: 49 ticks > +8.0001, up to
                // 11.636, zero below -8.0001). Those >8 ticks cannot be THIS
                // branch -- a state this integration touches snaps to 8 in one
                // tick -- so they were rings/dash states that bypass it, and
                // the 8.283 the uncapped model reached (32-record fixup family
                // in the 2026-08-17 all-green run, x=6,160 / 9,414) was the
                // cost of the missing + side.
                // [2026-08-25] ...and those bypass states now have a name: the
                // clamp is GATED on GD's velocity-limit exemption
                // (State::boost; disassembly at its declaration). All three
                // 2026-08-17 probes above were taken with the flag clear, so
                // they measured the clamp, not its gate.
                double vyW = qVy(vpS) * gsS;
                if (!c.boost) {
                    if (vyW < -kSwingTerm)      { vyW = -kSwingTerm; vpS = vyW * gsS; }
                    else if (vyW > kSwingTerm)  { vyW = kSwingTerm;  vpS = vyW * gsS; }
                }
                // FORCE BOX (id 2069): the swing rides the field too -- and the
                // push lands AFTER the terminal clamp. Measured on lv22 t=3,459
                // (free fall at the clamped -8 into the cy=105 carpet): the
                // first tick inside is -8 + 0.216 = -7.784 exactly (a
                // clamp-then-add; the single-stage clamp(vp+g+F) gives -7.870),
                // then a constant +0.130/tick = F - kSwingG. Same start-of-tick
                // position convention as every other force. The world frame is
                // the natural one here since the clamp above is already in it.
                if (!g_forceBoxes.empty()) {
                    const double fb =
                        forceBoxAcc(modX, modY, pHalf);
                    if (fb != 0.0) { vyW = qVy(vyW + fb); vpS = vyW * gsS; }
                }
                c.vy = (float)vyW;
                c.y = (float)((double)s.y + kYScale * (double)c.vy * tScale);
                yFree = c.y;
            }
        } else {
            c.grounded = 0;
            // A CARRIED grounded state (vy = the floor's velocity, see the
            // catch below) thrusts ON TOP of that velocity, not from rest:
            // GD t=21,814 shows 1.173 = 1.046 (carried) + 0.127 (one thrust
            // step). Only the resting slide (vy == 0) restarts from 0.
            // [2026-08-21 r88] The ladder during a ride is also a "carried vy"
            // (measurements at the declaration of ridingCarry).
            const double vp = (groundedNow && !ridingCarry)
                                  ? 0.0 : (double)s.vy * gsign;
            // the 4th argument mirrors the accel-switch threshold; leaving it at
            // its default picked the wrong branch for an inverted ship, which is
            // how lv8 t=21137 got +0.069/tick where GD had +0.103
            // In a DUAL the threshold is the flipped one for BOTH bodies.
            // Injected on lv16's second dual (mini UFO, t=12,418, p1 upright):
            //   vy=+0.37..-0.69: -0.152/tick (strong)   single-player: weak
            //   vy=-1.5: 3 strong steps then 3 weak = crosses -1.9165 exactly
            //   vy=-2.2 / -3.0: -0.101/tick (weak)
            // -> p1's threshold is -1.9165 while dual, i.e. the flipped side,
            // and it snapped to strong on the very tick `dual` went 1.
            // The mirrored second body (flip2=1) already gets -S from its own
            // flip, which is what the measured p1+p2=const mirror requires --
            // so the rule that fits both is "dual => flipped threshold".
            const bool thrFlip = s.dual ? true : (s.flip != 0);
            // FORCE BOX (id 2069): applies to the ship/UFO under the same
            // convention (start-of-tick position, world->player via gsign).
            // Measurements at the declaration of kFF2069 (a ship passing through
            // has a constant dvy +0.056 = 0.125 - 0.069).
            const double fbAcc = g_forceBoxes.empty()
                ? 0.0 : forceBoxAcc(modX, modY, pHalf) * gdSign;
            const double vpNew = qVy(
                (isUfo ? gdapprox::UfoModel::stepVy(
                            vp, act, ufoParamsFor(useDx, c.mini, K), thrFlip)
                       : gdapprox::ShipModel::stepVy(
                            vp, act, shipParamsFor(useDx, c.mini, K), thrFlip))
                + fbAcc);
            c.vy = (float)(vpNew * gsign);
            c.y = (float)((double)s.y + kYScale * (double)c.vy);
            yFree = c.y;
            // [2026-08-21 r86] **The flight modes also carry over "the half gravity
            // step of the tick they sat on a block".** The cube side held it in
            // pinnedOnBlock, but the flight branch had none, and on the tick it
            // launches off a ramp's top end only the model's y failed to move.
            // Measured on the calibration rig ceilrel07's 4 UFO runs: on the launch
            // tick GD first drops y by **0.019 = 0.225x0.086 (the UFO's weak
            // gravity)** and then sets vy to the launch value. The convention of
            // releasePin restoring prePinY can be used as is (same shape as the
            // cube).
            // Arming is read from **start-of-tick grounding** (s.grounded).
            // groundedNow is dropped by the support scan, so it was false on **the
            // very tick of the launch** (the tick the ramp's window closed), which
            // is exactly where it was needed most.
            if (s.grounded && !pinnedOnBlock
                && (s.flip || (double)s.y > (kGroundY + pHalf) + 0.5)) {
                pinnedOnBlock = true;
                prePinY = c.y;
            }
            // A RISING FLOOR CATCHES a ship that cannot outclimb it -- and the
            // catch imparts the floor's OWN velocity. Measured on lv19
            // t=21,806..21,820 (mini ship on the +0.2616/tick lift, taps at
            // 21,808/21,812, hold from 21,815; dump rows):
            //   - resting slide (no press yet): g=1, vy=0        (the hold)
            //   - press lands (t=21,810): vy = 1.047 = 0.2616/0.25 EXACTLY
            //     -- the floor's velocity in GD's internal dt=0.25 units, NOT
            //     the from-rest thrust (0.127, the corpus' 8-of-11 value).
            //     The 1.047/1.046 alternation is dcy's own float wobble.
            //   - press effect on a CARRIED state (t=21,814): 1.173 =
            //     1.046 + 0.127 -- thrust on top, catch keeps the LARGER vy
            //     (max toward the push direction), y still glued.
            //   - released: vy decays, falls back to floorVel on re-catch
            //     (21,816: 1.092 -> 1.046).
            //   - escape (21,818): the tick the thrust-move outruns the
            //     floor's rise, the foot ends above the face and there is no
            //     catch -- the ship leaves WITH its carried vy (1.3, 1.427,
            //     +0.127/tick from there).
            // So the catch condition is simply "after the move the foot is
            // at/below THIS tick's face", and vy := max(vy, floorVel) in the
            // gravity frame. The carried no-press ticks re-enter here through
            // the `s.vy == 0` gate on the hold above.
            if (groundedNow && flyRide) {
                const double newFoot = (double)c.y - gsign * pHalf;
                if ((newFoot - flyRideFace) * gsign <= 0.0) {
                    c.y = (float)(flyRideFace + gsign * pHalf);
                    c.grounded = 1;
                    const double floorVel = flyRideDcy / 0.25;
                    if ((double)c.vy * gsign < floorVel * gsign)
                        c.vy = (float)floorVel;
                }
            }
        }
        // The ground plane blocks an INVERTED ship too. The old comment here
        // said "an inverted one falls toward the ceiling instead", which is true
        // of falling but not of flying down: measured on lv11 t=9793, an
        // inverted mini ship descending at vy = -3.424 is stopped at y = 99.000
        // (ground 90 + mini half 9) and held there for two ticks before its own
        // gravity carries it back up. The model let it sink out of the world and
        // the whole tail of the level was solved for a player GD had killed.
        // The flying band's floor, where the section has one, replaces the
        // world's ground line entirely: on lv12 the UFO is put back at y=195
        // (floor 180) with nothing underneath, and 90 never comes into it.
        // (floorY is declared at the top of this branch)
        // `c.frame == 0` on BOTH band clamps and the carry, for the same reason as
        // fly/out-of-play below: the band is a world-Y quantity and in a turned
        // frame `c.y` is derived from world x, so the comparison is meaningless.
        // Ungated, the floor clamp SEATED a freshly rotated ship at those nonsense
        // coordinates and held it there -- measured lv22 t=16,429+: GD un-grounds
        // on the rotation tick (onGround 1 -> 0) and falls to its death at
        // t=16,437 on the wall at (20145,645), while the model kept grounded=1,
        // pinned y, and sailed on immortal. Every [SOLVED] tail the game refuted
        // at that wall was this: the DP can never prefer the robot branch while
        // the ship branch cannot die.
        if (c.frame == 0
            && !c.grounded && c.y <= floorY + pHalf && c.vy <= 0) {
            c.y = (float)(floorY + pHalf);
            c.vy = 0; CLAMP0("fly/bandfloor");
            if (!c.flip) c.grounded = 1;
        } else if (c.frame == 0 && btFlyOn && !c.flip && s.grounded
                   && c.y < floorY + pHalf) {
            // `s.grounded`: it carries a ship IT HAD ALREADY SEATED, which is
            // exactly what the measurement below covers (t=16,409 is the catch,
            // by the landing clamp above; 16,410-16,427 is the carry). Without
            // that gate the branch also fires on any flying state that happens
            // to be under the recorded floor -- and once the in-process loop
            // began passing --bandtrack (81f2a09) that became every ship in the
            // level, pinned and grounded against a floor the camera moves.
            //
            // Bisected 2026-08-25 on lv22's in-process cold run, with the
            // earlier 2900 regression held reverted at every point:
            //   8107314 (the commit before)  x=9,890, climbing
            //   81f2a09 (this branch added)  x=3,633, stuck
            // and the four commits between 0a1e8b6 and it are all good. That is
            // the second half of the 84% -> 41% drop.
            // A RISING recorded floor CARRIES the ship it seated. The landing above
            // is gated on vy <= 0, so one thrust tick after touchdown the clamp went
            // dead and the model hovered in place while the floor rose through it.
            // Measured (lv22 t=16,410-16,427, the shaft entrance, camera panning up
            // at +1.44/tick): GD keeps y = seat on every single tick while vy just
            // carries the taps' own wobble (0..0.46) -- so the seat is written and
            // vy is left alone, which is also how it differs from the object-floor
            // catch (that one stamps the face's speed into vy; the band floor
            // demonstrably does not, or vy would read 6.4). Unreachable over a
            // static floor: the landing clamp never lets y get below the seat in
            // the first place, so only a floor that moved up can put a ship here.
            // The INVERTED mirror (a falling recorded ceiling) is unmeasured and
            // left alone.
            c.y = (float)(floorY + pHalf);
            c.grounded = 1; CLAMP0("fly/bandcarry");
        }
        // GD clamps the CENTRE to ceil - 15*vsize (checkCollisions, 0x213c38).
        // The WAVE never reaches this line -- it has its own branch and its own
        // kWaveClamp (10.0, already correct); this one is ship / UFO / ball,
        // whose half IS 15*vsize, so pHalf is right for all of them.
        // ...and the band's HEIGHT is 270 / camScale, not a per-mode constant
        // (see ZoomTrig: the product is 270.000 exactly in every mode GD was
        // sampled in). The stored bandCeil is the camScale == 1 value, so it is
        // only rewritten where the level actually zooms -- lv1-21 own no id 1913
        // and keep the old number bit-for-bit.
        double bandCeilNow = btFlyOn ? btFlyC : (double)s.bandCeil;
        if (!btFlyOn && !g_zoomTrigs.empty() && bandCeilNow < 1e8) {
            const double cs = camScaleAt(x, useDx);
            if (cs > 0.0) {
                // The band derived from the zoom sometimes disagrees with GD's
                // measurement. Near x=18,000 on lv22 GD's band is [405.7, 905.7]
                // = **500**, yet the model gave 255 and the ship topped out at
                // y=660.67. The step's top is 674, so this section **had become
                // impassable in principle inside the model** (cannot be cleared
                // even holding). The coefficient is calibrated **once only** from
                // the height of the band GD was asked for at the anchor
                // (--startband). The zoom's relative changes still apply as is.
                if (g_bandK <= 0.0 && g_startBandCeil < 1e8
                    && g_startBandCeil > g_startBandFloor)
                    g_bandK = (g_startBandCeil - g_startBandFloor) * cs
                              / kBandBase;
                const double k = (g_bandK > 0.0) ? g_bandK : 1.0;
                bandCeilNow = (double)s.bandFloor + kBandBase * k / cs;
            }
        }
        const double yMax = bandCeilNow - pHalf;
        if (g_bandDbg && c.mode == 4 && (double)c.y > yMax - 6.0)
            std::printf("banddbg t=%lld x=%.1f y=%.2f band=[%.0f,%.0f] yMax=%.1f\n",
                        (long long)K.t, x, (double)c.y, (double)s.bandFloor,
                        (double)s.bandCeil, yMax);
        // ...and the SWING is not clamped by it at all, the same way the wave
        // has its own branch. Measured on lv22 t=4,497: GD's band is
        // [90, 428.444] so `ceil - 15` is 413.44, and GD's swing sits at
        // 416.769 and keeps climbing with no clamp of any kind. With the clamp
        // on, the model pinned it and lost 5 px within four ticks.
        if (c.frame == 0 && c.mode != 7 && (double)c.y > yMax) {
            c.y = (float)yMax; c.vy = 0; CLAMP0("fly/bandceil");
        }
        // In a rotated frame `c.y` is the frame's vertical (= derived from world x),
        // while floorY and g_yBound are world-y quantities, so this comparison is
        // meaningless. lv22 t=14,307 (frame 3, ship, world x=15,465) was dying
        // instantly on this: GD flies on to 14,632, yet only the model dropped 325
        // ticks short. "Left the level" on the rotated side is handled by
        // out-of-play (g_yBoundTurned). Same family as escapee-prune -- a predicate
        // becomes a lie the moment it crosses coordinate systems.
        if (c.frame == 0
            && (c.y < (floorY + pHalf) - 40 || c.y > g_yBound))
            DIE("fly/out-of-play", nullptr);
        for (const Obj* o : *K.near) {
            if (dead) break;
            if (o->type != 0) {
                for (int si = 0; si <= kSubSteps && !dead; ++si) {
                    const double f = si / (double)kSubSteps;
                    const double sx = xPrev + (x - xPrev) * f;
                    const double sy = (double)s.y + ((double)c.y - (double)s.y) * f;
                    if (hazardHit(o, sx, sy, pHalf, kHazMargin,
                                  sawMarginFor(s.mode), s.mode, c.mini)
                        && (!g_obbAll || !o->obbOk
                            || obbOverlap(*o, sx, sy, pHalf, (double)s.rot)))
                        DIE("fly/hazard", o);
                }
                continue;
            }
            // face = the surface the ship rests ON, head = the one it bumps
            // into, both in the PLAYER frame so a flipped ship swaps them.
            // FIRST ATTEMPT, REVERTED: "lands while its centre is still above
            // the surface" (dropping the tolerance entirely), inferred from a
            // single landing at penetration 3.019 and a single death at 38.293.
            // It made things worse -- lv5 went from cleared to stuck at
            // x=16,166 -- because the model then landed where GD does not.
            // One data point does not pin a rule; the sweep behind
            // kShipLandTol does (see its comment).
            const double face = c.flip ? (o->cy - o->hh) : (o->cy + o->hh);
            const double head = c.flip ? (o->cy + o->hh) : (o->cy - o->hh);
            const double vpNow = (double)c.vy * gsign;
            const double prevFootP = ((double)s.y - gsign * pHalf - face) * gsign;
            const double newFootP = ((double)c.y - gsign * pHalf - face) * gsign;
            const double prevHeadP = ((double)s.y + gsign * pHalf - head) * gsign;
            const double newHeadP = ((double)c.y + gsign * pHalf - head) * gsign;
            const bool xOver = std::fabs(x - o->cx) <= o->hw + pHalf;
            // GD also resolves an OVERLAP the ship did not fly into, but only
            // once it is deep enough horizontally. Read straight off the hooked
            // collidedWithObject return (cfg `hitboxtrace=1`), which reports
            // false for every tick the rects overlap shallowly and true on the
            // tick it snaps. Both 0->1 transitions in the lv11 replay:
            //   mini   (half 9)  last false at xpen 14.670, true at 15.970
            //   normal (half 15) last false at xpen 25.930, true at 27.230
            // so the gate is xpen > 1.75 * half: 15.75 and 26.25 both land
            // inside their bracket. The ratio xpen/ypen does NOT work (the two
            // brackets are 3.01..3.30 and 7.49..8.00, which do not intersect).
            // A first attempt without this depth gate broke lv10 -- the ship got
            // pinned to ceilings it was merely passing under.
            const double xPen = (o->hw + pHalf) - std::fabs(x - o->cx);
            const bool deepIn = xPen > kSolidResolveX * pHalf;
            // does the SURFACE climb away from the head faster than the head
            // climbs into it? (see the ceiling-ride branch below)
            const bool overtaking = ((double)o->dcy * gsign) > vpNow * kYScale;
            // [2026-08-19] **While riding a ramp, landing is strict.** The flight
            // version of the cube side's `landTol = c.onSlope ? 0.001 : kLandTol`
            // (the "While riding a slope the ordinary landing test must be strict"
            // above). The flight branch lacked this gate, and the 6.0 reach-back
            // let a neighbouring plate steal the ramp ride.
            // Measured lv18 t=3,008 (mini ship, hang-riding ceiling ramp uid1807):
            // against the neighbouring solid uid1814 (box x[4050,4065] y[420,435])
            // at prevFootP -5.826 (just inside the -6.0 tolerance), only the model
            // landed on the underside 420 (y 411, vy 0). GD's hitboxtrace hbox line
            // says **hit=0** -- it does not grab even at 4.2px penetration (xPen is
            // 1.06). That false landing dropped the ride for 1 tick, uphillExit's
            // -2.762 came out on the next tick, and it became the census family
            // clamp:fly/land/uid1814. The flight branch runs before the slope loop
            // (below), so the ride test reads the previous tick's `s.onSlope`.
            // **Rejected: "deepIn for landings that did not cross"** (same day): as
            // a general rule it regressed lv13 19,383->18,907 and lv14
            // 20,000->19,597, families 29->30 (+4 in the air group). GD does grab
            // a shallow reach-back when not riding.
            // [2026-08-21 r53] **Penetrating a moved actual object pushes out to the
            // face.** The landing gate (below) only looks at "came down" or "the
            // face overtook", so a player **already penetrating** while rising
            // passes straight through. GD returns the foot to the face even while
            // rising, as long as the centre is above the face (no grounding, vy
            // untouched). Measured lv20 t=21,452 (UFO, sp1.1, moving solid uid18338
            // (30x30, rising +0.15 every tick, top 193.425)) -- the MOD's `hbox:`
            // line names the resolved amount directly:
            //   ppre=(32070.73,**191.03**,30,30) -> player=(32070.73,**193.43**,30,30)
            //   = the foot pushed up 2.40px (the x overlap is only 0.73px)
            //   GD stays onGround=0 and leaves vy at 6.871
            // The model passed through, left census `m3/mini0/g0/gdg0/sp1.1/air`
            // edy +2.399, and from then on ran a parallel -2.399 to the end.
            // Limited to **moved actual objects only** (o->dcy != 0): penetration
            // of static objects is measured by the existing landing / head bonk /
            // side kill, and those are left alone.
            // All three gates come from measurement: **the face is rising toward
            // the player** (dcy*gsign > 0), **already penetrating at the start of
            // the tick** (prevFootP < 0 = a resolution, not a new landing), **the
            // centre is above the face**. Loosening these (dcy != 0 alone, or not
            // looking at prevFootP) raises families 19->22 (lv19 regresses 3
            // sections, lv21 1 section; new are ship 2 + UFO 2).
            if (o->dcy * gsign > 0.0 && !c.grounded && xOver
                && prevFootP < 0.0
                && ((double)c.y - face) * gsign > 0.0 && newFootP < 0.0) {
                if (g_slopeDbg)
                    std::printf("mpush t=%lld uid=%d ocy=%.3f ohh=%.3f face=%.3f "
                                "dcy=%.4f pHalf=%.3f y %.3f->%.3f\n",
                                (long long)K.t, o->uid, o->cy, o->hh, face, o->dcy,
                                pHalf, (double)c.y, face + gsign * pHalf);
                c.y = (float)(face + gsign * pHalf);
                // [2026-08-21 r59] **The push-out also carries the face's velocity**
                // (same convention as the ceiling pin's `dcy/0.25`, but max).
                // Measured lv20 t=21,718..21,722 (UFO, moving solid uid18453 rising
                // ~1.0 px every tick):
                //   t      GD vy   model vy   that tick's dcy   dcy/0.25
                //   21719  3.945    2.356        0.9862           3.9448
                //   21720  3.985    2.227        0.9963           3.9852
                //   21721  4.026    2.098        1.0064           4.0256
                //   21722  4.066    1.969        1.0165           4.0660
                // y matches exactly on all 4 ticks (both stuck to the top face), and
                // **only vy** becomes the face's velocity itself in GD.
                // **max, not assignment**: at t=21,453 in the same branch (uid18338,
                // dcy 0.151 -> 0.604) GD leaves vy 6.742 alone. A fast player keeps
                // its own velocity even when pushed out.
                const double surfVp = (double)o->dcy * gsign / 0.25;
                if (surfVp > (double)c.vy * gsign) {
                    c.vy = (float)(surfVp * gsign);
                    CLAMP0O("fly/mpush", o);
                }
                continue;
            }
            const double shipLandTol = s.onSlope ? 0.001 : kShipLandTol;
            if (!c.grounded && (vpNow <= 0 || overtaking) && xOver
                && prevFootP >= -shipLandTol && newFootP <= 0) {
                // Used by the flight version of item 14 (seatFromPreLand). lv20
                // t=21,983: on the UFO's landing tick, onto the ramp seat 222.90
                // rather than the plate 222.35.
                if (!landedThisTick) preLandY = c.y;
                landedThisTick = true;
                c.y = (float)(face + gsign * pHalf);
                if (vpNow <= 0) {
                    c.vy = 0; CLAMP0O("fly/land", o);
                } else {
                    // A rising floor RE-CATCHING a climbing ship (overtaking:
                    // the face gains on the foot) is not a landing -- it
                    // imparts the floor's own velocity instead of zeroing.
                    // Measured on lv19 t=21,816: the tap's vy decays 1.173 ->
                    // 1.092 -> and the catch puts it back to 1.046
                    // (= dcy/0.25), y glued to the lift again. See the
                    // carried-state catch in the flight integration above.
                    const double fv = (double)o->dcy / 0.25;
                    if ((double)c.vy * gsign < fv * gsign)
                        c.vy = (float)fv;
                }
                c.grounded = 1;
            } else if (o->oneway && !c.flip) {
                // a one-way platform is transparent to everything but a landing
                // -- **but only from its WORLD underside.** The transparency
                // direction is decided by the world, not by the player's gravity. A
                // flipped ship's "head" points up in the world, so it enters the
                // plate's **top face** from above, and GD pushes it out here as
                // usual.
                //
                // Measured lv18 t=8,203 (worker98, hbox line with hitboxtrace=1):
                //   ppre=(10980.49,176.40,30,30) -> player=(...,180.00,...)
                //   obj=5425 type=21 rect=(11010,150,30,30) hit=1
                // On the tick the bottom of a flipped ship's box (usd1, pulling away
                // at vy -1.562) breaks 3.6025 into the top face 180 of plate
                // uid5425, GD **pushes y up** to 195 (=180+pH) and in the same pass
                // lands it on the underside 210 (uid5430, type 0) with vy=0 / og=1.
                // The model passed straight through on this `oneway` continue,
                // ramp-side branch and all, and the census family
                // m1/mini0/g0/gdg1/sp0.9 edy +3.6025 stayed as it was (the true
                // identity of the old "face magnet" hypothesis).
                //
                // Disassembly backing: PlayerObject::collidedWithObjectInternal
                // (rva 0x391a70) starts the tolerance at 10.0 and swaps in **6.0**
                // if any of the 4 flight modes (m_isShip/m_isBird/m_isDart/m_isSwing
                // = +0x9b9/9ba/9bc/9c4) is set. These are the existing measured
                // values kLandTol=10.0 / kShipLandTol=6.0 themselves, and this
                // branch's prevHeadP=3.251 is inside 6.0 -- the gate was never the
                // tolerance, only the transparency direction.
                continue;
            // A RISING PLATFORM THAT OVERTAKES THE SHIP IS NOT A CEILING.
            // The ride below asks whether the player's head crossed the
            // underside, which is only a contact if the two are CLOSING. When
            // the surface itself climbs faster than the player, GD lets the
            // object pass through and the player carries on.
            // Measured on lv20 (2026-08-09, the `clamp:fly/ceilride/uid4627`
            // class the fixup log kept re-recording): uid4627 is a 30x30 solid
            // on a group that rises ~2.05 px/tick; at t=4607 the recording puts
            // it at cy=289.167 and the model's clamp says it used exactly
            // 289.167, so the GEOMETRY was right. The mini ship was climbing at
            // 7.281 * 0.225 = 1.64 px/tick -- slower than the platform -- and
            // GD simply let the block swallow it (GD's y is 6.20 px above the
            // model's pinned one and keeps rising). The model pinned the ship
            // to the underside for four ticks running.
            // Static objects have dcy = 0, so nothing that already works moves.
            } else if (!overtaking
                       && (vpNow >= 0 || (prevHeadP > 0 && deepIn)) && xOver
                       && prevHeadP <= kShipLandTol && newHeadP >= 0) {
                // Ceiling ride. The ship stops dead against a block's underside
                // and slides along it -- it does NOT die and it does NOT pass
                // through. The model had no ceiling case at all (the header's
                // "no floor/ceiling rides" under-approximation), so the ship
                // flew straight through solids from below. Measured on lv6
                // t=11091: block x[14430,14460] y[330,360], GD pinned the ship
                // at y = 330 - 15 = 315.00 with vy = 0 and onGround = 0, while
                // the model sailed on to 315.06 and kept climbing.
                c.y = (float)(head - gsign * pHalf);
                // [2026-08-19 item 13] **A moving ceiling's pin carries the face's
                // velocity** (dcy/0.25, same convention as the floor catch). The
                // identity of "only vy grows while held, y matches exactly" at lv19
                // t=21,350 uid14543 (+0.576) and lv15 uid3211 (+0.432). A static
                // ceiling (dcy=0) stays 0 as before -- trying "free vy integration
                // while pinned" made 8 static-ceiling sites at sp0.9 spring up at
                // once (GD does zero it on a static one).
                if (c.mode == 1 && o->dcy != 0.0)
                    c.vy = (float)((double)o->dcy / 0.25);
                else
                    c.vy = 0;
                CLAMP0O("fly/ceilride", o);
            // SWING PUSH-OUT. A swing that merely CLIPS a solid is not killed:
            // GD shoves it out along y to the nearer face, keeps vy and sets
            // onGround. Measured on lv22 t=3,669 with the pre-resolution rect
            // (the `ppre=` field, added to the MOD for exactly this):
            //   block  x 5,160.00..5,190.00  y 326.31..356.31
            //   player x 5,131.18..5,161.18  y 302.38..332.38   (PRE)
            //   player x 5,131.18..5,161.18  y 296.31..326.31   (POST)
            // so x overlaps by 1.18 and y by 6.07, and GD resolves the DEEPER
            // axis -- "least penetration" is NOT the rule here. What decides it
            // is the same `deepIn` bracket the ship's snap uses: 1.18 is far
            // under 1.75*pHalf = 26.25, so this is not a side hit, and a
            // non-side hit is resolved vertically. vy is untouched (GD's -1.171
            // carries straight through) and the face picked is the NEARER one
            // (6.07 px down against 53.9 px up).
            // Gated to mode 7 because only lv22 has a swing portal, so nothing
            // already green can move.
            // ...and only on the FIRST tick of the x contact. The measured case
            // is a fresh clip: at t=3,669 the previous x was 30.44 px from the
            // centre (outside hw + pHalf = 30) and this one is 28.82 (inside).
            // Without that, the rule fired on every tick of a long-standing
            // overlap and teleported the player 9 px at lv22 t=4,580, where GD
            // does nothing at all.
            // [2026-08-19 rejected] **Extend to the UFO**. Measured lv20 t=21,453
            // (UFO, rising at vy=+6.742) is indeed this shape: on the tick the
            // box's lower-right corner catches the upper-right corner of moving
            // block uid18338 (box x[32100,32130] y[163.43,193.43]) at xPen 0.73 /
            // yPen 2.40, GD pushes y up to **193.43+pH=208.425** (hbox ppre 191.03
            // -> player 193.43, return value hit=0). The census family
            // m3/mini0/g0/gdg0/sp1.1/air edy +2.399 is exactly this.
            // But no gate can be found:
            //   * adding the UFO plainly -> families 27->**39**, lv12/13/14/19/20
            //     regress. The additions are all over-push-outs
            //     `clamp:fly/swingpush/...`
            //   * `o->dcy != 0.0` (moving blocks only) -> families stay at 27, lv20
            //     gains +217 but **lv19 goes 21,942->21,612** (the t=19,800 section
            //     400->70), red
            // There is a residual too: the push-out target is 0.744 higher than
            // GD's = **the moving block's y itself is off** (uid18338, separate
            // issue).
            } else if (c.mode == 7 && !deepIn && xOver
                       && std::fabs(xPrev - o->cx) > o->hw + pHalf
                       && std::fabs((double)c.y - o->cy) < o->hh + pHalf) {
                const double above = o->cy + o->hh + pHalf;
                const double below = o->cy - o->hh - pHalf;
                const double yNow  = (double)c.y;
                const double tgt = (std::fabs(above - yNow) <= std::fabs(below - yNow))
                                   ? above : below;
                // ...and only when the push goes the way the player is ALREADY
                // moving, i.e. GD is helping it out of a face it is leaving.
                // Measured both ways on lv22:
                //   t=3,669 near face is BELOW (6.09 px) and vy = -1.257 -> GD
                //           pushes, y 302.38 -> 296.31
                //   t=4,580 near face is ABOVE (9.45 px) and vy = -0.157, i.e.
                //           the player is heading INTO it -> GD does not move it
                //           at all (`ppre` equals the post rect for four ticks
                //           running); what actually happens there is a SLOPE
                //           ride plus the swing's own tap flips, and the push
                //           was stealing it.
                if ((tgt - yNow) * (double)c.vy < 0.0) {
                    for (int si = 0; si <= kSubSteps && !dead; ++si) {
                        const double f = si / (double)kSubSteps;
                        const double sx = xPrev + (x - xPrev) * f;
                        const double sy = (double)s.y + ((double)c.y - (double)s.y) * f;
                        if (std::fabs(sy - o->cy) < o->hh + pInner + kHazMargin
                            && std::fabs(sx - o->cx) < o->hw + pInner + kHazMargin)
                            DIE("fly/solid-side", o);
                    }
                    continue;
                }
                c.y = (float)tgt;
                // NOT grounded. GD's dump does report onGround=1 for the ticks
                // that follow, but it keeps integrating freely (t=3,671 onward
                // is a plain swing step: vy -1.171 -> -1.085 -> -0.999 and y
                // follows it), so the flag is cosmetic here. Setting it made
                // the model's own grounded short-circuit zero vy and the push
                // then re-fired every tick, pinning the player to the face.
                CLAMP0O("fly/swingpush", o);
            } else if (!o->oneway) {
                // **A one-way plate never kills** (the note at the `oneway` branch
                // above). When the flipped-side transparency was removed (made
                // world-based), things began falling through to this last else, and
                // a flipped ship slipping past the side of a plate started dying at
                // `fly/solid-side`. **Add the push-out but not the kill** is the
                // correct shape.
                // Measured, lv18 cold run (hp18, r13): 13 iterations CLEARED (r12)
                // worsened to ITER-CAP(39) / 75 minutes, bouncing between x=11,006
                // and x=4,318. The death at x=11,006 is a different route ("climb
                // the y=345 corridor and run into wall uid5427"); the cause was the
                // low route (through the row of plates to the wedge at 195) being
                // blocked by this kill.
                for (int si = 0; si <= kSubSteps && !dead; ++si) {
                    const double f = si / (double)kSubSteps;
                    const double sx = xPrev + (x - xPrev) * f;
                    const double sy = (double)s.y + ((double)c.y - (double)s.y) * f;
                    if (std::fabs(sy - o->cy) < o->hh + pInner + kHazMargin
                        && std::fabs(sx - o->cx) < o->hw + pInner + kHazMargin)
                        DIE("fly/solid-side", o);
                }
            }
        }
    }
    // Slopes (see slopeXOffset / slopeExitVy), for EVERY mode: measured on
    // lv18 t=2761, a mini SHIP rides a 45 degree slope with vy reported as 0
    // and y climbing exactly dx per tick, the same way the cube does. Only the
    // upright case is modelled; GD's downhill and ceiling slopes are left alone
    // until a level needs them.
    //
    // This runs after the per-mode collision code on purpose. lv19's first
    // slope has a 1.5 px slab (id 468) sitting at its top, and while the cube
    // is still climbing its foot is 5.5 px below that slab -- inside kLandTol,
    // so the ordinary landing test used to teleport it up onto the slab at
    // t=245 where GD stayed on the slope until t=253 (the landing test is
    // tightened to a strict crossing while c.onSlope, see landTol there).
    {
        // [2026-08-19] The spider's slope half-width is 13.5 (kSpiderHalf). lv21
        // t=16,710: GD's seat 155.066 = line(140) + 13.5*sqrt(1.25) exactly (with
        // 15 it is 1.68 too high). The corpus has no other spider ride family, so
        // only this moves (the regression is the judge).
        const double pH = (c.mode == 6)
            ? (c.mini ? kSpiderHalfMini : kSpiderHalf)
            : (c.mini ? kMiniHalf : kCubeHalf);
        c.onSlope = 0;
        // Is the player still inside SOME ramp's window this tick? That is what
        // tells "jumped off the middle of a ramp" apart from "ran off its top",
        // and GD gives those two different velocities (see the launch below).
        bool rampWindowHere = false;
        // On the tick a mode portal ends a swing's ride, GD leaves with this free y
        // without taking the seat's step (measurements at the rule's site).
        yFreeBeforeSlope = c.y;
        if (K.slopes) {
            // --slopedbg: what the layer actually handed this tick. `slopewin`
            // can only report ramps that ARE in the set, so a ramp missing from
            // the set leaves no trace at all -- lv22 t=4,630 looked like a
            // window rejection when uid4359 was simply not there.
            if (g_slopeDbg) {
                std::printf("slopeset t=%lld x=%.2f n=%zu uids=", (long long)K.t,
                            x, K.slopes->size());
                for (const Obj* sp : *K.slopes) std::printf("%d ", sp->uid);
                std::printf("\n");
            }
            // Two passes over the slopes, and the ORDER is the whole point.
            // The ride is resolved first: on the tick a falling player reaches
            // a ramp its foot is genuinely below the surface, so an inside-test
            // run before the ride reads a legitimate landing as a kill.
            // Measured on lv16 t=4678 -- the ball comes down at vy=-10.13, ends
            // 2.59 px under the ramp's rest height, and GD puts it ON the ramp
            // at y = 300.3406 and rides it for the next 66 ticks. The model
            // killed it there instead, and that single tick was the lv16 wall:
            // the frontier died at x=6,687 while GD sails past on the same
            // input (one extra tap anywhere in a 55-tick window clears the saw
            // beyond it, so the search was never short of moves).
            // After the ride c.y is the resting height, whose foot sits on the
            // surface at the CONTACT point -- which is at or above the surface
            // at the centre on both an uphill and a downhill ramp -- so a rider
            // can never trip the inside-test in the second pass.
            // A ball that taps OFF a ramp still takes this tick's ride
            // position: GD moves it to the ramp's resting height and only lets
            // the flip own the velocity. Measured on lv16 t=4379 -- riding a
            // 0.5 ramp at +0.8071 px/tick, the tap tick goes 360.4255 ->
            // 361.000 (the ramp's own top + 15, clamped at its right edge)
            // while vy is already the flipped 3.7056. Freezing y the way a cube
            // jump does left the model 0.57 px low for the whole flight, and
            // that was the first divergence of the lv16 run.
            // ...and the same holds for a cube's jump, which is the other
            // discrete impulse off a ramp. Only the ball case is measured.
            const bool impulsedOffSlope = impulsedThisTick && s.onSlope;
            const bool tappedOffSlope = ballFlippedThisTick && s.onSlope;
            // Already riding? Then the player STAYS on the surface, even when a
            // free fall would leave it above one. A DOWNHILL ramp drops 0.807
            // px/tick (m=0.5 at speed 1.1) while gravity only moves the player
            // 0.049 px in its first tick, so the acquisition test -- "the foot
            // has reached the surface" -- can never fire again once the surface
            // is running away downwards, and the model simply fell off the top
            // of every downhill ramp. Measured on lv16 t=6296..6316: GD keeps
            // onGround=1 and y dropping at exactly 0.8071/tick for the whole
            // ramp, and this is what killed the frontier at x=9,372.
            // (flipped riders stick too -- they could not ride at all before,
            // so allowing them here cannot change any upright behaviour)
            const bool stickToSlope =
                s.onSlope && !impulsedThisTick && !stepLandedThisTick;
            // At the seam of an equal-gradient chain both the old and the new ramp
            // pass acquisition and it becomes "last one wins". The loop runs in cx
            // order, so **going forward the new one (in-span) comes last and is
            // smooth; in reverse the old one (the end-clamped flat) comes last**,
            // and a 1-tick flat enters at the seam (lv22 t=18,526; GD keeps
            // climbing smoothly at 1840.48). Direction-independent rule: a tick
            // acquired from an in-span sample is not overwritten by an acquisition
            // from an end-clamped sample. The end of the chain (a tick with no
            // in-span candidate) still takes the clamp as before -- that is what
            // provides the "last 1 tick" lingering before the slope-exit launch.
            // [2026-08-18]
            bool slopeTookInside = false;
            // [r33] The "highest face under the box" (ridge) on a tick grabbed by
            // push-out. So as not to depend on loop order: whichever ramp made this
            // seat, later `top` values never fall below it.
            bool pushOutSeat = false;
            double pushOutTop = -1e18;
            for (const Obj* sp : *K.slopes) {
                // the wave does not ride anything -- it only ever dies
                if (c.mode == 4) break;
                // A SPIKED ramp is not a surface. m_slopeIsHazard (the `shz`
                // column) was being loaded and then never read anywhere, so the
                // model happily rode the spikes: lv18 lines its ramp tunnel
                // with them (a hazard ramp 4 px under every solid one -- e.g.
                // (5115,405) carries a solid 420->390 and a hazard 416->386),
                // and the search flew a UFO down the tunnel that GD kills at
                // x=5,119. 154 of lv18's 759 ramps are spiked, 192 of lv17's,
                // 139 of lv19's, 88 of lv16's; lv20-22 have none.
                if (sp->slopeHazard) continue;
                // Riding: an upright player rides the line from above, a
                // FLIPPED one hangs under it. The flipped case used to be
                // skipped outright, and that was lv16's wall: at t=4111 an
                // upside-down ship falls UPWARD into the V-shaped ceiling made
                // by the ramps at (5730,435) 450->420 and (5790,435) 420->450,
                // and GD stops it dead at y=405.000 = 420 - 15. The model flew
                // straight through.
                const double x0 = sp->cx - sp->hw, x1 = sp->cx + sp->hw;
                const double m = (sp->sy1 - sp->sy0) / (x1 - x0);
                if (m == 0.0) continue;
                // ...but "flipped" does NOT mean "ceiling". A FLOOR ramp blocks
                // an inverted player from below exactly as the ground plane does
                // ("GD's ground plane blocks the player from BELOW whatever its
                // gravity is", see the kGroundY clamp) -- and the comment at the
                // ceiling branch already says it: slopeDir says which side of the
                // ramp is SOLID, not which face a player may land on.
                //
                // Measured 2026-08-09, lv20 t=14005..14013, an INVERTED
                // (upsideDown=1) full-size ship at speed 1.1. GD names the ramp
                // itself (`standtrace=1`):
                //   stand: t=14005 y=204.529 onG=0
                //          slope=[uid11804 id652 type25 @20722,181 60x30]
                // sy0=166 -> sy1=196 over 60 px, so m = 0.5 exactly, and GD holds
                //   y - line(x) = 16.7705  at every tick of the ride
                // which is this file's own upright resting height, unchanged:
                //   m*slopeXOffset(m,15) + 15 = 0.5*3.5410 + 15 = 16.7705
                // (= 15/cos(atan 0.5) -- the box rotated onto the surface with
                // its centre one pHalf off it). NO new constant.
                //
                // The model routed flip=1 to the hang-under branch, so its y kept
                // integrating 0.225*vy while GD slid it along the line: dy 0.4653
                // -> 0.5739 against GD's flat 0.8066 (= 0.5*dx). That family was
                // 16 of the 80 fixups in lv20's cold run (11 as a delta, 5 as the
                // kill the accumulated offset then missed) -- the biggest single
                // cause at the x=28,867 wall.
                //
                // STRICTLY ADDITIVE: upright is untouched (ridesTop is true, as
                // before) and flipped-under-a-ceiling-ramp is untouched. The only
                // new case is flipped ABOVE a floor ramp, which previously took
                // the hang-under geometry, failed the came-from-above gate by
                // ~32 px and `continue`d -- i.e. no ride at all.
                // ...and the SWING gets the full set too. Measured on lv22
                // uid4359 (slopeDir = 5, so `slopeIsCeiling` says ceiling but
                // the {1,3} subset does not): at t=4,633 the player's gravity
                // flips and the model, thinking this was a floor ramp, moved it
                // from hanging under (419.7) to standing on top (460.5) -- a
                // 40.8 px teleport. GD stays under at 418.077 = the line minus
                // pH*sqrt(1+m^2), i.e. **the side is the ramp's, not the
                // player's**, which is what slopeDir has always meant.
                // [2026-08-21 r63] **Remove the mode whitelist.** It grew one mode at
                // a time, ship -> swing -> cube, but the reason was the same every
                // time, "a slopeDir=5 ramp looks like a floor under the {1,3}
                // subset", and has nothing to do with the player's mode. The spider
                // showed the same picture: at lv22 t=8,250 (uid9296, sdir=5, m=-2,
                // line 570->510) only the model **jumped +85.8px** to the top face
                // 523.500 (GD passes straight through at 437.670). Same shape as
                // uid4359's 40.8px.
                const bool ceilRampSide = slopeIsCeiling(sp->slopeDir);
                // SHIP ONLY. The first attempt let every mode take this branch
                // and lv16 went from CLEARED in 32 iterations to STUCK at
                // x=19,711 in 60 (2026-08-09, same pool, same everything else).
                // The reason is `ceilRampSide`: only the ship is converted to
                // GD's full {1,3,5,6} set, so for cube/ball/UFO/wave a large
                // share of genuine CEILING ramps still read as "floor" here, and
                // the new branch then offered them a top ride they must not have.
                // The measurement is a ship's; keep the rule where the side test
                // is known to be right.
                // **The contact side is decided by the gravity from before the
                // press** (GD's tick order is update -> collisions -> buttons). The
                // ball's tap IS the gravity flip, so reading c.flip on the tap tick
                // reinterprets the ramp it is still riding as "ceiling side" and
                // discards the candidate outright (--slopedbg: lv16 t=4,730 shows
                // ridesTop=0 / gs=-1 / reject=1). That loses the last one step GD
                // carries it (m*|dx| = 0.807px). This is the identity of this
                // family.
                // It only applies on the tappedOffSlope tick = "tapped" and "was on
                // the ramp at the start of the tick".
                const bool flipForRide =
                    tappedOffSlope ? (s.flip != 0) : (c.flip != 0);
                bool ridesTop = !flipForRide;
                // ...and the SWING needs it as much as the ship. It toggles its
                // gravity every couple of ticks, so `!c.flip` made it lose and
                // re-find the same ramp on alternating ticks. Measured on lv22
                // uid5430 (m=2) with --slopedbg's candidate lines: t=4,572 and
                // 4,573 have ridesTop=1 and a sensible top of 368.021, while
                // t=4,570/4,571/4,574/4,575 have ridesTop=0, top=315.000 and
                // reject=1 -- the flip is the only thing that changed. GD rides
                // it right through, because slopeDir says which side is SOLID,
                // not which way the player's gravity points.
                // [2026-08-21 r60] **Remove the mode whitelist.** As the note above
                // says, "slopeDir says which side is SOLID, not which way the
                // player's gravity points", so this is not a property of
                // ship/swing. Measured lv22 t=8,232 (spider, flip=1, floor ramp
                // uid17788 m=+1): GD's `slp:` line has onSlope=1 and lifts y from
                // the free-fall 413.728 to the seat 414.592, then carries it at the
                // face's own 1.614/tick until 8,239. The model, with mode 6 not on
                // the whitelist, got ridesTop=0 / gs=-1 / reject=1 (--slopedbg's
                // slopecand lines print gs=1 at 8,231 -> gs=-1 at 8,232 just so),
                // free-fell for 7 ticks, and left census
                // `m6/mini0/g0/gdg0/sp1.1/air` edy +0.864. The whitelist had been
                // growing one mode per family (ship -> swing), so it is folded
                // here.
                if (flipForRide && !ceilRampSide) {
                    // came from ON or ABOVE the upright resting height?
                    const double ru = xPrev + (m > 0 ? slopeXOffset(m, pH)
                                                     : -slopeXOffset(m, pH));
                    const double topPrevUp =
                        sp->sy0 + m * (std::min(std::max(ru, x0), x1) - x0) + pH;
                    ridesTop = ((double)s.y - topPrevUp) >= -kLandTol;
                }
                const double gs = ridesTop ? 1.0 : -1.0;
                // Downhill ramps hold the player up exactly the same way; the
                // contact point is just on the other side of the box, because
                // GD tilts the player the other way. lv18 x=3,615 is a 210->180
                // ramp immediately followed by a 180->210 one, and skipping the
                // first is what stopped the search at x=3,716.
                // Where the surface is sampled. Upright: the rotated box's
                // lowest corner (measured, see slopeXOffset). Flipped: a
                // CEILING binds at the LOWEST point of the surface over the
                // player's width, so take the better of the two box edges
                // clamped into the ramp. Measured on lv16 t=4111 -- the ship's
                // box spans x[5759.3,5789.3] and GD pins it at 420 - 15, which
                // is the surface at 5760 (its left edge, the ramp's own corner)
                // and not at the centre (427.15) nor at the rotated contact
                // point (428.9).
                // `ok` is "this ramp is in play for the ride" (1.5 px of slack
                // past the right edge keeps the last tick on the surface);
                // `inside` is the strict version, for telling "still on it"
                // apart from "just ran off the top" (see rampWindowHere).
                struct Smp { double xr; bool ok; bool inside; };
                auto sampleAt = [&](double cx) {
                    // ridesTop, not !c.flip: an inverted player standing on a
                    // FLOOR ramp contacts it with the same rotated corner an
                    // upright one does (see the note at ridesTop).
                    if (ridesTop) {
                        const double r = cx + (m > 0 ? slopeXOffset(m, pH)
                                                     : -slopeXOffset(m, pH));
                        // [2026-08-19] A continued ride that is **downhill** in the
                        // travel direction keeps descending the extrapolated line
                        // past the low end (GD slopeYPos extrapolates outside the
                        // box -- the implementation difference from findings
                        // 2026-08-01 surfaces here). og drops where the centre has
                        // descended to the low end's line value sy1 = end + off/|m|
                        // (slopeland rig measured k3/k6: x-x1 = 33.97/34.77 vs
                        // formula 33.54; k12/k15: 21.34/22.31 vs 21.21). The
                        // identity of lv16 t=6,316 (running flat at 165.000 at
                        // uid2580's end while GD descends to 164.900).
                        // Only the upright cube's continuation was measured, so it
                        // is limited to that.
                        {
                            const bool revE =
                                ((double)K.dxF < 0.0 || s.rev != 0);
                            const double mTravS = revE ? -m : m;
                            if (mTravS < -0.01 && stickToSlope && s.onSlope
                                && !c.flip && c.mode == 0) {
                                double extC =
                                    pH * std::sqrt(1.0 + m * m)
                                    / std::fabs(m);
                                // [2026-08-21 r79] The extrapolated descent (extC)
                                // is the measurement for **air past the end**
                                // (slopeland). When a **flush flat** (an actual top
                                // face within +-0.5 of the end's line) continues
                                // past the low end, GD's slope state lasts only
                                // until "the tick the rotated box's rear corner
                                // (centre - (pH-xoff)) exits the end", and the
                                // release pulse (mTravel*dx/0.25) comes out there.
                                // Measured lv16 t=6,972 (cube m=-0.5, x1=10,380,
                                // 1.5px slab top 449.95): the pulse is on the tick
                                // cx crosses x1+11.459 (=pH-xoff). Keeping extC
                                // (33.54) is 1 tick late (census nearceil edvy
                                // -3.229).
                                if (K.near) {
                                    const double xEndF = revE ? x0 : x1;
                                    const double yEndF = revE
                                        ? (double)sp->sy0 : (double)sp->sy1;
                                    const double probeX =
                                        xEndF + (revE ? -1.0 : 1.0);
                                    for (const Obj* nb : *K.near) {
                                        if (nb->type != 0) continue;
                                        if (std::fabs((nb->cy + nb->hh)
                                                      - yEndF) > 0.5)
                                            continue;
                                        if (nb->cx - nb->hw <= probeX
                                            && probeX <= nb->cx + nb->hw) {
                                            // [r83] The boundary is "until the
                                            // extrapolated seat is kStickGap px
                                            // below" = the continuation's gap rule
                                            // itself (same formula as the general
                                            // window below; settled by the seam rig
                                            // at 3 speeds)
                                            extC = slopeXOffset(m, pH)
                                                 + kStickGap / std::fabs(m);
                                            break;
                                        }
                                    }
                                }
                                const bool okE = revE
                                    ? (r <= x1 && cx >= x0 - extC)
                                    : (r >= x0 && cx <= x1 + extC);
                                return Smp{r, okE, r >= x0 && r <= x1};
                            }
                        }
                        // The 1.5 px of grace keeps the ride alive on its
                        // LAST tick -- which is past x1 travelling forward
                        // but past x0 in a REVERSE section. Without the
                        // mirror the reverse rider loses the ramp one tick
                        // before the top and the uphill exit (below) never
                        // fires: lv22 t=18,567, mini cube riding the 30x60
                        // at x-decreasing travel, GD launches at 13.064 and
                        // the model kept riding with vy pinned (the
                        // ride24+ fixup family). Forward runs are
                        // bit-identical (useDx >= 0 keeps the old window).
                        // [2026-08-18] `useDx < 0` was a dead test:
                        // useDx = (s.dx > 0) ? s.dx : K.dxF, and s.dx is the
                        // speed's absolute value, so via an anchor that carries dx
                        // it is always positive. It lives on a cold run (s.dx=0 ->
                        // K.dxF's sign gets through) and dies silently only in
                        // anchored replays -- a "written but never effective" rule
                        // of the same shape as tappedOffSlope. The true travel
                        // direction is K.dxF's sign or s.rev.
                        const bool rev = ((double)K.dxF < 0.0 || s.rev != 0);
                        // [2026-08-19 item 4, both proposals rejected] Tried
                        // widening the exit-side window to take lv18@20,467
                        // (edy +28.359, the largest remaining error). GD keeps the
                        // player standing at y=405.000 (=line(x1)+pH) with og=1
                        // even when the centre is 12.5 past the high end of ramp
                        // uid13971 (m=+2, x1=27,720), and launches at vy=+5.226 the
                        // next tick (the model's launch value already matches).
                        //   Proposal 1, "while the box overlaps the end"
                        //     (x-pH <= x1): families 27->**44**, lv16 tracking
                        //     17,926->16,124, plus 5 more levels regress
                        //   Proposal 2, 1 + "the box bottom below the end's line
                        //     (penetrating the object)": families 27->**31**,
                        //     lv16/lv18/lv20 regress
                        // Both add **over-retention** in the
                        // `slope+0.50|+1.00/ride...` group. What GD does here is a
                        // push-out from the object, not a ride continuation, so the
                        // rule belongs on the push-out side, not the window.
                        // **And the true first divergence is not here**: GD fires
                        // the same portal pair (cube uid14052 @27,755.8 and size
                        // uid14055 @27,757.6, both rot=51, 34x86) **both on the
                        // same tick (20,466)**, while the model fires the size one
                        // at 20,467. For that 1 tick pH stays 9, so no window
                        // whatsoever keeps the acquisition from being 1 tick late.
                        // **Fix the size portal's same-tick firing first.**
                        // [2026-08-19 rejected] Widening the entry side by xoff
                        // ("grab with the high end's flat clamp even when the
                        // contact point is short of the end"): lv19@15,113 (note
                        // below) disappears, but lv16's tracking regresses
                        // 17,926->17,226 (400->41 at t=6,200), lv19 21,942->21,814,
                        // families 27->30 (+5/-4). Too wide as a general rule --
                        // the grab appears to happen only "when at the flat
                        // clamp's height on the landing tick".
                        // [2026-08-20 rig ramps, 48 units] This grace is not the
                        // constant 1.5 but **one tick's movement**. Solving jointly
                        // for each of the rig's 44 units (the 4 ball m=0.5 ones
                        // never launch at all) "the tick before the launch is
                        // still riding / the launch tick is off" brackets
                        //     1.2181 <= G < 1.3082
                        // The rig's one-tick movement is **1.29825** (sp0.9), which
                        // is inside the interval. That is, `r_new <= x1 + dx` <=>
                        // **`r_prev <= x1`** -- the window is the bare end seen at
                        // the start-of-tick x, and no grace constant exists (same
                        // convention as the portals' "the contact side is decided
                        // at the start-of-tick position").
                        // The constant 1.5 is outside the interval, and 4 of the 48
                        // rigs were 1 tick late (robot m2 mini n1 / spider m1 mini
                        // n3 / spider m0.5 mini n1 / spider m2 mini n3 -- all rigs
                        // whose end phase is borderline).
                        const double rideGrace = std::fabs(useDx);
                        bool ok = rev
                            ? (r >= x0 - rideGrace && r <= x1)
                            : (r >= x0 && r <= x1 + rideGrace);
                        // [2026-08-20] **On the landing tick only, grab if at the
                        // height of the high end's flat clamp.** Puts in, in that
                        // narrow form, the note from when "widen the entry side by
                        // xoff" was tried as a general rule and rejected on
                        // 2026-08-19 (lv16 tracking 17,926->17,226, families
                        // 27->30): "the grab appears to happen only when at the
                        // flat clamp's height on the landing tick".
                        // Measured lv19 t=15,112 (UFO, sp1.1, plate uid9970's top
                        // 270 flush with the high end of ramp uid9899 (x0=21,480,
                        // sy 270->240)): on the landing tick GD grabs **the ramp**
                        // and seats at y=285 (=270+pH), releases the next tick
                        // (vy=-6.457 = -1.6143/0.25) and starts descending. The
                        // model's contact point (21,475.5) fell 4.5px short of x0,
                        // so it stayed landed on the plate, frozen; from there GD
                        // descends while the model runs level, opening to the end.
                        // Limited to a ramp that **descends** in the travel
                        // direction (high end on the entry side). Without that it
                        // grabs lv20 t=13,800 (`slope+0.50/ride0`, the uphill side)
                        // and becomes an over-acquisition, edvy=-1.492.
                        const double mTravHi = rev ? -m : m;
                        // And it also needs to have **crossed** the flat clamp's
                        // height from above (at/above the cap at the start of the
                        // tick, at the cap after landing).
                        // lv16 t=7,059 is walking the flat 464.950 where og drops
                        // for just 1 tick, and its start-of-tick y (464.952) is
                        // **below** the cap (465.000) -- GD does not grab. Without
                        // this, `slope-1.00/ride0` produces an over-launch,
                        // edvy=+6.457.
                        if (!ok && c.grounded && !s.grounded
                            && mTravHi < 0.0) {
                            const double capHi =
                                std::max((double)sp->sy0, (double)sp->sy1) + pH;
                            if ((double)s.y >= capHi - 0.02
                                && std::fabs((double)c.y - capHi) <= 0.6
                                && cx + pH >= x0 && cx - pH <= x1)
                                ok = true;
                        }
                        // [2026-08-21 r79] **When a flush flat continues past the
                        // downhill end, the ride (= the time of the release pulse)
                        // stays alive until "the tick the rotated box's rear corner
                        // (centre -/+ (pH-xoff)) exits the end".**
                        // rideGrace (based on r) is the measurement for a launch
                        // into air (slopeland rig).
                        // Measured lv18 t=12,280 (ball m=-1, x1=16,590, flush slab
                        // top 210): GD's pulse (-6.457 = mTravel*dx/0.25) is on the
                        // tick cx crosses x1+8.787 (=pH-xoff). The model fired 1
                        // tick early with the r window (census
                        // `m2/.../slope-1.00/ride24+` edvy +6.457).
                        // The cube side has the same rule in the extC branch above
                        // (lv16 t=6,972).
                        // [2026-08-21 r83] **The boundary turned out to be the same
                        // thing as the continuation's gap rule**: the ride lasts
                        // "until the extrapolated seat falls kStickGap px below" =
                        // cx <= x1 + xoff + kStickGap/|m|. The seam rig at 3 speeds
                        // (seam/seam07/seam11, dx 1.047/1.298/1.614, cube+ball x
                        // |m|{1,0.5} x 12 phases = 132 runs) agrees on 130 of them.
                        // r79's provisional form (pH - xoff + c, c=1.1) was 0.36px
                        // too narrow at |m|=1, and on the rig a quarter of the
                        // phases released 1 tick early.
                        if (!ok && stickToSlope && s.grounded && K.near
                            && (rev ? -m : m) < -0.01) {
                            const double xEndF = rev ? x0 : x1;
                            const double yEndF = rev
                                ? (double)sp->sy0 : (double)sp->sy1;
                            const double probeX = xEndF + (rev ? -1.0 : 1.0);
                            for (const Obj* nb : *K.near) {
                                if (nb->type != 0) continue;
                                if (std::fabs((nb->cy + nb->hh) - yEndF)
                                    > 0.5)
                                    continue;
                                if (nb->cx - nb->hw <= probeX
                                    && probeX <= nb->cx + nb->hw) {
                                    const double cornB =
                                        slopeXOffset(m, pH)
                                        + kStickGap / std::fabs(m);
                                    ok = rev ? (cx >= x0 - cornB)
                                             : (cx <= x1 + cornB);
                                    break;
                                }
                            }
                        }
                        return Smp{std::min(std::max(r, x0), x1), ok,
                                   r >= x0 && r <= x1};
                    }
                    const double a = std::min(std::max(cx - pH, x0), x1);
                    const double b = std::min(std::max(cx + pH, x0), x1);
                    const double sa = sp->sy0 + m * (a - x0);
                    const double sb = sp->sy0 + m * (b - x0);
                    // The SWING has to have its CENTRE inside the ramp, not
                    // just a box edge. Measured on lv22 uid4359 (a static
                    // 30x30 at cx=6,705, rot=-90, m=-1, so x0=6,690):
                    //   t=4,623  centre 6,684.57  GD onSlope=0  -- model GRABBED
                    //   t=4,627  centre 6,691.03  GD onSlope=1  -- first grab
                    // The box-edge test fires a full pH early, and the point it
                    // then picks is the lowest surface over the box width
                    // (6,699.57 -> 440.43), which hangs the player at 425.43
                    // while GD's own line at the centre is 453.8 -- 28 px out.
                    // Ship/UFO keep the box-edge rule: it is what lv16 t=4,111
                    // was measured with (the ship pinned by its LEFT edge on the
                    // ramp's own corner).
                    // ...and it samples the surface at the CENTRE, not at the
                    // lowest point over the box width. Measured on the same
                    // ramp at t=4,627 (centre 6,691.03, line there 448.97):
                    // GD rests the player at 427.763 = 448.97 - 21.21, and
                    // 21.21 is pH*sqrt(1+m^2) -- the same rotated-box quantity
                    // the upright ride uses. Taking the box's lowest point
                    // (6,706 -> 434) would have put it at 419.
                    // ...and the CUBE hang needs the centre rule as much as
                    // the swing. With the box-edge rule the flipped dual body
                    // of lv16's first dual "landed" on the NEXT ramp of the
                    // chain while only its outer 2.3 px column overlapped it:
                    // t=7,774, p2 riding uid3341's line at 559.6, grabbed
                    // uid3357 (x0=11,700, 13 px ahead) at top=553.835 -- a
                    // 5.73 px downward teleport (the prevTop gate passed by
                    // 0.27 px against kLandTol). GD keeps the body on the
                    // current line and only moves at the seam. Ship/UFO keep
                    // the box-edge rule (measured, lv16 t=4,111).
                    // ...the BALL's hang uses the centre rule too (2026-08-19, lv16
                    // t=13,115's 12.728 is exact against line(centre x). This
                    // branch only sees the hang side (gs<0), so the upright ball is
                    // unaffected).
                    // [2026-08-21 r71] swing moved from centre to contactPt.
                    // Calibration rig ceilramp unit36 (flipped swing, m=-1): GD
                    // follows the line from the tick the contact point (x+xoff)
                    // enters x0 (the centre is 5px short). The lv22 uid4359
                    // "centre rule" measurement is at m=1, where
                    // line(centre)-pH*sqrt(2) == line(contact)-pH degenerate and
                    // could not be told apart. The ball stays centre (the ball
                    // column is bit-identical with centre).
                    const bool centre = (c.mode == 2
                                         || (c.mode == 0 && s.dual));
                    // [2026-08-20] **The cube's and UFO's hang grab at the contact
                    // point.** Lining up "the x where it leaves the flat ceiling
                    // and starts descending" against GD on the calibration rig
                    // ceilramp (5 modes x |m| 3 x normal/mini = 30 units, after
                    // the header fix): ship 6/6 and ball 4/4 are exact under the
                    // current box-edge/centre rules, while **only cube 6/6 and
                    // UFO 6/6 are 2.6-11.7px early**. In all 12 cases that
                    // earliness matches **pH - slopeXOffset(m,pH)** within 1 tick
                    // (1.298px):
                    //   cube m1   15-6.213=8.787 vs measured 9.085
                    //   cube m0.5 15-3.541=11.459 vs 11.681
                    //   cube m2   15-9.271=5.729 vs 5.195
                    //   UFO  m1   8.787 vs 7.793 ...(and so on; mini agrees too)
                    // The hang's contact point is "the upright one mirrored in y"
                    // = the sign of m flips, so `cx - (m>0 ? xoff : -xoff)`. This
                    // is the same formula as the rc the ceiling push-out branch
                    // (ceilpush) already uses.
                    // Keeping the ship at the box edge is supported both by the
                    // lv16 t=4,111 measurement (the left edge digging into the
                    // ramp's corner) and by the rig's 6/6 agreement.
                    // [2026-08-21 r70] robot/spider added. Calibration rig
                    // ceilramp unit24 (flipped robot, m=-1 chain) measured:
                    //   acquisition: GD follows the line from the tick the contact
                    //     point (x+xoff ~= 6.2) enters x0. The model started
                    //     descending 15px early at the box edge
                    //   release: on the tick the contact point exits x1,
                    //     vy=-7.405 = slopeExitVy's cube value (robot=cube, no
                    //     0.75, matches the table). There is **no** flat corner
                    //     tick like the ship's, so the bare contactPt window fits
                    //     both ends
                    const bool contactPt = (c.mode == 0 || c.mode == 3
                                            || c.mode == 5 || c.mode == 6
                                            || c.mode == 7);
                    const double rcH =
                        cx + (m > 0 ? -slopeXOffset(m, pH)
                                    : slopeXOffset(m, pH));
                    // [r71c] Only the swing gets grace on the exit side (the end
                    // in the travel direction) = the corner flat's 1 tick (same
                    // shape as ship r68b / ball okHi).
                    // Measured unit36/40: GD stops for 1 tick at 185.000
                    // (= line(x1)-pH) on the first tick the contact point exits
                    // x1, and launches on the **next** tick (-5.554/-7.880 -- the
                    // values already match, only the time was 1 tick early in the
                    // model). robot/spider have no corner tick (unit24 measured),
                    // so bare contactPt.
                    // [2026-08-21 r74] The grace is not the constant 1.5 but **one
                    // tick's movement** (= "the previous tick's contact point is
                    // still inside the end"). Same conclusion as the ride window's
                    // rideGrace (2026-08-20 rig ramps, 1.2181<=G<1.3082). ceilramp
                    // unit41's (mini m=2, dx=1.297) release tick has the contact
                    // point at x1+1.36 -- falling in the (dx,1.5] gap, so only the
                    // model lingered one more tick on the corner (unit4's ship is
                    // the same phase).
                    const bool revT7 = ((double)K.dxF < 0.0 || s.rev != 0);
                    const double cornG = std::fabs((double)useDx);
                    const bool ok = centre
                        ? (cx >= x0 && cx <= x1)
                        : contactPt
                        ? ((c.mode == 7)
                           ? (revT7 ? (rcH >= x0 - cornG && rcH <= x1)
                                    : (rcH >= x0 && rcH <= x1 + cornG))
                           : (rcH >= x0 && rcH <= x1))
                        : (cx + pH >= x0 && cx - pH <= x1);
                    // [r71b] The swing's xr is the contact point too (clamped).
                    // Kept at the centre clamp, the acquisition tick jumps 5.1px
                    // down to line(x0)-rotated distance (unit36: GD 273.881 =
                    // line(rc)-pH / M 268.787). At m=1, line(rc)-pH ==
                    // line(centre)-pH*sqrt(2) (the lv22 uid4359 measurement), so
                    // this is consistent with the existing corpus measurements.
                    if (c.mode == 7)
                        return Smp{std::min(std::max(rcH, x0), x1), ok, ok};
                    if (c.mode == 2) {
                        // The hang's window is cut at the contact point (cx+-xoff)
                        // only at "the low end descending in the travel direction"
                        // -- GD ends the ride on the tick the contact point leaves
                        // the end, and stands on the flat clamp for the 1 tick
                        // just before (lv16 t=13,116-13,117; the counterpart of
                        // the top side's max clamp).
                        const bool revB = ((double)K.dxF < 0.0 || s.rev != 0);
                        const double xoffB = slopeXOffset(m, pH);
                        double okLo = x0, okHi = x1;
                        if (!revB && m < 0) okHi = x1 - xoffB + 1.5;
                        else if (revB && m > 0) okLo = x0 + xoffB - 1.5;
                        return Smp{std::min(std::max(cx, x0), x1),
                                   cx >= okLo && cx <= okHi,
                                   cx >= x0 && cx <= x1};
                    }
                    // [2026-08-19 D7b] In a ship's hang, **once the centre passes
                    // the high end**, the binding is that corner (the flat at
                    // line(end) - pH). Kept at the box edge's lower side, it
                    // "lands" 4-5px lower at the exit: lv16 t=4,142-4,145, GD
                    // stays at 435.000 = line(x1) - pH while the model dropped to
                    // the box left edge's 430.48. The entry corner (t=4,111's 405)
                    // is equivalent to the box-edge rule, so behaviour there is
                    // unchanged. Measured for the ship only.
                    // The window ends on the tick **the contact point (centre -/+
                    // (pH - xoff)) leaves the end** (same shape as the ball's hang
                    // window okHi): GD releases at t=4,147 (x=5,832.4 > x1 + pH -
                    // xoff = 5,831.5) and emits +3.229. Box overlap would be 2
                    // ticks late.
                    if (c.mode == 1 && (m > 0 ? (cx > x1) : (cx < x0))) {
                        const double rel = pH - slopeXOffset(m, pH);
                        const bool okC = (m > 0) ? (cx <= x1 + rel)
                                                 : (cx >= x0 - rel);
                        return Smp{(m > 0) ? x1 : x0, okC, okC};
                    }
                    // [2026-08-21 r68] **The hang's exit at forward x m<0 (reverse
                    // x m>0) closes at the contact point.** D7b above covers only
                    // the "past the high end" side; the **low end** in the travel
                    // direction stayed at box overlap (cx-pH <= x1) = a window
                    // 15px too long, so the model kept sitting on the flat corner
                    // and the launch (uphillExit) never fired.
                    // Measured, calibration rig ceilramp unit0 (flipped ship, m=-1
                    // chain end x1=780): GD is at the flat corner 185.000 on the
                    // first tick the contact point (cx+xoff) exits x1 (781.27),
                    // and **vy=-5.554 on the next tick** = exactly what the
                    // existing uphillExit emits. The grace is the same "corner
                    // flat's 1 tick" as the ball's okHi.
                    // [2026-08-21 r74] That 1 tick is not the constant 1.5 but the
                    // movement |useDx| (see the note at the swing side's cornG;
                    // unit4 was 1 tick late at a (dx,1.5] phase). Consistent with
                    // unit0's measurement too: the corner tick's contact point is
                    // x1+1.27 <= dx=1.298.
                    // [2026-08-24] **...and the hang's ENTRY closes at the contact
                    // point too.** The two rules above end the ride at the far end;
                    // nothing guarded the near one, so a ramp whose surface RISES
                    // away from the player was grabbed on the tick the box merely
                    // reached it, and `xr`'s clamp then read the ramp's low end --
                    // a phantom flat ceiling extending backwards out of the box.
                    // Measured on lv16's ceiling V at x=13,220 (uid3763 m=-1 into
                    // uid3765 m=+1, both sdir ceiling, flipped ship, forward):
                    //   t=8,715  centre 13,206.35, box edge 1.35px past x0
                    //            GD 546.441 = uid3763's line - pH*sqrt(2). The
                    //            model took uid3765 at 539.000 = sy0 - pH, 7.44
                    //            px early, and never left it again
                    //   t=8,719  centre 13,212.80  GD 539.984  (still uid3763)
                    //   t=8,720  centre 13,214.42  GD 539.000  -- which uid3763's
                    //            own end already gives, so this tick does not
                    //            decide anything on its own
                    // The gate is the SAME contact point the two exits above use,
                    // and its sign is not free: the seat is written as
                    // line(xr) + gs*pH, and for it to come out at the measured
                    // line(centre) + gs*pH*sqrt(1+m^2) the hang's contact must sit
                    // at `centre - sign(m)*xoff` -- which is exactly the point the
                    // exits test (m<0 forward uses centre+xoff at x1). So the
                    // entry mirrors them: centre-xoff against x0 when m>0 forward.
                    // t=8,720's launch is the third measurement and it is what
                    // fixes the sign: GD leaves at -6.330 = the m=-1 mirror of
                    // uid3763's own launch (6.905 x 22/24), so GD is still on
                    // uid3763 on that tick and uid3765 must not be held yet -- the
                    // looser `centre+xoff` reading takes it 0.63px too early and
                    // flips slopeM, which turns the launch off.
                    // Only the rising-away side can produce the phantom (on the
                    // falling side the clamp reads the HIGH end, which is not the
                    // binding ceiling for a hang), which is why the sign gates
                    // match the two exits' mirrors.
                    // Ship only, as the two exits above are -- this is where it is
                    // measured. ([[gd-hang-side-mirrors-floor-rules]])
                    if (c.mode == 1) {
                        const bool revH =
                            ((double)K.dxF < 0.0 || s.rev != 0);
                        const double xoffH = slopeXOffset(m, pH);
                        const double cornGS = std::fabs((double)useDx);
                        if (!revH && m < 0 && cx + xoffH > x1 + cornGS)
                            return Smp{x1, false, false};
                        if (revH && m > 0 && cx - xoffH < x0 - cornGS)
                            return Smp{x0, false, false};
                        if (!revH && m > 0 && cx - xoffH < x0)
                            return Smp{x0, false, false};
                        if (revH && m < 0 && cx + xoffH > x1)
                            return Smp{x1, false, false};
                    }
                    return Smp{(sa <= sb) ? a : b, ok, ok};
                };
                const auto smp = sampleAt(x);
                // Which half of the box is solid is slopeDir's business, not
                // the line's -- see slopeIsCeiling, derived from
                // collidedWithObjectInternal. Only the SHIP is converted to the
                // full {1,3,5,6} set so far; every other mode keeps the {1,3}
                // subset it was validated with.
                const bool ceilRamp = ceilRampSide;
                // ---- CEILING ramp met from UNDERNEATH ------------------------
                // slopeDir says which half is solid (slopeIsCeiling; this test
                // predates it and still uses the {1,3} subset for the non-ship
                // modes). m_slopeDirection was loaded and never read, so every ramp
                // was treated as ridable-from-above only and an upright player
                // rising into a ceiling one simply passed through it. That is
                // lv20's wall at x=6,343: a mini ship rises into the 340->310
                // ramp at (6195,325) and GD presses it DOWN along the surface
                // while the model sails on.
                //
                // Measured on that ride (lv20 t=4339..4344, mini ship, pH 9):
                // contact 12.72 px below the surface line, then y tracks the
                // line exactly (-1.6143/tick = -dx), vy snapped to 0 on the
                // contact tick, and GD reports onGround=0 throughout -- a BLOCK,
                // not a ride, so `grounded` stays clear (a cube must not get its
                // jump back by touching a ceiling).
                // 12.72 is not a new constant: it is pH + slopeXOffset(m,pH),
                // the same rotated-box contact the floor ride uses, sampled from
                // the other side of the box. At m=1 that equals pH*sqrt(2) --
                // an identity at 45 degrees only, so do NOT simplify it to that
                // (lv20 has four |m|=0.5 ceilings too).
                //
                // GATED ON COMING FROM BELOW, and it does NOT skip the ride
                // below. The first attempt clamped and `continue`d, which threw
                // away the ordinary ride for every ceiling-flagged ramp: lv16
                // went from CLEARED in 17 iterations to STUCK at x=19,435 in 60,
                // and lv18 from 3.5 to 17.2 minutes. slopeDir says which side of
                // the ramp is solid, not which face a player may land on -- an
                // upright player landing on a ceiling ramp's other face is
                // ordinary, and those levels are full of it.
                // CONTIGUOUS ceiling ramps are ONE surface. Clamping the
                // reference point to [x0,x1] flattens the line at a ramp's
                // edge, and the came-from-below gate then tests against the
                // FLATTENED line, so the next ramp of a chain is never
                // adopted: lv16 t=15,338 (uid7606, x0=23,460, sy0=570) has
                // the model parked at the flat 555.000 while GD keeps
                // descending along the extension (554.420), and the model's
                // y in the 2026-08-17 fixup families sits FLAT at 475.9 /
                // 585.0 / 525.0 across ticks -- three ramp chains, one bug
                // (43 records + 4 kills, the census #2/#3 families).
                // When the reference point leaves this ramp's span, the
                // constraint is the LOWEST solid point over the box, so the
                // neighbour's line only takes over where it is LOWER than
                // the flat corner value: a down->down seam follows the
                // extension (lower), while a down->up VALLEY keeps the flat
                // corner -- measured on the same chain, t=15,367 of the
                // 2026-08-17 lv16 reference: GD contacts the V at exactly
                // 525.000 = the corner (540) - pH, NOT the rising line.
                // (An unconditional extension broke that acquire: the
                // rising line outruns the ship's climb and `c.y > lim`
                // never fires.) Neighbour = same ceiling side, meets at the
                // shared edge, not a hazard twin (the twin sits on a
                // DIFFERENT line 4 px off, so the y-continuity test rejects
                // it as well). First match wins; ramps are sorted by x.
                auto ceilLimAt = [&](double cx) {
                    const double r = cx + (m > 0 ? -slopeXOffset(m, pH)
                                                 : slopeXOffset(m, pH));
                    const double rc = std::min(std::max(r, x0), x1);
                    const double flat = sp->sy0 + m * (rc - x0) - pH;
                    // SHIP ONLY, like everything else measured here: letting
                    // the extension serve every mode moved lv19's UFO at
                    // t=14,799 onto a ceiling it never touches in GD (58 px,
                    // vy pinned 0) and shrank lv20 too.
                    if (c.mode == 1 && (r > x1 || r < x0)) {
                        for (const Obj* nb : *K.slopes) {
                            if (nb == sp || nb->slopeHazard) continue;
                            const double nx0 = nb->cx - nb->hw;
                            const double nx1 = nb->cx + nb->hw;
                            if (nx1 <= nx0) continue;
                            const double nm = (nb->sy1 - nb->sy0)
                                            / (nx1 - nx0);
                            if (nm == 0.0) continue;
                            const bool nbCeil =
                                (c.mode == 1 || c.mode == 7 || c.mode == 0)
                                ? slopeIsCeiling(nb->slopeDir)
                                : (nb->slopeDir == 1 || nb->slopeDir == 3);
                            if (!nbCeil) continue;
                            // [2026-08-19] At a same-sign seam (the line continues)
                            // follow the neighbour's line directly. Keeping the
                            // flat corner via min is **only for a valley where the
                            // sign reverses** (lv16 t=15,367: GD contacts the V's
                            // corner 525.000 = corner(540)-pH and does not use the
                            // rising line -- that measurement is min's
                            // justification and is correct). In a rise->rise chain
                            // ext > flat, so min stops early at the flat corner:
                            // at lv19 t=5,460 (uid3205->3207, 540->570->600) only
                            // the model stuck to 555.000 while GD climbs to the
                            // next ramp's line (contact 561.461 = line(x-off)-pH)
                            // -- the identity of the census m1/.../clamp:slope/
                            // ceillim family. A descending chain has ext < flat,
                            // equivalent to min (no change).
                            if (r > x1 && std::abs(nx0 - x1) <= 0.5
                                && std::abs((double)nb->sy0
                                            - (double)sp->sy1) <= 0.5) {
                                const double rr = std::min(r, nx1);
                                const double ext = nb->sy0
                                    + nm * (rr - nx0) - pH;
                                return (nm * m > 0.0) ? ext
                                                      : std::min(ext, flat);
                            }
                            if (r < x0 && std::abs(nx1 - x0) <= 0.5
                                && std::abs((double)nb->sy1
                                            - (double)sp->sy0) <= 0.5) {
                                const double rr = std::max(r, nx0);
                                const double ext = nb->sy0
                                    + nm * (rr - nx0) - pH;
                                return (nm * m > 0.0) ? ext
                                                      : std::min(ext, flat);
                            }
                        }
                    }
                    return flat;
                };
                // ...and a FLIPPED player meets that same ceiling from the other
                // side. GD does not hang it under the line -- it pushes it OUT of
                // the solid half, down to the very limit the upright case uses.
                //
                // Measured on lv21 t=19,837 (cfg `standtrace=1` names the object:
                // `slope=[uid26823 id1338 type25 @26415,315]`, sy 330->300,
                // slopeDir 1): a flipped cube falling at vy=-9.884 through
                // y=330.9276 is placed at **y=314.8259**, with vy UNTOUCHED and
                // onGround 0 -- a push-out, not a ride. And
                //   ceilLimAt(26393.9609) = 330 + m*(26400.1741 - 26400) - 15
                //                         = 314.8259
                // to four decimals, i.e. exactly the pH + slopeXOffset contact the
                // ship's ceiling measurement produced. Nothing new to fit.
                //
                // The model sailed straight through and ran **13.9265 px HIGH for
                // the rest of the level** (dy constant, dvy 0). That one tick is
                // the whole of lv21's wall at x=26,516: every tail the DP built
                // was solved for a trajectory GD does not fly. Proof it was not
                // the search: from the ladder's own anchor (t=19,834) the DP
                // returns SOLVED at x=27,505 in 8 edges, and splicing that tail on
                // still dies at t=19,930 in GD.
                //
                // The upright branch's `came from below` gate cannot be reused
                // here: this player arrives from ABOVE the limit, which is exactly
                // why it is inside the solid. The gate is the overlap itself.
                // The window is the CONTACT POINT being genuinely inside the ramp,
                // not the box overlapping it. Tried the box first and it fired two
                // ticks early: at lv21 t=19,835 the player's box already reaches
                // x0 while its contact point is 2.4 px left of the ramp, and GD
                // does nothing until t=19,837 -- the tick rc crosses x0 (26,400.17
                // against x0 = 26,400). Clamping early put the model at y=315 and
                // then dropped it into the ordinary ride, 8.8 px below GD.
                // NOT a spiked slope. GD pairs a plain ramp with a hazard twin at
                // the same cx (lv16 x=28,425: id309 sy 780->750 shz=0 and id366
                // sy 776->746 shz=1), and only the plain one is solid. Pushing
                // against the twin's line -- 4 px lower -- drove lv16's flipped
                // dual body from GD's 770.38 (= the PLAIN ramp's own limit, to
                // the digit) down to 766.38, straight into the spikes, and the
                // segment at t=18,200 fell from 296 followed ticks to 236.
                // NOT the swing. "A flipped player never HANGS from a ceiling
                // ramp" was measured on lv20's mini SHIP (uid26823, listed as
                // m_currentSlope on exactly one tick of a whole pass), and the
                // swing does the opposite: lv22 t=4,627..4,635 hangs from
                // uid4359 (slopeDir=5) for nine ticks, sliding down it at
                // exactly -dx per tick, and GD's own y is the line minus
                // pH*sqrt(1+m^2) throughout. Letting this skip fire cost the
                // ride and the model free-fell 3 ticks early.
                // [2026-08-20] **An upright player is also pushed out of a ceiling
                // ramp's solid.** The branch below was `c.flip`-only, but the
                // geometry is the same: the clearance of an axis-aligned box
                // against a tilted line is **pH + slopeXOffset(m,pH)**, not the
                // box's top edge (pH). The "when coming from below" gate above
                // rejects a player that entered from above.
                // Measured lv20 t=18,826 (upright ship, right after wave->ship,
                // vy=-1.046, ceiling ramp uid15572 x[28,290,28,320] line 300->270
                // sdir=1):
                //   line(28,300.8) = 289.2, lim = 289.2 - 21.213 = 267.99
                //   GD  y=271.812 -> **268.021** (vy=-2.000), then follows the
                //       line at -dx/tick
                //   the model passed straight through and stayed 3.5px high from
                //   then on (census edy -3.540)
                // The gate is "the box's top edge inside the ramp's rect" --
                // without it, an upright player **standing on the rect's top
                // face** also gets picked up and dropped below the line.
                // [2026-08-21 r65] **Remove the swing exclusion (upright side
                // only).** `c.mode != 7` merely copied the flipped side's (branch
                // below) "the swing hangs" measurement here; there was no evidence
                // for the upright side. Calibration rig ceilpush8 (one situation x
                // 7 modes) measured: on the tick an upright swing falls off an
                // edge and is caught by a descending ceiling ramp (m=-1), GD sets
                // **vy := -2.000** (same value as ship/cube) and y follows the
                // line at -dx/tick. From then on the swing's own g (-0.086/tick)
                // accumulates. The model, because of the exclusion, passed through
                // and floated +2.5px/3 ticks. The flipped-side exclusion (the
                // swing hangs, lv22 uid4359) stays.
                if (!c.flip && ceilRamp && !sp->slopeHazard
                    && (double)c.y + pH <= sp->cy + sp->hh + 0.001) {
                    const double rcU = x + (m > 0 ? -slopeXOffset(m, pH)
                                                  : slopeXOffset(m, pH));
                    const double limU = ceilLimAt(x);
                    const bool inYU = ((double)c.y + pH >= sp->cy - sp->hh)
                                   && ((double)c.y - pH <= sp->cy + sp->hh);
                    if (g_slopeDbg)
                        std::printf("upceil t=%lld x=%.2f uid=%d y=%.3f "
                                    "lim=%.3f rc=%.2f x0=%.1f x1=%.1f inY=%d\n",
                                    (long long)K.t, x, sp->uid, (double)c.y,
                                    limU, rcU, x0, x1, inYU ? 1 : 0);
                    // [2026-08-21 r40] **No grace on the exit side.** The push-out
                    // is a geometric constraint, not a "ride", so it no longer
                    // applies on the tick the contact point exits the span. The
                    // constant 1.5 can be smaller than one tick's movement
                    // (dx=1.613 here), grabbing one extra tick GD has let go.
                    // Measured lv18 (upright cube, ceiling ramp uid13979
                    // span [27,750, 27,780], line 450->390, pH=15):
                    //   t=20,490 x=27,769.62 rc=27,778.89 (in span) -> both clamp
                    //   t=20,491 x=27,771.24 rc=27,780.51 (**0.51 outside**) ->
                    //     GD free-falls (376.182 = 377.213 - 0.225x4.580); the
                    //     model grabbed via the grace and pulled down 1.178px to
                    //     lim=375.000 (= line(x1) - pH), then ran a parallel
                    //     -1.182 to the end
                    //   (census `m0/mini0/g0/gdg0/sp1.1/air` edy +1.179)
                    // [2026-08-21 r66b] **The flight modes' exit is "until y comes
                    // out below the line".** Measured ceilrel (ship d4 / UFO d0,d4
                    // / swing d0): on the tick rc exits the low end, GD pushes one
                    // more step to **the corner flat (line(x1) - pH = ceilLimAt's
                    // rc-clamped value)** before releasing (169.000 / 165.000
                    // exactly). The push only moves y down, so on the next tick
                    // the free-fall y drops below lim and the contact ends
                    // naturally = the release tick. Cutting the window at rc drops
                    // the corner's 1 tick, the pressed-tick count k also comes up
                    // 1 short, and the release vy (ceil/release) shifts with it.
                    // The cube has no corner tick (lv18 t=20,491, the r40
                    // measurement: GD free-falls on the tick rc is 0.51 out) --
                    // keeps the rc window.
                    // **Only past the exit**: right after rc exits "the low end in
                    // the travel direction", until the centre reaches end + pH.
                    // This was written wrongly twice:
                    //   1) a one-sided condition (x <= x1+pH) -> another ramp
                    //      ahead also enters the window
                    //   2) picking the end by m's sign -> in lv20's W-shaped chain
                    //      (alternating m) the **approach to the next uphill
                    //      ramp** (rc < its x0) was misread as "the reverse exit"
                    //      and jumped 9px to that corner's flat 255.000
                    // The end is decided by the **travel direction**, not m (useDx
                    // is a speed, always positive -- the true direction is K.dxF's
                    // sign or s.rev). Downhill (mTrav<0) only.
                    const bool revU = ((double)K.dxF < 0.0 || s.rev != 0);
                    // [2026-08-21 r80] spider (mode 6) added. Measured lv22
                    // t=8,262 (spider, m=-2 push-down, sp1.1): on the tick rc
                    // exits the span, GD takes the corner flat 436.500
                    // (= line(x1) - pH) for 1 tick before letting go (vy stays
                    // plain gravity, -0.387). Off the whitelist the push ends 1
                    // tick early and that tick alone free-falls (census
                    // `m6/.../sp1.1/air` edy -0.686, re-converges next tick).
                    const bool contU =
                        (c.mode == 1 || c.mode == 3 || c.mode == 6
                         || c.mode == 7)
                        && s.ceilT > 0 && (revU ? -m : m) < 0.0
                        && (!revU ? (rcU > x1 && x <= x1 + pH)
                                  : (rcU < x0 && x >= x0 - pH));
                    const bool winU = (rcU >= x0 && rcU <= x1) || contU;
                    if (inYU && winU
                        && (double)c.y > limU) {
                        c.y = (float)limU;
                        // r66: the release vy counts. **Downhill only (the line
                        // descends in the travel direction)**: on leaving the hang
                        // of a rising ceiling (slope/ceillim's hang) GD emits no
                        // release set -- lv16 t=3,281 / lv19 t=5,465 (both
                        // rise->hang; GD just droops from vy 0 under plain g)
                        // became new families from the mark alone.
                        // (useDx is a speed, always positive, so direction is revU)
                        if ((revU ? -m : m) < 0.0) {
                            ceilPressedNow = true;
                            ceilPressM = m;
                        }
                        // [2026-08-20 r36] **The push-out drops vy to the face's
                        // value too.** The lv20 t=18,826 measurement above says so
                        // ("GD y=271.812 -> 268.021 (**vy=-2.000**)"), yet only y
                        // was applied. Second example lv18 t=20,479 (upright cube,
                        // ceiling ramp uid13979 m=-2, charging into the line at
                        // vy=+3.076): GD sets vy=-2.000 on the same tick, then
                        // plain gravity at -0.215/tick (20,480 -2.215 / 20,481
                        // -2.430 ...) while the clamp pulls y along and continues.
                        // The identity of family `m0/mini0/g0/gdg0/sp1.1/air`
                        // edvy=-4.861.
                        // Value and sign follow the same rule as the ceiling-ramp
                        // acquisition's +-2.000 (the `slope/ceillim` note, dir
                        // following the face).
                        // **Written as "capped at -2.000" so it only takes effect
                        // on the acquisition tick** -- while riding, gravity is
                        // already below -2.
                        // [2026-08-21 r64] **"Capped at -2" fires every tick.**
                        // The note above expected "while riding, gravity is
                        // already below -2", but **while pressing it never goes
                        // below**. Measured lv20 t=18,826..18,830 (ship, sp0.7,
                        // held=1):
                        //   GD  -2.000 -> -1.892 -> -1.784 -> -1.676 -> -1.568
                        //       (+0.108/tick = the ship's holdStrong)
                        //   the model reset to -2.000 every tick (census
                        //   `m1/.../clamp:slope/upceil/uid15572/air` edvy +0.108)
                        // GD places it **once, on the contact tick**. The gate is
                        // whether it was below the previous tick's line
                        // (ceilLimAt(xPrev)) -- while stuck, s.y is exactly that
                        // line, so it never fires again.
                        // The value is 0 **for the spider only**. 3 measurements:
                        //   lv20 t=18,826 ship   vy -1.046 -> **-2.000**
                        //   lv18 t=20,479 cube   vy +3.076 -> **-2.000**
                        //   lv22 t=8,259  spider vy +3.440 -> **0.000**
                        const double dirU =
                            (m * (double)useDx >= 0.0) ? 1.0 : -1.0;
                        const double limUPrev = ceilLimAt(xPrev);
                        // The gate is "did **this clamp place it** on the previous
                        // tick too", not "was it below the line". lv20 t=18,826's
                        // player is already 1.67px below the line (270.14) at the
                        // start of the tick, but the previous tick did not clamp
                        // (rcU outside the span) -- written as "was below", it
                        // stopped firing.
                        // While stuck, s.y is exactly the previous tick's lim.
                        const bool freshU =
                            std::fabs((double)s.y - limUPrev) > 0.001;
                        // [2026-08-21 r66e] **While being pressed down, re-clamp on
                        // the input's release edge too.** Measured lv20
                        // t=18,826..18,842 (ship, sp1.1, a pressed-down section
                        // with holds mixed in): after the catch's -2.000,
                        // holdStrong brings vy back up to -1.568, but on the held
                        // 1->0 tick (18,831) GD **places -2.000 again**. The
                        // release edges at 18,836 / 18,842 have vy already below
                        // -2, no-ops -- consistent at 3 points. No re-clamp while
                        // pressing continues (e.g. the press edge at 18,833).
                        const bool holdRelU = s.held && !input;
                        const double vClampU = (c.mode == 6) ? 0.0 : -2.0;
                        if (dirU < 0.0 && (freshU || holdRelU)
                            && (double)c.vy > vClampU) {
                            c.vy = (float)vClampU;
                            CLAMP0O("slope/upceil", sp);
                        }
                    }
                }
                if (c.flip && ceilRamp && !sp->slopeHazard && c.mode != 7) {
                    const double rc = x + (m > 0 ? -slopeXOffset(m, pH)
                                                 : slopeXOffset(m, pH));
                    const double lim = ceilLimAt(x);
                    // ...and the player has to be IN the object, not merely over
                    // its x span. rc alone teleported lv16's flipped dual body
                    // from y=877.57 down to 770.38 (the ramp's limit, 107 px away)
                    // because the ramps at x=28,425 sit under it: the same segment
                    // fell 296 -> 236 followed ticks. The y test is the ramp's own
                    // box, which is what "inside the solid half" needs.
                    const bool inY = ((double)c.y + pH >= sp->cy - sp->hh)
                                  && ((double)c.y - pH <= sp->cy + sp->hh);
                    if (g_slopeDbg)
                        std::printf("ceilpush t=%lld x=%.3f uid=%d y=%.3f "
                                    "vy=%.3f lim=%.3f rc=%.2f x0=%.1f x1=%.1f "
                                    "inY=%d sOn=%d sG=%d cOn=%d\n",
                                    (long long)K.t, x, sp->uid, (double)c.y,
                                    (double)c.vy, lim, rc, x0, x1,
                                    inY ? 1 : 0, (int)s.onSlope,
                                    (int)s.grounded, (int)c.onSlope);
                    // [2026-08-21 r74] The exit-side +1.5 is the "corner flat's 1
                    // tick" grace, and that 1 tick is not a constant but the
                    // **movement |useDx|** (same conclusion as rideGrace / the
                    // sampleAt-side cornG). ceilramp unit4 (flipped ship m=2,
                    // dx=1.297): the release tick's rc is x1+1.365 -- falls in the
                    // (dx,1.5] phase, and only this window grabbed 1 extra tick,
                    // keeping the y pin and vy=0, so the launch (-7.880) was 1
                    // tick late (the swing never reaches this branch, mode!=7, so
                    // the sampleAt-side fix closed it first).
                    if (inY && rc >= x0
                        && rc <= x1 + std::fabs((double)useDx)) {
                        // Inside the ramp the flipped player is either pushed out
                        // of the solid half or in free flight -- it never HANGS
                        // from a ceiling ramp. GD's own standtrace says so: over
                        // the whole pass it lists uid26823 as `m_currentSlope` on
                        // exactly ONE tick (19,837) and `[-]` on every other.
                        // Without the skip the ordinary ride grabs the player two
                        // ticks later (t=19,842, sampling the ramp's far edge) and
                        // parks it at 299.54 where GD is still falling at 304.68.
                        // THIS is the branch that lv22's last divergence block
                        // runs through (2026-08-14, t=4,636, x=6,705, a cube one
                        // tick after the swing->cube portal, ramp uid4359
                        // (id1743, (6705,435), m=-1). The push-out is already
                        // RIGHT: line(6704.98) = 435.02 and 435.02 - (pH +
                        // slopeXOffset(m,pH)) = 413.81, which is GD's 413.802.
                        // What GD does and this does not is call it STANDING --
                        // it reads onGround=1 and JUMPS off the underside the
                        // next tick at vy=-2.390, while the model stays airborne
                        // and falls for 739 ticks.
                        // Setting `c.grounded = 1` here alone is a NO-OP
                        // (measured: 749 divergent ticks before and after): the
                        // flag does not survive, because the next tick's support
                        // scan only re-acquires a ramp when `!s.flip` and only
                        // when `s.onSlope` is already set -- neither holds. The
                        // three have to move together, and the `!s.flip` half is
                        // the one that took lv16 from CLEARED to STUCK once
                        // before, so it needs its own A/B.
                        // [2026-08-19 D7b] A flipped ship's hang acquisition is
                        // **the gravity mirror of the seat +1.0 snap**: lv16
                        // t=4,128, GD lifts a free climb 423.26 by +0.85 to
                        // lim=424.102 (= the hang line), og=1, vy=0. The model
                        // waited for the crossing and was 2 ticks late.
                        // The gate is the same shape as slopeland: mirrored
                        // downhill only (uphill in the world along travel), rising
                        // along gravity, within 1.0px short of lim, entry side
                        // +0.8.
                        // ...the **ride** after acquisition follows the line by
                        // the same lift (vy=0 while riding, so with only a vy>0
                        // gate the final +0.405 to 4,142's high-end corner
                        // (line(x1)-pH = 435.000) never appears -- GD stands at
                        // 435.000 until 4,146).
                        const bool hangSnap =
                            c.mode == 1
                            && ((double)c.vy > 0.0
                                || (s.onSlope && s.grounded))
                            && ((((double)K.dxF < 0.0 || s.rev != 0) ? -m : m)
                                > 0.01)
                            && ((((double)K.dxF < 0.0 || s.rev != 0))
                                    ? (rc <= x1 - 0.8) : (rc >= x0 + 0.8))
                            && (double)c.y <= lim
                            && lim - (double)c.y <= 1.0;
                        if ((double)c.y > lim || hangSnap) {
                            c.y = (float)lim;
                            // vy: TWO cases share this pin. A body moving AWAY
                            // from the surface (falling through from above,
                            // lv21 t=19,837, vy=-9.884) keeps its vy -- the
                            // original measurement. A body moving INTO it
                            // (the flipped body rising onto the ramp's
                            // underside -- its own floor) is LANDING and GD
                            // zeroes the velocity: lv16 first dual, t=7,755,
                            // p2 (flipped cube) hits uid3341's underside at
                            // exactly 574.900 with vy 12.23 -> 0, then rides
                            // the line down at vy=0 for 50+ ticks. The model
                            // kept the +12 and rocketed off at the release,
                            // which is where the dual section's plans and GD
                            // split (the model's p2 ran 5.7 px high in
                            // parallel and the spike-row deaths at t=8,80x
                            // were planned around a p2 world that was wrong).
                            // CUBE ONLY -- the measurement is a (dual) cube's.
                            // Letting the BALL take it froze lv22's rotated
                            // ball section at t=6,010 for 8 straight
                            // iterations (proof run 3): the ball's underside
                            // contact keeps its momentum (it bounces/rolls),
                            // and zeroing it erased the route the same build
                            // had cleared with in the morning.
                            // ...and DUAL only: both measurements are the
                            // dual p2's, and letting the single-player
                            // flipped cube of lv22's rotated complex take
                            // them (with the centre rule below) erased the
                            // route the same-day 44-iteration run had used
                            // (t=6,010 pin, proof runs 3-5). quick_regress
                            // cannot see rotated frames, so "no regressions"
                            // was blind here.
                            // [2026-08-18] The BALL is the same **in a non-rotated
                            // world**. The basis of the exclusion above was "lv22's
                            // rotated ball section (t=6,010) froze for 8
                            // iterations", and that is frame != 0. Measured lv16
                            // t=13,104 (a flipped mini ball hits uid6592's
                            // underside at 4.418): GD zeroes vy and just slides
                            // down the line for 50+ ticks. The model held the
                            // 4.547 for 1 tick.
                            // The frame==0 gate means "no positive evidence in
                            // rotated sections", preserving lv22's counter-example
                            // as is.
                            // [2026-08-19 D7b] The SHIP too: hitting the underside
                            // while **rising** is a landing = vy to 0 (lv16
                            // t=4,128: vy +6.35 -> 0.000, and vy stays 0 through
                            // the 13 riding ticks after -- the census edvy -6.400
                            // was the model holding the 6.4). The side falling
                            // away (lv20 uid26823, vy -9.884 kept) is outside the
                            // vy>0 gate, as before. frame==0 restriction for the
                            // same reason as the ball (no positive evidence in
                            // rotated sections).
                            // [2026-08-21 r72] The UFO too. The calibration rig
                            // ceilramp's 6 UFO cells (unit18-23, all |m| x mini)
                            // share the signature: y stays bit-identical to the
                            // line while vy alone accumulates +g (0.129, mini
                            // 0.152)/tick (GD is 0.000 throughout the ride). The
                            // pin here places y and grounding every tick, yet
                            // mode 3 was missing from vy's whitelist, and for
                            // flight modes the grounded flag does not stop gravity
                            // integration -- another mouth of the ship's hole.
                            // robot/spider are symptom-free because grounding
                            // stops their gravity.
                            // [2026-08-21 r88] **The hanging ride, too, runs the
                            // ship's ladder "only while pressing"** -- the mirror
                            // of the floor side's r39a (`c.mode==1 && input` ->
                            // acquisition 2.000 x dir, then its own g). The
                            // calibration rig **ceilhold** (ceilramp's geometry
                            // plus one press window) has all 6 ship cells
                            // (|m| 1/0.5/2 x normal/mini) in the same shape:
                            //   press-start tick   GD vy = **-2.000** exactly
                            //   after              -2.086 / -2.172 ...
                            //                      (mini -2.101 / -2.202 ...
                            //                       = that mode's own g itself)
                            //   from the release tick GD vy returns to **0.000**
                            // The corpus's lv18@3,010 (mini ship, m=-1, in=1,
                            // edvy -1.899) is this one tick of pressing.
                            // The ladder is left to its own integration
                            // (accumulating g from s.vy is bit-identical to GD).
                            // **The entry marker is s.vy==0** -- an unpressed ride
                            // keeps vy pinned at 0, so the same discriminator as
                            // the floor side works.
                            // [r89] The release-to-0 applies to the **UFO too**
                            // (the rig's 6 UFO cells: while pressing, plain decay
                            // from its own jump -6.871/-6.648 is bit-identical to
                            // GD, and on the release tick only GD returns to
                            // 0.000). A pressing UFO gets nothing = left to its
                            // own decay.
                            const bool contH =
                                s.onSlope && c.slopeM == (float)m;
                            bool flyHangRide = false;
                            if (!g_noHangLadder && c.mode == 1 && input) {
                                const double dirH =
                                    (m * (double)useDx >= 0.0) ? 1.0 : -1.0;
                                if (!(contH && s.vy != 0.0f))
                                    c.vy = (float)(2.0 * dirH);
                                flyHangRide = true;
                            } else if ((c.mode == 1 || c.mode == 3)
                                       && !input && contH) {
                                // Release returns to 0. **Limited to a ride
                                // continuation** -- a craft falling from above
                                // through the line (lv21 t=19,837's vy=-9.884)
                                // has s.onSlope unset, so as before.
                                c.vy = 0.f;
                                flyHangRide = true;
                            }
                            if (!flyHangRide
                                && ((c.mode == 0 && s.dual)
                                    || ((c.mode == 2 || c.mode == 1
                                         || c.mode == 3)
                                        && c.frame == 0))
                                && (double)c.vy > 0.0) c.vy = 0.f;
                            // ...and GD calls that STANDING (onGround=1 at
                            // t=4,636, then a jump at vy=-2.390). The flag only
                            // survives if the ramp is remembered too -- the
                            // next tick's support scan needs `s.onSlope` and,
                            // for a flipped player, a CEILING ramp (see there).
                            // [2026-08-21 r91] **A jump tick folds the ride.** The
                            // y step (the seat) is taken (GD's jump-tick y also
                            // follows the line -- the same asymmetry as the floor
                            // side's 9878), but keeping onSlope makes **the uphill
                            // launch overwrite the jump velocity on the next
                            // tick**. Calibration rig ceilhold's cube: GD decays
                            // under plain gravity, -13.031 -> -12.815, while the
                            // model wrote -7.405 (= slopeExitVy(1,cube)) on the
                            // next tick and fell back onto the ramp (dy 46px).
                            if (!impulsedThisTick) {
                                c.grounded = 1;
                                c.onSlope = 1;
                                c.slopeM = (float)m;
                                // the ramp the ride began on (State::slopeUid0)
                                c.slopeUid0 =
                                    s.onSlope ? s.slopeUid0 : sp->uid;
                                c.slopeUidNow = sp->uid;
                            }
                        }
                        continue;
                    }
                }
                if (!c.flip && ceilRamp) {
                    // The ceiling's contact point is the MIRROR of the floor's,
                    // so `smp` is the wrong window to open this test with. On
                    // lv20's (6195,325) 340->310 the mini ship reaches x=6182.6
                    // while the floor sample is still at 6178.9, left of the
                    // ramp -- the test was skipped, and by the next tick the
                    // ship was already 1.4 px past the limit, so the
                    // came-from-below gate failed too and the ride below lifted
                    // it 22.5 px onto the ramp's top. Ship only; the other
                    // modes keep the floor window.
                    const double rc = x + (m > 0 ? -slopeXOffset(m, pH)
                                                 : slopeXOffset(m, pH));
                    // TWO CONTACT REGIMES, with different timing (both
                    // measured on the 2026-08-17 references):
                    //  - IN-SPAN: the contact point rc is inside the ramp's
                    //    span; the LINE presses the ship. Resolves on the
                    //    tick that CROSSES the (moving) limit -- lv20
                    //    t=14,404 (uid12025, m=-0.5): s.y is 0.28 BELOW
                    //    lim(xPrev) and GD still snaps to lim=185.584 /
                    //    vy=-2.000 that tick, because the descending line
                    //    came down onto it. The old came-from-below gate is
                    //    exactly right here.
                    //  - CORNER: rc has left the span but the BOX still
                    //    overlaps the ramp's LOW corner (a V-valley: lv16
                    //    x=23,490, 570->540 meets 540->570). GD contacts
                    //    the corner at exactly corner-pH = 525.000, and
                    //    only on the tick whose START y is already inside
                    //    (t=15,366 sits 0.889 inside, still free; snap at
                    //    t=15,367). With the rc-only window this test was
                    //    skipped outright and the valley was bought by
                    //    fixups on every solve.
                    const bool inSpan = (rc >= x0 && rc <= x1 + 1.5);
                    // ...but the ramp's low END is only a CORNER if the line
                    // actually turns there. Two ceiling ramps of the SAME sign
                    // laid end to end share an edge and a y exactly like a
                    // V-valley does, and the shared-edge test in ceilLimAt
                    // above (which exists to EXTEND the line across that join)
                    // cannot tell them apart -- only the neighbour's SIGN can.
                    //
                    // lv16 t=3,281 (x=4,433) is the straight case: uid1077
                    // (4380..4440, 330->360) is continued by uid1065
                    // (4440..4500, 360->390), both m=+0.5, and the model called
                    // uid1065's left end a corner while the ship was still
                    // being carried by uid1077's line. It then fired the corner
                    // RELEASE and left at vy=-2.762 where GD simply free-falls
                    // from 0 (-0.069/tick, its own gravity). That is the whole
                    // of this family: the same three lines in fixcensus
                    // (lv16 t=3,281 / t=4,024, lv20 t=14,018).
                    //
                    // The real valley at lv16 x=23,490 is uid7606 (m=-1.0)
                    // meeting uid7640 (m=+1.0): opposite signs, so the surface
                    // just before the corner is ABOVE it and the ship really
                    // does separate there. No neighbour at all = an exposed low
                    // tip, which is a corner too -- that is why the default is
                    // "corner" and only a same-sign continuation cancels it.
                    auto cornerContinued = [&](void) {
                        const double edge = (m < 0) ? x1 : x0;
                        const double edgeY = (m < 0) ? sp->sy1 : sp->sy0;
                        for (const Obj* nb : *K.slopes) {
                            if (nb == sp || nb->slopeHazard) continue;
                            const double nx0 = nb->cx - nb->hw;
                            const double nx1 = nb->cx + nb->hw;
                            if (nx1 <= nx0) continue;
                            const double nm = (nb->sy1 - nb->sy0) / (nx1 - nx0);
                            if (nm == 0.0) continue;
                            if (!slopeIsCeiling(nb->slopeDir)) continue;
                            // meets at the same edge, at the same height, and
                            // keeps going the same way -> no turn, no corner
                            const bool joins = (m < 0)
                                ? (std::abs(nx0 - edge) <= 0.5
                                   && std::abs((double)nb->sy0 - edgeY) <= 0.5)
                                : (std::abs(nx1 - edge) <= 0.5
                                   && std::abs((double)nb->sy1 - edgeY) <= 0.5);
                            if (joins && (nm > 0.0) == (m > 0.0)) return true;
                        }
                        // [2026-08-19] **A flat solid also continues the ceiling.**
                        // Even when the neighbour is not "a ramp of the same
                        // sign", if a **plate with an underside at the same
                        // height** abuts the low end, the ceiling is not
                        // interrupted and the player does not separate from the
                        // face there = not a corner.
                        // Measured lv18 t=8,461 (upright ship, sp0.9): flat slab
                        // uid5599 (box (11310,360,30,14), underside 360, right
                        // edge 11,340) sits exactly against the low end of ceiling
                        // ramp uid5597 (sy0=360 @ x0=11,340, sdir=3). GD stops at
                        // y=345.000 (=360-pH) with vy=0; the next tick is plain
                        // gravity (-0.069) only.
                        // The model read the low end as "an exposed corner" and
                        // fired the corner release (-2.762) -- the "neighbour is a
                        // plate" case of the same family as lv16 t=3,281 above.
                        if (K.near) {
                            for (const Obj* nb : *K.near) {
                                if (nb->type != 0) continue;
                                const double face = nb->cy - nb->hh;
                                if (std::fabs(face - edgeY) > 0.5) continue;
                                const bool touches = (m < 0)
                                    ? (std::fabs((nb->cx - nb->hw) - edge) <= 0.5)
                                    : (std::fabs((nb->cx + nb->hw) - edge) <= 0.5);
                                if (touches) return true;
                            }
                        }
                        return false;
                    };
                    const bool cornerReg = (c.mode == 1) && !inSpan
                        && (m < 0 ? (rc > x1 && (double)x - pH <= x1 + 0.5)
                                  : (rc < x0
                                     && (double)x + pH >= x0 - 0.5))
                        && !cornerContinued();
                    // [2026-08-21 r41] **The cube is gated on the mirrored contact
                    // point too.** The note at 8386 above says "the ceiling's
                    // contact point is the **mirror** of the floor's, so smp is
                    // the wrong window to open this test with", yet only the ship
                    // was fixed and the cube stayed on `smp.ok` (the floor-side
                    // window). The floor-side r is off from the ceiling-side rc by
                    // 2*slopeXOffset = 18.5px, so **the floor window stays open
                    // even after the contact point exits the span**.
                    // Measured lv18 (upright cube, ceiling ramp uid13979
                    // span [27,750, 27,780], line 450->390, pH=15, dx=1.613):
                    //   t=20,490 rc=27,778.89 (in span)  GD and the model both clamp
                    //   t=20,491 rc=27,780.51 (**0.51 outside**); the floor-side
                    //     r=27,761.97 is still inside the span -> only the model
                    //     grabs and pulls down 1.178px to lim=375.000
                    //     (=line(x1)-pH). GD free-falls (376.182 = 377.213 -
                    //     0.225x4.580). A parallel -1.182 remains to the end
                    //     (census `m0/.../sp1.1/air` edy +1.179)
                    // The exit grace 1.5 goes too: the push-out is a geometric
                    // constraint, not a ride, and here it is a constant smaller
                    // than one tick's movement (1.613), so it can only ever "grab
                    // one extra tick GD has let go".
                    // **The window stays `smp.ok`**. This branch not only clamps
                    // but `continue`s at the end to **suppress the ride below**,
                    // so tightening the window makes the ceiling ramp be grabbed
                    // as a floor (the r41 measurement: at lv18 t=20,491 the model
                    // bounced 65px up to 441.064 = line+pH -- exactly the
                    // "came-from-below gate falls too and the ride lifts onto the
                    // ramp's top" that the 8386 note foretold).
                    const bool ceilOk = (c.mode == 1)
                        ? (inSpan || cornerReg) : smp.ok;
                    // [2026-08-21 r42] ...but **the push-down itself** applies only
                    // while the contact point is inside the span. The cube opens
                    // on the floor-side window (`smp.ok`), and the floor's r is
                    // off from the ceiling's rc by 2*slopeXOffset (18.5px here),
                    // so **the push-down keeps going after rc exits the span**.
                    // Measured lv18 (upright cube, uid13979 span
                    // [27,750, 27,780], line 450->390, pH=15, dx=1.613):
                    //   t=20,490 rc=27,778.89 (inside)   GD and the model to 377.21
                    //   t=20,491 rc=27,780.51 (0.51 out) GD free-falls
                    //     (376.182 = 377.213 - 0.225x4.580); only the model pulls
                    //     down 1.178px to lim=375.000 (=line(x1)-pH), and a
                    //     parallel -1.182 remains to the end
                    //   (census `m0/mini0/g0/gdg0/sp1.1/air` edy +1.179)
                    // The ship is measured separately via inSpan/cornerReg, so it
                    // is untouched.
                    const bool rcInSpan = (rc >= x0 && rc <= x1);
                    const double lim = ceilLimAt(x);
                    // The tolerance was 0.001 and that is far too tight for a
                    // player that is ALREADY pressed against the ceiling: it sits
                    // exactly at the limit, so any residual between GD's y and
                    // this formula's throws the gate. Measured on lv16 t=15,270
                    // (ship, sp1.1, ramp uid7585 (23355,675) sy 690->660): GD has
                    // y=664.4371 and ceilLimAt(xPrev)=664.4352 -- the gate missed
                    // by **0.0019 px**, the model free-fell (dy -0.44 against GD's
                    // -1.61 = the line's own slope) and never re-acquired. That
                    // one tick is 26 of lv16+lv20's fixups.
                    // 0.5 was the first try and it OVER-GRABS. The case this gate
                    // exists to exclude (a player riding the ramp's TOP) is
                    // 2*pH = 30 px away, so half a pixel looked free -- but the
                    // player that is merely PASSING under a ceiling ramp comes
                    // within half a pixel of the line all the time. Measured on
                    // lv16 x=4563..4581 (ship, ramps uid1093/1103 sdir 1/3): with
                    // 0.5 the model acquires the ramp and slides while GD flies
                    // on (gdg0 against the model's g1), costing 13 fixups one per
                    // tick across iterations 3-6 and 57 iterations for the level.
                    // 0.05 is 26x the residual this gate has to survive (0.0019)
                    // and 10x tighter than the over-grab: lv16 27 iters / 44
                    // fixups, lv19 26->19, lv21 34->21, lv20 28->37, 21/21 green.
                    // TRIED (2026-08-14) and it is a NO-OP: adding `!c.flip`
                    // here, on the theory that a flipped player hanging under a
                    // ceiling ramp is standing on it and must not be skipped.
                    // The lv22 case it was aimed at (t=4,636, x=6,705, a cube
                    // one tick after the swing->cube portal, ramp uid4359
                    // (6705,435) m=-1) is NOT thrown out here: the replay's
                    // divergence count was 749 ticks before and after, and the
                    // model's y at that tick already equals GD's 413.802.
                    // What is missing there is the GROUNDING, not the height --
                    // GD reads onGround=1 and leaves at vy=-2.390 the next tick
                    // while the model stays airborne at vy=+0.447. The ramp
                    // stops being a candidate at some OTHER gate; `slopeset`
                    // lists uid4359 and neither `slopecand` nor `slopewin`
                    // prints it.
                    // GD resolves the press on the tick whose START position
                    // is already inside the solid, one tick after the move
                    // that crossed the line: the 2026-08-17 lv16 reference
                    // has y=525.889 (0.889 INSIDE) at t=15,366 still free,
                    // and the snap to 525.000 / vy=-2.000 at t=15,367. The
                    // old `s.y <= lim + 0.05` fired on the crossing tick
                    // itself (one early) AND would grab a ship passing just
                    // under the line; requiring s.y to be AT or INSIDE the
                    // limit keeps the ride ticks (s.y == lim exactly) and
                    // the 0.0019 px residual case, and the +2.0 bound (max
                    // penetration is 0.225*8 = 1.8) keeps excluding a ship
                    // riding the ramp's TOP, 2*pH away.
                    const double limPrev = ceilLimAt(xPrev);
                    // Start-inside timing is the CORNER regime's; in-span
                    // (and every non-ship mode) keeps the old
                    // came-from-below gate. Using start-inside for in-span
                    // broke lv20 t=14,404; using came-from-below for the
                    // corner fired the valley one tick early.
                    const bool pressGate = cornerReg
                        ? ((double)s.y >= limPrev - 0.05
                           && (double)s.y <= limPrev + 2.0)
                        : ((double)s.y <= limPrev + 0.05);
                    if (ceilOk && pressGate) {
                        if ((double)c.y > lim
                            && (c.mode == 1 || rcInSpan)) {   // r42: see the rcInSpan note
                            c.y = (float)lim;
                            // ...and GD does NOT stop the player dead here: it
                            // SETS the velocity to the ride's own +-2.000, the
                            // same number the swing's ordinary ride is set to on
                            // acquisition (see the `2.0 * dir` below) and with
                            // the same sign rule -- it follows the SURFACE.
                            // The `vy > 0` gate is what makes this the
                            // ACQUISITION tick: the player is still rising into
                            // the line, and every later tick of the ride comes
                            // in already descending along it.
                            // Measured on lv16 (GD's own dump for
                            // solution_lv16A, ship, sp1.1, ramp uid7606
                            // (23475,555) sy 570->540, slopeDir 1):
                            //   t=15,319  vy = +6.355  (free flight, rising)
                            //   t=15,320  vy = -2.000  <- SET, y on the line
                            //   t=15,321  vy = -2.069  (its own gravity from there)
                            // The model wrote 0 and then free-fell from 0, so it
                            // ran the whole 37-tick ride 2.0 of velocity light
                            // and left the ramp with the wrong number.
                            // SHIP ONLY -- that is where it is measured, and the
                            // other modes say it is not universal. Applying the
                            // 2.000 to every mode costs follow length in three
                            // places at once (quick_regress, this build):
                            // lv16 t=3,000 400->277, lv18 t=8,200 400->314,
                            // lv22 t=600 400->333 -- all of them cube/ball
                            // sections, i.e. exactly the family the generic
                            // ride's `c.vy = 0` was measured on.
                            if ((double)c.vy > 0.0) {
                                const double dir =
                                    (m * (double)useDx >= 0.0) ? 1.0 : -1.0;
                                c.vy = (c.mode == 1 && dir < 0.0)
                                           ? (float)(2.0 * dir) : 0.f;
                                CLAMP0("slope/ceillim");
                            }
                        // [2026-08-21 r78] **While still being pressed on the same
                        // tick, do not fire the valley's corner release.** lv20
                        // t=18,835 (ship, W-shaped chain, sp1.1): uid15572's
                        // (m=-1) exit-continuation window is still pressing on
                        // this tick (ceilPressedNow; GD's release is r66's
                        // ceil/release emitting -2.877 the next tick, 18,836 --
                        // the model matches), yet the corner of the next uphill
                        // ramp uid15570 alone caught cornerReg and double-fired
                        // -2.762 one tick early (census
                        // `m1/mini0/g0/gdg0/sp1.1/air/in1` edvy +0.840 =
                        // 2.762-1.922, a half y step 0.189 to the end).
                        // The original measurement site lv16 t=15,368 (V valley)
                        // fires on the tick the press has ended, so
                        // ceilPressedNow=0 there -- unchanged.
                        } else if (cornerReg && !ceilPressedNow
                                              && (double)s.vy >= -2.05
                                              && (double)s.vy <= 0.05) {
                            // RELEASE tick: the press ends (the ceiling no
                            // longer reaches the free c.y) and GD SETS the
                            // velocity to -2.762 AFTER the move -- the y of
                            // this tick still steps with the lift-updated vy.
                            // Measured on the 2026-08-17 lv16 reference,
                            // t=15,368 (the valley at x=23,490): y goes
                            // 525.000 -> 524.574 = 0.225*(-1.892), i.e. the
                            // ordinary -2.000+0.108 step, while vy comes out
                            // -2.762. The fixup families say the same number
                            // regardless of the press's own vy or the button:
                            // vy=-2 -> -2.762 (dvy -0.762, in0 AND in1) and
                            // vy=0 -> -2.762 (dvy -2.762) across the y=585 /
                            // 525 / 475.9 clusters. Gated to the measured
                            // pressed band [-2.05, +0.05] -- a ship separating
                            // at some other speed is unmeasured. Speed/mini
                            // scaling UNVERIFIED (all measurements are sp1.1
                            // full-size).
                            c.vy = -2.762f;
                        }
                        // ...and this ramp is now DONE with this player. The
                        // ride below must not run: its `insideSolid` branch
                        // assumes the solid is under the line and would lift a
                        // player that came from below onto the ramp's top
                        // (measured here: the clamp put the mini ship at
                        // line-12.73 and the ride then teleported it to
                        // line+12.73, 25 px up). The skip is inside the
                        // came-from-below gate on purpose -- doing it for every
                        // ceiling ramp is what took lv16 from 17 iterations to
                        // STUCK, because a player ABOVE such a ramp rides it
                        // perfectly ordinarily.
                        continue;
                    }
                }
                // ...and print the ones the WINDOW throws away too. The
                // `slopecand` line below sits after this `continue`, so a ramp
                // rejected here left no trace at all -- lv22 t=4,633 spent a
                // diagnosis cycle looking like "the model never sees the ramp".
                // [2026-08-19 corner-stand] While a grounded cube's tip touches
                // the span of a slope that is "downhill in the travel direction",
                // and its centre is still short of the entry-side end, GD **stands
                // it on the top corner (the entry-side line value + pH)**.
                // gdref lv16 measured t=7,048-7,057: a cube walking the slab (top
                // 449.95) sits at exactly y=465.000 from the tick its right edge
                // touches uid3026 ([10530,10560], line 450->420), and returns to
                // the slab's 464.95 on the tick the centre crosses x0 (with a
                // 1-tick fall of vy=-0.215).
                // Item 10's "0.05px sunk-in" (t=6,231 and others) is this corner
                // being missed. The step-height cap 1.0 is the same thematic
                // constant as the acquisition tolerance (only 0.05 is measured;
                // the upper side is unconstrained -- if layer 4 rejects, suspect
                // this). Corner contact from the air is a separate rule (lv20
                // t=5,269's "no landing until penetration > 1.000"), hence the
                // s.grounded gate.
                if (ridesTop && !c.flip && c.mode == 0 && s.grounded
                    && !s.onSlope) {
                    const bool revC = ((double)K.dxF < 0.0 || s.rev != 0);
                    const double mTravC = revC ? -m : m;
                    const double cxE = revC ? x1 : x0;
                    const double cyE = sp->sy0 + m * (cxE - x0);
                    const bool edgeIn = revC
                        ? (x > cxE && x - pH <= cxE + kContactEps)
                        : (x < cxE && x + pH >= cxE - kContactEps);
                    if (mTravC < -0.01 && edgeIn) {
                        const double d = (cyE + pH) - (double)c.y;
                        if (d > 0.0005 && d <= 1.0) {
                            if (g_slopeDbg)
                                std::printf("slopedbg t=%lld x=%.2f uid=%d "
                                            "cornerstand y=%.3f (d=%.3f)\n",
                                            (long long)K.t, x, sp->uid,
                                            cyE + pH, d);
                            c.y = (float)(cyE + pH);
                            c.vy = 0; c.grounded = 1;
                            continue;
                        }
                    }
                }
                if (g_slopeDbg && !smp.ok)
                    std::printf("slopewin t=%lld x=%.2f uid=%d m=%.3f "
                                "x0=%.1f x1=%.1f xr=%.2f ridesTop=%d ceil=%d "
                                "REJECTED-BY-WINDOW\n",
                                (long long)K.t, x, sp->uid, m, x0, x1, smp.xr,
                                ridesTop ? 1 : 0, ceilRamp ? 1 : 0);
                // [2026-08-20 r33] **Even when the window drops it, push out if
                // the box is penetrating the ramp's solid.** On 2026-08-19 two
                // "widen the window itself" proposals were tried and rejected
                // (families 27->44 / 27->31; the additions are **over-retention**
                // of `slope+0.50|+1.00/ride...`), and as that conclusion said,
                // what GD does here is a **push-out from the solid**, not a ride
                // continuation, so the rule is needed only on the acquisition
                // side. Over-retention is closed structurally by `!s.onSlope` --
                // a player already riding some ramp never reaches this branch.
                // Measured lv18 t=20,465 (after r32 made the size portal fire on
                // the same tick and the box became 30x30. GD's `slp:` line names
                // uid13971): the centre 27,732.52 is 12.5px past the high end of
                // uid13971 (m=+2, span [27,690, 27,720]), but the box's left edge
                // 27,717.5 is still inside the span and the box bottom 361.64 is
                // below the face (390 at the end) = inside the solid. GD stands
                // it, y 376.641 -> 405.000 (= line(x1) + pH), and emits the m=2
                // launch 5.226 the next tick.
                // Requires **the centre to have already passed "the high end seen
                // along the travel direction"**. The mirror of the existing
                // corner-stand (standing on the entry-side corner); this one is
                // **the exit-side corner**. It is the gate that keeps from
                // grabbing a ramp being merely approached from before it, and the
                // measured pair is:
                //   lv18 t=20,465  m=+2 travel+ -> high end x1=27,720, centre
                //                  27,732.52 = passed -> GD stands it on the
                //                  corner (390+pH)
                //   lv16 t=10,802  m=-1 travel+ -> high end x0=16,590, centre
                //                  16,576.80 = still short -> GD names uid5229 in
                //                  its `slp:` line while never moving y a single px
                // (penetration cannot split them: lv16's is 2.291px, over 1.0)
                const double sgnPO =
                    ((double)K.dxF < 0.0 || s.rev != 0) ? -1.0 : 1.0;
                const double xHiPO = (m > 0) ? x1 : x0;
                bool okHere = smp.ok;
                if (!okHere && ridesTop && !c.flip && !ceilRamp && !s.onSlope
                    && (x - xHiPO) * sgnPO > 0.0
                    && x + pH > x0 && x - pH < x1) {
                    const double lineHere =
                        sp->sy0 + m * (std::min(std::max(x, x0), x1) - x0);
                    // [2026-08-22 r98] Penetration is measured with **the y from
                    // before this tick's landing**. Measured lv16 t=17,627: under
                    // a falling cube, a 1.5px slab (top 599.95) and ramp uid8864's
                    // high-end corner (600.000) overlap, and once **the solids
                    // loop that runs first seats it on the slab**, only 0.05 of
                    // penetration remains here and the 1.0px gate fails (GD stands
                    // at 615.000 = corner + pH and emits, the next tick, the m=0.5
                    // launch x the 1-tick-ride factor 0.4 = **+1.989**). With the
                    // pre-landing y there is 1.17px and it passes.
                    const double yPO = landedThisTick ? (double)preLandY
                                                      : (double)c.y;
                    const double pLo = std::max(yPO - pH,
                                                (double)(sp->cy - sp->hh));
                    const double pHi = std::min(yPO + pH, lineHere);
                    // **No push-out until the penetration exceeds 1.0px.** Merely
                    // touching a corner from the air does not land in GD -- the
                    // same constant already in the ball's corner landing (the
                    // ballCornerM note, worker98's bisection 0.9955/1.0055, same
                    // origin as preSlopeCollision's 1px end strip). Put in at
                    // 0.001, the census went 23->24, and the 3 added items' edy
                    // were exactly the then-current penetration amounts:
                    //   lv16 t=10,802 pen 0.362 / lv20 t=5,269 pen 0.230
                    //   (the lv20 one is the ball's corner landing itself, which
                    //    this branch was hijacking from the existing rule)
                    // lv18 t=20,467's ridge has pen 28.359, so it passes.
                    if (pLo < pHi - 1.0) {
                        okHere = true;
                        // **The ridge is decided by "the highest face under the
                        // box".** A single ramp's rotated-box formula cannot
                        // handle an apex: lv18's uid13971 (uphill) and uid14050
                        // (downhill) form an apex at x=27,720, and with the window
                        // merely opened, the later-coming 14050's seat 398.50 wins
                        // and falls 6.5px short of GD's 405 (r32's census kept
                        // exactly that value).
                        double best = -1e18;
                        for (const Obj* nb : *K.slopes) {
                            if (nb->slopeHazard) continue;
                            const bool nceil = (c.mode == 1 || c.mode == 7
                                                || c.mode == 0)
                                ? slopeIsCeiling(nb->slopeDir)
                                : (nb->slopeDir == 1 || nb->slopeDir == 3);
                            if (nceil) continue;
                            const double a0 = nb->cx - nb->hw;
                            const double a1 = nb->cx + nb->hw;
                            if (a1 <= a0) continue;
                            if (x + pH <= a0 || x - pH >= a1) continue;
                            const double nm = (nb->sy1 - nb->sy0) / (a1 - a0);
                            if (nm == 0.0) continue;
                            const double sfc = nb->sy0
                                + nm * (std::min(std::max(x, a0), a1) - a0);
                            // a face sticking out above the box is not a ride surface
                            if (sfc > (double)c.y + pH) continue;
                            if (sfc > best) best = sfc;
                        }
                        if (best > -1e17) {
                            pushOutSeat = true;
                            pushOutTop = best + pH;
                        }
                        if (g_slopeDbg)
                            std::printf("slopepush t=%lld x=%.2f uid=%d m=%.3f "
                                        "line=%.3f y=%.3f ridge=%.3f\n",
                                        (long long)K.t, x, sp->uid, m,
                                        lineHere, (double)c.y, pushOutTop);
                    }
                }
                if (!okHere) continue;
                const double xr = smp.xr;
                // `top` is the resting y: the surface plus the player's half in
                // the PLAYER's frame, so a flipped player hangs under a ceiling
                // ramp at surface - pH.
                // ...and the SWING hangs at the ROTATED-box distance, not at a
                // flat pH. Measured on lv22 uid4359 at t=4,627 (centre
                // 6,691.02, line 448.98): GD rests it at 427.763, i.e.
                // line - 21.21 = line - pH*sqrt(1+m^2) -- the same quantity the
                // upright ride uses, and 6.21 px away from the flat line - 15
                // the ship's branch was measured with (lv16 t=4,111).
                // ...the BALL's hang is the rotated amount too (2026-08-19). lv16
                // t=13,108-13,115: a flipped mini ball slides along the underside
                // of uid6592 (sdir1, line 622->592, m=-1) at line - 12.728 =
                // line - 9*sqrt(2) = line - pH*sqrt(1+m^2) (mini pH=9), and at the
                // end emits slopeExitVy(1) x (13/24) x sp = 3.741 in the flipped
                // direction (the exit machinery's existing flip handling emits it
                // as is). The ceilpushball rig's upright push-down pin is the same
                // amount (16.771/21.213).
                const double hangOff =
                    // [r71b] swing removed from the rotated distance (xr is now
                    // the contact point, so flat pH -- same as the old formula at
                    // m=1, splits first at m=2)
                    (c.mode == 2 && gs < 0.0)
                    ? (pH + std::fabs(m) * slopeXOffset(m, pH))
                    : pH;
                double top = sp->sy0 + m * (xr - x0) + gs * hangOff;
                if (c.mode == 2 && gs < 0.0) {
                    // The hang's low end is bounded by the **flat** at
                    // line(end) - pH (lv16 t=13,116: 583.000 = sy1 - 9, not the
                    // rotated amount 581.6).
                    const double lowLine = (m < 0)
                        ? (sp->sy0 + m * (x1 - x0)) : sp->sy0;
                    top = std::max(top, lowLine - pH);
                }
                // [r33] On a tick pushed out at the ridge, no ramp's seat drops
                // below the apex height (measurements at pushOutSeat's declaration).
                if (pushOutSeat && !c.flip) top = std::max(top, pushOutTop);
                // ...and it only counts as "a ramp still has me" if its resting
                // height is where the player actually is. The window alone is
                // far too coarse: at lv16 t=6516 the player runs off the top of
                // the 330->360 ramp at (9645,345) while the 300->330 one at
                // (9675,315) -- 30 px lower and never touched -- still passes
                // the window and suppressed the launch.
                // ...AND the sample point has to be genuinely inside the ramp.
                // `sampleAt` allows 1.5 px past the right edge so that the ride
                // survives the last tick on the surface, and that slack made the
                // EXIT tick look like a ramp that still holds the player: the
                // sample is clamped back to x1, so `top` is exactly the top of
                // the ramp, which is exactly where the player is. The launch was
                // then suppressed and a jump taken on that tick stood instead.
                // Measured on lv18 t=10,640 (full-size cube, speed 0.9, the
                // 60x30 ramp 674 at (14,520,225) 210->240): GD leaves at
                // vy = 3.999 -- the ramp's own exit for m=0.5 -- and the model
                // left at 11.180, the cube's jump. That is the whole x=14,673
                // wall: 7 vy of difference on the tick the corridor starts.
                // |useDx|: in a reverse section useDx is negative and the old
                // form made the bound negative, so the window (and the stick
                // below) could never hold -- one of the forward-only signs the
                // reverse family keeps finding.
                // [2026-08-18] **A convex seam is a launch, not a connection.** At
                // a seam in an uphill chain where the next ramp becomes
                // "shallower" (convex), GD fires the previous ramp's launch as
                // is. Measured lv22 t=18,583 (reverse travel, mini robot sp1.1):
                // m=-1 (uid16681) joins exactly onto the end of m=-2 (uid17074),
                // yet GD takes off at vy=13.064 (= the m2 launch 10.507 x
                // sp1.243) while the model spliced onto the next ramp and kept
                // climbing.
                // The floor-side mirror of the ceiling ramps' "corner vs seam"
                // (same day's findings): convex (next is shallower) = the face
                // folds away under the foot = launch; concave / equal gradient =
                // connection.
                // This skip must come **before** rampWindowHere -- a shallow-seam
                // ramp not only (a) is no transfer target but also (b) must not
                // act as the launch-suppression window (rampWindowHere). Placed
                // after, (b) remained and the launch was silently suppressed.
                // Only the floor side / upright was measured, so limited to
                // ridesTop && !flip. Equal gradient (difference under 0.01) still
                // connects as before. sgnT is the true travel direction (useDx is
                // always positive -- the rev note at sampleAt).
                {
                    const double sgnT =
                        ((double)K.dxF < 0.0 || s.rev != 0) ? -1.0 : 1.0;
                    const double mTravOld = (double)s.slopeM * sgnT;
                    const double mTravNew = m * sgnT;
                    if (s.onSlope && ridesTop && !c.flip
                        && mTravOld > 0.01 && mTravNew > 0.0
                        && mTravNew < mTravOld - 0.01)
                        continue;
                }
                // [2026-08-21 r68] **At a hang's equal-gradient seam, do not move
                // to the neighbour until the previous ramp lets go.** Calibration
                // rig ceilramp unit0 (flipped ship, m=-1 3-ramp chain) measured:
                // GD keeps sliding on ramp1's line until t=551, when the contact
                // point (x+xoff = 717.6..718.9) exits ramp1's span, but at t=549
                // the model's neighbour (uid2037) became a landing candidate at
                // **the approach-side corner value (line(x0)-pH = 245.000)**,
                // stole the ride, and froze the line for 2 ticks (GD 247.34 /
                // M 245.00). The lines are colinear, so this skip is
                // value-neutral -- it removes only the corner-value theft.
                if (!ridesTop && s.onSlope && s.slopeUidNow >= 0
                    && s.slopeUidNow != sp->uid
                    && std::fabs((double)s.slopeM - m) < 0.01) {
                    const double xoffH = slopeXOffset(m, pH);
                    const double rcH2 = x + ((m > 0) ? -xoffH : xoffH);
                    bool prevHolds = false;
                    for (const Obj* pv : *K.slopes) {
                        if (pv->uid != s.slopeUidNow) continue;
                        const double px0 = pv->cx - pv->hw;
                        const double px1 = pv->cx + pv->hw;
                        const bool adj = std::fabs(px1 - x0) <= 0.5
                                      || std::fabs(x1 - px0) <= 0.5;
                        if (adj && rcH2 >= px0 && rcH2 <= px1)
                            prevHolds = true;
                        break;
                    }
                    if (prevHolds) continue;
                }
                if (smp.inside && std::fabs(top - (double)s.y)
                                      <= 3.0 * std::fabs((double)useDx))
                    rampWindowHere = true;
                // Only ride it if the player came from ON or ABOVE the surface.
                // Without this a ship flying INTO the slope from below gets
                // lifted onto it, which GD does not do -- it dies. Measured as
                // a regression: lv18's wall went backwards from x=5,115 to
                // x=3,716 the moment the ship branch got the unconditional
                // version, because the search then planned through the ramp.
                // sampleAt already clamps to [x0,x1] on the normal path. The outer
                // re-clamp was redundant and killed the prevTop of the
                // extrapolated ride continuation (the new branch above), so it was
                // removed (2026-08-19).
                const double xrPrev = sampleAt(xPrev).xr;
                const double prevTop = sp->sy0 + m * (xrPrev - x0) + gs * pH;
                // Came from the wrong side? Normally not a ride -- but if the
                // player is INSIDE the ramp's solid slice, GD lifts it out onto
                // the surface rather than letting it through, and that is the
                // whole reason a plain ramp does not need to kill.
                // Measured on lv16's untwinned (6708,293), line 278->308, whose
                // resting height at x=6708 is 308+... = 311.385: the ball
                // injected at y = 262, 270, 278, 285, 293, 300 and 308 -- from
                // under the box to deep inside the triangle -- comes out at
                // y=311.385, vy=0, onGround=1 in EVERY case.
                // Dropping the kill without this made the model plan straight
                // through ramps and took lv16 from x=11,895 back to x=5,024.
                // (the lift only applies to the upright case -- a flipped
                // player under a ceiling ramp is BLOCKED, not pushed through,
                // and its resting height is already what `top` says)
                const double sLo = std::max((double)c.y - pH, sp->cy - sp->hh);
                const double sHi = std::min((double)c.y + pH,
                                            sp->sy0 + m * (std::min(std::max(x, x0), x1) - x0));
                // ...and the slice it tests -- box floor up to the line -- is
                // the FLOOR ramp's solid half. On a ceiling ramp that region is
                // empty air, so a ship flying through it was "lifted out" onto
                // line + pH, 22.5 px above where GD keeps it (lv20 t=4761,
                // y 326.0 -> 348.5 with grounded set, then sliding down the
                // wrong face at dx/tick). Ship only; the other modes keep the
                // slice they were validated with.
                // ridesTop, not !c.flip: the "solid is under the line" slice is
                // the FLOOR ramp's, and that is exactly the case an inverted
                // player standing on one is in.
                // [2026-08-21 r43] **The ceiling ramp's "solid slice" is not used
                // for any mode.** This test looks at "box bottom up to the line =
                // the floor ramp's solid side", and on a ceiling ramp that region
                // is air. Only the ship had been excluded, but the cube had the
                // same hole: the moment r42 correctly cut off the ceiling
                // push-down, the ride grabbed that ramp as a floor and at lv18
                // t=20,492 **bounced 62.7px up** to 437.838 (=line+pH).
                // (The same picture the 8386 note foretold for the ship)
                const bool insideSolid = ridesTop && !ceilRamp
                    && (x + pH > x0 && x - pH < x1) && (sLo < sHi - 0.001);
                // --slopedbg also prints the ramps that were CONSIDERED and
                // rejected. Without it the flag only ever shows acquisitions,
                // so "the model never even looks at this ramp" and "it looks
                // and throws it away" are indistinguishable -- which is exactly
                // where lv22's uid5430 (m=2) stalled the diagnosis.
                if (g_slopeDbg)
                    std::printf("slopecand t=%lld x=%.2f uid=%d m=%.3f "
                                "sy=%.3f prevTop=%.3f top=%.3f y=%.3f sy0=%.3f "
                                "gs=%.0f inside=%d ridesTop=%d ceil=%d "
                                "reject=%d\n",
                                (long long)K.t, x, sp->uid, m, (double)s.y,
                                prevTop, top, (double)c.y, sp->sy0, gs,
                                insideSolid ? 1 : 0, ridesTop ? 1 : 0,
                                ceilRamp ? 1 : 0,
                                (!insideSolid
                                 && ((double)s.y - prevTop) * gs < -kLandTol)
                                    ? 1 : 0);
                if (!insideSolid && ((double)s.y - prevTop) * gs < -kLandTol)
                    continue;
                // A ship lands on a ramp from slightly ABOVE it, exactly as it
                // does on a flat surface. Measured on lv16
                // t=3027: the ship is 0.63 px above the ramp's surface and GD
                // puts it down at 239.770 = the surface + 15; the strict test
                // left the model falling and it was 6.4 vy out one tick later.
                // ...but only DOWNWARD. The tolerance applied to a ceiling pins
                // the ship 6 px before its head gets there. Measured on lv16
                // t=4108: GD is at y=399.0 still rising freely and the model
                // had already clamped to 405.0 (= the ramp's 420 minus 15).
                // Direction gate, UFO ONLY (2026-08-05, second attempt): GD
                // leaves a RISING UFO arcing above the line alone (lv19's
                // staircase, 5.15 px at x=20,631/t=14,585, rejoins by itself 4
                // ticks later) -- grabbing it built the whole x=21,386 wall
                // and burned the fixup budget one delta per tick. The SHIP
                // keeps the unconditional grab: gating it too is what broke
                // lv16 (stuck at x~4,909 -- its ships genuinely settle onto
                // ramps while rising). Mode is the distinguishing condition
                // we have measurements for on both sides; if a rising-ship
                // arc ever shows up, this needs a finer rule.
                // TRIED AND REVERTED (2026-08-19): "leave a riser alone only when
                // more than 1.0 above the line" -- aimed at lv20 t=21,983 (GD
                // grabs a rising UFO 0.55px above), but the target site did not
                // move (the grab goes through a different path entirely) and it
                // wrongly grabbed the rising UFO at lv19 t=14,936, regressing
                // 400->336. 21,983 is a different mechanism (still unexplained).
                const bool ufoRising =
                    (c.mode == 3) && (double)c.vy * gs > 0.0;
                // kShipLandTol (6.0) is the LEDGE sweep's penetration limit;
                // as an above-the-surface allowance on an ASCENDING ramp it
                // grabs a falling ship 2-3 ticks before GD, because the
                // resting height climbs 1.6 px/tick into the gap. Measured
                // on the 2026-08-17 lv16 reference, t=15,521..15,523
                // (uid7702, m=+1): the model snapped DOWN 4.7 px onto the
                // ramp at a 3.78 px gap while GD stays free until its own y
                // sinks below the rising line (lands t=15,523 at 464.04,
                // vy=0). The early land also poisons the slope-exit charge:
                // 23/24 ride ticks against GD's 21/24 = the +0.767 cube
                // launch at t=15,544.
                // ...and 0.65 was still too generous. It was kept for a
                // "0.63 above and GD lands anyway" reading at lv16 t=3027, but
                // that tick no longer involves a ramp at all in the current
                // geometry (model and GD agree there to the digit, both free
                // at vy=-4.651) -- the reading was an artefact of an older
                // `top`, from before slopeXOffset and the ceiling-ramp work.
                // What is measurable NOW says the ASCENDING ramp takes NO
                // allowance at all: lv20 t=14,018 (uid11804, m=+0.5, resting
                // centre 211 at the ramp's right end) has the ship 0.1955 px
                // above and GD stays free, landing the next tick when its own
                // free y sinks 0.331 px THROUGH the line. 0.001 reproduces
                // both ticks exactly.
                // kShipLandTol (6.0) stays for the DESCENDING side: there it
                // is a penetration limit (the line drops onto the ship), which
                // is a different quantity from an above-the-surface gap.
                // [2026-08-28] ...and that reading of it is wrong. The test this
                // feeds is `(c.y - top) * gs <= landAllow`, in which penetration
                // is the NEGATIVE side and passes whatever the constant is; all
                // 6.0 buys is a grab from up to 6 px ABOVE a line that is
                // falling away from the ship. GD does not do that: measured on
                // lv16 t=9,402 (dual ship, uid4070 m=-0.5, sy0=390 sy1=360) p2's
                // foot is 0.989 px above the seat and the gap GROWS 0.807 px a
                // tick, and GD leaves it alone -- its two bodies stay exact
                // mirrors, p1 + p2 = 960.000 to the digit.
                // So a SHIP acquires either side of a ramp the same way, by
                // crossing the seat. Both sides of the price were measured cold
                // on builds identical but for it:
                //   lv16  55 iterations / 120 fixups  ->  118 / 296
                //   lv20  DOES NOT CLEAR (walled 70+  ->  35, clears
                //         iterations at t=17,123)
                // lv16's 63 iterations are the cost of being right; lv20 is why
                // it was never worth keeping the 6 px reach-back for. Both
                // numbers are about the SAME corridor as the dual-birth work of
                // the same day, and with that corridor closed both levels clear
                // cold with this unconditional (lv16 at 69, lv20 at 33). The
                // corpus's replay never visits a case that still wants the
                // reach-back on the downhill side, so there is no reading left
                // that a switch here could select between.
                // kShipLandTol (6.0) stays for the UFO, whose own acquisition
                // has not been measured this way.
                const double landAllow =
                    (ridesTop && (c.mode == 1 || c.mode == 3) && !ufoRising)
                        ? (c.mode == 1 ? 0.001 : kShipLandTol)
                        : 0.001;
                // The stick must stay CONTINUOUS. It bypasses the acquisition
                // test entirely, so with several ramps stacked in the same x
                // window it could snap a rider onto whichever one the loop
                // happened to reach last -- measured on lv16 t=6515, a cube
                // riding to a ramp's top at y=374.577 was teleported to
                // y=316.192 (58 px down, still `grounded`) instead of launching,
                // and the DP then planned around a move GD cannot make. A real
                // rider's resting height moves by dx*|m| per tick, so anything
                // beyond a couple of ticks' worth is a different surface.
                // ...and the continuity glue obeys the same UFO-only gate: an
                // uphill UFO arcing away from the line is launching, not
                // riding (see the landAllow note above).
                // [2026-08-21 r83] **This tolerance is a px constant, not a tick
                // count.** The seam rig (seam / seam07 / seam11 = dx
                // 1.047/1.298/1.614, cube+ball x |m| 1/0.5 x 12 phase points =
                // 144 units) measured: where a flush flat continues past the
                // downhill low end, the position at which GD ends the ride is
                // **speed-independent**. Rereading the same boundary as
                //   D = |m|*(cx - xoff - x1)   [px]     -> all 11 columns 4.03+-0.06
                //   k = D/dx                   [ticks]  -> 3.82 / 3.10 / 2.50
                // only the px form is a constant (3.0*dx at 1x is 3.895 = merely
                // coincidentally close; at sp1.1 it is 4.84, too loose, and in
                // the 0.7 band 3.14, too strict, releasing 1 tick early).
                // The value takes the |m|=0.5 columns' intersection [4.031,
                // 4.037) (the |m|=1 columns prefer 4.07, but those scatter by
                // 0.04 across cube/ball/3 speeds).
                // [2026-08-24] **A pressing ship leaves the ramp the moment the
                // surface stops keeping up with it** -- the same claim the UFO
                // clause below makes, on the mode where the ride is a thrust.
                // The gap test alone cannot see this: the seat re-pins y every
                // tick, so `s.y` is always the seat and the 4.034 px never opens.
                // The two measured ship presses are told apart by kinematics
                // alone, with no constant of their own:
                //   lv16 t=8,720 (flipped, ceiling ramp uid3765 m=+1, dx=1.6143):
                //     the surface climbs +1.614 px/tick, the press drives the
                //     player -0.500 px/tick the other way -> 2.11 px/tick apart.
                //     GD drops onGround on the very next tick and emits the ramp's
                //     own launch, -6.330; the model laddered 2.000 -> 2.086 -> ...
                //     and sat on the seat for the remaining 480 ticks
                //   x=23,148 (upright, floor ramp m=+1, sp1.1): the surface climbs
                //     +1.776 px/tick against the press's +0.500 -> the surface
                //     OUTRUNS the player, contact is remade every tick, and GD
                //     ladders 2.000 -> 2.086 -> ... -> 2.430 (the ladder's own
                //     measurement, at the branch that writes it)
                // Written as the state the kinematics produce rather than as the
                // rates themselves: past the seat on the free side AND still
                // moving away. The x=23,148 ladder never gets past the seat, so it
                // is untouched. ([[gd-hang-side-mirrors-floor-rules]])
                // TRIED AND REVERTED (2026-08-24): folding this off the ship.
                // lv16's p2 shows the same two ticks at t=13,264 (mini ball: GD
                // and the model launch together at vy=7.680, the model then writes
                // vy=0 twice and returns at 4.604 against GD's 7.293), so the
                // family looked foldable. It is not reachable from here -- p2 is
                // the dual body and carries its own y/vy, so dropping the mode
                // gate cannot touch it: lv16 came out **bit-identical** (1,853
                // diverging ticks, same death), and lv22's first divergence went
                // from t=3,806 dy +0.000 to t=3,800 dy **+7.754** (426 -> 432).
                // Fix p2's copy where p2 lives. ([[gd-dual-p2-fidelity]])
                const bool shipLeaving =
                    c.mode == 1 && (double)c.vy * gs > 0.0
                    && ((double)c.y - top) * gs > landAllow;
                const bool stickHere =
                    stickToSlope
                    && std::fabs(top - (double)s.y) <= kStickGap
                    && !(ufoRising && m > 0.0
                         && ((double)c.y - top) * gs > landAllow)
                    && !shipLeaving;
                // an end-clamped sample never overwrites an in-span acquisition
                // (note at the loop head)
                if (slopeTookInside && !smp.inside) continue;
                // ---- [2026-08-19 slopeland calibration rig] A falling cube's
                // acquisition starts 1.0px above the seat. GD snaps it downward
                // and seats it 1 tick before "the centre crosses the seat" (the
                // identity of lv16 t=5,923's 0.68 downward snap). Of 37 units,
                // every fast-approach unit agrees at tolerance 1.0 (+-0.07, the
                // same value as preSlopeCollision's constant 1.0). The entry-side
                // gate is the sample point r 0.5px inside the end (measured
                // window (0.49, 0.90], constrained by the m=-1/-2 grazing
                // entries). Measured only for cube / upright / normal size /
                // falling from the air, so limited to that scope. Only the 3
                // relative-rise grazing units (arc gradient ~= m) seat 1-2 ticks
                // later in GD and do not fit this formula -- recorded in findings
                // as unexplained. Purely additive: the tick seated under the old
                // condition (landAllow) is unchanged; only the earlier side is
                // added.
                // ...and **limited to ramps downhill in the travel direction**.
                // The rig's 4 uphill control units all had seat-tick gaps <= 0
                // and never actually constrained a positive tolerance. lv18
                // t=11,023 (uid7232 m=+0.5, gap 0.151 and GD does not seat) is
                // the uphill counter-example = uphill still waits for the
                // crossing, as before. Same asymmetry as the ship landAllow's
                // "no tolerance on an uphill ramp (lv20 t=14,018)".
                // ...the ball shares the same rule (slopelandball rig 19u: 17/19
                // at a=1.0/b=1.0; the old crossing rule was 6/19. The misses are
                // only the same 2 relative-rise grazes as the cube). The entry
                // gate takes a=0.8 from both datasets' common window
                // (cube (0.49,0.90] with ball (~0.6,1.0]).
                const double rRaw = x + (m > 0 ? slopeXOffset(m, pH)
                                               : -slopeXOffset(m, pH));
                const bool revT = ((double)K.dxF < 0.0 || s.rev != 0);
                const bool seatTolOk =
                    ridesTop && !c.flip && (c.mode == 0 || c.mode == 2)
                    && !s.dual
                    && !s.grounded && (double)c.vy * gs <= 0.0
                    && (revT ? -m : m) < -0.01
                    && (revT ? (rRaw <= x1 - 0.8) : (rRaw >= x0 + 0.8))
                    && smp.inside
                    && ((double)c.y - top) * gs <= 1.0;
                // [2026-08-19 item 14] When landing on a flat on the same tick,
                // but the pre-clamp position (preLandY) had already crossed the
                // seat, GD lands on the slope's seat (the slope beats the plate).
                // lv19 t=15,514: caught on plate uid10342 (right edge 22,110, top
                // 360) with the centre +7.33 while crossing ramp uid10385's seat
                // 373.88 -- GD is at 373.88, the model stood on the plate's 375.
                // The edgeland rig confirmed "a plain plate catches even at 12.3
                // overhang", so this is not a rule rejecting the plate but one
                // where the seat side overwrites. Upright cube only (that is
                // where it was measured).
                // ...the SPIDER has the same mechanism (lv21 t=16,710: the model
                // standing on the plate 150+13.5=163.5 vs GD's ramp seat 155.066,
                // edy 8.43).
                // ...the UFO too (lv20 t=21,983: on the landing tick, onto the
                // ramp seat rather than the plate 222.35, edy +0.55. preLandY
                // wired in from the fly/land side).
                const bool seatFromPreLand =
                    landedThisTick && !stepLandedThisTick && ridesTop && !c.flip
                    && (c.mode == 0 || c.mode == 6 || c.mode == 3) && !s.dual
                    && ((double)preLandY - top) * gs <= landAllow;
                // [2026-08-19] A non-riding player **standing supported by an
                // actual flat** is not dragged down by a seat below. lv19
                // t=14,074: a UFO on a platform (landAllow 6.0) was pulled to a
                // seat 0.89 below, but GD keeps standing on the platform. The
                // gate is "a flat exists at foot height" -- with a bare
                // s.grounded it also blocked lv21 t=18,088 (right after a portal
                // seating, with only a slope for support; GD keeps descending
                // the m=-2 line) and regressed. The lift when buried
                // (insideSolid) stays as before.
                if (s.grounded && !s.onSlope && !insideSolid
                    && ((double)c.y - top) * gs > 0.001) {
                    bool flatSup = false;
                    const double footG = (double)c.y - gs * pH;
                    for (const Obj* fo : *K.near) {
                        if (fo->type != 0) continue;
                        if (std::fabs(x - fo->cx) > fo->hw + pH + kContactEps)
                            continue;
                        const double ff = (gs > 0.0) ? (fo->cy + fo->hh)
                                                     : (fo->cy - fo->hh);
                        if (std::fabs(footG - ff) <= 0.6) {
                            flatSup = true;
                            break;
                        }
                    }
                    if (flatSup) continue;
                }
                if (((double)c.y - top) * gs <= landAllow || seatTolOk
                    || seatFromPreLand || stickHere || insideSolid) {
                    // [2026-08-19] On the tick a downhill rider's centre crosses
                    // the slope rect's top edge (cy+hh), GD clamps **to there for
                    // exactly 1 tick** and returns to the line the next tick.
                    // 3 gdref lv16 measurements: t=5,945 (uid2292,
                    // 270.152->**270.000**->268.538), t=5,982 (uid2293,
                    // 240.288->**240.000**->238.674), t=6,948
                    // (480.602->**480.000**->478.987). Every clamp value is
                    // sp->cy + sp->hh, not a face or line value (matching
                    // preSlopeCollision's "player.y vs rect top" gate). Measured
                    // only for the cube, so the scope is limited.
                    // ...but **only when the descent since mounting exceeds the
                    // hang amount pH*sqrt(1+m^2)** (slopeland3 calibration rig,
                    // 2026-08-19). The first rTop crossing after seating on one's
                    // own ramp (descent <= 16.77 is the geometric upper bound)
                    // passes through; only seam-crossing / long-ride crossings
                    // clamp. Measured boundary (16.0, 17.1): lv16 uid2580 passes
                    // at descent 9.1, uid2292 clamps at 17.1. The descent is
                    // reconstructed from slopeT (a new state dimension cannot be
                    // carried by anchors -- slopeT is already transported). One
                    // known counter-example: the rig's unit1 (K=6, descent 33.9)
                    // passed through -- unexplained by phase or ride length. 1
                    // tick is 0.8px, so recorded and moved on.
                    // [2026-08-19, night-3] Settled: **the descent amount is not
                    // the discriminator.** Extending to the ball and dropping the
                    // descent gate removes the 3 target families (lv16@6,948 /
                    // lv18@12,250 / lv19@15,537) but 4 families spring up
                    // instead. Ordered by descent, correct hits and false firings
                    // **alternate perfectly**:
                    //   clamps:  8.07 (lv18@12,250) / 14.53 (lv16@6,948)
                    //            / 28.55 (lv19@15,537)
                    //   passes:  8.88 / 12.98 / 19.37 / 38.7 (false-firing sites)
                    //            + 9.1 (lv16 uid2580) + 33.9 (rig unit1)
                    // So the gate below is merely "happening to be right on the
                    // current corpus", and **some other geometric/phase quantity
                    // is the true discriminator**. Re-sweep with a calibration
                    // rig (a slopeland3 variant).
                    // [2026-08-19, night-3] **It was the seam hand-over.**
                    // Calibration rig slopeland4 + calib_sl4seam.py measured: on
                    // the tick **just before** standtrace's slope uid changes
                    // from ramp 1 to ramp 2, at |m|=1 GD's centre y is, 6/6,
                    // **the seam's line value itself** (21.21px below the normal
                    // seat line+pH*sqrt(2)).
                    // The corpus has the same shape -- lv16@6,948's 480.000 is
                    // uid2791's low end = uid2865's rect top = the seam (GD's
                    // stand goes t=6,929 uid2791 -> t=6,949 uid2865, and the
                    // clamp is 1 tick before the boundary); lv18@12,250's 270.000
                    // is the uid8272/8273 seam too. **It never happens at the
                    // high end of ramp 1 (the ramp one landed on), 12/12** = it
                    // does not happen where no ramp connects just before.
                    // So the gate is not the descent but "does this rect top
                    // coincide with **the preceding ramp's low end**". A
                    // geometric test of the same shape as cornerContinued.
                    const double rTopHere = sp->cy + sp->hh;
                    bool rTopIsSeam = false;
                    {
                        const bool revT = ((double)K.dxF < 0.0 || s.rev != 0);
                        const double entryEdge = revT ? x1 : x0;
                        for (const Obj* nb : *K.slopes) {
                            if (nb == sp || nb->slopeHazard) continue;
                            const double nx0 = nb->cx - nb->hw;
                            const double nx1 = nb->cx + nb->hw;
                            if (nx1 <= nx0) continue;
                            const double nedge = revT ? nx0 : nx1;
                            const double nlo = revT ? (double)nb->sy0
                                                    : (double)nb->sy1;
                            if (std::fabs(nedge - entryEdge) <= 0.5
                                && std::fabs(nlo - rTopHere) <= 0.5) {
                                rTopIsSeam = true;
                                break;
                            }
                        }
                    }
                    // **The seam test alone is not enough** (same day, retracted):
                    // with `(c.mode == 0 || c.mode == 2) && rTopIsSeam` the 2
                    // target families (lv16@6,948 / lv18@12,250) disappear, but 3
                    // families spring up in the same section (slope-0.50/ride11,
                    // slope-0.50/ride24+/nearceil, slope-1.00/ride24+).
                    // The rig drops 6/6 at |m|=1 seams but not at |m|=0.5 -- yet
                    // the corpus's lv16@6,948 drops at m=-0.5. The rig (1x) and
                    // the corpus (sp1.1) differ in **speed**, so next: sweep
                    // speed on the rig.
                    // (`rTopIsSeam` is kept -- it does become part of the gate.)
                    // **The geometric seam alone is not enough either** (same
                    // day's measurement): lv16 uid2580's (x[9270,9330], sy0=180)
                    // 180 coincides with the preceding ramp's low end -- a
                    // "geometric seam" -- but the player **landed on that ramp
                    // itself** (ride11 = 17.8px earlier; x~=9,287 is inside x0),
                    // so GD does not clamp (t=6,298 GD 179.428 / model 180.000).
                    // Same as the rig's 12/12 -- **it does not happen on the ramp
                    // the ride began on**. So the gate is "current ramp != the
                    // ramp the ride began on".
                    // [2026-08-20] The "not on the ramp the ride began on" gate is
                    // **cube only**. The ball clamps even on the ramp it began on:
                    //   lv18 t=12,250 (ball, landed on uid8273, ride5, rect top
                    //     270): GD stops at exactly y=270.000 for 1 tick and
                    //     returns to the line the next. The model stayed on the
                    //     line (edy +0.695)
                    //   lv16 t=6,298 (cube, landed on uid2580, ride11, rect top
                    //     180): GD does **not** stop, 179.428
                    // Removing the gate entirely makes lv16@6,298 spring up
                    // (measured, families -2/+3).
                    if (ridesTop && !c.flip && (c.mode == 0 || c.mode == 2)
                        && rTopIsSeam && s.onSlope && s.slopeUid0 >= 0
                        && (c.mode == 2 || sp->uid != s.slopeUid0)) {
                        const double rTop = rTopHere;
                        if ((double)s.y > rTop + 0.0005 && top < rTop - 1e-9) {
                            if (g_slopeDbg)
                                std::printf("slopedbg t=%lld x=%.2f uid=%d "
                                            "recttop=%.3f (top=%.3f)\n",
                                            (long long)K.t, x, sp->uid,
                                            rTop, top);
                            c.y = (float)rTop;
                            c.vy = 0; c.grounded = 1;
                            c.onSlope = 1; c.slopeM = (float)m;
                            c.slopeUid0 = s.onSlope ? s.slopeUid0 : sp->uid;
                    c.slopeUidNow = sp->uid;
                            continue;
                        }
                    }
                    if (smp.inside) slopeTookInside = true;
                    // --slopedbg: WHICH condition acquired the ramp, and with
                    // what numbers. The three are very different claims (landed
                    // / still riding / lifted out of the solid half) and the
                    // trace's onslope column cannot tell them apart, so a false
                    // acquisition reads as "the model zeroed vy for no reason".
                    if (g_slopeDbg)
                        std::printf("slopedbg t=%lld x=%.2f y=%.3f pH=%.1f "
                                    "uid=%d m=%.3f top=%.3f gap=%.3f allow=%.3f "
                                    "why=%s\n",
                                    (long long)K.t, x, (double)c.y, pH, sp->uid,
                                    m, top, ((double)c.y - top) * gs, landAllow,
                                    (((double)c.y - top) * gs <= landAllow)
                                        ? "land"
                                        : (seatTolOk
                                               ? "seat1"
                                               : (stickHere ? "stick"
                                                            : "inside")));
                    // [2026-08-20] **While being caught by a flat solid, do not
                    // transfer to another ramp.**
                    // A ride following a descending ramp's line was passing
                    // through a flat solid whose top face is above the line. GD
                    // seats on that top face and stops, and **the ride continues
                    // on the same ramp** (a single release blip on the tick the
                    // window runs out). 3 measurements, all census families:
                    //   lv16 t=6,967 (cube, thin plate uid2937 top 449.95,
                    //     m=-0.5): GD y=464.950, vy=-3.229 at t=6,972
                    //     (=-0.5x1.6143/0.25). edy +0.491
                    //   lv19 t=15,537 (cube, solid uid10386 top 330, m=-1):
                    //     GD y=345.000, vy=-5.193 at t=15,540. edy +0.992
                    //   lv21 t=18,098 (mini UFO, thin plate uid24225 top 180,
                    //     m=-2): GD y=189.000, vy=-12.914 at t=18,099.
                    //     edy +2.223
                    // **Not applied to a continuation on the same ramp** --
                    // applied there, lv16 releases abruptly at t=6,967 (GD is 5
                    // ticks later). Stopping only the transfer, the release comes
                    // out matching GD on the tick the current ramp's window runs
                    // out. The y side is aligned by the flat clamp below.
                    if (ridesTop && !c.flip && K.near && s.onSlope
                        && sp->uid != s.slopeUidNow) {
                        bool blockedByFlat = false;
                        for (const Obj* nb : *K.near) {
                            if (nb->type != 0 || nb->oneway) continue;
                            // The player's **centre** must be over that solid.
                            // Allowing out to the box edge (+pHalf) picks up a
                            // block that ends short and stops the descent at lv16
                            // t=6,316 (GD descends past 150 there).
                            if (std::fabs(x - nb->cx) > nb->hw) continue;
                            const double face = nb->cy + nb->hh;
                            if (face > (double)c.y + pHalf) continue;
                            if (face + pHalf > top + 0.001) {
                                blockedByFlat = true;
                                break;
                            }
                        }
                        if (blockedByFlat) continue;
                    }
                    c.y = (float)top;
                    // the impulse tick takes the POSITION only -- the jump or
                    // flip already set vy and cleared grounded, and the player
                    // is leaving. Without this the ride simply cancelled a cube
                    // jump taken off an UPHILL ramp: the jump freezes y, the
                    // ramp's next resting height is 0.8 px ABOVE that, so the
                    // acquisition test fired and put vy back to 0.
                    // ...and the ball's TAP is the same story as the cube's
                    // jump: take the ride POSITION only and leave the velocity
                    // (and grounded/onSlope) to the flip, which already ran.
                    // `tappedOffSlope` has carried this meaning in a comment
                    // since it was written but was never read anywhere -- the
                    // compiler had been warning C4189 about it the whole time.
                    if (impulsedOffSlope || tappedOffSlope) {
                        // [2026-08-19] On a **downhill** (travel-direction) ride,
                        // the jump tick also takes the seat's step first
                        // (collisions before buttons, and a falling seat does not
                        // cancel the jump). lv16 t=5,991: GD y=232.217 = the seat
                        // one more step down, vy=11.42 = the jump value. Uphill
                        // stays frozen as before (measurement in the 8273 note).
                        // Position only; vy/grounded stay the jump's.
                        const double sgnT2 =
                            ((double)K.dxF < 0.0 || s.rev != 0) ? -1.0 : 1.0;
                        if (ridesTop && !c.flip && c.mode == 0
                            && m * sgnT2 < -0.01)
                            c.y = (float)top;
                        continue;
                    }
                    // The SWING does not have its vy zeroed by the ride. GD
                    // starts it at 2.000 on acquisition and then integrates the
                    // swing's own gravity every riding tick. Measured on lv22
                    // uid5430 with the presses REMOVED from the plan (they were
                    // what made the earlier probe look like two families):
                    //   2.000 2.086 2.172 2.258 2.344 ... 3.376   (+0.086/tick)
                    // It only shows on the tick the player LEAVES -- the ride
                    // overwrites y anyway -- but there it is the whole 0.4 px:
                    // GD steps y by 0.225*1.652 = +0.372 where the model, with
                    // vy at 0, stepped -0.019.
                    // Sign follows the surface: the ramp climbs as x advances
                    // here, and vy is positive. The 2.000 itself is measured,
                    // not derived -- it is neither didHitHead's +-2 nor the
                    // take-off's -1.000.
                    // [2026-08-20 r39] **The ship's ride is the same as the
                    // swing's.** The census shows just one
                    // `m1/.../slope+0.50/ride0` edvy=+2.000, but **on a cold run
                    // the same family becomes a 19-item grind** (hp32's lv16, the
                    // main cause of 22->55 iterations): iterations 24-48 are a
                    // grind moving a few px at x=22,631-23,364, and fixups_log
                    // contains nothing but
                    //   `m1/mini0/g1/gdg1/sp1.1/slope+1.00/rideNN, edy=0, edvy=+2`
                    // The post-correction ladder also has the swing's shape
                    // (2.000 -> 2.086 -> 2.172 -> 2.258 -> 2.344 -> 2.430,
                    //  6 consecutive ticks x=23,148-23,156, +0.086/tick).
                    // So **2.000 x dir on acquisition, then +0.086 x dir every
                    // tick** -- the swing's rule itself.
                    // ([[gd-fixup-grind-means-missing-rule]]: a few-px/iteration
                    //  grind is not a "point" but a missing rule)
                    // Measured at **normal size / sp1.1 / uphill**. The mini
                    // acquisition 2.000 does **not scale**: the census's lv16
                    // `mini1/sp0.7/slope+0.50/ride0` emits the same +2.000.
                    // The downhill (dir=-1) ladder is unmeasured -- lv18's
                    // `mini1/slope-1.00/ride2` is -1.899, 0.27 off this formula's
                    // -2.172. The coefficient may differ by direction.
                    // [2026-08-20 r39a] **The gate is the press.** Applied to
                    // every ship in r39, the acceptance gate fell (families
                    // 22->24; all 5 new ones are `m1/.../g0/gdg1/.../air` with
                    // edvy exactly **-2.000** = only the model holds the 2.000).
                    // gdref splits on the input:
                    //   lv16 t=9,847  in=0  GD rides at vy=**0.000** (stays 0 after)
                    //   lv16 t=15,514 in=0  lands from terminal -6.400, GD vy=0.000
                    //   lv16 x=23,148 **in=1** GD's ladder 2.000->2.086->...->2.430
                    //   the census lv16 `mini1/sp0.7/slope+0.50/ride0` is **in=1**
                    // So the ride itself pins vy to 0, and the ladder rises from
                    // 2.000 **only while pressing**.
                    if (c.mode == 1 && input) {
                        // [2026-08-24] ...and the ladder is MIRRORED BY GRAVITY. A ship's
                        // press acts toward the player's own "up", which is world-down when
                        // it is flipped, so the entry value follows the flip and not the
                        // ramp's world sign alone. The note above already says the dir=-1
                        // side was unmeasured; here is a measurement of it.
                        // lv16 t=8,720 (ship, flip=1, ceiling ramp uid3763 slopeDir=1 at
                        // x=13,205, m=+1, travelling forward, the press tick):
                        //   GD    vy = -2.000, and it leaves the ramp next tick (onGround
                        //         1 -> 0) with the ramp's own launch, -6.330
                        //   model vy = +2.000 -- pressed INTO the ceiling, stayed attached,
                        //         and sat at y=539.000 for the rest of the level
                        // That single tick is where lv16's wall comes from: by the death at
                        // t=8,768 the two are 113 px apart, which is what refuses the kill
                        // record the loop needs there.
                        // The one exit that contradicts a flip mirror elsewhere (t=4,147,
                        // same level, same flip=1) is not this branch at all -- it has
                        // input=0 and leaves through the release.
                        const double dir = ((m * (double)useDx >= 0.0) ? 1.0 : -1.0)
                                         * (c.flip ? -1.0 : 1.0);
                        // The ladder's entry is "carried is 0" = was not pressing
                        // until just now (same shape as the swing's walkIn0
                        // keying continuation on vy==0)
                        c.vy = (s.onSlope && c.slopeM == (float)m
                                && s.vy != 0.0f)
                                   ? (float)qVy((double)s.vy + kShipRampG * dir)
                                   : (float)(2.0 * dir);
                    } else if (c.mode == 7) {
                        const double dir = (m * (double)useDx >= 0.0) ? 1.0 : -1.0;
                        const double gRide = swingG(c.mini != 0);
                        // TRIED AND REVERTED (2026-08-14): basing the step on
                        // `s.vy` instead of `c.vy`. `c.vy` already carries the
                        // airborne branch's own `vpS -= kSwingG`, so the ride
                        // integrates the swing's gravity TWICE per riding tick,
                        // and GD does not: measured on lv22's ride
                        // t=3,774..3,805, GD is SET to 2.000 on acquisition and
                        // reads 4.666 thirty-one ticks later (0.086/tick) where
                        // the model reads 7.418 (0.172/tick).
                        // Fixing it alone makes the replay WORSE -- the
                        // divergence block from t=3,806 goes 223 -> 760 ticks --
                        // because the doubling was silently paying for a
                        // MISSING RELEASE IMPULSE. GD adds one on the tick the
                        // swing leaves the ramp, and it is not a constant:
                        //   t=3,806  vy 4.666 -> 6.906  (+2.240)
                        //   t=4,585  vy 2.000 -> 4.899  (+2.899)
                        // Those are the only two same-gravity jumps in the
                        // whole swing stretch that are not the ride's own
                        // "set to +-2.000" or its landing "set to 0.000", so the
                        // release is a real rule with a variable of its own
                        // (the ramp's gradient and/or the ride's length -- cf.
                        // slopeExitVy). The two have to land TOGETHER.
                        // ...but a WALK-IN rides with vy pinned at 0, the
                        // cube's convention, not the airborne 2.000. Measured
                        // on lv22 t=3,490-3,494: the swing is pushed along the
                        // band floor into this ramp by the 2069 carpet (GD's
                        // g stays 1 through the lift AND the ride) and GD
                        // reports vy 0,0,0,0,0 while the y climbs the ramp
                        // bit-exactly. The model's own grounded flag is
                        // already 0 here (the lift integration cleared it),
                        // so the gate below uses "inside a lifting force box"
                        // as a PROXY for "came in grounded" -- the only
                        // measured walk-in is the force-box lift, and the
                        // 2.000 site (uid5430) has no boxes anywhere near it.
                        // If a walk-in without a force box (or a fast flier
                        // crossing a box onto a ramp) ever shows up in the
                        // census, replace this with a real came-in-grounded
                        // bit. Continuation keys on vy == 0: the airborne
                        // convention starts at 2.000 and integrates AWAY from
                        // zero, so the two cannot alias.
                        // [2026-08-20] The case the note above predicted appeared
                        // on a rig. **A walk-in with no force box**: on the new
                        // calibration rig flyramps (a swing sliding along the
                        // floor into a ramp, zero input) GD stays vy=0.000 for
                        // all 70 riding ticks, yet the model accumulated from
                        // 2.000 up to 7.934 and on the departure tick flew up by
                        // that much (7.934x0.225 = **+1.785px**). The launch
                        // velocity itself (5.554) already matches; the offset is
                        // only the departure tick's y, then a permanent parallel
                        // shift. 8 units (m=1,2 x normal/mini x 2 ride lengths)
                        // give the same 1.785 / 0.895 / 1.937. **Replace the
                        // proxy with the real "entered grounded"** -- lv22's 2069
                        // carpet has the grounded flag down, so the force-field
                        // proxy stays as an OR.
                        const bool walkIn0 =
                            (s.onSlope && c.slopeM == (float)m)
                                ? (s.vy == 0.0f)
                                : (s.grounded
                                   || (!g_forceBoxes.empty()
                                       && forceBoxAcc(modX, modY, pHalf)
                                                  * (s.flip ? -1.0 : 1.0)
                                              > kSwingG));
                        c.vy = walkIn0
                                   ? 0.0f
                                   : (s.onSlope && c.slopeM == (float)m)
                                   ? (float)qVy((double)s.vy + gRide * dir)
                                   : (float)(2.0 * dir);
                    // [2026-08-21 r61] **A ride pushed out onto a floor ramp with
                    // gravity still pointing the other way does not zero vy.**
                    // Measured lv22 t=8,233..8,235 (spider, flip=1, floor ramp
                    // uid17788 m=+1): GD keeps y stuck to the seat while **vy
                    // stays in free integration** (2.891 -> 3.020 -> 3.149 ->
                    // 3.278 = +0.129/tick = the spider's g) for 3 ticks, and vy
                    // first becomes 0 at t=8,236, the tick flipGravity(false) ran
                    // (the MOD's `pfg:` line, slopeUp=1 = m_maybeUpsideDownSlope).
                    // Once r60 made the ride acquirable, y matched exactly for 15
                    // ticks, but zeroing here merely morphed the family from
                    // edy +0.864 to edvy +3.020.
                    // **The mode-specific ladders (the ship's 2.000/+0.086, the
                    //   swing's walkIn0) apply even when flipped** -- 6 ticks
                    // earlier on the same ramp (t=8,227, still swing, flip=1) GD
                    // emits the swing's acquisition value, 1.234 -> **2.000**.
                    // Skipping this else entirely made that a new family,
                    // `m7/.../air` edvy +0.680.
                    // [2026-08-21 r67] **r61's gate is "push-out onto the top
                    // face" only.** r61 was written as `!flipForRide`, but that
                    // keeps accumulating vy even for the **hang (ridesTop=0 =
                    // sliding a ceiling ramp's underside)**. GD maintains vy=0
                    // during a flipped ship's hang (calibration rig ceilramp
                    // unit0: vy=0.000 on every riding tick -- r61 floated 21px
                    // and became a regression where the model dies).
                    // The lv22 spider measurement is only flipForRide **and**
                    // ridesTop=1 (the case pushed out onto a floor ramp's top
                    // face by r60's hatch).
                    } else if (!(flipForRide && ridesTop)) {
                        c.vy = 0;
                    }
                    // ...and **it is not grounded either**. GD's onGround stays 0
                    // throughout this ride (the measurement table above, every
                    // tick of 8,227..8,235). Setting it lets the next tick's
                    // "standing = vy 0" apply directly and the carefully kept vy
                    // vanishes in 1 tick (r61b's remainder: only t=8,233 matches
                    // at 3.020, and from 8,234 only the model is 0). r67: this
                    // too is limited to the push-out case.
                    if (!(flipForRide && ridesTop)) c.grounded = 1;
                    else rodeFlipped = true;
                    c.onSlope = 1;
                    c.slopeM = (float)m;
                    c.slopeUid0 = s.onSlope ? s.slopeUid0 : sp->uid;
                    c.slopeUidNow = sp->uid;
                }
            }
            // Pass 2: only a SPIKED ramp kills. A plain one does not -- it lifts
            // the player out.
            //
            // This whole test used to fire for every ramp, and all the evidence
            // for that came from lv18, where 116 of the 154 spiked ramps sit on
            // TOP of a plain one at the same (cx,cy). Every "GD kills a player
            // inside a ramp" observation was the spiked twin.
            // Measured on lv16's (6708,293) 60x30, line 278->308, which has no
            // twin: injecting the ball at y = 262, 270, 278, 285, 293, 300 and
            // 308 -- from under the box to inside the triangle -- GD puts ALL of
            // them at y = 311.385 with vy = 0 and onGround = 1, i.e. on the
            // surface. None dies. Scanning y in 2.5 px steps across three x
            // inside that ramp and the one at (6768,323), the only lethal band
            // is at a CONSTANT y (the spike row at 244), nowhere near the line.
            //
            // The old "solid slice = box floor to the line" also cannot be the
            // rule even for the spiked ones. Point-probed on lv18's (5115,405)
            // pair (solid 420->390, spiked 416->386), 1 tick per point, x and y
            // both injected:
            //   x=5096  line 424.0  alive top<=410.45  dead top>=410.47
            //   x=5100       420.0             406.45            406.47
            //   x=5104       416.0             402.45            402.48
            //   x=5108       412.0             398.45            398.48
            //   x=5112       408.0             394.46            394.48
            // The boundary is exactly PARALLEL to the line (9.54 px under the
            // spiked one), while the old rule's lower edge is the box floor -- a
            // HORIZONTAL line. The two only agree near the middle of the box,
            // which is why lv16 (ridden across the middle) worked and lv18's
            // tunnel (flown through near the ends) did not. Left as-is for the
            // spiked ramps because it errs toward killing, which only costs
            // reachability; the parallel form needs one more sweep to pin down
            // (see docs/findings.md -- gradient and mini are unmeasured).
            for (const Obj* sp : *K.slopes) {
                if (!sp->slopeHazard) continue;
                const double sx0 = sp->cx - sp->hw, sx1 = sp->cx + sp->hw;
                const double sm = (sp->sy1 - sp->sy0) / (sx1 - sx0);
                // The solid half is the TRIANGLE inside the object's box, not
                // the whole half-plane under the line -- so the box's y range
                // is part of the test. Without it the model killed everything
                // under a floor ramp, which is most of a corridor, and lv18
                // lost its whole frontier 400 px early.
                if (x + pH > sx0 && x - pH < sx1) {
                    // sampled at the player's CENTRE (see the pass-1 note)
                    const double sxc = std::min(std::max(x, sx0), sx1);
                    const double surf = sp->sy0 + sm * (sxc - sx0);
                    // The solid is the slice between the box's floor and the
                    // line, so the test is a real interval overlap, not "below
                    // the line". At a ramp's low corner that slice has ZERO
                    // height, and the half-plane version killed every ball that
                    // clipped the next ramp's leading edge -- lv16 t=4701,
                    // riding up one 60x30 ramp into the contiguous next one,
                    // 0.8 px into a slice of height 0.
                    // ...and WHICH slice is solid is slopeDir's business, exactly
                    // as it is for the ride (see slopeIsCeiling). This test had
                    // the floor ramp's answer hardcoded -- box floor up to the
                    // line -- so on a CEILING ramp, where the solid is the line
                    // up to the box's top, the interval came out empty and the
                    // model flew straight through the spikes.
                    // Measured on lv16 t=12,685 (mini UFO, pbox 18x18 so pH=9,
                    // injected y=590 at x=19,177.52): GD destroys the player on
                    // uid6404 (id367 shz=1, box (19142,613) 60x30, sy 624->594,
                    // slopeDir 1). The old form gave lo=598 (box floor)
                    // hi=594 (line) -> empty; the ceiling form gives lo=594
                    // (line) hi=599 (player top) -> hit, which is GD's answer.
                    // Same {1,3} vs {1,3,5,6} caveat as everywhere else: only the
                    // ship is on GD's full set so far.
                    const bool ceilSlope = (c.mode == 1)
                        ? slopeIsCeiling(sp->slopeDir)
                        : (sp->slopeDir == 1 || sp->slopeDir == 3);
                    const double lo = ceilSlope
                        ? std::max((double)c.y - pH, surf)
                        : std::max((double)c.y - pH, sp->cy - sp->hh);
                    const double hi = ceilSlope
                        ? std::min((double)c.y + pH, sp->cy + sp->hh)
                        : std::min((double)c.y + pH, surf);
                    if (lo < hi - 0.001)
                        DIE(sp->slopeHazard ? "slope/spiked" : "slope/inside", sp);
                }
            }
        }
        // [2026-08-20] **A ride cannot descend below a flat solid.** The pair of
        // the "do not transfer" gate above; aligns the y side with GD. Limited to
        // rides descending in the travel direction (uphill is mid-push-up by the
        // face, and picking up a neighbouring solid's top face over-lifts).
        const double mTravRide = (double)c.slopeM
                               * (((double)K.dxF < 0.0 || s.rev != 0)
                                      ? -1.0 : 1.0);
        if (c.onSlope && !c.flip && K.near && mTravRide < 0.0) {
            double bestFace = -1e18;
            for (const Obj* nb : *K.near) {
                if (nb->type != 0 || nb->oneway) continue;
                if (std::fabs(x - nb->cx) > nb->hw) continue;   // centre over it
                const double face = nb->cy + nb->hh;
                if (face + pHalf <= (double)c.y + 0.001) continue;
                if (face > (double)c.y + pHalf) continue;   // a wall is a different story
                if (face > bestFace) bestFace = face;
            }
            if (bestFace > -1e17) c.y = (float)(bestFace + pHalf);
        }
        // Leaving the top launches. GD applies it like a pad: the y move for
        // this tick already happened with the old velocity.
        // Only an UPHILL exit launches; running off the bottom of a downhill
        // ramp just leaves the player falling.
        // The launch BEATS a jump taken on the same tick. Measured on lv16
        // t=6516: the plan presses at the top of the 330->360 ramp at
        // (9645,345) and GD comes out at 9.208 -- the ramp's own exit for m=1
        // at speed 1.1 -- not the cube's 11.420. The old `c.vy <= 0` guard let
        // the jump stand, and the model then ran 2.2 vy fast for the rest of
        // the section. `rampWindowHere` is what keeps a MID-ramp jump intact:
        // there a ramp still holds the player, so this is not an exit.
        // ...and a FLIPPED player leaving a CEILING ramp is the mirror of it:
        // "uphill" for it is m < 0, and the launch points the other way.
        // Measured on lv22 t=4,637 (cube, ramp uid4359 (6705,435) m=-1,
        // speed 0.7, one riding tick): GD leaves at vy = -2.390 and then decays
        // at +0.212/tick, which is the cube's own gravity -- the last press was
        // the release at t=4,635, so this is the RAMP's launch, not a jump.
        // "Uphill" is TRAVEL-relative, not a world-m sign: a reverse rider on
        // a world-descending ramp is climbing it. Measured on lv22 t=18,567
        // (mini cube, x-decreasing, world m<0, y rising +2*|dx|/tick): GD
        // launches at t=18,568 with vy=13.064 while the sign test read the
        // ride as downhill and never fired. sign(useDx) is +1 everywhere a
        // forward section runs, so nothing else moves.
        // [2026-08-18] sign(useDx) was another dead test (same reason as the rev
        // note at sampleAt: useDx is always positive via anchors). Redrawn with
        // the true travel direction.
        const double mTravel = (double)s.slopeM
                             * (((double)K.dxF < 0.0 || s.rev != 0)
                                    ? -1.0 : 1.0);
        // [2026-08-24] **A flipped player's "uphill" depends on WHICH SIDE it
        // rode, not on its gravity.** The mirror above was written as "flipped =>
        // uphill is m<0", and its stated evidence (lv22 t=4,637, flipped cube off
        // uid4359, -2.390) does not reproduce: replaying lv22's own solution, GD
        // is at t=4,637 upsideDown=**0**, mode=cube, yvel=**-0.454**. That note is
        // from an older lineage ([[gd-constant-justification-goes-stale]]).
        // What the corpus actually holds is one flipped launch on each side, and
        // the ride side tells them apart where the flip alone cannot:
        //   lv22 t=3,806  flipped swing on uid5387 **sdir=0** (a FLOOR ramp, ridden
        //                 on top: the offset is line + pH*sqrt(2) = 21.213 for
        //                 three ticks running, then the high-end flat 195.000),
        //                 mTravel=+1. GD launches at **6.906** = the m=1 non-cube
        //                 exit; the flip mirror called it downhill and emitted the
        //                 release pulse dx/0.25 = 6.457 instead, and lv22's model
        //                 then cannot replay its own solution past t=4,231
        //   lv16 t=8,721  flipped ship hanging under uid3763 **sdir=1** (a CEILING
        //                 ramp), mTravel=-1, GD launches at -6.330
        // So: hanging under a ceiling, uphill is m<0 (the mirror, kept); riding a
        // floor ramp's top, uphill is m>0 -- the same test an upright rider uses,
        // because "uphill" is a property of the surface being climbed. Non-flipped
        // players are left exactly as they were; only the flipped case splits.
        // The side is read off `s.grounded`, NOT off the ramp behind
        // s.slopeUidNow. That uid is documented as not carried by the anchor, and
        // keying physics on it splits the run in two: the loop re-anchors every
        // iteration, so it PLANS with the uid gone (-1 -> the old mirror) and
        // replays from t=0 with it present. lv22 cold went 84% -> 41% that way.
        // `grounded` says the same thing and is carried: the ride writes
        // `if (!(flipForRide && ridesTop)) c.grounded = 1`, so during a flipped
        // ride grounded==0 IS "riding a ramp's top" and grounded==1 is the hang.
        // Both measurements agree with it --
        //   lv22 t=3,803..3,805  flipped swing on the floor ramp, grounded 0,0,0
        //   lv16 t=8,717..8,720  flipped ship under the ceiling, grounded 1,1,1,1
        const bool rodeCeil = (s.grounded != 0);
        const bool uphillExit = (c.flip && rodeCeil) ? (mTravel < 0.0)
                                                     : (mTravel > 0.0);
        // [2026-08-19 rejected] "Launch only when s.grounded". Meant to stop the
        // false launch (-2.762) at lv18 t=8,461, but **the real fix there is the
        // flat solid's cornerContinued (below)**, and this gate never moved a
        // single px. And it broke lv16's cold run: an isolated A/B (lv16 only,
        // pool 96/97, same source on the same day) gave
        //   flatCorner only = **28 iterations 9.4 min CLEARED**
        //   both (flatCorner + this gate) = **ITER-CAP(81) 18.4 min, x=21,866**
        // An upright player hitting a ceiling ramp from below never sets
        // grounded, so gating here erases every launch of a "riding but not
        // standing" ride.
        // [2026-08-21 r89] **The UFO's jump beats the uphill launch.** The
        // calibration rig ceilhold's 4 UFO cells (|m| 1/0.5 x normal/mini,
        // pressing while hung flipped from a ceiling ramp): GD emits the plain
        // UFO jump **-6.871 / mini -6.648** as is, while the model overwrote it
        // with this launch, slopeExitVy(|m|,UFO) (-5.554 / -2.999). The 2 |m|=2
        // cells never entered the launch window, kept the plain value, and
        // matched GD = the difference was only the launch overwrite. The
        // downhill release side (just below) already carries !impulsedThisTick
        // ("loses to a same-tick jump"), so for the UFO the two sides now share
        // the same convention. ship/swing pressing is not an impulse, so
        // unrelated; cube/ball/robot are measured on the on-ramp jump's additive
        // side (kSlopeJumpBonus), so they are untouched here.
        // [2026-08-21 r91] The UFO-only gate was widened. The rig ceilhold's 12
        // cube/robot cells all show "the jump tick keeps the jump value (+ the
        // ramp bonus), then plain gravity decay from the next tick", and the
        // launch never appears.
        // [2026-08-21 r95] ...but **the hang (flipped) side only**. Widened to
        // every mode, lv16's cold run hit a wall at x=9,790 (at t=6,516's
        // `slope+1.00/ride24+` GD emits the launch 9.208 on the press tick =
        // on the floor side **the launch beats the jump**, exactly as the note
        // below says). The census stayed green -- an instance of "if you touch
        // acquisition/launch conditions, measure at layer 4 (cold)"
        // ([[gd-census-green-cold-broken]]).
        // The rig's cells are all flip=1, so the evidence covers the c.flip side
        // only.
        if (!c.onSlope && s.onSlope && uphillExit && !rampWindowHere
            && !(impulsedThisTick && (c.mode == 3 || c.flip))) {
            // ...and the SIGN follows the same ride side as uphillExit above: a
            // launch throws the player off the surface it climbed, so a flipped
            // rider on a floor ramp's TOP leaves upward exactly as an upright one
            // does. lv22 t=3,806 measures it: GD emits **+6.906**, and keyed on
            // the flip alone the model emitted -6.906 (dvy -13.812).
            c.vy = (float)(((c.flip && rodeCeil) ? -1.0 : 1.0)
                           * slopeExitVy(std::fabs((double)s.slopeM), c.mode,
                                         useDx, c.mini != 0)
                           * slopeRampFactor((int)s.slopeT + 1));
            c.grounded = 0;
            // The slope machinery sets GD's velocity-limit exemption
            // (postCollision +0x18ca, next to the launch record update). A
            // slow exit clears it at the next tick's band check; a fast one
            // (>= 6.4 on the fall side) is what removes the swing's terminal
            // -- lv22 x=5,391. Swing-scoped like the rest of State::boost.
            if (c.mode == 7) c.boost = 1;
            // ...and "like a pad" includes the y: the ramp is a block, so the
            // tick's gravity step is in y and nothing re-seats the player.
            // Measured on lv19 t=254 (ramp uid=33 at (300,105), exit 3.999):
            // GD 135.000 -> 134.9514 while the model stayed at 135.000. That
            // 0.0486 rode the whole arc and made the landing at t=290 one tick
            // late, which is where lv19's 2,818 divergent ticks start.
            releasePin();
        }
        // [2026-08-19] **The downhill release stamps the face's velocity.** The
        // counterpart of the uphill launch: on the downhill release tick GD
        // assigns vy := mTravel*dx/0.25 (the slope version of "the rising-floor
        // catch stamps the floor velocity" = the dcy/0.25 convention).
        // The old note's "running off the bottom of a downhill ramp just leaves
        // the player falling" was a placeholder, not a quoted measurement.
        //
        // 2 measurements, both a **seam blip** of og=0 for exactly 1 tick
        // (standtrace's slope=[-] names it):
        //   lv16 t=4,147 (flipped ship, uid1468 m=+0.5, flat clamp 435.000):
        //     GD vy=+3.229 = 0.5x1.6143/0.25, y stays 435.000 (edy 0)
        //   lv19 t=15,113 (UFO, uid9899 m=-1, flat clamp 285.000):
        //     GD vy=-6.457 = -1.6143/0.25, y is 284.9807 (-0.0193)
        // Both re-grab onto the flat clamp the next tick, back to vy=0 / og=1.
        // The release gate is "the tick the contact point (centre -/+ (pH-xoff))
        // exits the ramp's end"; for lv16, the first tick where 5,832.42-11.459
        // = 5,820.96 > x1=5,820 holds.
        // **Loses to a same-tick jump** (the opposite of the uphill launch).
        // GD's order is update->collisions->buttons, so the vy written by the
        // release (collision phase) is overwritten by the button afterwards.
        // Removing the gate exposed itself at once: lv16's tracking
        // 17,872->17,427, lv21 -1, families +4 (all in1).
        else if (!c.onSlope && s.onSlope && !rampWindowHere
                 && !impulsedThisTick) {
            c.vy = (float)(mTravel * std::fabs((double)K.dxF) / 0.25);
            c.grounded = 0;
            // same as the uphill launch above: the slope machinery sets the
            // velocity-limit exemption (State::boost)
            if (c.mode == 7) c.boost = 1;
            releasePin();
        }
        // Ride counter for the exit ramp above: 0 on the contact tick,
        // +1 per riding tick, saturated at 24 (factor is 1.0 from there on).
        // A chain of contiguous ramps keeps counting -- GD only writes
        // m_slopeStartTime on the air->slope transition.
        //
        // ...which it did not, because the seam between two ramps drops onSlope for a
        // single tick and that reset the counter. The player does not leave the ground
        // there, so GD keeps the start time and the ride is one ride.
        //
        // Measured on lv16 (the ball chain at x=7,770/7,815/7,845, m=0.5 -> 1 -> 1):
        //   t=5,377  onSlope=1 slopeT=14   GD onGround=1
        //   t=5,378  onSlope=0 slopeT=0    GD onGround=1, y=341.000 -- the seam
        //   t=5,379  onSlope=1 slopeT=0    counting again from nothing
        //   t=5,400  onSlope=1 slopeT=21 -> factor 22/24 -> exit 6.330
        // GD launches at 6.906 = the saturated value (cube 9.207 x 0.75 = 6.905), i.e.
        // its ride was >= 24 ticks: 14 + the seam + 21 = 36. Carrying the counter across
        // the seam reproduces that exactly. It is the only exit in the 22-level corpus
        // that is neither saturated nor short, which is why the other twelve all matched
        // to four decimals with the counter broken.
        //
        // Gated on slopeT > 0 as well as grounded, so that acquiring a ramp from flat
        // ground still starts at 0 -- the contact tick is not a seam.
        const bool rampSeam = (s.grounded != 0 && s.slopeT > 0);
        c.slopeT = c.onSlope
            ? (uint8_t)((s.onSlope || rampSeam) ? std::min<int>(24, (int)s.slopeT + 1) : 0)
            : (uint8_t)(rampSeam ? s.slopeT : 0);
        (void)0;
    }
    // Out of play. The ship branch already had this; the cube branch had no
    // bound at all, so a flipped cube with nothing above it kept "falling" up
    // forever and the search followed it (lv10: the frontier was at y = 2,792
    // by t=1686, and GD agreed tick-for-tick -- it was not a fidelity bug, the
    // model simply had no reason to stop expanding a branch that can never come
    // back). The bound comes from the level's own highest surface.
    if (c.frame == 0) {
        if (c.y > g_yBound || c.y < (kGroundY + pHalf) - 40)
            DIE("out-of-play", nullptr);
    } else if (std::fabs((double)c.y) > g_yBoundTurned) {
        // In a turned frame `y` runs along a world axis with either sign, so
        // the un-turned bounds mean nothing. This one only stops a branch that
        // has left the level entirely.
        DIE("out-of-play", nullptr);
    }
    // upside down, airborne, and above every surface still ahead: it can never
    // come down again (see g_topAhead). Sound for cube and ball, whose airborne
    // input does nothing -- but NOT for the ship, which flies back down by
    // holding. Including the ship broke lv8 (cleared -> stuck at t=3002).
    // ...and the WAVE is excluded for the same reason as the ship: it flies
    // back down the moment the input is released, so "above everything ahead"
    // is not a dead end for it.
    // ...and it is skipped in a turned frame, where BOTH sides of the test mean
    // something else: `x` is the frame's progress axis (= -world y) so
    // topAheadAt() is looking up a level column that does not exist, and `c.y`
    // is a world x. In lv22's rotated corridor that made every tap branch die
    // instantly ("flip -> airborne -> 8231 > topAhead(-744)"), which is why the
    // DP reported `0 edges` there -- a search bug wearing a physics costume.
    // In a rotated frame, the same test is done **in that frame's coordinates**
    // (topAheadAtF). 2026-08-15: it had been disabled by restricting to frame 0,
    // but that left never-returning branches and lv22's cold entrance regressed
    // (the old exe reached x=2,441 at iter 11; the disabled build 2,343 in 26
    // iterations). `flip` still means "flipped against that frame's vertical" in
    // a rotated frame, so it can be used as is.
    if (!dead && c.mode != 1 && c.mode != 4 && c.flip && !c.grounded
        && (double)c.y - pHalf > topAheadAtF((int)c.frame, x))
        DIE("escapee-prune", nullptr);
    // Portals fire when the player's box first OVERLAPS the portal's box, not
    // when it crosses the portal's centre column. Measured on lv4's gravity
    // portal at cx=8775 (rect 25 wide, so left edge 8762.5):
    //   t=6730 x=8747.16 -> right edge 8762.16, no overlap, GD does not fire
    //   t=6731 x=8748.46 -> right edge 8763.46, overlap,    GD fires
    // Centre-crossing put the model 27 px (~20 ticks) late, which on lv4 meant
    // the flip happened after the player had already run past the ledge.
    // ...and the test is a plain box OVERLAP, checked every tick the player is
    // inside the portal, not a one-tick crossing of its left edge. With the
    // crossing form, a player whose y did not line up on that exact tick missed
    // the portal FOREVER: lv7 t=15490 crossed the cube portal's edge 1.3 px too
    // high (|dy| = 59.26 vs the 58 limit) and the model stayed a ship for the
    // rest of the level, while GD entered four ticks later at |dy| = 57.57.
    // Re-firing is prevented by the no-change guard below, not by the geometry.
    // ...and the MINI WAVE tests portals with GD's own getObjectRect (6x6, half
    // 3.0), not with kWaveHalfMini. That 2.0 is a bracket off a SPEED portal's
    // firing (see the constant) and it is 1 px too small here -- the same gap the
    // hazard box already had to split out. Measured on lv20's stacked cube +
    // RegularSize portals (uid525/527 at (1139.11,177.23), rot 53, real box
    // 34x86), read off a plain replay of the verified solution:
    //   t=841 x=1090.539 y=162.853  GD does NOT fire -> half <  3.9988
    //   t=842 x=1091.837 y=165.449  GD DOES fire     -> half >= 2.7008
    // With 2.0 the model fires NEITHER portal and runs the whole rest of the
    // level as a mini wave: GD is a full-size cube from t=842 and the model is
    // still oscillating at t=20,000. The replay of lv20's own solution used to
    // die at t=965; with 3.0 it reaches t=4,601.
    const double pHalfPortal =
        (c.mode == 4 && c.mini) ? kWaveContactHalfMini : pHalf;
    // [night-3, rejected] Making the mini's portal contact half-width 7.5
    // (kMiniContactHalf). The rig ramps' "only the mini->normal size portal is 1
    // tick early, 8/8" is most likely **the timing of the re-seat after the size
    // change, not the contact half-width** (GD's dump still writes 99 on the
    // firing tick and becomes 105 the next tick).
    // Widened to all portals, families 27->64 (11 levels regress, gdm1/gdm2 =
    // missed mode portals); even limited to size portals, families 27->37
    // (8 levels regress). **The rig and the corpus contradict each other**, so
    // on hold.
    const double pHalfSize = pHalfPortal;
    // [2026-08-20 r32] **Only rotated objects are tested against a box rebuilt
    // within the same pass.** The disassembly in the note above says so: the
    // AABB is copied into 4 registers at call entry (0x2149b8), but for objects
    // with `[rdi+0x2e8]` set, 0x214BA1 **re-calls the player's getOrientedBox()
    // on the spot**. So axis-aligned portals keep seeing the entry box (the two
    // "shadow" cases, lv19 t=20,309 / lv18 t=14,612, are both axis-aligned),
    // while **a rotated portal sees the result of the mode/size portal that
    // fired just before it**.
    // Measured lv18 t=20,465 (mini wave, all 3 at rot=51):
    //   uid14052 cube  OBB 34x86 width axis 20.33 ... fires at half>2.377;
    //                                    not firing at 20,464 needs half<4.005
    //   uid14054 speed OBB 51x56 width axis 28.07 ... half>1.83; 20,464 needs
    //                                    half<3.46
    //   uid14055 size  OBB 31x90 width axis 22.80 ... needs **half>5.209**
    // No single half satisfies all (the cube demands <4.005). If the cube fires
    // first and the box becomes the mini cube's 9, then 15.5+9*1.4008=28.1 >
    // 22.80 passes, and at 20,464 the cube itself does not fire so the size one
    // does not fire either -- all 3 fit.
    double pHalfLive = pHalfPortal;
    // GD's collisionCheckObjects snapshots the player's rect ONCE at call
    // entry (disasm 0x2149b8: plain getObjectRect -> four AABB registers) and
    // nothing later in the pass refreshes it -- not a landing clamp, not a
    // size portal firing earlier in the same pass. Both measured mistimings
    // are exactly that shadow:
    //   lv19 t=20,309: mini ball (18x18, ccl/ptt-verified -- the dump's
    //     vsize column said 1.0 and lied) walks into a RegularSize portal,
    //     re-seats 264->270 mid-pass; the SPEED portal right behind it still
    //     sees the ENTRY box (top 273 < 275) and fires only next tick.
    //   lv18 t=14,612: the landing clamp seats the mini cube at 219 during
    //     the tick, but the registers hold the pre-clamp 217.1 (top 226.15 <
    //     226.4) -- the speed portal fires at 14,613 with the full box.
    // So the speed test reads THIS snapshot: tick-start y on a landing tick
    // (the clamp is inside GD's collision phase, after the registers), the
    // pre-pass y otherwise. The half is pHalf, which is tick-start by
    // construction (mode/size changes only happen inside the pass).
    const double yColl = (c.grounded && !s.grounded) ? (double)s.y
                                                     : (double)c.y;
    // A SPIDER that teleported on this tick has only just arrived: GD's tick is
    // update -> collisions -> buttons, so the landing spot's portal (and pad,
    // and orb -- see the `teleportedThisTick` breaks further down, which say the
    // same thing for the type-28 teleport portal) is read on the NEXT tick.
    // Measured on lv22 t=2,023: GD teleports to y=163.5 and leaves with the
    // teleport's own vy=-1.000, and only at t=2,024 does the yellow pad at
    // (2407,152) give it 9.600 and the ball portal uid1590 switch the mode.
    // The model did all of it in the one tick, so its y ran 2.385 px above GD's
    // for the next 182 ticks.
    // ...and the SAME rule applies to the destination's PORTALS, which this
    // loop was missing: the note above says so but nothing enforced it, and a
    // gravity portal sitting at the landing spot fired on the arrival tick.
    // Measured on lv21 t=4,710 (teleport uid5090 at (6147,275) -> y=415, with
    // the type-4 gravity portal uid5050 at (6129,415) waiting there):
    //   GD    t=4,710 y=415 vy=2.428 up=1   t=4,711 vy=1.322 up=0
    //   model t=4,710 y=415 vy=1.214 up=0   (halved and flipped a tick early)
    // GD's tick is update -> collisions -> buttons, so the collision pass for
    // t=4,710 already ran at the OLD y (254.95) where that portal is 160 px
    // away. edvy = +1.214, edy = 0 -- the family
    // `m0/mini0/g0/gdg0/sp0.9/air/in0` in fixcensus.
    //
    // Two passes rather than one `continue`, because the order the slice yields
    // is by cx and the teleport can sit AFTER the portal it should suppress
    // (here 6,147 vs 6,129). Pass 0 is the teleport, pass 1 is everything else
    // and is skipped entirely once one fired. With no teleport in the window
    // the two passes are the single old loop, unchanged.
    // It is a lambda so the order can be **swapped only on a teleport tick**
    // (measurements at teleUid's declaration). GD processes in uid order, so when
    // the landing site's pad has a smaller uid than the portal, the order becomes
    // pad -> portal. lv20 t=7,282: yellow pad uid7025 (vy := -16 with flip=1) is
    // followed by gravity portal uid7028 (flip + halve), giving -8.000. With the
    // model's fixed order (portal -> pad) it becomes flip=0 first, takes +16, and
    // flies opposite to GD from then on.
    auto runPortalPass = [&](int pport) {
    for (const Obj* p : *K.ports) {
        if (spiderWarpedThisTick) break;
        if ((p->type == 28) != (pport == 0)) continue;
        // uid-order gate (measurements at teleUid's declaration). Objects
        // processed before the teleport were judged at the old position = do not
        // fire at the landing site.
        if (pport == 1 && teleportedThisTick && p->uid < teleUid) continue;
        // only the size portal has the smaller contact half (pHalfSize note)
        // ...only rotated objects use "the current box" (pHalfLive note).
        // Axis-aligned ones keep the entry snapshot.
        const double pHalfP =
            p->oriented ? pHalfLive
                        : ((p->type == 17 || p->type == 18) ? pHalfSize
                                                            : pHalfPortal);
        if (std::fabs(x - p->cx) > p->hw + pHalfP) continue;
        // ...and a TURNED portal's bound is far wider than the portal. lv18's
        // cube portal at (27,755.8,371.162) is 51 degrees over, bounds 88 px and
        // is really 34: firing on the bound made the model a cube 48 px early,
        // it flew over the ramp that kills GD's wave, and the DP returned SOLVED
        // on a plan GD kills at x=27,713. See Obj::oriented / orientedHit.
        // ...at the rotation GD has on THIS tick. `s.rot` is where the previous
        // tick ended and the model applies its own step later (the block by the
        // stair snap), so the portal looks one step back: measured on lv22,
        // s.rot sits 1.706 deg past GD's value for the same tick and one step
        // back matches it to 0.025 deg.
        double pRotHere = (double)s.rot;
        if (s.mode == 0)
            pRotHere += (s.rotNeg ? 1.0 : -1.0) * (s.mini ? 2.25 : 1.7307692);
        // --slopedbg: make "passed the x window but rejected by the OBB" visible.
        // Without this it cannot be told apart from "never entered the window"
        // (item 4's diagnosis stalled on whether the size portal's 1-tick delay
        // came from here or the x window).
        if (g_slopeDbg && p->oriented
            && !orientedHit(*p, x, (double)c.y, pHalfP, pRotHere))
        {
            const double thO = std::atan2(-p->rs, p->rc);
            const double a1 = -thO;
            const double a2 = -pRotHere * 3.14159265358979 / 180.0;
            const double ddx = x - p->cx, ddy = (double)c.y - p->cy;
            const double c1 = std::cos(a1), s1 = std::sin(a1);
            const double c2 = std::cos(a2), s2 = std::sin(a2);
            const double axv[4] = { c1, -s1, c2, -s2 };
            const double ayv[4] = { s1,  c1, s2,  c2 };
            double marg[4];
            for (int i = 0; i < 4; ++i) {
                const double ux = axv[i], uy = ayv[i];
                const double r1 = p->ohw * std::fabs(c1 * ux + s1 * uy)
                                + p->ohh * std::fabs(-s1 * ux + c1 * uy);
                const double r2 = pHalfP
                                  * (std::fabs(c2 * ux + s2 * uy)
                                     + std::fabs(-s2 * ux + c2 * uy));
                marg[i] = (r1 + r2) - std::fabs(ddx * ux + ddy * uy);
            }
            std::printf("portobb t=%lld uid=%d type=%d x=%.2f y=%.2f "
                        "pcx=%.2f pcy=%.2f ohw=%.2f ohh=%.2f pHalf=%.1f "
                        "pRot=%.3f thO=%.3f marg=%.3f/%.3f/%.3f/%.3f "
                        "REJECTED-BY-OBB\n",
                        (long long)K.t, p->uid, (int)p->type, x, (double)c.y,
                        p->cx, p->cy, p->ohw, p->ohh, pHalfP, pRotHere,
                        thO * 180.0 / 3.14159265358979,
                        marg[0], marg[1], marg[2], marg[3]);
        }
        if (p->oriented
            && !orientedHit(*p, x, (double)c.y, pHalfP, pRotHere))
            continue;
        // A portal SITTING ON THE ROTATION TRIGGER THAT PUT US IN THIS FRAME
        // does not fire again: GD fires on ENTRY and the player is already deep
        // inside these when the frame turns (lv22: 36 px into uid4360's 52.5 at
        // t=4,665), so GD has them in its touched list. `r.frame == c.frame`
        // keeps this to the trigger that owns the current frame -- matching ANY
        // trigger also ate the robot portal uid4607, 10 px from uid4467, and
        // cost the mode switch GD makes at t=5,108.
        if (c.frame != 0 && !g_rotTrig.empty()) {
            double wpx, wpy;
            fromFrame((int)c.frame, p->cx, p->cy, wpx, wpy);
            bool onRot = false;
            for (const RotTrig& r : g_rotTrig)
                if (r.frame == (int)c.frame
                    && std::fabs(r.cx - wpx) <= 60.0
                    && std::fabs(r.cy - wpy) <= 60.0) { onRot = true; break; }
            if (onRot) continue;
        }
        // Would this portal change anything at all? An inert one is a no-op and
        // dodging it means nothing (see the halving note below).
        // A teleport's REAL target: the linked exit half (tpEy) for the 2902
        // family, the closed tpY formula for id 747 -- see Obj::tpEx/tpEy.
        // lv22's uid 6437 sits AT its own tpy (1905) and read as inert while
        // GD drops the player to its orange half at 645.000.
        const double tpTarg = (p->type == 28 && p->id != 747
                               && (p->tpEx != 0.0 || p->tpEy != 0.0))
                                  ? p->tpEy : p->tpY;
        const bool changes =
            (p->type == 23 || p->type == 24)
                ? (c.dual != (uint8_t)(p->type == 23 ? 1 : 0))
            // a teleport is inert only when the player is already at its
            // target AND its gravity mode asks for nothing new (teleportPlayer
            // itself has no inertness check -- it always moves and applies
            // m_gravityMode, so "inert" is only ever true when both halves are)
            : (p->type == 28) ? (std::fabs((double)c.y - tpTarg) > 0.01
                                 || (p->tpGrav == 3)
                                 || (p->tpGrav == 1 && gdUpOf(c) != 0)
                                 || (p->tpGrav == 2 && gdUpOf(c) == 0))
            // Gravity portals state GD's `upsideDown`, which is NOT the model's
            // `flip` inside a turned frame -- see gdUpOf.
            : (p->type == 3)  ? (gdUpOf(c) != 1)
            : (p->type == 4)  ? (gdUpOf(c) != 0)
            : (p->type == 17) ? (c.mini != 0)
            : (p->type == 18) ? (c.mini != 1)
                              : (c.mode != ((p->type == 5)    ? 1
                                            : (p->type == 16) ? 2
                                            : (p->type == 19) ? 3
                                            : (p->type == 26) ? 4
                                                              : 0));
        // ---- hairline dodges are not routes (user's call, 2026-08-03) -------
        // The physics is untouched: GD fires on box overlap and so does the
        // model. What is dropped here is the SEARCH BRANCH that slips past a
        // portal by less than g_portalDodgeMin -- such a portal is treated as
        // unavoidable. Real levels do not place traps that need a sub-pixel
        // dodge, so a hairline miss is a modelling artefact, not a plan.
        // WHY: lv18's RegularSize portal at (20,386.6, 266.992) is missed by
        // 0.01 px -- the plan rides the flying band's ceiling at y=321.000, so
        // its foot is at 312.00 against the portal's top edge of 311.99 -- and
        // the level cannot be played mini from there: the mini cube's jump
        // apex is +43.6 px where the corridor at x=24,660 needs +60.
        // (Widening the FIRING rule instead would be wrong: GD really does not
        // fire at 0.01 px, and the model would then plan a size change GD never
        // performs.)
        // y is **the pre-clamp position** (post-update, pre-collision = the
        // register snapshot of GD's collision pass, same mechanism as the 8862
        // note) [2026-08-19, settled with worker98's 6 injection points].
        // lv22 uid8743's (spider portal, cy=469 hh=43) y approach:
        //   riding a ramp: no fire at post-clamp 411.87 / fires at 412.40 (free
        //     y) -> measuring with the y the ride's clamp lifted is 1 tick early
        //   free flight: fires at post-move 411.357 (411.357+15=426.36 >= 426 OK)
        //   the x side is consistent at both sites with the post-move position
        //   (pH=15 unchanged):
        //     uid4445 @4,635 |dx|=31.06 <= 32 fires / 32.68 no fire
        //     uid8743 30.99 fires / 32.39 no fire
        // [2026-08-27] ...and that is `yFree`, which the tick now carries
        // outright (see its declaration). The two things this replaces were both
        // approximations of it: `c.y`, correct only when nothing in the
        // collision pass moved the player, and a slope-riding special case
        // `s.y + vy*kYScale` reconstructing the same number from the free
        // bookkeeping vy keeps while riding.
        // What the general value adds is every OTHER way the pass moves the
        // player. Measured on lv20 t=17,108/17,109 (a UFO pressed down a ceiling
        // ramp -- not a slope ride, so the old special case did not apply):
        //   t=17,108  seat 299.193 (fires, wrongly)   free 300.105 (does not)
        //   t=17,109  seat 297.580                    free 298.492 (fires)
        // and GD fires the dual portal uid13879 at 17,109. The seat runs ~0.9 px
        // ahead of the free y here, so the model was buying a mode change one
        // tick early and planning the rest of the section from it.
        // This is also what the "the dual portal's box is 0.7 px shorter than
        // objrects says" reading of 2026-08-27 really was: swept from the
        // player's post-clamp y it needs a shortened box, from the free y it
        // fires at the dump's own h=91.
        const double yPort = (double)yFree;
        const double gapY = std::fabs(yPort - p->cy) - (p->hh + pHalfP);
        // --slopedbg: `changes` and the y gap, the two things between a portal
        // being a candidate and it firing. `portfire` above only says "in the x
        // window", so a portal that reaches this line and dies here looked
        // identical to one the window never let in (lv22 t=5,110).
        if (g_slopeDbg)
            std::printf("portgate t=%lld uid=%d type=%d changes=%d gapY=%.3f "
                        "y=%.3f pcy=%.3f phh=%.2f pHalf=%.1f flip=%d frame=%d "
                        "prot=%.3f orient=%d\n",
                        (long long)K.t, p->uid, (int)p->type, changes ? 1 : 0,
                        gapY, (double)c.y, p->cy, p->hh, pHalfP,
                        (int)c.flip, (int)c.frame, pRotHere,
                        p->oriented ? 1 : 0);
        if (gapY >= 0.0) {
            if (changes && gapY < g_portalDodgeMin)
                DIE("portal/hairline-dodge", p);
            continue;
        }
        // [2026-08-21 r52] **A gravity portal right after a rotation-frame
        // change does not fire if the player was already inside it.** Changing
        // up on frame entry/exit is not the portal's job, but the `changes` gate
        // only looks at "is the current up different from the wanted up", so a
        // portal being sat in comes to "correct" it the next tick.
        // Measured lv22 t=6,323 (exit of the rotated section, ball, sp1.1,
        // type-4 uid13833 (id10, 25x75, cy=395 hh=12.5)):
        //   t=6,310..6,322 inside the box the whole time (changes=0, no fire)
        //   t=6,322 both GD and the model leave the frame, up 0->1, vy=-7.813
        //   t=6,323 GD stays at -7.684 (=-7.813+0.129). Only the model re-fires,
        //     restores up, and halves vy to -3.842
        //     (census `m2/mini0/g0/gdg0/sp1.1/air` edvy -3.842)
        // **Generalising is already rejected** ("a gravity portal fires only on
        // the entry tick" regressed lv5/14/18/20/21. GD really does re-fire in
        // some situations). The test is the same geometry as the firing, at the
        // previous tick's position.
        if ((p->type == 3 || p->type == 4) && s.frameChg
            && s.frame == c.frame) {
            const double gapYPrev =
                std::fabs((double)s.y - p->cy) - (p->hh + pHalfP);
            const bool wasInside =
                std::fabs(xPrev - p->cx) <= p->hw + pHalfP
                && gapYPrev < 0.0
                && (!p->oriented
                    || orientedHit(*p, xPrev, (double)s.y, pHalfP, pRotHere));
            if (wasInside) { gravHoldOver = true; continue; }
        }
        // ---- TELEPORT portal (type 28) --------------------------------------
        // Below the y test, not above it: lv20 stacks two teleports at the SAME
        // x (four such pairs, 300 px apart) and firing on the x overlap alone
        // sent the player to the other one's destination every time -- 241
        // where GD lands on 335. It is the y that tells the pair apart.
        //
        // Separate from the mode-portal block that follows because a teleport
        // is not a mode change: it moves the player and touches NOTHING else.
        // Measured across the teleport tick on lv20's nodeath pass (four
        // portals: full cube, mini cube, wave): y becomes the portal's absolute
        // target, vy keeps stepping by its ordinary per-tick gravity with NO
        // halving, x keeps its uniform advance, the mode is unchanged. So it
        // must not fall through to the "every portal halves vy" rule.
        // Gravity is switched only when the portal asks (m_gravityMode); all
        // twenty of lv20's ask for nothing.
        // Grounded is dropped: whatever the player stood on is 200 px away now.
        // Every measured teleport was already airborne, so that part is
        // reasoning rather than measurement -- if a grounded one ever shows up,
        // check it.
        if (p->type == 28) {
            if (!changes) continue;   // already at the target
            // The destination's PADS and ORBS do not fire on the arrival tick.
            // GD's checkCollisions has already run for this tick at the old
            // position, so nothing at the target is touched until the next one.
            // Measured at lv20's own wall, t=2571 (teleport to y=136 with a
            // yellow pad under it): both sides agree on y=136, GD keeps its
            // incoming vy=5.858 (= 6.074 - 0.216, the ordinary gravity step)
            // and only reports 16.000 at t=2572, while the model fired the pad
            // on the arrival tick and left 2.3 px high from there on.
            // Hazards are deliberately NOT skipped: not killing where GD kills
            // is the dangerous direction, and no measurement covers it.
            teleportedThisTick = true;
            teleUid = p->uid;      // uid-order gate (measured, see teleUid's decl.)
            c.y = (float)tpTarg;   // exit half for 2902, closed tpY for 747
            // A teleport is the one thing in the pass that MOVES the player for
            // the objects behind it: the uid-order gate above exists because
            // lv20 t=7,295's pad and gravity portal (both larger uids) fire at
            // the landing site on the arrival tick. So the position the rest of
            // the pass judges from is the target, not the free y that got here.
            // Without this the gravity portal uid7028 stops firing and the
            // yellow pad's -16 is never halved to GD's -8.000.
            yFree = c.y;
            c.grounded = 0;
            c.snapObj = nullptr;
            c.snapDist = 0.f;
            c.onSlope = 0;
            c.slopeM = 0.f;
            // tpGrav states GD's upsideDown (flipGravity's argument), which is
            // the model's `flip` mirrored in frame 3 -- same conversion the
            // gravity portals make (gdUpOf). flipGravity is a NO-OP when the
            // player already has that gravity (disasm 0x39a1e9: early-out on
            // equality), and the vy halving below lives INSIDE that no-op
            // guard, so "did gravity actually change" is load-bearing.
            int wantUp = -1;
            if (p->tpGrav == 1) wantUp = 0;
            else if (p->tpGrav == 2) wantUp = 1;
            else if (p->tpGrav == 3) wantUp = gdUpOf(c) ? 0 : 1;
            const bool gravChanged =
                (wantUp >= 0 && gdUpOf(c) != (uint8_t)wantUp);
            if (gravChanged)
                c.flip = (c.frame == 3) ? (uint8_t)!wantUp : (uint8_t)wantUp;
            if (p->id == 747) {
                // A gravity-switching 747 ASSIGNS its exit vy -- it does not
                // carry the entry vy the way lv20's plain pairs (tpGrav=0) do.
                // Measured on lv22 uid6158 (x=13,035, tpy=605, tpg=1): entry vy
                // 8.000 and 15.000 (the flipped-rise cap; 20 was capped to 15
                // by GD itself first) both exit at EXACTLY +18.000 world-up on
                // the arrival tick, decaying by ordinary cube gravity from
                // there (GD 9079: 17.785 -- so no rise cap on the way out
                // either). The missing 3.000 lowered the arc 51 px and hid the
                // yellow orb at x=13,155 from the DP: press branches merged
                // with coasting everywhere (alive=1) and the frontier died in
                // the spike V at x=13,330. UNVERIFIED: tpGrav==2/3 (no
                // instance in lv1-22), mini scaling, non-cube modes.
                if (p->tpGrav == 1) c.vy = 18.0f;
            } else {
                // The 2902 family carries vy through flipGravity instead:
                // *0.5 when gravity actually changes (PlayerObject::flipGravity
                // 0x39a2d4: m_yVelocity *= 0.5, world frame, sign kept),
                // nothing otherwise. Both lv22 crossings land EXACTLY:
                //   t=13,223 uid6425 airborne, up1->0: (-4.408+0.216)*0.5
                //            = -2.096, GD -2.096
                //   t=14,213 uid6437 grounded on blocks, up0 stays: one full
                //            old-gravity step -0.216, GD -0.216 (no halving)
                // The grounded value is the same collision-order story as the
                // gravity portals' (see the vAtPortal note below): standing on
                // a SOLID the clamp lives in checkCollisions and the teleport
                // gets the gravity-stepped vy; on the GROUND layer the update
                // already zeroed it. UNVERIFIED: non-cube grounded entry.
                double vTp = (double)c.vy;
                if (s.grounded && c.mode == 0) {
                    const bool onFloor = !s.flip && s.frame == 0
                        && (double)s.y <= (kGroundY + pHalf) + 0.5;
                    if (!onFloor)
                        vTp = (s.flip ? 1.0 : -1.0)
                            * std::fabs(cubePhysFor(useDx).g);
                }
                c.vy = (float)(gravChanged ? vTp * 0.5 : vTp);
            }
            continue;
        }
        {
            // EVERY portal halves vy, not just the gravity ones. Measured on
            // lv5's ship portal (cx=13793) at t=10584: the tick's normal cube
            // gravity takes vy to -8.044 and GD then enters the portal with
            // -4.022 -- exactly half. The model kept -8.044, hit the ship
            // terminal -6.400, and dived 4.5 px below GD within ten ticks.
            // A cube standing still has vy = 0 in the model (the grounded branch
            // skips gravity), but GD still applies that tick's gravity before
            // halving. Measured on lv6 t=13616: the cube sat grounded at y=195
            // with vy = 0, crossed the inverse gravity portal, and GD came out
            // at -0.1080 = (0 - 0.216)/2 -- exactly "gravity, then halve".
            // Without this the model leaves the portal 0.108 (half a gravity
            // step) high and stays parallel to GD forever after.
            // A portal that does not CHANGE anything is a no-op -- in
            // particular it does not halve vy. Measured on lv7 t=6439: an
            // already-upright ship crosses a normal-gravity portal (type 4) and
            // GD leaves vy at 6.817, while the model halved it to 3.409 and was
            // 6 px below GD eight ticks later.
            // --slopedbg doubles as the portal trace: which portal fired, on
            // which tick, in which frame, and at what coordinate. Without it a
            // portal that fires at the WRONG TIME in a rotated frame is
            // indistinguishable from a physics bug (lv22 t=4,688).
            if (g_slopeDbg)
                std::printf("portfire t=%lld frame=%d x=%.2f y=%.2f uid=%d "
                            "type=%d pcx=%.2f pcy=%.2f\n",
                            (long long)K.t, (int)c.frame, x, (double)c.y,
                            p->uid, (int)p->type, p->cx, p->cy);
            const uint8_t wantFlip = (p->type == 3) ? 1 : 0;
            const uint8_t wantMode = (p->type == 5)    ? 1
                                     : (p->type == 16) ? 2
                                     : (p->type == 19) ? 3   // UFO
                                     : (p->type == 26) ? 4   // wave
                                     : (p->type == 27) ? 5   // robot
                                     : (p->type == 33) ? 6   // spider
                                     : (p->type == 41) ? 7   // swing
                                                       : 0;
            const uint8_t wantMini = (p->type == 18) ? 1 : 0;
            const bool isGrav = (p->type == 3 || p->type == 4);
            const bool isSize = (p->type == 17 || p->type == 18);
            // Dual (23) / solo (24). Measured on lv16 x=10,551: the second
            // player appears AT THE SAME POINT with the opposite gravity and
            // the opposite vy, so the two are mirror images until one of them
            // touches something. The solo portal drops the second half.
            //
            // "THE SAME POINT" IS THE PRE-COLLISION ONE (`yFree`), not `c.y`.
            // In free space the two are the same number, which is why this went
            // unnoticed: it only shows on a tick where the collision pass moved
            // p1 -- and there it puts the whole dual section in the wrong place.
            // Measured on lv20 t=17,109 (UFO pressed down a ceiling ramp at
            // x=25,750, dual portal uid13879):
            //   GD  p1 y = 297.580 (the ramp's seat), p2 born at **298.472**
            //   298.472 = 299.193 (p1's y one tick earlier) + 0.225 * -3.204
            //           = p1's position before the ramp pushed it back down
            // and three injections at the same site pin it further: p1 clamps to
            // the same 297.2145 for vy = -2.172 / -4.086 / -6.400 while p2 is
            // born at 298.675 / 298.581 / 298.060 -- i.e. at a place that still
            // depends on vy, so it cannot be the clamped position.
            // The old `c.y` put p2 0.892 px off, the mirror axis split by the
            // same amount, and the offset survived to the end of the level.
            if (p->type == 23 || p->type == 24) {
                const uint8_t wantDual = (p->type == 23) ? 1 : 0;
                if (c.dual == wantDual) continue;
                if (wantDual) {
                    c.y2 = yFree;
                    c.vy2 = -c.vy;
                    // born in whatever mode and size the first body is in right
                    // now (this pass's earlier portals included), and with no
                    // press behind it -- it has not been anywhere yet.
                    c.mode2 = c.mode;
                    c.mini2 = c.mini;
                    c.ceilT2 = 0;
                    c.ceilM42 = 0;
                    c.flip2 = c.flip ? 0 : 1;
                    c.grounded2 = 0;
                    c.ringHold2 = 0;
                    c.onSlope2 = 0;
                    c.slopeM2 = 0.f;
                    c.snapObj2 = nullptr;
                    c.snapDist2 = 0.f;
                    c.usedOrb2 = nullptr;
                    for (int i = 0; i < 4; ++i) c.usedPad2[i] = nullptr;
                }
                c.dual = wantDual;
                // Entering a dual re-derives the band from THIS portal's cy
                // with the current mode's dual height, and pins bandRefY there
                // for the whole dual section. Leaving one (24) does not touch
                // the band: measured on lv16, after the merge at x=15,103 GD
                // still reports [330,630], the ship-in-dual band.
                if (wantDual) {
                    c.bandRefY = (float)p->cy;
                    const FlyBand nb = bandFor(p->cy, bandHeightDual(c.mode));
                    c.bandFloor = (float)nb.floorY;
                    c.bandCeil = (float)nb.ceilY;
                    if (g_shipCeilSet) c.bandCeil = (float)(g_shipCeil + kCubeHalf);
                    if (g_ufoCeil > 0.0) c.bandCeil = (float)g_ufoCeil;
                    if (g_flyFloor > 0.0) c.bandFloor = (float)g_flyFloor;
                }
                continue;
            }
            // The band is written HERE, by the portal that actually fired --
            // not by every portal the layer's x has passed (see State::band*).
            //
            // ...and it is written BEFORE the "already in that state" bail-out
            // below, because **a mode portal you are already in still rewrites
            // the band**. Measured on lv20 with the ceilprobe build's pmin/pmax
            // columns: the wave portal at (4445,303) is entered as a wave at
            // t=3406 (x=4420.69, its box starts at 4425.28 and the wave's half
            // is 5) and GD's band goes [90,390] -> [150,450] on exactly that
            // tick -- which is bandFor(303, 300) to the pixel. Nothing else
            // changes: vy stays -5.193 across the portal, so there is no
            // halving and no impulse. The old comment called this portal INERT
            // and skipped it, which left the model on the x=867 portal's band
            // and clamped the wave at 380 (= 390 - 10) while GD climbed past
            // 400. That clamp was lv20's wall: the model sat grounded at
            // y=380.000 from t=3865 on.
            //
            // Safe for the other kinds: bandHeightFor returns 0 for gravity
            // (3/4) and size (17/18) portals, so an inert one of those still
            // writes nothing.
            {
                // Inside a dual the reference y stays the dual portal's cy and
                // only H follows the new mode -- and a cube portal, which is
                // inert outside a dual, updates it too (H = 270). Gravity and
                // size portals never switch mode, so they never call
                // updateDualGround and must not reach this at all.
                const bool isMode = !isGrav && !isSize;
                const bool inDual = isMode && c.dual != 0;
                const double H = inDual ? bandHeightDual(wantMode)
                                        : bandHeightFor(p->type);
                const double refY = inDual ? (double)c.bandRefY : p->cy;
                if (H > 0.0) {
                    if (!inDual) c.bandRefY = (float)p->cy;
                    const FlyBand nb = bandFor(refY, H);
                    c.bandFloor = (float)nb.floorY;
                    c.bandCeil = (float)nb.ceilY;
                    if (g_shipCeilSet) c.bandCeil = (float)(g_shipCeil + kCubeHalf);
                    if (g_ufoCeil > 0.0) c.bandCeil = (float)g_ufoCeil;
                    if (g_flyFloor > 0.0) c.bandFloor = (float)g_flyFloor;
                }
            }
            // Everything from here on is the state CHANGE, which an inert
            // portal does not do (the band above is the only thing it touches).
            // ...and this SECOND gate reads GD's statement too. It was the last
            // one on lv22 t=5,110: with only `changes` converted the portal
            // reported changes=1 and still did nothing, because
            // `c.flip == wantFlip` is 0 == 0 in frame 3.
            // ...and it reads the gravity the player had BEFORE this tick's
            // BALL TAP. GD's tick is update -> collisions -> buttons, so a
            // gravity portal has already decided by the time the tap flips;
            // the model taps first and then walks the portals, so a portal that
            // was inert on entry came out "changing" and fired.
            // Measured on lv22 t=5,940 (ball corridor, x=8,204): uid5810 is an
            // id-10 (normal) gravity portal at (8251,945) 75x25, i.e. active for
            // y in 917.5..972.5. The ball is at y=972.02 in NORMAL gravity, so
            // GD's portal is a no-op and the tap leaves at vy=+3.426 flipped.
            // The model tapped, became flipped, and the portal then flipped it
            // back AND halved it: vy=+1.713 unflipped, and the corridor after it
            // was a different level. The pad block below states the same
            // ordering the other way round ("the pad wins over the tap").
            const uint8_t gdUpPortal =
                (isGrav && ballFlippedThisTick)
                    ? ((c.frame == 3) ? (uint8_t)!s.flip : (uint8_t)s.flip)
                    : gdUpOf(c);
            // [2026-08-22 r100] **A gravity portal during a dual passes through**,
            // and **even a same-mode portal produces the flying-end halving**.
            // Measured lv16 t=13,496 (second dual, a mini ship, still mini,
            // touches the boxes of ship portal uid6721 and gravity portal
            // uid6748 (id10=normal gravity) on the same tick):
            //   GD p1 vy 1.153 -> **0.319** = (1.153+0.122) x **1/4**
            //   GD's upsideDown **stays p1=1 / p2=0 to the very end**
            //   (no flip even following past the gravity portal's 25px span)
            // So only **the same-mode ship portal** is in effect, and its value
            // is two flying ends (leaving 1/2 x entering 1/2) = x1/4. The
            // gravity portal does not fire in the dual (r82's "gravity portal
            // passes through" reading is correct for this sample).
            // [2026-08-22 r101] **In a dual, when both bodies enter the same
            // gravity portal's box, flipGravity runs twice.** GD itself was made
            // to state it (py/gdprobe.py 16 --cfg hitboxtrace=1; the MOD's
            // `pfg:` line writes m_player1's flipGravity per call):
            //   pfg: t=13,495 flip=0 up 1->0 vy=0.637   (half of 1.275)
            //   pfg: t=13,495 flip=1 up 0->1 vy=0.319   (halved again)
            // -> **vy is x1/4, up round-trips back to what it was**. The
            // identity of corpus family `m1/mini1/g0/gdg0/sp1.1/air` edvy
            // -0.318; it was neither "same-mode ship portal is x1/4" nor
            // "gravity portal passes through" (both refuted on the dualport
            // calibration rig).
            // **When only one body is inside, as before** (dualport 12 units:
            // the touching body gets x1/2 + flip, the partner flip only).
            // **Entry tick only.** This bypasses the inert gate (`changes`), so
            // it would re-fire for as long as the player is inside the box (the
            // first implementation did exactly that and broke t=13,497 onward).
            // "Was already inside" is read at the previous tick's position.
            const bool wasInBoxPrev =
                std::fabs(xPrev - p->cx) <= p->hw + pHalfP
                && std::fabs((double)s.y - p->cy) < p->hh + pHalfP;
            const bool dualBothGrav =
                isGrav && c.dual && !wasInBoxPrev
                && std::fabs((double)c.y2 - p->cy) < p->hh + pHalfP;
            // ...and **a dual's gravity portal fires only on the entry tick**.
            // Once the double call returns up to what it was (above), the next
            // tick reads "still not the wanted up" = `changes` raises and the
            // normal path re-fires. On GD's side the object's activation flag
            // makes it one-shot.
            // **Dual only** -- the non-dual "entry tick only" generalisation is
            // already rejected (lv5/14/18/20/21 regressed, the r52 note).
            if (isGrav && c.dual && wasInBoxPrev) continue;
            // [Rejected (2026-08-22 r100)] Two rules were tried from the reading
            // above:
            //   (a) `if (isGrav && c.dual) continue;` (gravity portals pass
            //       through during a dual)
            //   (b) emit the x1/4 for same-mode fly portals too
            // **With both in, lv16's second dual is already dy -0.466 by
            // t=13,494** -- i.e. (a) also kills the gravity portal **before**
            // this section, one GD genuinely fires. Same shape as r82's failure.
            // At least 3 combinations produce the same x1/4 for the single
            // sample (t=13,496):
            //   ship 2 ends x1/4 + gravity pass-through / ship 1 end x1/2 +
            //   gravity that "does not flip but does halve" / ...
            // **Do not touch until a dedicated dual rig separates them.**
            if (!dualBothGrav
                && (isGrav ? (gdUpPortal == wantFlip)
                           : isSize ? (c.mini == wantMini)
                                    : (c.mode == wantMode))) continue;
            // ...except the BALL portal, which does not halve at all. Measured
            // on lv10 t=1743: vy goes 0.5960 -> 0.3800, i.e. that tick's cube
            // gravity (-0.216) and nothing else. Ship, cube and gravity portals
            // all halve (lv5 -8.044 -> -4.022, lv7 -1.979 -> -0.990, lv6
            // 0 -> -0.108); type 16 is the exception. Measured once -- re-check
            // at lv10's second ball portal (x=18405) when a plan reaches it.
            // ...and neither does the SIZE portal. Measured on lv11 t=3384:
            // the cube is grounded (vy = 0) when it enters the mini portal, and
            // GD's next tick shows vy = -0.2160 -- one FULL cube gravity step.
            // The "gravity then halve" rule would have produced -0.1080.
            // The type-16 exemption below was the zero-flying case of the
            // "one half per flying end" rule wearing a disguise (the big
            // comment below even says so). Measured on lv16's second dual,
            // UFO -> ball at (19193,543): GD comes out at 2.868 =
            // (5.888 - 0.152) * 0.5 -- ONE flying end, ONE halving, ball
            // portal or not. So the ball portal only skips the halving when
            // NEITHER end flies (the lv10 cube -> ball measurement).
            const bool halves = !isSize;
            double vAtPortal = (double)c.vy;
            // [2026-08-18] ...but **it differs only when the footing is an
            // object**. GD's ground layer stops y inside PlayerObject::update =
            // before checkCollisions, so the portal halves 0 and emits 0.
            // Standing on a solid, the grounding clamp lives inside
            // checkCollisions and, depending on uid, the portal can come first
            // -- there it halves "the value with one tick of gravity applied".
            // GD measurements (gdref all levels, every cube that crossed a
            // gravity portal while grounded):
            //   lv5  t=17,015 y=195 (block)  up1->0  vy 0 -> **+0.108**
            //   lv6  t=13,616 y=195 (block)  up0->1  vy 0 -> **-0.108**
            //   lv7  t=7,766  y=285 (block)  up1->0  vy 0 -> **+0.108**
            //   lv4  t=6,731  y=105 (ground) up0->1  vy 0 -> **0.000**
            //   lv12 t=4,733  y=99  (ground) up0->1  vy 0 -> **0.000**
            // The sign is the **old gravity's** direction (+ when flip=1). A
            // hack with the same intent used to live here and was removed after
            // colliding with lv12's 0.000, but the real split was "ground layer
            // or object".
            if (isGrav && s.grounded && c.mode == 0) {
                const bool onFloor = !s.flip && s.frame == 0
                    && (double)s.y <= (kGroundY + pHalf) + 0.5;
                if (!onFloor)
                    vAtPortal = (s.flip ? 1.0 : -1.0)
                              * std::fabs(cubePhysFor(useDx).g);
            }
            // TRIED AND REVERTED (2026-08-19): "a ball crossing while grounded
            // on the ceiling side leaves carrying +-2.000 (+-1.000 after
            // halving)". The measurement is lv22 t=6,307-8 (frame1's ceiling
            // corridor, ud 1->0): GD is exactly vy=-1.000 at 6,308, then plain
            // decay at -0.129/tick.
            // Put in as `mode==2 && s.grounded && s.flip` it **over-fired at
            // lv16 t=4,2xx** (the census m2/fly/land family 1->3, edvy +1.000,
            // lv16's t=4,200 section 400->37) -- GD emits no +-1.000 in lv16's
            // twin case (flipped grounded ball x gravity portal). The condition
            // separating lv22's -1.000 (rotation frame? kind of g2? firing
            // order?) is unidentified.
            // One point is not a rule -- lv22 t=6,308 stays a known census item.
            // A GROUNDED player brings vy = 0 into the halving -- GD does not
            // slip an extra gravity step in first. There used to be one here,
            // fitted to lv6 t=13616 where a grounded cube left an inverse portal
            // at -0.1080 = (0 - 0.216)/2. lv12 t=4733 is the same shape and GD
            // comes out at 0.0000: grounded going in, so the tick produced no
            // gravity, and halving zero is zero. The lv6 reading was really a
            // one-tick offset in `grounded` caused by the old x constant (see
            // kDxF), and the hack was compensating for that rather than
            // modelling anything. With x exact it is simply wrong.
            // TRIED AND REVERTED (2026-07-31): snapping the halved value to a
            // 0.001 velocity grid. The observation behind it is real and
            // reproduces on demand -- a halving is the only thing that puts vy
            // off a 0.001 grid, and the very next tick then moves by 0.0005 more
            // than the mode's gravity:
            //   lv7  -1.979 -> -0.9895 raw, GD reports -0.990
            //   lv12 -10.275 -> -5.1375 raw, next step -0.0865, not -0.086
            //        (= -5.138 - 0.086), reproduced twice by re-injection
            //   lv12 -7.456 -> -3.728 raw (on grid), next step the plain -0.086
            // Left alone it is 0.0001 px of y per tick, which had grown to
            // 0.049 px by lv12 t=16796 -- exactly enough to put the model
            // 32.3457 from a saw of radius 32.3 where GD sat at 32.2997 and died.
            // But snapping HERE breaks lv10 (cleared -> stuck at x=11,243) with
            // either rounding direction, so the grid is not applied at the
            // portal. It is probably a property of the velocity update itself,
            // and the samples so far are all negative and all from portals.
            // CLOSED (2026-08-07): it was exactly that -- the grid belongs to
            // the velocity update, see qVy. The halving's own output is
            // reported raw and lands back on the grid one tick later. So the
            // rejection above is right and stays: this is not the rounding
            // point.
            // ...and the factor is ONE HALF PER FLYING END, not one per portal.
            // Counting ship and UFO as flying modes, all five measurements line
            // up (each is the value AFTER that tick's gravity, before the mode
            // change):
            //   cube -> ball  0 flying  x1     lv10 t=1743   0.596 -> 0.380
            //   cube -> ship  1 flying  x1/2   lv14 t=5340  -2.560 -> -1.280
            //   cube -> UFO   1 flying  x1/2   lv12         -2.376 -> -1.188
            //   UFO  -> ship  2 flying  x1/4   lv13 t=10798  4.549 -> 1.1373
            //                                  lv14 t=11655 -6.195 -> -1.5488
            //   ship -> UFO   2 flying  x1/4   lv14 t=14192  6.489 -> 1.6222
            // The ball portal's "no halving" (measured separately, see `halves`)
            // is the zero-flying case of the same rule, which is a second reason
            // to believe it. ball <-> cube is still handled as a half by
            // `halves` above; nobody has measured one.
            // ...and the WAVE is a flying end too. Two more measurements, both
            // from lv17's verified replay (so a plain run, not an injection):
            //   cube -> wave  1 flying  x1/2  t=3395  -5.899 -> -2.9495
            //   wave -> ball  1 flying  x1/2  t=8497   5.193 ->  2.5965
            // Both land exactly on the half, and both are half-grid values,
            // which is the halving's own fingerprint (see qVy). The model kept
            // the full value and lv17's first divergence was reported as
            // "wave/ball's velocity is exactly 2.0x" in update 29 -- it was not a factor
            // applied twice, it was a halving that never happened.
            // The ball still does NOT count (lv10 t=1743 cube -> ball is x1),
            // so this set is {ship, ufo, wave} and NOT GD's `flyish`.
            // ...and the SWING (mode 7) is one too. lv22 is the only level with
            // one, and all three of its swing mode portals are exact halves of
            // "the old mode's gravity step, then x1/2" (2026-08-18):
            //   t=3,323 cube  -> swing   2.801 -> 1.379
            //           (2.801 - 0.0432 [cube g under the 0.2x warp]) * 0.5
            //   t=6,938 ball  -> swing   5.809 -> 2.969   (ud=1)
            //           (5.809 + 0.129 [ball g]) * 0.5
            //   t=4,635 swing -> cube    0.770 -> 0.428   (ud=1)
            //           (0.770 + 0.086 [kSwingG]) * 0.5
            // The missing membership is why the model came out of every swing
            // portal at twice GD's speed. lv22 t=8,231 (swing -> spider off a
            // RAMP) does not fit any of this -- GD adds +0.504 there -- but
            // that tick is a known open (the ride's exit impulse), not
            // evidence about the ladder.
            // ...but **a swing riding a ramp does not count as a flying end**
            // (2026-08-19). lv22 t=8,230 (swing->spider, departing from a ramp
            // ride): GD's vy 2.258 is exactly 2.000x the model's (with x1/2
            // applied) = one rung too many on the ladder. The note above's "GD
            // adds +0.504" was a stale-worldline reading (the
            // [[gd-constant-justification-goes-stale]] pattern).
            const bool srcFly =
                ((c.mode == 1 || c.mode == 3 || c.mode == 4 || c.mode == 7)
                 && !(c.mode == 7 && s.onSlope));
            const bool dstFly =
                (wantMode == 1 || wantMode == 3 || wantMode == 4
                 || wantMode == 7);
            const int flyEnds = (srcFly ? 1 : 0) + (dstFly ? 1 : 0);
            // Gravity portals halve regardless of mode (lv5: cube -8.044 ->
            // -4.022 with zero flying ends); the flying-ends ladder is a MODE
            // portal rule, and its wantMode is meaningless for isGrav anyway.
            const double portalScale =
                isGrav             ? (dualBothGrav ? 0.25 : kGravPortalScale)
                : (flyEnds == 2)   ? 0.25
                : (flyEnds == 1)   ? kGravPortalScale
                                   : 1.0;
            if (halves) c.vy = (float)(vAtPortal * portalScale);
            if (isGrav) {
                // r101: both bodies in the same box = flipGravity twice -> up
                // round-trips back to what it was.
                // **Must not fall out of this branch** -- falling out runs the
                // `else { c.mode = wantMode; }` below, and type 4's wantMode=0
                // turns the player into a cube (stepped on once).
                if (dualBothGrav) continue;
                // 3 = inverse (upside down), 4 = back to normal -- stated in
                // GD's `upsideDown`, so convert back to the frame's own sign.
                const uint8_t flipBefore = c.flip;
                c.flip = (c.frame == 3) ? (uint8_t)!wantFlip : wantFlip;
                // [2026-08-21 r70] **On the tick a flipped body grounded on a
                // ceiling is returned to normal, g/2 is placed in the old
                // gravity direction (up).** Calibration rig ceilramp measured
                // (the tick a ceiling-running robot/spider passes the
                // gravity-restore portal):
                //   robot  vy 0 -> **+0.097** (= 0.194/2), then -0.194/tick
                //   spider vy 0 -> **+0.065** (= 0.129/2), then -0.129/tick
                // The model stayed 0, and a parallel g/2 vy offset persisted to
                // the end (a uniform tail of family dvy -0.097/-0.065).
                // **No blip on normal->flipped (grounded on a floor)** -- the
                // same rig's unit heads (GRAV_FLIP, floor-grounded) are
                // bit-identical. And **none for the ball** (gating cube/ball in
                // split the ball's 6 units by 1.5px/dvy13.9 -- GD only peels the
                // ball off). Only the measured robot/spider.
                // y also gets the half step: GD moves y by +0.225*g on the blip
                // tick (robot +0.044 / spider +0.029 -- vy alone leaves a y
                // residual).
                // [2026-08-21 r73] **No blip when the grounding surface is an
                // invisible ceiling (band/portal ceiling).** Calibration rig
                // ceilramp's spider m=2 (unit34/35): after a big fall from a
                // ramp launch, re-hung on the band ceiling (pmax=360,
                // y=346.5=360-pHalf), passing the restore portal GD moves
                // neither y nor vy and starts falling under plain gravity
                // (t=43,347). m=1 (the original measurement, hung on an actual
                // block's underside 290) shows the +g/2 in the dump as before
                // (t=38,304 vy=+0.065).
                // The blip's identity is the half step of collision resolution
                // against an actual object, and an invisible ceiling is not an
                // object. The discriminator is the same formula and same 0.6
                // tolerance as the support test (the invisible-ceiling clause of
                // `sup`).
                const double bandCG = (double)s.bandCeil;
                const bool bandUG = bandCG < 1e8
                    && std::fabs(bandCG - std::round(bandCG / 30.0) * 30.0)
                           < 0.01;
                const double invCeilG = std::min(
                    playerCeilAt(x),
                    (c.mode == 6 && bandUG) ? bandCG : 1e9);
                if (flipBefore && !c.flip && s.grounded
                    && std::fabs((double)c.vy) < 1e-6
                    && (c.mode == 5 || c.mode == 6)
                    && !(std::fabs((double)s.y + pHalf - invCeilG) < 0.6)) {
                    const double gMag =
                        (c.mode == 6) ? -kBallG
                        : (((double)useDx > 1.78) ? 0.195 : 0.194);
                    c.vy = (float)(0.5 * gMag);
                    c.y = (float)((double)c.y + 0.225 * gMag);
                }
                // [2026-08-21 r76] **The cube's gravity restore adds only the y
                // half step.** The calibration rig ceilramp's 6 cube cells
                // (unit6-11, all |m| x mini) show a uniform dy -0.049: on the
                // restore tick GD emits both vy=+0.108 (=g/2, the old gravity's
                // +0.216 accumulated then halved by the portal -- the model
                // already matches via the same path) and **y+0.0486
                // (=0.225*g)**. The model's seat clamp put y back on the face,
                // and that 0.049 remained parallel to the end (GD leaves with
                // the foot 0.049 embedded at 290.049).
                // vy untouched (already matches). The gate is "exactly the
                // halved g/2 is left" = only this shape of lift-off. Same family
                // as the robot/spider blip (above), but that one places from
                // vy 0; this one is y only.
                if (flipBefore && !c.flip && s.grounded && c.mode == 0) {
                    const double gMagC = std::fabs(cubePhysFor(useDx).g);
                    if (std::fabs((double)c.vy - 0.5 * gMagC) < 0.002)
                        c.y = (float)((double)c.y + 0.225 * gMagC);
                }
                c.grounded = 0;
            } else if (isSize) {
                // 18 = mini, 17 = back to normal. grounded is NOT forced here:
                // the support test at the top of the next tick decides it, and
                // on lv11 that is precisely what drops the player -- it stands
                // at y=165 on a surface whose top is 150, which supports a
                // half of 15 but not a half of 9.
                const double oldHalf = c.mini ? kMiniHalf : kCubeHalf;
                const double newHalf = wantMini ? kMiniHalf : kCubeHalf;
                c.mini = wantMini;
                pHalfLive = (c.mode == 4 && c.mini)
                                ? kWaveContactHalfMini
                                : playerHalf(c.mode, c.mini != 0);
                // A player standing on something re-seats IN THE SAME TICK --
                // but only when it GROWS. Measured on lv12 t=3286: a mini cube
                // grounded at y=189 on a surface of 180 enters the regular-size
                // portal and GD reports y=195 on that very tick, not the next
                // one. Without this the model ran a tick behind for the rest of
                // the level -- and the lag is invisible while airborne (the
                // position does not depend on the size), so it only surfaced 60
                // ticks later as a one-tick offset in `grounded`.
                //
                // SHRINKING does not re-seat: GD resolves the overlap by
                // pushing OUT of the surface, and there is nothing to push
                // against when the box gets smaller. Measured on lv11 t=3384
                // (the mini portal at x=4,395): a cube grounded at y=165 on a
                // surface of 150 goes mini and GD leaves it at y=165, drops
                // onGround on the next tick and lets it FALL the 6 px, reaching
                // its new rest height 159 only at t=3400 -- 16 ticks later.
                // The model teleported it to 159 instantly, which is the
                // "exactly 6.000px" (= 15 - 9) entry in update 29's diff list.
                // ...and only when the player was grounded AT TICK START.
                // A player that LANDED this very tick keeps the old-half seat
                // through the portal tick and the ordinary support clamp lifts
                // it next tick. Measured on lv18 t=14,612 (mini cube lands on
                // 210-top blocks the same tick the RegularSize portal uid10479
                // fires): GD's row is y=219 (=210+9, the MINI seat) with the
                // box already 30x30 (pbox size=1.000, 6 px embedded), and only
                // t=14,613 shows 225. The same-tick re-seat (lv12 t=3286) had
                // s.grounded=1 -- the walk-in case -- and stays.
                // [2026-08-21 r69 -> retracted] The growth re-seat on the world
                // ground splits its tick by mode (calibration rig ceilramp's
                // unit boundaries measured: cube next tick, ball/UFO same
                // tick). Written as "next tick if on the ground", the UFO's
                // re-seat disappeared entirely (persisted at 99), so back to the
                // unconditional same tick. The cube's 1-tick 6px stays as a
                // known quirk of the rig's boundary band only (excluded from
                // scoring with calib_units --settle). OPEN: is the
                // discriminating axis the mode, or the kind of support.
                if (s.grounded && c.grounded && newHalf > oldHalf)
                    c.y = (float)((double)c.y
                                  + (c.flip ? -1.0 : 1.0) * (newHalf - oldHalf));
            } else {
                const uint8_t oldMode = c.mode;
                c.mode = wantMode;
                pHalfLive = (c.mode == 4 && c.mini)
                                ? kWaveContactHalfMini
                                : playerHalf(c.mode, c.mini != 0);
                // [2026-08-19 item 4 -> adopted in 2026-08-20 r32] **A mode
                // portal changes the box seen by later portals of the same
                // pass.** The 2026-08-19 version applied this to **all
                // portals** and fell at the acceptance gate (lv18 tracking
                // 19,927->19,801, quick_regress red). As the disassembly
                // (pHalfLive's note) says, **only rotated objects** re-call
                // getOrientedBox(), so r32 applies it only for `p->oriented` --
                // and that passed (families stay 23, lv18's max error
                // 28.359->6.498, lv19/lv20 tracking improved).
                // Grounds (measured lv18 t=20,466, mini wave, box 6x6 =
                // half 3.0): cube portal uid14052 (OBB 31x86, rot=51) fires, but
                // the size portal uid14055 (OBB 31x90) 1.8px to the right is
                // **3.095px short on the width axis, 22.798 vs 15.5+4.203**.
                // GD fires both on the same tick (the dump's mode/vsize are
                // same-tick, and pbox emits one 30x30 row at t=20,466). With
                // half=15 after becoming a cube it passes with +13.7 to spare.
                // Uniformly enlarging the half breaks down: "uid14052 must not
                // fire at 20,465" (r2 < 5.578) and "uid14055 must fire at
                // 20,466" (r2 >= 7.298) cannot both hold.
                // That a size portal does not change later boxes is measured at
                // lv19 t=20,309 (note above), so the only consistent reading is
                // "the box rebuild is done by the mode switch".
                // **Why the 2026-08-19 version was not taken** (applied to all
                // portals): census stays 27 but lv18's tracking drops
                // 19,927->19,801 (the t=13,800 section 400->274) and
                // quick_regress goes red. r32's oriented-only form shows no such
                // regression.
                // The rest of the acquisition was taken over by r33
                // (`slopepush`): once mini resolves on the same tick, it then
                // grabs ramp uid14050 (m=-2) on the far side of the **ridge**
                // and seats at 398.50 (GD is at the ridge 390+pH=405). It cannot
                // be solved unless the seat is "the **highest** face under the
                // box" -- a single ramp's rotated-box formula cannot handle a
                // ridge.
                if (c.mode == 1) c.grounded = 0;
                // [2026-08-19 D8] When a mode portal ends a ramp ride, the swing
                // **fires the ball-rate slope-exit launch as an assignment**
                // (the flying-end ladder does not run -- another face of the
                // same picture as srcFly's exclusion). Confirmed by worker98
                // injection at ride 2/4/6 ticks, all with post-vy = 2.762 =
                // exit(1) x 0.75 x clamp(ride/24 -> 0.4) x sp1.1 (findings
                // 2026-08-19 D8; the old records' +2.240/+2.899 fit the same
                // formula at ride 14.4/18.7). Passing the same portal without
                // riding gives the normal ladder (0.408->0.161 measured), so the
                // gate is the ride itself.
                // The sign "follows the face" (same rule as acquire's +-2.000).
                if (oldMode == 7 && wantMode != 7 && s.onSlope) {
                    const double mTravP = (double)s.slopeM
                        * (((double)K.dxF < 0.0 || s.rev != 0) ? -1.0 : 1.0);
                    c.vy = (float)((mTravP >= 0.0 ? 1.0 : -1.0)
                                   * slopeExitVy(std::fabs((double)s.slopeM),
                                                 2, useDx, c.mini != 0)
                                   * slopeRampFactor((int)s.slopeT + 1));
                    c.grounded = 0;
                    c.onSlope = 0;
                    // [2026-08-21 r44] **This tick does not take the seat's
                    // step.** The ride puts y on the face, but GD leaves the
                    // tick a portal ends the ride on with a free step. Measured
                    // lv22 t=8,231 (flipped swing, rode the m=+1 ramp to ride3,
                    // passes spider portal uid8743, sp1.1, dx=1.6133):
                    //   t=8,230  both y=411.870 vy=2.258
                    //   t=8,231  GD    y=412.398 (+0.528 = 0.225x(2.258+0.086)
                    //                   = free integration one rung up the
                    //                   swing's ladder)
                    //            model y=413.485 (+1.615 = the ramp's step m*dx)
                    //   a parallel +1.087 remains after (census
                    //   `m7/mini0/g1/gdg0/gdm6/sp1.1/slope+1.00/ride3` edy -1.087)
                    // vy is decided separately by the formula above (2.762), so
                    // only y is restored.
                    c.y = yFreeBeforeSlope;
                }
                // [2026-08-28] **...and the y half of that is not the swing's,
                // nor the slope ride's.** "GD leaves the tick a portal ends the
                // ride on with a free step" is a statement about being CARRIED,
                // and a ceiling ramp's push-down carries just as much as a seat
                // does. Measured on lv20 t=17,110 -- a UFO pressed down the
                // ceiling ramp chain at x=25,750, mode portal uid13881:
                //   t=17,109  GD and the model both y=297.5798, vy=-3.204
                //   t=17,110  GD    y=296.8395  (-0.7403 = 0.225 x (-3.204
                //                   - 0.086), the free UFO integration)
                //             model y=295.9665  (-1.6133 = the press's own
                //                   step, m*dx)
                // -- the same 1.087-shaped parallel offset the swing case
                // measured at lv22 t=8,231, and it is what puts the model's
                // second body 1.585 px clear of the slope GD kills it on
                // 5 ticks later (p2dead=1 at t=17,115, x=25,760.89).
                // The swing's vy assignment above stays swing-only: what is
                // measured twice is the POSITION, and the ceiling press has its
                // own release velocity (ceil/release) already.
                else if (oldMode != wantMode && s.ceilT > 0 && !s.onSlope)
                    c.y = yFreeBeforeSlope;
                // A MODE portal that changes the resting half re-seats a
                // grounded player the same tick, exactly like the size portal
                // above (and by the same mechanism: the box GROWS into the
                // surface and is pushed out; shrinking has nothing to push
                // against). The only pair that differs is the spider's 13.5
                // against everyone else's 15. Measured on lv22, both
                // orientations of spider -> cube:
                //   t=1,168 upright on a floor: 223.5 -> 225.0 (+1.5) on the
                //           portal tick, vy 0, grounded stays 1
                //   t=8,759 flipped under a ceiling at 840: 826.5 -> 825.0
                //           (-1.5) on the portal tick
                // The model kept the old y and ran one tick late out of both
                // (fixcensus edy = +1.5 / -1.5 exactly, clamp:fly/land).
                // Placed after the ship's grounded=0 so flying destinations
                // are excluded automatically.
                {
                    const double oh = playerHalf(oldMode, c.mini != 0);
                    const double nh = playerHalf(c.mode, c.mini != 0);
                    if (c.grounded && nh > oh)
                        c.y = (float)((double)c.y
                                      + (c.flip ? -1.0 : 1.0) * (nh - oh));
                    // [2026-08-19] Even in the air, if the grown box penetrates a
                    // floor ramp's face it seats on the same tick -- the rest of
                    // the collision pass resolves the face with the new body.
                    // The seating formula is the rider's own "sample the line at
                    // the rotated box's lowest point" (slopeXOffset): measured
                    // lv21 t=18,087 (mini wave -> mini UFO, sliding on the
                    // sdir=4 m=-2 floor ramp): on the firing tick GD does
                    // y 217.006 -> 222.269, vy=0, g=1, matching
                    // line(x-5.562)+9 = 222.26 exactly (= line+9*sqrt(5)).
                    // The model kept the wave's step and seated by itself the
                    // next tick, and the 1-tick edy +8.49 remained as the census
                    // m4/mini1/gdg1/gdm3 family. The gate is "centre above the
                    // line, below the seat" plus non-flipped floor side, not
                    // spiked. The flipped side and ceiling ramps are unmeasured
                    // and untouched (the ceiling ramps' record is the HANDOFF's
                    // most important section).
                    // ...and **a uid gate** (same as the teleport's teleUid): a
                    // ramp processed before the portal was judged with the old
                    // body, so it does not seat on the same tick. Measured pair:
                    //   lv21 t=18,087  slope 24198 > portal 24194 -> same tick
                    //   lv18 t=12,243  slope  8273 < portal  8326 -> next tick
                    //     (GD: the firing tick stays y 269.14 vy -3.229 g=0, and
                    //      it seats at 12,244 at y 278.985 = line+15*sqrt(2).
                    //      Without the gate, census lv18's t=12,200 section
                    //      shrank 50->43 -- only the model seated 1 tick early)
                    else if (!c.grounded && !c.flip && nh > oh && K.slopes) {
                        for (const Obj* sp : *K.slopes) {
                            if (sp->uid < p->uid) continue;
                            if (sp->slopeHazard) continue;
                            if (slopeIsCeiling(sp->slopeDir)) continue;
                            const double sx0 = sp->cx - sp->hw;
                            const double sx1 = sp->cx + sp->hw;
                            if (x < sx0 || x > sx1) continue;
                            const double sm =
                                (sp->sy1 - sp->sy0) / (sx1 - sx0);
                            const double line = sp->sy0 + sm * (x - sx0);
                            const double r = x + (sm > 0
                                ? slopeXOffset(sm, nh)
                                : -slopeXOffset(sm, nh));
                            const double seat =
                                sp->sy0
                                + sm * (std::min(std::max(r, sx0), sx1) - sx0)
                                + nh;
                            if ((double)c.y < line) continue;  // centre below the line
                            if ((double)c.y >= seat) continue; // not touching
                            c.y = (float)seat;
                            c.vy = 0.f;
                            c.grounded = 1;
                            break;
                        }
                    }
                }
                // (Entering the SWING halves vy. That used to be a special case
                // right here, one-directional; it is now the flying-ends ladder
                // above, which also covers LEAVING one -- see the swing note by
                // `srcFly`. Keeping both halved twice: lv22 t=3,323 came out at
                // 0.689 instead of GD's 1.379.)
                // The impulse belongs to the NEW mode (see impulsedThisTick).
                // Re-issue it here, unhalved, because GD's buttons run after
                // the collision pass that changed the mode.
                if (impulsedThisTick && oldMode != c.mode) {
                    const double sgn = c.flip ? -1.0 : 1.0;
                    const double ms = c.mini ? kMiniImpulse : 1.0;
                    if (c.mode == 0 || c.mode == 5) {  // -> cube / robot: a jump
                        const CubePhys jph = cubePhysFor(useDx);
                        c.vy = (float)(jph.jump * (c.mini ? (kCubeJumpMini / kCubeJump) : 1.0)
                                       * (c.mode == 5 ? kRobotJumpScale : 1.0)
                                       * sgn);
                        c.grounded = 0;
                        // the hover budget belongs to the jump, not to the mode
                        if (c.mode == 5) c.rHover = (uint8_t)kRobotHoverTicks;
                    } else if (c.mode == 3) {   // -> UFO: a flap
                        c.vy = (float)((c.mini ? gdapprox::UfoParams::mini().flapVy
                                               : gdapprox::UfoParams::normal().flapVy)
                                       * sgn);
                        c.grounded = 0;
                    }
                    // -> ball: NO re-issue. A ball tap only acts while
                    // ROLLING, and a player leaving a portal is airborne.
                }
                // ship->UFO, the case where a fresh edge on the portal tick is
                // eaten by the old mode (thrust = the edge is not consumed).
                // There is no impulse, so it does not ride the re-issue above.
                // GD buffers it and emits the flap on the **next tick** (lv19
                // t=13,892: both agree at vy=-1.573 -> 13,893 GD alone +6.871).
                // Carried in a 1-tick-lifetime pending.
                {
                    // [2026-08-20] Neither ship-only nor fresh-edge-only. lv19
                    // t=19,491 is **wave->UFO**, with the button held since
                    // t=19,484 (not a fresh edge), yet GD emits a +6.648 flap
                    // the next tick. The same happens whenever the old mode is a
                    // kind that does not consume the edge (the ship's thrust,
                    // the wave's steering).
                    if (!impulsedThisTick && c.mode == 3 && input
                        && ((oldMode == 1 && !s.action) || oldMode == 4))
                        c.pFlap = 1;
                    // The old branch flipped the player and set the flip
                    // impulse right at the portal: measured on lv16's second
                    // dual (UFO -> ball at (19193,543), press on the portal
                    // tick), GD comes out UPRIGHT at (5.888-0.152)*0.5 while
                    // the model came out flipped at +2.683 and climbed away
                    // at +0.129/tick.
                }
            }
        }
    }
    };  // runPortalPass
    runPortalPass(0);                              // teleports only
    if (!teleportedThisTick) runPortalPass(1);     // no jump -> as before
    // Pads fire ONCE PER CONTACT, like orbs. The guard used to be a one-tick
    // lockout (padCd), so a pad the player was still overlapping fired again
    // every OTHER tick: measured on lv6, the pad at t=14175 re-fired at t=14177,
    // 14179, 14181... each time resetting vy to the pad value, which pinned the
    // model at terminal velocity while GD decayed normally. That single
    // difference is what put the model 260 px away from GD by t=14355. The
    // per-object memory below (usedPad, mirroring GD's touched list) covers that
    // case exactly, and the tick lockout is GONE: it was global, and GD fires a
    // DIFFERENT pad on the very next tick. Measured on lv13, where pad (1395,150)
    // fires at t=1053 and pad (1395,152) at t=1054 -- GD holds vy at exactly
    // 16.000 for both ticks instead of decaying to 15.784.
    // There is NO side test. There used to be one -- "the pad's centre has to be
    // inside the player's box", i.e. (player y - pad cy) > -pHalf -- fitted to
    // eleven firing observations (+13.00 .. +16.98 on lv2/3/5/6/7/9, and
    // -1.07/-2.87/-4.89/-10.29 on lv12) against lv10's non-firing case at -17.6.
    // lv13 t=1053 falsifies it: the player is at y=134.237 under the mid-air pad
    // at (1395,150), i.e. (player y - pad cy) = -15.76 with the pad's centre
    // 0.76 px ABOVE the player's top edge, and GD sets vy = 16.000 anyway.
    // Plain box overlap (the test on the line below) accounts for all twelve
    // firing observations AND for lv10's silence: that pad's box never overlaps
    // the swept player either (-17.6 is outside hh + pHalf = 17), so it was
    // never evidence for a side rule in the first place. The gravity direction
    // still decides which way the pad THROWS the player, not whether it fires.
    // ...and the GRAVITY pad's flip is IDEMPOTENT WITHIN A TICK. Every pad the
    // player overlaps is still consumed on the tick it is entered (see lv13
    // below), but two gravity pads entered together produce ONE flip, not two.
    //
    // Measured on lv21 (2026-08-10): blue pads at (9405,213) and (9435,213), both
    // rot=0, boxes 25x6. The player falls at terminal -15 and its box bottom
    // crosses their top edge (216) on t=7250, entering BOTH at once. Injecting
    // GD's own t=7249 state and stepping:
    //   GD     t=7250..7253  vy = +6.400 +6.616 +6.832 +7.048   up 0 -> 1
    //   model  t=7250..7253  vy = -6.400 -6.616 -6.832 -7.048   flip unchanged
    // An exact mirror: the magnitudes agree tick for tick, so the constant and the
    // timing were never wrong -- the model toggled twice and cancelled.
    // The driver could not repair this either. A fixup carries dy/dvy and CANNOT
    // carry `flip`, so it patched vy on one tick, diverged again on the next, and
    // re-recorded one tick earlier every iteration (t=7257 -> 7250 across 40
    // iterations) -- the "fixup grind = missing rule" signature.
    //
    // NOT "one pad per tick": lv13's yellow pads at (1395,150) and (1395,152) are
    // both inside the player's box from t=1053, and GD's own reference decays to
    // 15.784 on t=1054 -- i.e. GD consumed BOTH on t=1053 and had none left. (The
    // comment above this loop claimed GD held 16.000 for both ticks; the recorded
    // reference says otherwise. Deferring the second pad to t=1054 re-fires it and
    // takes lv13 from bit-identical to 61/400 ticks of follow.)
    // So consumption is per-pad and unconditional; only the flip is collapsed.
    // This is what a SET (gravity := the pad's own direction) looks like from the
    // outside whenever the pads agree, and it leaves every single-pad measurement
    // -- lv11 t=4722, lv12 t=18838, lv14 t=7030 -- reading exactly as before.
    bool gravPadFlipped = false;
    for (const Obj* pd : *K.pads) {
        if (spiderWarpedThisTick) break;
        // The landing site follows uid order (measurements at teleUid's
        // declaration). A pad processed before the teleport was judged at the
        // old position, so it does not fire on the landing tick.
        if (teleportedThisTick && pd->uid < teleUid) continue;
        // counted even when merely nearby (diagnosing missed collisions)
        // -- the landing site's contact counts from the next tick (type 28)
        // count the vicinity first (same reason as g_nearOrb -- diagnosing missed impulses)
        if (std::fabs(x - pd->cx) < pd->hw + pHalf + kPadReach + 30.0
            && std::fabs((double)c.y - pd->cy) < pd->hh + pHalf + 30.0)
            g_nearOrb = 1;
        if (std::fabs(x - pd->cx) >= pd->hw + pHalf + kPadReach) continue;
        if (std::fabs((double)c.y - pd->cy) >= pd->hh + pHalf) continue;
        // ...and **a rotated pad is judged with its rotated box**. Portals and
        // orbs already go through orientedHit, but only pads still used the
        // bounding rect. A thin pad (w0 x h0 = 25 x 4) rotated 29 degrees swells
        // its bounding rect to 34.5 x 22.6, firing 10px+ early in y.
        // Measured (lv20, id35 uid7030 (10835.9,133.5) rot=29):
        //   GD fires at dump t=7,286, player (10837.866,153.314) (|ly| = 18.28)
        //   the model fired at t=7,283, player (10832.016,159.151)
        //   (|ly| = 24.32, outside the rotated box's threshold ohh + r = 22.39)
        // The bounding rect is stage 1 of GD's own two-stage test, so the two
        // lines above stay.
        // The player's rotation is passed too (same promise as the portals:
        // `s.rot` is the end of the previous tick, so one step advanced is GD's
        // angle for this tick). An axis-aligned projection (pRot=0) is still 1
        // tick early: on uid7030 above, |ly|=19.08 passes against the threshold
        // 22.39.
        double pRotPad = (double)s.rot;
        if (s.mode == 0)
            pRotPad += (s.rotNeg ? 1.0 : -1.0) * (s.mini ? 2.25 : 1.7307692);
        if (pd->oriented && !orientedHit(*pd, x, (double)c.y, pHalf, pRotPad))
            continue;
        // NO FOOT-SIDE TEST (2026-08-07). The rule below was live for four days
        // and is falsified by two of the verified replays -- plain runs, not
        // injections, which is the stronger kind of evidence:
        //   lv13 t=1053  upright cube FALLING at -6.316, pads (1395,152) and
        //                (1395,150) with rot 0, player centre y=147.355 i.e.
        //                BOTH pads above it. GD sets vy = 16.000; the model
        //                skipped them and died 165 ticks later.
        //   lv14 t=7030  flipped cube grounded at y=735 under the solid whose
        //                underside is 750, blue pad (9165,733) with rot -180
        //                BELOW its centre. GD flips it and leaves at -6.400
        //                (= kPadBlueVy); the model sat there to the end.
        // Both are the tick the x-boxes FIRST overlap (lv13: player right edge
        // 1383.76 vs pad left edge 1382.5, 1.26 px in), which `usedPad` below
        // already models. The lv18 reading that motivated the side test came
        // from an INJECTED state placed inside the pad's x range, where there
        // is no entry to see -- see the block above this loop, which had
        // already concluded from twelve observations that plain box overlap
        // explains everything.
        //
        // (kept for the record) the rule that was here: a pad hung under a
        // ceiling belongs to a flipped player and an upright one cannot step on
        // it from below. Measured on lv18: the pad at (9,015,297) sits on the
        // underside of the solid at (9,015,315); injecting an upright cube at
        // y=290.5 anywhere inside the pad's box (x=8,988.6 and x=9,004.1 both
        // tried) leaves GD falling at -0.432, i.e. NO fire, while the model
        // fired at x=8,988.56 and planned a route GD could not follow.
        // `s.flip` and NOT `c.flip`: the contact belongs to the body as it was
        // when it touched the pad, the same rule the mode portals follow (see
        // pHalf). A ball that TAPS on the contact tick has already had c.flip
        // toggled by the time this runs, and the pad below (which the tap is
        // undone for, three lines down) then looks like it is on the head side.
        // Measured on lv9 t=16,254: the ball rests on the ceiling at y=255
        // under the yellow pad at (21,135,268), taps, and GD leaves at
        // vy = -9.600 (the ball's pad value) while the model left at -3.354
        // (the tap) and died 90 px later. With c.flip here lv9 goes from
        // CLEARED to STUCK at x=21,225 -- the whole 17-level regression's only
        // red, and it stayed red for a session because this rule landed
        // unverified.
        // (the `s.flip` note above is about WHICH flip the removed test read; the
        // tap-undo on the next line, which is what actually fixed lv9 t=16,254,
        // is independent of it and stays.)
        // The blue (gravity) pad **fires only when gravity actually changes**.
        // What GJBaseGameLayer::collisionCheckObjects (2.2081 win 0x2158f9) does
        // right after seeing this pad:
        //
        //   f = player->m_isSideways ? !obj->isFacingLeft() : !obj->isFacingDown()
        //   if (player->m_isUpsideDown == f) -> skip this pad (do not activate)
        //   ... propellPlayer(0.8, noFx, 10);  flipGravity(player, f, true)
        //
        // The gravity the pad installs is f = !isFacingDown, so the gate is
        // "current gravity == isFacingDown" = "that flip would not be a no-op".
        //
        // Verified against GD's own padact: lines (cfg padtrace=1,
        // activatedByPlayer). On all 94 gravity pads of
        // lv2/3/10/11/12/13/14/15/16 not only whether it fires but **the firing
        // tick** matches (scratchpad/padgate.py).
        // This also explains the observation that was added on 2026-08-07 as
        // the "foot-side test" and removed after lv13 t=1053 and lv14 t=7030
        // refuted it -- an upright cube entering lv18's (9,015,297) ceiling pad
        // from below does not fire.
        // That one was never a geometric rule -- it was this (up=0 against
        // fdown=1).
        //
        // The gate is re-evaluated **per object, with the gravity of that
        // moment**. lv14 t=13,428 is a tick entering 2 same-facing blue pads
        // 30px apart at once; GD fires only uid 4985, seen first, and by the
        // time gravity has changed, uid 4997 is rejected by the gate. What
        // gravPadFlipped below was holding down empirically is this phenomenon;
        // with the gate in, the 2nd pad never fires in the first place.
        //
        // A rotated section (m_isSideways) uses the isFacingLeft side, but id 67
        // exists only in lv2-21 and lv22 (the only rotated level) has none, so
        // it is not mirrored here. Implement this when a level with blue pads in
        // a rotated section arrives.
        if (pd->type == 10) {
            // A ball already flipped by its tap is judged with the value after
            // the rewind below (GD's tick order is update -> collisions ->
            // buttons, so the collision side sees the pre-press gravity).
            const bool upNow = ballFlippedThisTick ? (s.flip != 0)
                                                   : (c.flip != 0);
            if (upNow != objFacingDown(*pd)) continue;
        }
        // the pad wins over the tap: undo the flip before applying it
        if (ballFlippedThisTick) { c.flip = s.flip; ballFlippedThisTick = false; }
        bool already = false;
        int slot = -1;
        for (int i = 0; i < 4; ++i) {
            if (c.usedPad[i] == pd) { already = true; break; }
            if (!c.usedPad[i] && slot < 0) slot = i;
        }
        if (already) continue;   // already used, still inside it
        // No free slot means four pads overlap this player at once, which no
        // level does. Firing anyway would be the ratchet all over again, so
        // treat a full memory as "used" and skip.
        if (slot < 0) continue;
        c.usedPad[slot] = pd;
        // Pads call runNormalRotation too (the rotWrite note). The ordering of
        // the blue pad's flip against the call is unmeasured, so take the same
        // "before the branch" as the rings.
        rotWrite = true;
        rotWriteFlip = c.flip;
        // ...and a PAD also turns the sprite a full step on this very tick,
        // which a ring or an orb does not. See the spin block near the end of
        // the cube branch for the five measurements that separate them.
        rotPadSpin = true;
        if (pd->type == 10) {
            // the gravity pad scales too -- this branch was missed on the first
            // pass. Measured on lv11 t=4722 (pad at x[6110.5,6135.5]): GD came
            // out at 5.1200 where the model produced 6.4000, and 5.12/6.40 =
            // 0.800. Third independent confirmation of the same factor.
            const double bv = ((c.mode == 2) ? kPadBlueBall : kPadBlueVy)
                              * (c.mini ? kMiniImpulse : 1.0);
            // one flip per tick, however many gravity pads are entered together
            // (see gravPadFlipped). vy is re-set either way, which is a no-op the
            // second time round -- that is the idempotence.
            if (!gravPadFlipped) {
                c.flip = c.flip ? 0 : 1;
                gravPadFlipped = true;
            }
            c.vy = (float)(c.flip ? bv : -bv);
        } else {
            // UNVERIFIED for the pink pad: the ball ratio (the 0.600 below is
            // the yellow pad's, measured) and the mini factor. Both are carried
            // over from yellow; measure them the moment a plan leans on one.
            // The SPIDER takes the ball's 0.600 too, not the full 16. Straight
            // out of GD's own replay of lv21 (no injection), at both
            // orientations:
            //   t=9303 x=12,080 upright on the pad at (12105,212) rot=0
            //           -> vy = +9.600
            //   t=8470 x=10,999 flipped under the pad at (11025,388) rot=-180
            //           -> vy = -9.600
            // The model was giving it 16.0, and the driver logged exactly that
            // gap as edvy = -6.4 / +6.4 (16.0 - 9.6) at those two sites --
            // sign-consistent, so it is the value and not the timing.
            // Spider = ball keeps turning up: kSpiderGScale and kRingSpiderPost
            // are both the ball's numbers as well. The drop ring is the one that
            // is NOT (16.5 vs 15.0), so this is a per-constant fact, not a rule
            // about the mode -- do not generalise it to the untested ones.
            const bool ballish = (c.mode == 2 || c.mode == 6);
            const double pBase = (pd->type == 9)  ? kPadPink
                               : (pd->type == 34) ? kPadRed
                                                  : kPadYellow;
            // The ball ratio is measured per colour (history at kPadPinkBall's
            // declaration). Pink is 0.700, yellow 0.600 -- applying the generic
            // ratio 9.6/16 to pink gives 6.24, 0.48 off GD's 6.72. Red+ball
            // stays on the generic ratio, unmeasured.
            const double pv = (ballish ? ((pd->type == 9)
                                          ? kPadPinkBall
                                          : pBase * kPadYellowBall / kPadYellow)
                                       : pBase)
                              * (c.mini ? kMiniImpulse : 1.0);
            c.vy = (float)(c.flip ? -pv : pv);
            // The RED pad is the bumpPlayer type that sets the velocity-limit
            // exemption (type==34 at +0x191) where every other pad clears it.
            // Swing-scoped like the rest of State::boost; kPadRed is 20, so
            // without it the swing handed back 12 of those units next tick.
            if (pd->type == 34 && c.mode == 7) c.boost = 1;
        }
        c.grounded = 0;
        // ...and the ROBOT's hover budget dies with it. The budget belongs to
        // the jump that armed it (the comment at the mode-portal re-issue says
        // so already); a pad sets vy from the outside and GD does not carry the
        // boost through it. Measured on lv21 (2026-08-10, the cold run's own
        // fixup log): the pink pad at (18585,182) launches the mini robot at
        // t~14,300 with the button HELD, and GD then steps vy by -0.194 on
        // EVERY tick from there to t=14,359 while the model froze vy -- 75 of
        // the run's 88 records are that one hole, all with the identical
        // signature `edy=-0.0873 edvy=-0.194` (0.0873 = kYScale * 0.194). The
        // loop bought it back one tick per iteration: iters 29-46 never left
        // x=18,705. See docs/HANDOFF.md update 47 on what a grind means.
        c.rHover = 0;
        // the pad wins: undo the landing snap -- and for a player that was
        // already resting on the block, keep the tick's gravity step in y for
        // the same reason (GD never put it back). See `pinnedOnBlock`.
        releasePin();
    }
    // Release each remembered pad once the player is clear of it IN X. Leaving
    // in y is NOT enough, which is what a gravity-pad corridor shows: lv14
    // x=9195 y=677, fired at t=7066 from x=9181, the cube flies up to y=715
    // (its box nowhere near the pad) and falls back into the pad's box at
    // t=7092 with 1.3 px of overlap -- and GD does NOT fire it again. It lands
    // on the block underneath instead. With a y term in the release the model
    // fired it a second time, took +6.4 where GD had -9.2, and the whole
    // corridor after it was solved for a player GD had already put on the floor.
    for (int i = 0; i < 4; ++i) {
        const Obj* up = c.usedPad[i];
        if (up && std::fabs(x - up->cx) >= up->hw + pHalf + kPadReach)
            c.usedPad[i] = nullptr;
    }
    // Orbs: unlike pads they need the button, on a fresh press while touching.
    // Measured on lv3 (dump_lv3_ref.csv): the yellow orb (id 36, type 11) sets
    // vy := 11.18 -- the same value as a cube jump -- regardless of the
    // incoming velocity (28 activations, vy_in -1.3 .. -11.5). Like the pads,
    // y moves with the pre-boost velocity first and vy is set afterwards.
    // An orb can only be used ONCE per touch. GD keeps the ring in
    // m_touchedRings and will not re-fire it until the player has left it, so a
    // second press while still inside the same orb does nothing. The model had
    // no such memory and happily fired the same orb twice: measured on lv6, the
    // orb at (4515,255) fired at t=3488 and AGAIN at t=3494, which put the model
    // 3 px above GD within ten ticks and grew from there.
    {
        const Obj* used = c.usedOrb;
        bool stillTouching = false;
        if (g_slopeDbg)
            std::printf("orbdbg t=%lld input=%d ringHold=%d used=%d\n",
                        (long long)K.t, (int)input, (int)s.ringHold,
                        used ? used->uid : -1);
        // `input`, not a rising edge. GD fires a ring whenever the button is
        // DOWN as the player touches it -- holding through an orb activates it,
        // which is how the mode is actually played. The old `input && !s.action`
        // gate needed a fresh press inside the orb's box and silently dropped
        // every hold-through activation. Measured on lv11 t=8567: the mini ship
        // is holding, flies into the orb at (11099,285) at a clamped distance of
        // 17.68, and GD sets vy = 8.9440 (the mini yellow orb value) while the
        // model carried on at -1.671 and was 12 px below GD five ticks later.
        // Re-firing is still prevented by usedOrb, which is the real "once per
        // touch" rule; the edge test was redundant with it and wrong besides.
        if (!input) c.ringHold = 0;   // released: the next press may ring again
        if (input && !s.ringHold) {
            // Two orbs can sit on the SAME square, and then which one fires is
            // not a detail: lv14 x=16875 y=585 carries a yellow ring (id 36,
            // uid 4774) and a gravity ring (id 84, uid 4775) exactly on top of
            // each other. GD takes the yellow one (t=13016: vy := 11.180),
            // i.e. the LOWER uid -- the same ordering the stair snap already
            // showed (GD walks the touched objects in descending uid and the
            // last one processed wins). The old loop fired whichever came first
            // out of the sort, which for equal cx is unspecified; here it was
            // the gravity ring, so the model flipped and rose at 4.472 where GD
            // jumped at 11.180. Pick the candidate with the smallest uid.
            const Obj* pick = nullptr;
            // WHERE the contact is measured. GD's per-tick order is
            //   update -> checkCollisions (this is where the ring is latched)
            //   -> processQueuedButtons (this is where the press is consumed)
            // so a press consumed on GD's tick P sees the ring state from P's
            // collision pass, i.e. the position at the END of P. The model
            // applies an input `latOf(mode)` ticks LATE, so the tick where it
            // fires is P+1 and its `c.y` is one move too far along.
            // Measured on lv20 (cfg orbtrace=1, the event list): the plan's
            // press is `INJECT_handleButton 983 1`, GD's yvel jumps on t=984,
            // and at t=983 the cube sits 31.7 px under the ring (1291,183) --
            // inside the model's own gate. At t=984 it is 34.2 px under it,
            // outside. The model was testing the right ring at the wrong tick
            // and skipped it, and the replay went 22 px/tick the other way.
            // Both positions are accepted rather than just the earlier one:
            // GD is at least this permissive (it keeps m_ringObject rather
            // than re-deriving it), and loosening only ADDS candidates -- a
            // false pass dies in replay and comes back as a fixup.
            for (const Obj* ob : *K.orbs) {
                if (spiderWarpedThisTick) break;
                // the landing site follows uid order (measured, teleUid's decl.)
                if (teleportedThisTick && ob->uid < teleUid) continue;
                //// the landing site counts from the next tick (type 28)
                const double odxNow = std::fabs(x - ob->cx);
                const double odyNow = std::fabs((double)c.y - ob->cy);
                const double odxPre = std::fabs(xPrev - ob->cx);
                const double odyPre = std::fabs((double)s.y - ob->cy);
                // the pair that is closest to the ring decides the test below
                const bool usePre = (odxPre <= odxNow && odyPre <= odyNow)
                                 || (odyPre + odxPre < odyNow + odxNow);
                const double odx = usePre ? odxPre : odxNow;
                const double ody = usePre ? odyPre : odyNow;
                // **Count merely having been NEARBY, first.** The orbs that did NOT fire are
                // exactly the diagnosis targets: there are 12 divergences where only GD gains
                // velocity on the order of a unit, orders of magnitude beyond the weak/strong
                // acceleration differences (0.026, 0.041), so a missed impulse (orb/pad/ring)
                // is the suspect. Without presence-nearby in the signature, that class cannot
                // be told apart from the others.
                if (odx < ob->hw + pHalf + 30.0 && ody < ob->hh + pHalf + 30.0)
                    g_nearOrb = 1;
                if (odx >= ob->hw + pHalf || ody >= ob->hh + pHalf) continue;
                // The activation shape is PER MODE. Point-probed, 1 tick per
                // point, all mini:
                //   BALL, lv16 yellow ring (20355,581):
                //     corner dx=25.97 dy=21.61 (clamp 21.1)  FIRED
                //     x edge dx=26.3 FIRED / dx=27.9 did NOT
                //     y edge dy=26.3 FIRED / dy=27.8 did NOT
                //     -> plain AABB, both edges at 18 + 9 = ob->hw + pHalf.
                //        No circle fits (y-edge non-fire at clamp 18.8 vs
                //        corner fire at 21.4).
                //   CUBE, lv14 yellow ring (11475,129):
                //     (dx=20.0, dy=21.7) FIRED / (dx=25.9, dy=10) FIRED /
                //     (dx=25.9, dy=21.7) did NOT
                //     -> dy matters at fixed dx: circle-like, and the old
                //        clamped radius (17.8 mini) brackets all three.
                // So the ball gets the AABB and every other mode keeps the
                // fitted circle -- which cleared lv1-15/17, so it stays until
                // a probe refutes it for ship/UFO too. Gravity rings (13)
                // keep the circle even for the ball: the one non-fire on
                // record (clamp 17.94, lv16 t=9490) is a gravity ring and
                // its ball corners are unprobed.
                // The same sweep confirmed the trigger is "the box overlaps
                // while the button is HELD" -- press 2 ticks early + release
                // did NOT fire on contact, hold-through DID -- which is the
                // `input && !s.ringHold` rule this loop already implements.
                if (ob->oriented) {
                    // Rotated ring: the firing box is the rotated box (the note at the
                    // parser's orb branch). The world axes are already checked by the AABB
                    // gate above, so this is the box test on the ring's own axes = the
                    // remaining 2 axes of the SAT. The fitted circle is a constant measured
                    // on unrotated rings, so it is not used for oriented ones.
                    const double relx = (usePre ? xPrev : x) - ob->cx;
                    const double rely = (usePre ? (double)s.y : (double)c.y)
                                        - ob->cy;
                    const double lx = relx * ob->rc - rely * ob->rs;
                    const double ly = relx * ob->rs + rely * ob->rc;
                    const double ph = pHalf * (std::fabs(ob->rc)
                                               + std::fabs(ob->rs));
                    if (std::fabs(lx) >= ob->ohw + ph
                        || std::fabs(ly) >= ob->ohh + ph)
                        continue;
                // GRAVITY ORB (13): plain AABB, like the ball's yellow/pink.
                // The fitted circle is REFUTED: lv22 t=3,002 has a MINI cube
                // firing the orb at (4,305,281) from (4,284.15, 307.276) --
                // clamped 20.96 against a radius of 17.8 -- and without that
                // fire the model cannot make the flip corridor's exit.
                // The AABB passes it with room (odx 20.85 and ody 26.28 against
                // hw + pHalf = 27), and the whole suite stays byte-identical.
                }
                // ...AND SO IS EVERY OTHER RING: the test is a plain AABB,
                // |dx| < ob->hw + pHalf and |dy| < ob->hh + pHalf, with **the
                // acting mode's own half**. That is exactly the gate 45 lines
                // up, so no extra test belongs here and the fitted circle is
                // gone. Measured on purpose-built sweeps (2026-08-17,
                // py/mklevel.py `orbedge`/`orbedge2`/`orbedge3`: a single-tick
                // press with the ORB moved 1 px at a time, so the resolution is
                // not the 1.298 px tick and nothing is injected):
                //   2-D, full cube: the largest firing |dx| is 33 at dy = 0,
                //     10, 20, 25, 30 AND 32 (next non-fire 34.8..35.9), and at
                //     dy = 34 nothing fires at any dx. A circle of 19.5 would
                //     have shrunk that to 24.5 by dy = 30. It does not shrink.
                //   1-D by mode (pink, boundary = fires / does not):
                //     robot  full 33.79 / 34.45   -> 18 + 15
                //     spider full 31.85 / 33.02   -> 18 + **13.5**
                //     robot  mini 27.05 / 28.97   -> 18 + 9
                //     spider mini 26.47 / 27.75   -> 18 + **8.1**
                //   The spider rows are what settle it: the half is the MODE's,
                //   not a fixed 15. They are also why this could not be turned
                //   on from the cube sweep alone -- the AABB is 1.5 px TIGHTER
                //   than the circle for a spider and lv22's rotated route was
                //   riding that slack (reach_check caught it as 22@5400
                //   SOLVED -> PARTIAL).
                // KNOWN CONFLICT: the note this replaces recorded GD firing
                // id141 (2025,221) at dyy = -35.0 on lv22 t=1,569, which the
                // AABB (33) rejects. That orb is an ordinary 36x36 (checked),
                // so the likely cause is the trace's tick being one off the
                // firing tick -- at vy about -10 the player moves 2.25 px of y
                // per tick, which covers the 2 px gap. If lv22 regresses at
                // x=2,277 this is the first thing to re-measure, with the
                // MOD's orbtrace stamped on the firing tick itself.
                // A SPIDER ORB with nothing on the other side is not a
                // candidate at all: GD's spiderTestJump finds no surface and
                // the tap does nothing, so it must not eat the press (or the
                // shared tail below would unground the player for free).
                if (ob->type == 43) {
                    double tgt = 0.0;
                    if (!spiderTargetY(K, x, (double)c.y, c.flip != 0,
                                       c.mini != 0, pHalf, tgt, c.frame == 0,
                                       nullptr, (int)c.frame))
                        continue;
                }
                if (ob == used) { stillTouching = true; continue; }
                if (!pick || ob->uid < pick->uid) pick = ob;
            }
            if (pick) {
                const Obj* ob = pick;
                // [OPEN 2026-08-18] lv20 t=11,896: on the tick a grounded cube
                // fires gravity orb uid10053, GD stays y=219.000 while the model
                // gets 218.9514 (-0.0486 = 0.225 x 0.216 = one gravity step).
                // Adding `if (s.grounded) c.y = s.y;` here did not change the
                // value (suspect the RIDE below happening at c.y == s.y and
                // re-seating at the same y). This 0.0486 advances the robot's
                // landing tick at t=12,033 by one -- the identity of
                // quick_regress's lv20 t=11,800 section (400 -> 233).
                rotWrite = true;   // rings/orbs too (the rotWrite note)
                rotWriteFlip = c.flip;   // default: gravity from before the branch
                // ring impulses scale with the section's speed (see ringScaleFor)
                const double ms = (c.mini ? kMiniImpulse : 1.0) * ringScaleFor(useDx);
                if (ob->type == 13) {
                    // ...and the SPIDER takes the same 0.700 post-multiplier
                    // the yellow/pink and red/green branches already apply.
                    // Only the BALL had a constant here, so a spider was
                    // getting the CUBE's gravity orb. Measured on lv21
                    // (2026-08-18, plain replay, no injection, both runs
                    // bit-identical up to the tick), orb id84 uid11777 at
                    // (11115,315):
                    //   t=8,580  GD vy := -3.130  / model -4.472
                    //   t=9,871  same difference (edvy +1.3416 = 4.472 - 3.1304)
                    // 3.1304 / 4.472 = 0.700000 = kRingSpiderPost, and it is
                    // the same number the ball's own measured constant already
                    // holds (kOrbGravityBall = 3.130). The ROBOT keeps 4.472
                    // (measured), which is why this is a post-multiplier list
                    // and not a mode table. Swing is left out for the same
                    // reason as in the yellow/pink branch: unmeasured.
                    const double gpost = (c.mode == 6) ? kRingSpiderPost : 1.0;
                    const double gv =
                        ((c.mode == 2) ? kOrbGravityBall : kOrbGravity)
                        * ms * gpost;
                    c.flip = c.flip ? 0 : 1;
                    c.vy = (float)(gv * (c.flip ? 1.0 : -1.0));
                } else if (ob->type == 37 || ob->type == 38) {
                    // DASH ring. Freezes vy and starts the straight-line
                    // travel; the ring's rotation is the angle. Every dash
                    // ring in lv21 and all but one in lv22 are rot = 0, i.e.
                    // dead horizontal -- the -45 one at lv22 x=13,333 uses the
                    // same formula and is UNVERIFIED.
                    // 38 flips gravity as well; that ordering (flip, then the
                    // dash line) is unmeasured -- the dash is horizontal in
                    // both of lv21's, so it does not show.
                    c.dashing = 1;
                    c.dashSlope = (float)std::tan(-ob->rot * 3.14159265358979 / 180.0);
                    // The vy GD's dump prints on the engage tick (4 measurements
                    // at g_dashVy's declaration). **The state stays 0** -- only
                    // the display is matched.
                    g_dashVy = (double)c.vy * (ob->type == 38 ? 0.5 : 1.0);
                    g_dashVySet = 1;
                    c.vy = 0.f;
                    c.grounded = 0;
                    if (ob->type == 38) c.flip = c.flip ? 0 : 1;
                } else if (ob->type == 32) {
                    // DROP ring: a fixed slam toward the player's own floor.
                    // No speed scale either -- the value is a literal in the
                    // function, not a multiple of the jump.
                    const double dv = (c.mode == 6)   ? kRingDropSpider
                                      : (c.mode == 3) ? kRingDropUfo
                                      : (c.mode == 1 || c.mode == 4
                                         || c.mode == 7) ? kRingDropFly
                                                         : kRingDrop;
                    // [2026-08-22 r102] **Hit while climbing fast enough, the
                    // next tick's terminal clamp is skipped once.** The dropair
                    // calibration rig, reshaped to "start pressing in mid-air",
                    // swept the incoming vy with off (the firing tick's vp
                    // after that tick's gravity is applied):
                    //   cube  3.085 / 2.005 -> **skips** (next tick -15.216)
                    //         1.789 / 1.357 / negative -> does not skip
                    //   ball  2.329 -> skips / 1.813 -> does not
                    //   robot 2.749 -> skips, spider 4.051 -> skips
                    // The boundary is **2.0** (the same number as GD's other
                    // 2.000s). The corpus's lv21@7,217 is on the skipping side
                    // at vp=2.176 -- the identity of family
                    // `m0/mini0/g0/gdg0/sp0.9/air` edvy -0.216.
                    // The delay is **only the next 1 tick**; the one after
                    // returns to -15.000.
                    if ((double)c.vy * (c.flip ? -1.0 : 1.0) >= 2.0)
                        c.pNoTerm = 1;
                    c.vy = (float)(c.flip ? dv : -dv);
                    c.grounded = 0;
                } else if (ob->type == 43) {
                    // SPIDER ORB (GameObjectType::SpiderOrb = 43, id 3004).
                    // NOT a teleport portal: it carries no stored target. It
                    // runs the SPIDER's own surface search from wherever the
                    // player is -- in any mode, in mid-air -- and flips gravity.
                    // Measured on lv22, plain replay of the level's only two:
                    //   uid 6179 (13965,975) cube, upright, airborne:
                    //     t=10353  y 952.98 -> 1035.000  flip 0 -> 1  vy := +1
                    //   uid 6197 (14325,975) cube, flipped, airborne:
                    //     t=10586  y 980.12 ->  915.000  flip 1 -> 0  vy := -1
                    // Both land exactly on spiderTargetY's face (underside minus
                    // pHalf going up, top plus pHalf going down), and x is
                    // untouched. The +-1 of leftover velocity points the way the
                    // teleport went -- the same rule the spider's own tap uses,
                    // and the arrival tick reports onGround=0 with the landing
                    // resolved on the next one, which is what the shared
                    // `c.grounded = 0` below already gives.
                    double tgt = 0.0;
                    const double gs = c.flip ? -1.0 : 1.0;
                    // the candidate loop already refused a target-less one
                    if (spiderTargetY(K, x, (double)c.y, c.flip != 0,
                                      c.mini != 0, pHalf, tgt, c.frame == 0,
                                      nullptr, (int)c.frame)) {
                        c.y = (float)tgt;
                        c.flip = c.flip ? 0 : 1;
                        c.vy = (float)gs;
                        // whatever held us up is a level away now
                        c.snapObj = nullptr;
                        c.snapDist = 0.f;
                        c.onSlope = 0;
                        c.slopeM = 0.f;
                    }
                } else if (ob->type == 35 || ob->type == 29) {
                    // RED (35) and GREEN (29), straight off the ringJump table.
                    // Both are expressed as a ratio of the cube's jump rather
                    // than as their own constant, because that is how GD
                    // computes them -- and it is what makes the mode entries
                    // that nobody has measured trustworthy.
                    const bool green = (ob->type == 29);
                    double r;
                    if (green) {
                        r = (c.mode == 1) ? kRingGreenShip : kRingGreen;
                    } else {
                        switch (c.mode) {
                            case 1: r = c.mini ? kRingRedShipMini : kRingRedShip; break;
                            case 3: r = c.mini ? kRingRedUfoMini : kRingRedUfo; break;
                            case 2: r = kRingRedBall; break;
                            case 5: r = kRingRedRobot; break;
                            case 6: r = kRingRedSpider; break;
                            default: r = kRingRedCube; break;
                        }
                    }
                    // TRIED AND REVERTED (2026-08-09): `if (green && c.mode==5)
                    // r *= kRingRobotDefault`. The reasoning ("the robot's
                    // blanket 0.9 applies to rings with no entry of their own,
                    // and green has none") was read off the ratio table at
                    // 0x399399, but the green arm does not reach that `mulss`.
                    // GD says so directly -- lv20 t=12,729, a full-size ROBOT
                    // takes the green ring on a plain replay (no injection):
                    //   GD    vy := -11.420 = 11.18 * 1.021467       (x 1.00)
                    //   model vy := -10.278 = 11.18 * 0.9 * 1.021467 (x 0.90)
                    // four digits each, and the two ran bit-identical up to
                    // that tick. The 0.9 put the robot 56 px below GD by
                    // t=12,886 and lv20's cold run could not get past x=18,214
                    // because every plan it built died there.
                    // kRingRobotDefault stays for whatever else needs it.
                    const double post = (c.mode == 2)   ? kRingBallPost
                                        : (c.mode == 6) ? kRingSpiderPost
                                        : (c.mode == 7) ? kRingSwingPost
                                                        : 1.0;
                    // GREEN flips gravity BEFORE the velocity is set, so the
                    // value is not halved and the sign follows the NEW gravity.
                    // Green flips **first** and writes the velocity after. The
                    // rotation-sign write also lands in between, so only green
                    // gets the new gravity (measurements at rotWriteFlip's
                    // declaration).
                    if (green) { c.flip = c.flip ? 0 : 1; rotWriteFlip = c.flip; }
                    const double ov = kOrbYellow * r * post * ms;
                    c.vy = (float)(c.flip ? -ov : ov);
                    // RED is one of the two ring/pad types that SET the
                    // velocity-limit exemption instead of clearing it
                    // (ringJump +0xc7a, type 35 only; every bumpPlayer orb
                    // clears it). Swing-scoped like the rest of State::boost.
                    // The full-size swing red ring is 11.18*1.38*0.6 = 9.257,
                    // above the terminal -- without the flag the model used to
                    // hand back everything over 8 on the next tick.
                    if (!green && c.mode == 7) c.boost = 1;
                } else {
                    // pink (12) and yellow (11), each with its own measured
                    // ball value -- there is no shared ball ratio.
                    // UNVERIFIED for pink: the mini factor (0.800 carried over
                    // from every other impulse).
                    const bool pink = (ob->type == 12);
                    // ...and the SPIDER takes the same 0.700 post-multiplier the
                    // red/green branch above already applies. This branch had no
                    // mode term at all, so a spider was getting the CUBE's orb.
                    // Measured on lv21 (2026-08-10) at the yellow orb
                    // (12225,285), on a plain replay with no injection:
                    //   GD    t=9427  vy := 7.826
                    //   model t=9427  vy := 11.180  (= kOrbYellow, untouched)
                    // and 7.826 / 11.180 = 0.700000, which is kRingSpiderPost to
                    // six digits. It was the WORST early divergence in the whole
                    // level: the fixup window at t=9427 followed GD for only 68
                    // ticks before this, against 300-400 for every other window.
                    // The ball is NOT given a post here -- it has its own
                    // measured constants (kOrbYellowBall / kOrbPinkBall) and
                    // multiplying again would double-count.
                    // Swing (7) is deliberately left out: the red/green branch
                    // applies kRingSwingPost there, but nothing has measured it
                    // for the yellow orb and no level reaches one yet.
                    const double post =
                        (c.mode == 6) ? kRingSpiderPost : 1.0;
                    // ...and the ROBOT takes the table's closing
                    // `else if (m_isRobot) *= 0.9` (kRingRobotDefault) on the
                    // YELLOW orb, which has no robot entry of its own.
                    // Measured on lv19 (2026-08-18, plain replay, no injection,
                    // both runs bit-identical up to the tick):
                    //   t=2,965  orb id36 uid1050  GD vy := +10.062  model 11.180
                    //   t=4,036  orb id36 uid2165  GD vy := -10.062  model -11.180
                    // and 10.062 / 11.18 = 0.900000 to six digits.
                    // PINK is deliberately excluded: its robot value is the
                    // CUBE's 8.050 (measured), i.e. type 12 has its own entry
                    // and never reaches the closing branch. Same reason the
                    // gravity orb (13) keeps 4.472 for a robot. Ship / UFO /
                    // wave / swing are unmeasured for the yellow orb and stay
                    // on the cube value.
                    const double robotR =
                        (!pink && c.mode == 5) ? kRingRobotDefault : 1.0;
                    const double ov =
                        ((c.mode == 2) ? (pink ? kOrbPinkBall : kOrbYellowBall)
                                       : (pink ? kOrbPink : kOrbYellow))
                        * ms * post * robotR;
                    c.vy = (float)(c.flip ? -ov : ov);
                }
                c.grounded = 0;
                // Same as the pad: an orb is an outside impulse, so the robot's
                // hover budget from an earlier jump does not survive it (see the
                // pad's note for the lv21 measurement). The dash ring above
                // relies on this too -- its own branch clears rHover a tick
                // later, which is one tick of frozen vy too many if the budget
                // is still armed when the ring fires.
                c.rHover = 0;
                // An orb taken off a block moves y exactly like a pad does --
                // the ordinary jump is the only launch GD re-seats. Measured:
                // lv19 t=254 (+3.999, dy -0.0486) and t=506 (+7.405, same),
                // lv16 t=6404 (+9.208, -0.0484), lv18 t=10824 (+2.499, -0.0486).
                // The model had already run its jump branch by the time it got
                // here, which is why the pin is still there to consume.
                // ...EXCEPT the GRAVITY ORB (13), which leaves y alone.
                // Swept the whole corpus for "grounded on a block, vy 0, an
                // impulse that FLIPS gravity" (2026-08-18) -- the split is by
                // object, not by value or mode:
                //   gravity ORB   cube 3.578 x2 (lv20 11,616/11,896)  y FROZEN
                //   gravity PAD   cube 5.12 x5, 6.4 x14              y MOVES
                //   spider 1.000 x35 (its own teleport)              y MOVES
                // No counter-example either way, but the ORB side is only TWO
                // samples from ONE level: the `cube 5.193 x2` pair first quoted
                // here (lv22 11,344/12,175) turned out to be FRAME TURNS, not
                // orbs -- 5.193 is what a gframe change writes at speed 0.9,
                // and it shows up in the same "grounded, vy 0, gravity flips"
                // sweep. Re-check on a third gravity orb when one appears.
                // Without this the model left
                // the block 0.0486 px low, which is what put lv20's t=11,800
                // segment a tick early into the landing at t=12,033.
                if (ob->type != 13) releasePin();
                c.usedOrb = ob;
                c.ringHold = 1;
                used = ob;
                stillTouching = true;
            }
        }
        // Only on a teleported tick are the landing site's portals applied here
        // (the runPortalPass note: GD goes in uid order, so pads/orbs come
        // first).
        if (teleportedThisTick) runPortalPass(1);
        // [2026-08-16] The "forget once out of the box" release was dropped.
        // **GD's orbs can fire only once per run** (consumed on use). Measured
        // (lv22, cfg orbtrace=1, the cold run's plan): GD fires id141
        // (2025,221) exactly once at t=1,569 and does not fire it when the
        // player re-enters the box at t=1,581.
        // The old rule dropped the memory at t=1,571 (ody=33.2 >= 33) and fired
        // **the same orb a second time** at t=1,581. That was the frozen diff's
        // first divergence, and everything after was a different worldline
        // (just before the cold run's x=6,380 wall).
        // usedOrb keeps holding "the last orb fired". It is not overwritten
        // until a different orb fires, so an A->B->A firing pattern cannot be
        // represented (none exists in lv1-21; switch to a uid set if one
        // appears).
        (void)stillTouching;
    }
    // Stair snap (GD PlayerObject::checkSnapJumpToObject). Cube only -- GD gates
    // the call on vType == Cube. Runs every grounded tick, once per touched
    // solid. The contact test (half-width 15.0, strict <) reproduced GD's own
    // call sequence on 2022/2022 grounded ticks of the hooked lv3 run.
    // (and the ROBOT is deliberately NOT included: GD's gate is on the vehicle
    // type being Cube, and the robot is its own type. UNVERIFIED against a
    // measurement -- if a robot landing ever runs a pixel behind GD, this is
    // the first thing to check.)
    // ...and NOT while a ramp is what is holding the player up. GD calls the
    // snap from the collision it resolved, and on a ramp that collision is the
    // ramp: measured on lv19 with `snaptrace=1` on one side and `--snaplog` on
    // the other, GD makes **zero** snap calls for the whole ride (its last one
    // before the ramp is t=433..~460, the next is well after t=512).
    // The model kept snapping because its contact test is `|foot - face| <= 0.6`
    // over type-0 solids, and at t=505 the ledge (675,179.25) has its top face
    // at exactly 180.0 with the player's foot at exactly 180.0 -- zero
    // penetration, which GD does not count as a collision. It then compared
    // against a `snapObj` left over from (585,119.25) 90 px back, matched the
    // `big` stair pattern (dx=90, dy=60) and nudged x by the clamped +1.0000.
    // That 1 px never comes back: lv19 ran a pixel ahead of GD from t=505 to
    // the end, which is the whole of its remaining 2,817 divergent ticks.
    // THE CUBE'S OWN SPIN. The portal test needs it once an object is turned
    // (see obbSat): GD compares the two ORIENTED boxes and the player's is its
    // 30x30 turned by this.
    //
    // `runNormalRotation` (win 0x38d220) sets
    //   m_rotationSpeed = 180 * (signs) * [+0xb84] * speedArg / k ,
    //   k = 0.4333333 (size 1.0) / 0.3333333 (mini)
    // i.e. 415.3846 deg/s and 540 deg/s -- 1.730769 and 2.25 per tick at 240 Hz,
    // which is exactly what the dumps show, and SPEED-INDEPENDENT (identical at
    // 0.7 / 0.9 / 1.1 over 5,600 airborne ticks).
    // The sign: a take-off sets it POSITIVE and every GRAVITY FLIP negates it.
    // Measured on lv22's seeded clear -- the only two sign changes in 4,000
    // ticks are t=732 (flip 1->0) and t=1,308 (flip 0->1), both mid-air, and no
    // flip passes without one.
    // Grounded, GD EASES back toward the nearest multiple of 90 rather than
    // snapping -- and that matters, because a take-off before it has arrived
    // leaves the next arc's `rot mod 90` off, which is the only thing the
    // square player box cares about. Fitted over 380 grounded ticks of lv22's
    // seeded clear: rot += 0.1181 * (nearest90 - rot), residual in the 5th
    // decimal (0.11810..0.11815 across the whole sample).
    if (c.mode == 0) {
        // The sign is NOT a running parity -- it is written fresh, from the
        // gravity IN FORCE AT THE MOMENT OF THE WRITE, and then held. Hooked
        // `PlayerObject::runNormalRotation` and logged every call (lv22 seeded
        // clear, cfg hitboxtrace=1, `rnr:` lines): all ten pass speed=+1.0 and
        //   flip=0 -> m_rotationSpeed = +415.3846
        //   flip=1 -> m_rotationSpeed = -415.3846
        // The writes land on take-offs and on the tick a gravity change fires
        // (t=684 with flip still 0 -> +, t=731 with flip still 1 -> -), which is
        // why the arc from t=685 to t=731 keeps turning the OLD way even though
        // the player is upside down for all of it. Two earlier guesses -- "flip
        // decides it every tick" and "each flip negates it" -- are both refuted
        // by exactly that stretch.
        c.rotNeg = s.rotNeg;
        // **This tick's step turns with the sign it came in with.** The write
        // happens this tick, but takes effect from the next. Measured (lv20,
        // t=7,154 where a green ring makes flip 0->1): GD's rot turns at +1.7308
        // through 7,154 and changes to -1.7308 from 7,155.
        const uint8_t rotSignNow = s.rotNeg;
        // ...and the sign written is **the gravity at the end of that tick**
        // (c.flip). The old implementation used s.flip (pre-change) and the
        // sign failed to reverse at t=7,154 above. The original measurements of
        // lv22's t=684 (+ with flip still 0) / t=731 (- with flip still 1) are
        // also consistent with c.flip, since the dump's flip is the
        // end-of-tick value.
        // The gate is not "did flip change" but **was runNormalRotation
        // called** (measurements at rotWrite's declaration). A portal-driven
        // gravity change does not call it.
        if (rotWrite) c.rotNeg = rotWriteFlip;
        else if (s.grounded && !c.grounded) c.rotNeg = c.flip;
        // [2026-08-18] The easing applies "**if either end is grounded**". On
        // both the take-off tick and the landing tick, GD applies the easing,
        // not the rotation step. Measured (lv20, GD's dump, rot column):
        //   take-off t=7,062 (s.grounded=1 -> c.grounded=0):
        //     -359.68869 -> -359.725464, 0.11813 against the remaining -0.31131
        //   landing t=7,395 (s.grounded=0 -> c.grounded=1):
        //     -238.571289 -> -243.933823, 0.17062 against the remaining -31.429
        //   landing t=7,033: 371.923676 -> 370.515167 (GD also subtracts 720),
        //     0.11812 against the remaining -11.9237
        // The old implementation looked only at c.grounded and turned one extra
        // step on the take-off tick. The rotated-box test is decided by rot mod
        // 90, so this accumulated once per ~60 ticks and drifted 30 degrees in
        // 300 ticks.
        // [2026-08-25] ...**except on a tick that called runNormalRotation**.
        // That call restarts the spin there and then: a FULL step, in the
        // direction it has just written -- not the take-off easing, and not the
        // previous tick's sign. `rotWrite` already marks exactly those ticks
        // (pads, rings and orbs; it was only feeding c.rotNeg for the NEXT
        // tick). Measured on lv22, GD's dump against a bit-exact replay:
        //   t=1,170 yellow pad, take-off (s.grounded=1 -> c.grounded=0):
        //     GD 270.002 -> 271.733 = a full +1.731. The model eased (-0.000)
        //     and was one step short from there on.
        //   t=1,484 pink pad in mid-air, vy -15.000 -> +10.400:
        //     GD 180.002 -> 181.733 = +1.731, i.e. it REVERSES on the same
        //     tick. The model kept the old sign for one more step (-1.731) and
        //     lost two more.
        // Those three steps are the whole 5.19-degree gap that made the spider
        // portal uid1156 (see turnedBox) fire on the wrong tick. A plain
        // walk-off take-off calls nothing and still eases, which is what lv20
        // t=7,062 measured (-359.68869 -> -359.725464).
        // ...and only a PAD does. A ring or an orb calls runNormalRotation too
        // but leaves this tick's turn alone. Five measurements on lv22, GD's
        // dump against a bit-exact replay, cover both sides:
        //   PADS -- the step happens and it turns toward the CURRENT gravity
        //     t=1,170 yellow pad, take-off (s.grounded=1 -> c.grounded=0):
        //             GD 270.002 -> 271.733 = a full +1.731; the model eased
        //             (-0.000) and ran one step short from there on
        //     t=1,484 pink pad mid-air, spin was -1.731/tick:
        //             GD 180.002 -> 181.733 = +1.731, i.e. it REVERSES here
        //   ORBS -- the sign simply carries on through the tick
        //     t=663   flip 0->1, spin was +  -> GD +1.731
        //     t=727   flip 1->0, spin was +  -> GD +1.731
        //     t=1,424 flip 1->0, spin was -  -> GD -1.731
        //   (663/727/1424 are the same orb type at the same speed, so it is the
        //    PREVIOUS sign that carries, not the gravity: 727 and 1,424 share a
        //    flip transition and disagree on the step.)
        // The SIGN THE CALL LEAVES BEHIND is a third quantity and is unchanged
        // (c.rotNeg = rotWriteFlip, above): at t=727 GD steps + here and then
        // runs at -1.731 from the next tick.
        // UNDER-DETERMINED: both pad samples are at flip=0, so "toward the
        // current gravity" and "always positive" fit them equally. A pad taken
        // in flipped gravity is what would tell them apart.
        const bool spinNow = rotPadSpin || (!c.grounded && !s.grounded);
        const uint8_t spinSign = rotPadSpin ? c.flip : rotSignNow;
        if (spinNow && !c.grounded) {
            const double rate = c.mini ? 2.25 : 1.7307692;
            c.rot = (float)((double)s.rot + (spinSign ? -rate : rate));
        } else {
            // ...and the rate is **0.13125 x GD's speed multiplier**.
            // Size-independent. Taking per-speed medians over 23,868 samples
            // (lv1-22's grounded 3-tick windows, mode=cube), at every one of
            // 0.7/0.9/1.1/1.3 rate / speed = exactly 0.13125:
            //   0.7 -> 0.09187   0.9 -> 0.11812   1.1 -> 0.14437   1.3 -> 0.17063
            // The old 0.1181 was the 0.9 row only, 1.44x short in the fast
            // band, leaving angle behind on every landing.
            const double tgt = std::round((double)s.rot / 90.0) * 90.0;
            const double rate = 0.13125 * speedMulForDx(useDx);
            c.rot = (float)((double)s.rot + rate * (tgt - (double)s.rot));
        }
    } else if (c.mode == 1) {
        // [2026-08-20 r37] **The ship's bank angle.** Until now the model only
        // turned the cube's spin, and the ship's `rot` stayed frozen at
        // whatever value it came in with (0 on a cold run). The SAT for rotated
        // portals/pads uses the player's `s.rot`, so that had been a lie the
        // whole time.
        //
        // `PlayerObject::updateShipRotation` (win 0x390c40) builds an angle
        // from the difference between getObjectRect()'s origin and the
        // previous tick's position [+0x9f8]/[+0x9fc] (0x390cfc-0x390d18:
        // xmm1 = dx, xmm3 = **-dy**, atan2 at 0x390da6), and does nothing if
        // the squared displacement is under a threshold (comiss at 0x390d7b).
        // The direction is cocos's clockwise-positive, so
        //     target = -atan2(dy, dx) [deg]
        // and it **eases linearly** toward it.
        //
        // Measured (all 104,873 ship ticks of gdref lv1-22, of which 89,251
        // have displacement): the coefficient is **exactly 0.0375**, and every
        // quartile is 0.0375 too.
        //   rot += 0.0375 * (target - rot)
        // Agreement **99.86%** (89,126/89,251). All 4 groups of normal/mini x
        // upright/flipped are 99.8-99.9%, so it depends on **neither size nor
        // gravity**. The 125 misses are only ticks whose y is clamp-driven,
        // like the ceiling-ramp ride from lv16 t=4,128 on, off by ~0.05 deg.
        const double ddx = x - xPrev;
        const double ddy = (double)c.y - (double)s.y;
        if (ddx != 0.0 || ddy != 0.0) {
            const double tgt =
                -std::atan2(ddy, ddx) * 180.0 / 3.14159265358979;
            c.rot = (float)((double)s.rot + 0.0375 * (tgt - (double)s.rot));
        }
    }
    // RIDE. GD carries a standing player with the surface under it; the support
    // test above only asks "is a face still within 0.6 px of the foot" and
    // leaves y where it was. A platform that settles DOWNWARD under a standing
    // player therefore drifts away until the gap opens past that tolerance and
    // the model falls -- while GD is still standing on it.
    //
    // Measured on lv22 (2026-08-13, runR3's own plan): uid 254 settles
    // elastically under the cube at x~700. GD's y tracks its face
    // (236.870 -> 236.375 over t=530..542); the model holds 236.935 and goes
    // airborne at t=543, THREE TICKS before GD. That is a permanent vy offset of
    // 3 * 0.216 = 0.648, and by t=2,820 the two are 66 px apart in y -- which is
    // the whole of the x=3,956 wall (GD kills on the solid at (3975,345) that
    // the model's arc passes 100 px under).
    //
    // Gated hard: only a support that actually MOVED (dcy != 0), only when the
    // state came in standing and nothing else re-seated it (c.y == s.y), same
    // gravity, no ramp. A static floor is bit-identical.
    // ...and the ball's TAP does not cancel that tick's ride. GD carries the
    // rider in the collision pass and only then reads the button, so the tap
    // leaves from the face's NEW height. Both gates above go false on a tap
    // tick (`c.grounded` because the ball left, `c.flip == s.flip` because it
    // flipped), so the model tapped off the OLD height. Measured on lv22
    // t=5,940: GD taps from y=972.020, one more 0.129 px ride step below the
    // model's 972.149 -- and that 0.129 then rode along as a constant offset
    // for the whole rest of the section.
    const bool tapRide = ballFlippedThisTick && !c.grounded;
    // ...and neither does a JUMP. Same tick order as the tap: the collision
    // pass seats the rider on the face's NEW height and only then does the
    // button fire, so the jump leaves from the seated y. Measured on lv22
    // t=5,553 (robot held re-jump off a descending eased platform): GD rides
    // one more 0.171 px step down to 874.500 and jumps from there with
    // vy=+5.71; the model jumped off the OLD height and carried the 0.171 as
    // a constant offset for the rest of the segment. The `c.y == s.y` gate in
    // the condition below keeps the held CUBE re-jump out (it moves on the
    // jump tick, so its y is no longer the frozen surface value), and
    // `c.flip == s.flip` keeps out every impulse that changed gravity (the
    // ball's tap has its own `tapRide` path).
    const bool jumpRide = impulsedThisTick && !c.grounded && c.flip == s.flip;
    if (rideOn && s.grounded && c.y == s.y && !s.onSlope && !c.onSlope
        && (tapRide || jumpRide || (c.grounded && c.flip == s.flip))) {
        // TRIED AND REVERTED (2026-08-14): `c.y = s.y + rideDy` -- carry the
        // player by the face's own delta instead of re-seating it on the face,
        // so an offset a RE-ANCHOR came in with survives. It is a no-op from the
        // head (lv22's r24 replay 223 divergent ticks and r36's 68, both
        // unchanged) but the **regression went red**: lv19 lost a whole segment
        // (t=7,800, followed 400 -> 180 ticks). lv19's lift needs the re-seat --
        // GD carries the player exactly ON the face there, and letting it drift
        // by its entry offset is not the same ride.
        // So the mid-ride anchor problem (lv22 t=5,769: the re-seat drags the
        // player 2.559 px down on the first tick after the anchor) has to be
        // fixed on the ANCHOR side, not here.
        c.y = (float)(rideFace + (s.flip ? -1.0 : 1.0) * pHalf);
        (void)rideDy;
    }
    if (c.mode == 0 && (c.grounded || cubeContact || cubeLandedThisTick)
        && !s.onSlope && !c.onSlope) {
        const double gs = c.flip ? -1.0 : 1.0;
        // on a jump tick c.y has already been frozen at the surface, so the foot
        // is still the contact face -- s.y and c.y agree there
        const double foot = (double)c.y - gs * pHalf;
        const double blockLen = gs * 30.0;   // GD: flipMod() * 30
        // The size was hardcoded to 1.0, so every mini player got the FULL-size
        // stair table. At speed 0.9 that is little = 120 instead of 90, and the
        // gate is exact (+/- threshold), so mini stairs simply never matched.
        // Measured on lv11 t=3926: the player leaves the block at cx=4995,cy=165
        // and lands on the one at cx=5085,cy=195 -- (dx,dy) = (+90,+30), the
        // mini `little` pattern exactly. GD advanced x by 2.2983 that tick
        // instead of 1.29825, i.e. the clamped +1.0000 nudge; the model advanced
        // the plain 1.29825 and was 1 px behind GD for the rest of the level.
        // ...and the SPEED that selects the table is this state's own, not the
        // layer's (see State::dx). They differ as soon as one branch takes a
        // speed portal and another does not.
        const StairParams sp =
            stairParamsFor((double)useDx, c.mini ? kMiniScale : 1.0);
        // contact set is decided on the pre-snap x (GD collects collisions
        // before resolving them); the nudge then accumulates within the tick
        const double xBase = x;
        double xs = xBase;
        // GD processes the touched solids in DESCENDING m_uniqueID. Verified on
        // the hooked lv3 run: 976/976 multi-contact ticks were in that order,
        // and the LAST one processed is what m_objectSnappedTo ends up as. It is
        // not cx order -- picking ascending cx would take the wrong object on
        // 816 of those 976 ticks, which shifts the next stair's dx by 30 and
        // silently turns a matching pattern into a non-matching one.
        // At most 4 solids can touch a 30px box, so an insertion sort is fine.
        const Obj* touch[8];
        int nTouch = 0;
        for (const Obj* o : *K.near) {
            if (o->type != 0) continue;
            if (std::fabs(xBase - o->cx) > o->hw + pHalf + kContactEps) continue;
            const double face = c.flip ? (o->cy - o->hh) : (o->cy + o->hh);
            if (std::fabs(foot - face) > 0.6) continue;
            if (nTouch == 8) break;
            int j = nTouch++;
            for (; j > 0 && touch[j - 1]->uid < o->uid; --j) touch[j] = touch[j - 1];
            touch[j] = o;
        }
        for (int ti = 0; ti < nTouch; ++ti) {
            const Obj* o = touch[ti];
            const double xIn = xs;
            const double sdIn = (double)c.snapDist;
            const Obj* prevObj = c.snapObj;
            // A ONE-WAY platform never produces the nudge. Counted over GD's own
            // snaptrace for lv1/3/11/13/19 (15,530 calls):
            //   prev type 0  -> obj type 0    105 moved / 15,313 still
            //   anything with a type 21       **0 moved / 108 still**
            // lv13 t=2142 is one of those: prev (2685,195) and obj (2775,255)
            // are both one-ways, dx=90 dy=60 matches the `big` pattern exactly,
            // and GD still reports d=+0.0000. The model took the pattern and
            // moved x by the clamped +1.0000, and lv13 ran a pixel ahead of GD
            // for the rest of the level (its whole remaining divergence).
            // The bookkeeping still happens -- GD's log updates `sd` on these
            // calls (15.7939 -> 8.8877 at that tick) -- so only the displacement
            // is skipped, not the snapObj/snapDist update below.
            const bool onewayPair =
                o->oneway || (c.snapObj != nullptr && c.snapObj->oneway);
            if (c.snapObj != nullptr && c.snapObj != o && !onewayPair) {
                const double dx = o->cx - c.snapObj->cx;
                const double dy = o->cy - c.snapObj->cy;
                if ((std::fabs(dx - sp.little) <= sp.threshold
                     && std::fabs(dy - blockLen) <= sp.threshold)
                    || (std::fabs(dx - sp.down) <= sp.threshold
                        && std::fabs(dy + blockLen) <= sp.threshold)
                    || (std::fabs(dx - sp.big) <= sp.threshold
                        && std::fabs(dy - 2 * blockLen) <= sp.threshold)) {
                    double d = (o->cx + (double)c.snapDist) - xs;
                    d = std::max(-sp.threshold, std::min(sp.threshold, d));
                    // GD calls setPositionX, so the result lands back in the
                    // same float the next tick accumulates from
                    c.xAbs = (float)(xs + d);
                    xs = (double)c.xAbs;
                }
            }
            c.snapObj = o;
            c.snapDist = (float)(xs - o->cx);
            if (g_snapOut) {
                char b[256];
                snprintf(b, sizeof(b),
                    "snap: t=%lld y=%.3f prev=(%.2f,%.2f) obj=(%.2f,%.2f) "
                    "dx=%.2f dy=%.2f x=%.4f->%.4f d=%+.4f sd=%.4f->%.4f",
                    (long long)K.t, (double)c.y,
                    prevObj ? prevObj->cx : 0.0, prevObj ? prevObj->cy : 0.0,
                    o->cx, o->cy,
                    prevObj ? o->cx - prevObj->cx : 0.0,
                    prevObj ? o->cy - prevObj->cy : 0.0,
                    xIn, xs, xs - xIn, sdIn, (double)c.snapDist);
                *g_snapOut << b << "\n";
            }
        }
    }
    // Speed portals, decided on THIS state's own x and y (see State::dx) with
    // the same box-overlap rule as every other portal. The new dx only takes
    // effect from the next tick, because c.dx is read at the top of the step.
    if (K.speeds) {
        for (const Obj* sp : *K.speeds) {
            if (std::fabs(x - sp->cx) >= sp->hw + pHalf) continue;
            // yColl, not c.y: the entry-snapshot registers (see its
            // declaration by pHalfPortal). c.y here already carries this
            // tick's landing clamp and any mid-pass size re-seat, which GD's
            // registers never see -- both measured 1-tick-early speed fires
            // (lv18 t=14,612 / lv19 t=20,309) came from exactly that.
            // ...EXCEPT past a teleport: the destination is another section =
            // a later collisionCheckObjects call of the same tick with FRESH
            // registers, so a destination speed portal (uid past the
            // teleport's, the same gate the portal pass uses) fires on the
            // arrival tick at the arrival y. Measured on lv20 t=7,282: the
            // 747 pair drops the cube at y=161 and GD's very next x step is
            // already 1.95 -- the snapshot-only form ran it a tick late and
            // shrank the t=7,000 segment 286 -> 283.
            const double ySp = (teleportedThisTick && sp->uid > teleUid)
                                   ? (double)c.y : yColl;
            // [2026-08-21 r77] **A rotated speed portal is judged with "the
            // current box".**
            // The entry snapshot (yColl) and the axis-aligned player box are the
            // measurements for **axis-aligned** portals (lv18 t=14,612 / lv19
            // t=20,309 -- the "shadows" of the note). Rotated objects, per r32's
            // disassembly, re-call getOrientedBox() within the same pass =
            // current position + the player's rotation.
            // Measured lv19 t=19,491 (wave, the rot=-41 0.5x portal uid13186):
            // GD fires on the same tick as the mode/size portals (portfire
            // t=19,491), and that tick's move is still at the old speed. The
            // model hit 1 tick early with the start-of-tick y + an unrotated
            // box, and census `m4/.../gdm3/sp0.7` edy +0.252 remained to the
            // end. Same formula as the portal pass's pRotHere.
            if (sp->oriented) {
                double pRotSp = (double)s.rot;
                if (s.mode == 0)
                    pRotSp += (s.rotNeg ? 1.0 : -1.0)
                              * (s.mini ? 2.25 : 1.7307692);
                if (!orientedHit(*sp, x, (double)c.y, pHalf, pRotSp))
                    continue;
            }
            const double gapY =
                std::fabs(ySp - sp->cy) - (sp->hh + pHalf);
            if (gapY >= 0.0) {
                // A branch that slips OVER a speed portal by a hair is dropped,
                // the same way g_portalDodgeMin drops one that slips past a
                // mode portal. Off by default (0) because the miss here is not
                // sub-pixel and killing it removes real routes.
                //
                // Why the knob exists: lv19's plan flies 1.1-2.4 px above the
                // 1.1x portal at (26,161,333) and runs the rest of the level at
                // 0.9, which puts it at the moving gate at x=27,690 about 160
                // ticks after the gate's own window. Measured against a working
                // replay of the same level, whose speed log reads
                // t=18,614 x=26,138 -> 1.1 where the plan's has nothing at all.
                // The search HAS the branch that dips into the portal; it has
                // no reason to prefer it, because the timeline it plans against
                // was recorded from a run that did not take it either -- the
                // loop confirms its own choice.
                if (g_speedDodgeMin > 0.0 && gapY < g_speedDodgeMin
                    && std::fabs(c.dx - dxForSpeedId(sp->id)) > 1e-6)
                    DIE("speed/hairline-dodge", sp);
                continue;
            }
            c.dx = dxForSpeedId(sp->id);
        }
    }
    // [2026-08-19 D9] The leave-face mark. In a rotated frame, when **the ball
    // loses grounding on the same tick as a gravity flip**, -1.000 is written
    // into the next tick's vy (applied in the `s.pBallOff` branch). The point is
    // to check **at the end of the tick** -- the flip's source is a gravity
    // portal, and the portal pass runs after the physics.
    // **Flips from taps/orbs/pads are excluded.** Measured lv22 t=6,140 (gf=1,
    // ball, og 1->0, ud 0->1) is a tap, and GD emits **+3.426** (the ball's tap
    // value) on that tick, +3.555 the next -- the normal ladder. No -1.000.
    // Without the gate, lv22's tracking shrinks 18,948->18,904.
    if (c.mode == 2 && c.frame != 0 && s.grounded && !c.grounded
        && c.flip != s.flip && !impulsedThisTick)
        c.pBallOff = 1;
    // [2026-08-21 r52] Carry "the tick the rotation frame changed" one tick
    // forward. Used only by the next tick's gravity-portal gate (the note at the
    // declaration).
    // **Not lowered while the suppression is active**: the player passes through
    // the portal's box for many ticks, so lowering after 1 tick merely produced
    // the same re-fire on the next (r52's measurement: the divergence just
    // shifted 1 tick, t=6,323 -> 6,324).
    // [2026-08-21 r61] **GD sees the portal's flip first and then seats on the
    // ramp.** The model's tick order is "ramp -> portal", so a ride on the same
    // tick as the flip is resolved still in the old (upside-down) orientation
    // and vy survives 1 tick.
    // Measured lv22 t=8,236 (spider stuck to floor ramp uid17788 while gravity
    // portal uid8817 returns it upright): GD has vy=0 / onGround=1 on the same
    // tick. The model carried 1.704 (= the just-flipped value; GD's `pfg:` line
    // shows the same number) for 1 tick, census `m6/.../slope+1.00/ride2`
    // edvy -1.704.
    // The gate is just two things -- "this tick's ride was the upside-down-side
    // branch" + "the portal returned upright" -- limited to the portal-order
    // hole itself.
    if (rodeFlipped && c.onSlope && !c.flip && c.flip != s.flip) {
        c.vy = 0;
        c.grounded = 1;
    }
    // [2026-08-21 r66] **The ceiling ramp's push-down release places vy on the
    // next tick.** GD keeps m_isOnSlope set even during the push-down (slp:
    // lines), and the release runs the slope-exit launch machinery as is:
    //   vy := -slopeExitVy(|m|, mode, dx, mini) x slopeRampFactor(pressed ticks)
    // applied **max-down** (do nothing if free fall is already faster -- this is
    // why nothing appears at m=0.5 or sp0.7). Measured on the calibration rig
    // ceilrel x 3 speeds (sp0.7/0.9/1.1, 4 kinds of d): in all 14 set events
    // k = the pressed tick count matches exactly, q = base/24 is strictly
    // proportional to dx, base = cube launch x 0.75 (the ship/UFO/swing flag
    // family). The corpus's lv20 t=18,836 is also exactly base(sp1.1) x 10/24 =
    // -2.877 (pressed section 18,826..18,835 = 10 ticks).
    // Modes are only the measured ship/UFO/swing. The cube is **not included**:
    // the corpus (lv18 t=20,491, r40) measured "pure free fall after the
    // release". ball/wave are unmeasured (the rig cannot catch them). y is
    // already integrated this tick with the old vy, so it is untouched (GD has
    // the same order).
    if (ceilPressedNow) {
        c.ceilT = (uint8_t)std::min<int>((int)s.ceilT + 1, 200);
        c.ceilM4 = (uint8_t)std::min<int>(
            255, (int)std::lround(std::fabs(ceilPressM) * 4.0));
    } else if (s.ceilT > 0) {
        // [2026-08-21 r81] The spider receives it too. Measured lv22 t=8,273 (a
        // 14-tick m=-2 push-down chain, sp1.1): GD places vy := -7.621.
        // 7.621 = base x 14/24 -> base 13.064 -- the very shape of this formula.
        // [2026-08-28] **...and not on a tick the body has come to rest.** The
        // release is a launch off the surface that was pressing, and a body that
        // ended this tick standing on something is not leaving anything: GD
        // holds it still and the launch appears on the tick it actually goes.
        // Measured on lv20 t=17,110 (the dual's second body, flipped, coming out
        // of the ceiling ramp chain at x=25,752 onto slope uid13913):
        //   GD    t=17,110 p2y=295.9665 p2vy=**0.000** p2ground=1
        //         t=17,111 p2y=295.9955 (+0.029 = 0.225 x 0.129, the launch
        //                  convention's "y moves with the OLD velocity")
        //                  p2vy=**-2.762**
        //   model t=17,110 y2=295.9665 (right) but vy2=-2.762 already, while
        //         reporting grounded2=1 -- a launch velocity on a resting body
        // and the tick it costs is what leaves the model's p2 1.585 px clear of
        // the slope GD kills it on five ticks later.
        if (c.grounded && !s.grounded) {
            // landed THIS tick: the launch waits one tick, held not dropped, so
            // the same value comes out where GD puts it (t=17,111 above). A body
            // that was already resting is not this case -- gating on `grounded`
            // alone withheld the launch for as long as the body stayed down, and
            // cost lv22 its whole run (cold, t=19,037 -> stuck at t=2,783).
        } else {
            if (c.mode == 1 || c.mode == 3 || c.mode == 6 || c.mode == 7) {
                const double exU = slopeExitVy((double)s.ceilM4 / 4.0, c.mode,
                                               useDx, c.mini != 0)
                                   * slopeRampFactor((int)s.ceilT);
                if ((double)c.vy > -exU) {
                    c.vy = (float)-exU;
                    CLAMP0("ceil/release");
                }
            }
            c.ceilT = 0;
            c.ceilM4 = 0;
        }
    }
    // [2026-08-21 r93] **The launch of a ride interrupted by a warp appears on
    // the next tick.** The value is computed on the interrupting tick and stored
    // in pExitVy (State::pExitVy). The same "place vy on the next tick" frame as
    // ceil/release, placed **after all the clamps** (GD too writes only vy
    // without moving y: lv21 t=16,714 stays og=0 / y=226.500 with +5.226, pushed
    // back onto the ceiling to 0 the next tick).
    if (s.pExitVy != 0.f) {
        c.vy = s.pExitVy;
        c.grounded = 0;
    }
    // [2026-08-22 r106] **Crush: touching a solid's interior with the inner box
    // is death.** (The full measurement table at kCrushHalf's declaration).
    // The rule that replaces, with physics, lv22's dead end at x=20,130 -- "the
    // phantom that survives with no input until t=17,025 because the model has
    // no crush" (the origin of the driver's 20130,20270 band). Placed
    // **after** all position resolution: a state pushed out by landing / snap /
    // ride re-seating is not killed (the seat centre is face+-pHalf, and the
    // inner box never reaches within pHalf-4.5 >= 4.5px of the face). Applies
    // to axis-aligned solids only -- slopes have different semantics
    // (push-down/launch), oneways have a measurement surviving 15px embedded
    // (the Obj::oneway note), and non-90-degree rotations have lying bboxes
    // (the Obj::oriented note). Rotated sections (frame != 0) are excluded,
    // unmeasured.
    if (!dead && !g_noCrush && s.frame == 0) {
        const double cyF = (double)c.y;
        for (const Obj* o : *K.near)
            if (o->type == 0 && !o->slope && !o->oneway && !o->oriented
                && std::fabs(x - o->cx) < o->hw + kCrushHalf + 1e-6
                && std::fabs(cyF - o->cy) < o->hh + kCrushHalf + 1e-6) {
                DIE("crush", o);
                break;
            }
    }
    c.frameChg = (gravHoldOver || c.frame != s.frame) ? 1 : 0;
    return c;
}

// Swap the primary player's fields with the second one's. Running the SAME
// stepOne twice (once per half) is what keeps the dual honest: the two halves
// see different geometry, so nothing about the second can be derived from the
// first once either of them touches anything.
inline void swapHalves(State& s) {
    std::swap(s.y, s.y2);
    std::swap(s.vy, s.vy2);
    std::swap(s.slopeM, s.slopeM2);
    std::swap(s.snapDist, s.snapDist2);
    std::swap(s.grounded, s.grounded2);
    std::swap(s.flip, s.flip2);
    std::swap(s.ringHold, s.ringHold2);
    std::swap(s.onSlope, s.onSlope2);
    std::swap(s.slopeT, s.slopeT2);
    std::swap(s.mode, s.mode2);
    std::swap(s.mini, s.mini2);
    std::swap(s.ceilT, s.ceilT2);
    std::swap(s.ceilM4, s.ceilM42);
    std::swap(s.snapObj, s.snapObj2);
    std::swap(s.usedOrb, s.usedOrb2);
    for (int i = 0; i < 4; ++i) std::swap(s.usedPad[i], s.usedPad2[i]);
}

// Touch-trigger boxes (see TouchTrig): plain rect overlap of the player's box
// with the trigger's, which is what GD was measured to do. Once a bit is set it
// stays set -- the doors do not close again, and a state that could un-touch a
// trigger would be a different world line, not the same one.
// The dual's second body is deliberately not tested: no level has put a touch
// trigger inside a dual section, and firing from the mirrored half would have to
// be threaded through swapHalves as well.
// `preY` is where the player was BEFORE this tick's button effects. GD's tick
// is update -> collisions (triggers) -> buttons, so a SPIDER that teleports on
// this tick has already been counted as touching whatever its old position was
// inside. Measured on lv22 t=1,753 (first rotated section, frame 1): the player
// is at v=2,266.5 and the box uid1417 (world 2277,273 -> frame u=-273,v=2277)
// is du=34.41 / dv=10.50 away, inside 21.165+13.5 and 15+13.5 -- it fires in GD
// and the spawn cascade under it moves the spike uid17571 by -150 from t=1,815.
// The model tested the POST-teleport v=2,143.5 (dv=133.5), never fired, and so
// kept that spike frozen at its t=1 position 150 px away from where GD has it --
// a phantom hazard sitting in the only lane out of the section.
inline void markTouched(State& c, const StepCtx& K, double preY) {
    if (!K.trigs || K.trigs->empty()) return;
    const double half = playerHalf(c.mode, c.mini != 0);
    for (const auto& tb : *K.trigs) {
        if (c.trig & tb.second) continue;
        const TouchTrig* T = tb.first;
        if (std::fabs((double)c.xAbs - T->cx) < T->hw + half
            && (std::fabs((double)c.y - T->cy) < T->hh + half
                || std::fabs(preY - T->cy) < T->hh + half)) {
            c.trig |= tb.second;
            c.trigT = (int32_t)K.t;
            {
                // Level-wide first-entry tick per box (see g_touchFireT):
                // the state only remembers its LAST box, and the switch
                // band's per-box delay needs each punch's own tick.
                uint32_t mbit = tb.second;
                int b = 0;
                while (!(mbit & 1u) && b < 31) { mbit >>= 1; ++b; }
                if (g_touchFireT[b] < 0) g_touchFireT[b] = (int)K.t;
            }
            if (g_slopeDbg)
                std::printf("trigfire t=%lld box(%.1f,%.1f) %.0fx%.0f "
                            "player(%.1f,%.1f) half=%.1f mask=0x%x\n",
                            (long long)K.t, T->cx, T->cy, 2 * T->hw, 2 * T->hh,
                            (double)c.xAbs, (double)c.y, half, c.trig);
        }
    }
}

}  // namespace dp
