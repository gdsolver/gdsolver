// --camy S2: the camera Y of a STATE, one tick, from the state's own fields and the camera timeline input of its group -- the same rules as cam_y_replay.hpp (S1), with the state of the camera carried in the State
// (camY, camPortal, camTwStart: padding fields, observation only) instead of in a replay's locals. Used by the search's phase 1 (camyChild) and, beside S1's CamyRun, by the replay (cli.hpp) so the two can be held against each
// other tick by tick.
// What the State carries, and how:
//   camY        the camera Y (+0x414) after the tick (-1 = never stepped, the State's default: the first step snaps, as a level start does)
//   camPortal   a mode portal's ground-limited static camera: 0 none; 1..120 normal tween (0.5 s), the value is the number of ticks since the portal (saturating at 120); 129..248 the short tween (0.4 s: a portal while limited, or a dual
//               portal), 128 + ticks (saturating at 120); 254 = a Camera Static trigger (1914) of the timeline is in force (not computed: the tick is `unk` and the camera holds)
//   camTwStart  the static tween's start point (+0x1dc = the previous tick's camera centre when the portal was hit)
// The static tween's target and height are recomputed each tick from the State (bandRefY = the portal's y, mode) by cam_y_portal.hpp; the 1914 point, Camera Edge object y, post500, offApplied, +0x284 are not carried (as in S1:
// the tick is `unk` or the input is its default).
// Ownership: the group input is a by-reference argument of the phase-1 lambda (CamYGroup, a local of the group's scope), the portal event a thread_local cleared before the child's advance and read right after it on the same
// thread; both carry the tick stamp and a mismatch is counted and not used. No camera value travels in a global.
#pragma once
#include <cmath>
#include "dp/cam_y.hpp"
#include "dp/cam_y_obs.hpp"
#include "dp/cam_y_portal.hpp"
#include "dp/cam_y_static.hpp"
#include "dp/state.hpp"
#include "dp/bands.hpp"       // bandTrackRowAt, bandProgress, kBandRetracted
#include "dp/modifiers.hpp"   // kBandBase
#include "dp/search_key.hpp"   // bandClamps

namespace dp {

inline void camyChildBegin() { g_camyEv.valid = false; }

struct CamyStateOut { float camY = 0.0f; uint8_t camPortal = 0; float camTwStart = 0.0f; float camDy = -1e30f, camTan = -1e30f; bool unk = false; };

inline int camyModeId(int mode) {   // State::mode -> the mode portal's id (getGroundHeightForMode's switch; as cam_y_replay.hpp)
    switch (mode) { case 1: return 5; case 2: return 16; case 3: return 19; case 4: return 26; case 5: return 27; case 6: return 33; case 7: return 41; default: return 6; }
}

// pCamY / pCp / pStart / pY: the parent's camY, camPortal, camTwStart and y; c: the child after its step (mode, flip, frame, dual, bandRefY); cwy: the child's world y; ev: the portal event of THIS child's step (or null)
// refY: the portal's y the static target is recomputed from (the child's bandRefY; the replay passes the seeded target for a static camera already running at an anchor, where bandRefY was not seeded)
inline CamyStateOut camyStepState(float pCamY, uint8_t pCp, float pStart, float pDy, float pTan, double pY, const State& c, double cwy, const CamYGroup& g, const CamyPortalEv* ev, long long t, float maxPlayY, float refY) {
    CamyStateOut o;
    const gdcam::CamYTick yt = g.valid ? g.tick : gdcam::CamYTick{};
    const float zp = g.valid ? g.zoomPrev : 1.0f, op = g.valid ? g.offYPrev : 0.0f;
    bool unk = !g.valid;
    const bool unseeded = pCamY < -0.5f;   // State::camY = -1: no camera yet
    if (unseeded) pCamY = 0.0f;
    const float centrePrev = pCamY + 160.0f / zp - op;
    uint8_t cp = pCp; float start = pStart; bool exitSnap = false, entered = false;
    float tan = pTan; bool known1914 = false; gdcam::CamYTween tw1914;
    if (yt.staticY) {   // a Camera Static trigger (1914) of the timeline is in force: its point from the trigger's keys, the start (the previous tick's centre) and tangent (the camera's speed) of the tick it fired, the target group's object
        cp = 254;
        ++g_camyObs.staticTicks; if (!yt.st.tgtKnown) ++g_camyObs.staticTgtUnk;
        if (yt.yev) { start = centrePrev; tan = (unseeded || yt.tw != 1.0f || pDy < -1e29f) ? -1e30f : pDy * 240.0f; }
        const bool rest = tan > 1e29f;   // S3-2: at rest at the anchor (camTan 1e30): the point is camTwStart, constant, whatever the trigger's target
        known1914 = rest ? (yt.tw == 1.0f && !yt.unkCam) : (tan > -1e29f && yt.st.tgtKnown && yt.tw == 1.0f && !yt.unkCam);
        unk = !known1914;
        if (known1914) { tw1914.follow = yt.st.follow; tw1914.bezier = yt.st.bez; tw1914.dur = yt.st.dur; tw1914.speed = yt.st.speed; tw1914.tangent = tan; tw1914.bz = yt.st.bz; tw1914.target = yt.st.tgtY; tw1914.ease = yt.st.ease; tw1914.easeRate = yt.st.easeRate; }
    }
    else if (cp == 254) { cp = 0; exitSnap = true; tan = -1e30f; }
    if (ev && ev->valid && ev->t == t) {
        if (ev->leave) { if (cp != 254) cp = 0; }
        else if (cp != 254) {
            gdcam::CamYPortalIn pi; pi.mode = camyModeId(ev->mode); pi.prevRealMode = pi.mode; pi.dual = ev->dual; pi.otherMode = 6; pi.freeMode = ev->freeMode;
            pi.limitedBefore = cp != 0; pi.portalY = ev->y; pi.noSnap = false;
            const gdcam::CamYPortalOut po = gdcam::camYPortal(pi);
            if (po.enter) { cp = po.dur < 0.45f ? 128 : 0; start = centrePrev; entered = true; }
            else cp = 0;
        }
    }
    gdcam::CamYIn ci;
    ci.y = (float)cwy; ci.pointY = (float)cwy;
    ci.upsideDown = c.flip != 0 && c.frame == 0; ci.rotated = c.frame != 0;
    const bool f1 = c.mode == 1 || c.mode == 3 || c.mode == 4 || c.mode == 7;
    ci.flierNo9c3_9c6 = !f1; ci.flier = f1 || c.mode == 2 || c.mode == 6;
    ci.zoom = yt.zoom; ci.zoomPrev = zp; ci.offY = yt.offY; ci.offYPrev = op; ci.tw = yt.tw; ci.k2cc = 0.5f; ci.rate2d0 = 10.0f;
    ci.xgate = true; ci.defY = 0.0f; ci.top37a8 = maxPlayY; ci.edgeT = yt.edgeTGrp; ci.edgeB = yt.edgeBGrp;
    if (yt.edgeTGrp > 0 || yt.edgeBGrp > 0 || yt.unkCam) unk = true;   // (unkCam: a Camera Static the model cannot time -- spawned, touched, or in a trigger channel -- may have started or changed the camera)
    bool snap = unseeded || exitSnap;
    bool staticOn = false;
    if (cp == 254) {
        staticOn = true;
        if (known1914 && tan > 1e29f) { ci.staticY = start; ci.staticStart = start; ci.staticTarget = start; ci.staticRate = 1.0f; }
        else if (known1914) { float r = 1.0f; ci.staticY = gdcam::camYStaticPointAt(tw1914, start, yt.k, yt.st.tgtY, &r); ci.staticStart = start; ci.staticTarget = yt.st.tgtY; ci.staticRate = r; }
        else { unk = true; ci.staticY = centrePrev; ci.staticStart = centrePrev; ci.staticTarget = centrePrev; ci.staticRate = 1.0f; }
    } else if (cp != 0 || entered) {   // (a portal entered this tick with the normal tween has cp 0 until k is stored below)
        const int k0 = entered ? 0 : (int)(cp & 127);
        const int k = k0 + 1 < 120 ? k0 + 1 : 120;
        gdcam::CamYPortalIn pi; pi.mode = camyModeId(c.mode); pi.prevRealMode = pi.mode; pi.dual = c.dual != 0; pi.otherMode = 6; pi.portalY = refY; pi.limitedBefore = (cp & 128) != 0;
        pi.portalY = refY;
        const gdcam::CamYPortalOut po = gdcam::camYPortal(pi);
        if (!po.enter) cp = 0;
        else {
            const double D = ((cp & 128) ? 0.4 : 0.5) * 240.0;
            const float u = (float)std::min(1.0, (double)k / D);
            const float e = gdtrig::eased(u, 1, 2.0f);
            ci.staticY = start + (po.target - start) * e; ci.staticStart = start; ci.staticTarget = po.target; ci.mode2ec = po.h; ci.b310 = true; ci.staticRate = 1.0f;
            staticOn = true; cp = (uint8_t)((cp & 128) | k);
        }
    }
    ci.staticOn = staticOn;
    if (!snap && !staticOn && c.frame == 0 && std::fabs(cwy - pY) > 20.0) { const double cy = pCamY; if (320.0 + cy + 90.0 < cwy || cy - 90.0 > cwy) snap = true; }
    ci.snap = snap;
    gdcam::CamYState st; st.camY = pCamY;
    o.camY = gdcam::camYStep(st, ci);
    o.camPortal = cp; o.camTwStart = start; o.camDy = unseeded ? -1e30f : o.camY - pCamY; o.camTan = tan; o.unk = unk;
    return o;
}

// --camyband (S3-1): the flight band of this tick, for a tick no recording covers, from the state's own camera. Branch A of GJBaseGameLayer::getMinPortalY / getMaxPortalY (base+0x31a980 / 0x31aa60, a Camera Static
// that is not a mode portal's): min = max(90, (L1.y - 1) / zoom + camY), max = (L2.y + 1) / zoom + camY, where the two ground layers shrink toward the mode's height H: L1.y = (322 - H) * p / 2, L2.y = 320 - L1.y (p =
// bandProgress(bandAnim), measured on a custom level: L1 0 -> 11, L2 320 -> 309 for H = 300). The state keeps its band as floor / ceiling (the readers take the State's band for a tick with no record); the height the dp
// reads from it (ceil - floor, as step.hpp's branch A numerator does) is kept and only the floor moves. Only for a 1914 static whose point this model could place (not unk), for a mode a band clamps, and only where
// no recording row covers the tick (a recording is the truth; past --bandtrackend it answers no row).
inline void camyWriteBand(State& c, const CamyStateOut& so, float zoom, float offY, long long t) {
    if (g_camyBand && so.camPortal != 254) {
        // Branch B (the camera is not a Camera Static's): getMinPortalY = max(90, F + offY - 0.5 H / zoom), getMaxPortalY = min + H / zoom ([R] 2.209b base+0x31a980 / 0x31aa60: +0x308 = F, +0x2ec = H, +0x1a8 the zoom,
        // +0x1b4 the camera Offset Y). The state keeps the band at zoom 1 and its readers scale it about its centre by the zoom of the tick, so what is left is the centre's move with the camera Offset Y, which a mode portal's
        // write (the portal's own centre, offY 0) does not have: move the band by the change of offY since the offY it carries (State::bandOffY). [M] 62 flights' dump.csv: F = (pmin + pmax) / 2 - camoffy and H = (pmax - pmin) *
        // camscale are constant over long runs while zoom and offY move (84.7% of the 840,896 banded ticks sit in such runs of 20 ticks or more; a custom level tick 16300-16350: pmin - camoffy = 583.333 on every tick).
        if (!bandClamps(c) || c.bandOffY > 1e29f || !(offY < 1e29f)) return;   // (offY 1e30: the tick has no camera timeline, or an unknown Y-side camera trigger may have fired: leave the band as it is)
        if ((c.bandBranch & kBandFreeKnown) && (c.bandBranch & kBandFree)) return;
        if (c.bandBranch & kBandAnchorA) return;   // the anchor's band is GD's branch A reading, the portal's own band is what the exit puts back (shadow_step.hpp): not a band this branch moves
        if (!(c.bandCeil < 1e8f) || !(c.bandCeil > c.bandFloor) || !(c.bandFloor > 90.0005f)) return;   // no band, or a floor held at the clamp (the unclamped one is not kept)
        // (the per-level overrides g_shipCeilSet / g_ufoCeil / g_flyFloor were false / 0 / 0 with no setter left since their flags went, 2026-08-31, so the early return for `a level's own knob owns the band` never fired: removed, same path)
        { double fl = 0.0, ce = 0.0; if ((g_bandTrackEnd < 0 || t <= g_bandTrackEnd) && bandTrackRowAt(t, fl, ce)) return; }   // a recording row is the truth
        const float d = offY - c.bandOffY;
        if (d == 0.0f) return;
        if (!(c.bandFloor + d > 90.0f)) return;
        c.bandFloor += d; c.bandCeil += d; c.bandOffY = offY;
        ++g_camyObs.bandWritesB;
        return;
    }
    if (!g_camyBand || so.camPortal != 254 || so.unk || !(zoom > 0.0f) || !bandClamps(c)) return;
    if ((c.bandBranch & kBandFreeKnown) && (c.bandBranch & kBandFree)) return;   // the game's Free Mode: no band clamps this body (the dump's `freemode`), whatever the camera says
    // a recording covers the tick only while it runs: past --bandtrackend the dp used to HOLD the last row, which is the thing this replaces
    { double fl = 0.0, ce = 0.0; if ((g_bandTrackEnd < 0 || t <= g_bandTrackEnd) && bandTrackRowAt(t, fl, ce)) return; }
    // a state with no band (a mode portal taken under a Camera Static with no recording writes none: 0 .. 1e9) gets the mode's own height; one that has a band keeps the height it has
    const bool have = c.bandCeil < 1e8f && c.bandCeil > c.bandFloor;
    const double Hmode = (double)gdcam::groundHeightForMode(camyModeId(c.mode), camyModeId(c.mode));
    const double H = have ? (double)(g_freeModeCol ? (double)c.bandCeil - (double)c.bandFloor : kBandBase) : Hmode;
    const double p = bandProgress(c.bandAnim);
    const double L1 = (kBandRetracted - H) * p / 2.0;
    const double fl = std::max(90.0, (L1 - 1.0) / (double)zoom + (double)so.camY);
    const double h = have ? (double)c.bandCeil - (double)c.bandFloor : Hmode;
    c.bandFloor = (float)fl; c.bandCeil = (float)(fl + h);
    c.bandBranch = (uint8_t)((c.bandBranch | 1) & ~2);
    ++g_camyObs.bandWrites;
}

// phase 1: the child's camera, from its parent and its group's input; writes the child's camY / camPortal / camTwStart (observation only: nothing reads them)
inline void camyChild(const State& parent, State& child, long long t, const CamYGroup& g, float maxPlayY) {
    ++g_camyObs.kidSteps;
    if (!g.valid) ++g_camyObs.noTick;
    else if (g.t != t) ++g_camyObs.stampMiss;
    else { ++g_camyObs.tickOk; if (g.tick.staticY) ++g_camyObs.tick1914; }
    const CamyPortalEv* ev = nullptr;
    if (g_camyEv.valid) { if (g_camyEv.t != t) ++g_camyObs.evStale; else { ++g_camyObs.evUsed; ev = &g_camyEv; } }
    const CamYGroup gu = (g.valid && g.t == t) ? g : CamYGroup{};
    const CamyStateOut so = camyStepState(parent.camY, parent.camPortal, parent.camTwStart, parent.camDy, parent.camTan, (double)parent.y, child, (double)child.y, gu, ev, t, maxPlayY, (float)child.bandRefY);
    child.camY = so.camY; child.camPortal = so.camPortal; child.camTwStart = so.camTwStart; child.camDy = so.camDy; child.camTan = so.camTan;
    camyWriteBand(child, so, gu.valid ? gu.tick.zoom : 1.0f, (gu.valid && !gu.tick.unkOff) ? gu.tick.offY : 1e30f, t);
    ++g_camyObs.stepped; if (so.unk) ++g_camyObs.unkTicks;
    g_camyEv.valid = false;
}

}  // namespace dp
