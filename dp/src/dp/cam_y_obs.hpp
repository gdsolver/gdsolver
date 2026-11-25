// --camy: the camera Y along the search's own states, OBSERVATION ONLY (design: notes camy_wiring_design_1115_gd05.md). Nothing here is read by the search, the dedupe key or the plan.
// S0 (this file): the flag, the counters and the print; the State carries camTwStart / camPortal / camY (state.hpp, in padding) and step.hpp sets camPortal / camTwStart where a mode portal enters or leaves the ground-limited camera.
#pragma once
#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <string>
#include "dp/cam_model.hpp"

namespace dp {

inline bool g_camy = false;   // --camy (reset.hpp sets it false)
inline double g_camyZoomSeed = -1.0, g_camyOffSeed = 0.0;   // --camyzoom Z,OFFY (S3-2): the anchor row's zoom (+0x1a8) and Y offset (+0x1b4): the timeline's catch-up at an anchor restarts every earlier Camera Zoom / Offset tween from rest, so the camera's zoom is seeded
inline double g_camyRest = -1e30;   // --camyrest P (S3-2): a Camera Static (1914) at rest at the anchor, its point P (+0x1e4 of the anchor row, tween done): the camera is P's window from the first tick, no trigger needed
inline bool g_camyBand = false;   // --camyband (S3-1, implies --camy): the flight band of a tick no recording covers is written from the state's own camera (cam_y_search.hpp camyWriteBand); reset.hpp sets it false

inline double g_camyStart = -1e30;   // --camystart V: the camera Y of the tick before the first one (an anchor input, as --start is; unset = the level start: the first tick snaps)
inline std::string g_camyStaticSeed;   // --camystatic START,TARGET,CUR,DUR,H: a mode portal's ground-limited camera that was already running at the anchor (the game's +0x1dc, +0x1ec, +0x1e4 of the tick before it, the tween's duration in seconds, +0x2ec): an anchor input like --start
inline std::string g_camyLevel;   // --camylevel FILE: the level string (.lvl) the camera model reads its triggers from (the replay path of --camy)

// a mode portal that entered / left the ground-limited camera on THIS tick, as the shadow's tick saw it (shadow_step.hpp). thread_local: written by the step of the thread that ran it and read (then cleared) by the replay right after that same
// step; the stamp t is the step's tick, checked by the reader. (S1, the replay path only.)
struct CamyPortalEv {
    bool valid = false, leave = false, dual = false, freeMode = false;
    long long t = 0;
    int mode = 0;      // State::mode the portal switches to
    float y = 0.0f;    // the portal's world y (portalBandY)
};
inline thread_local CamyPortalEv g_camyEv;

struct CamyObs {
    // atomic counters (plain ++, sequentially consistent; touched only with --camy): the search expands states on several threads
    std::atomic<long long> portalEnter{0};   // step.hpp: a mode portal / dual portal entered the ground-limited camera (updateDualGround's enter), camPortal = 1, camTwStart = camY
    std::atomic<long long> portalLeave{0};   // ... left it (the else branch: height 270 or a Free Mode portal), camPortal = 0
    std::atomic<long long> shPortalOld{0};   // shadow_step.hpp: a mode / dual portal tick the OLD tick ran (step.hpp's site handles it)
    std::atomic<long long> shPortalNew{0};   // ...a portal tick the shadow handled with no old tick (shadow_step.hpp's own site below handles it): the path the cold search takes
    std::atomic<long long> shEnter{0}, shLeave{0};   // what that site did: the ground-limited camera entered / left
    // S2.1 (cam_y_search.hpp, the search's phase 1): the children stepped, whether the group handed them its camera timeline input, and the portal events they produced
    std::atomic<long long> kidSteps{0}, noTick{0}, stampMiss{0}, tickOk{0}, tick1914{0}, evUsed{0}, evStale{0};
    std::atomic<long long> stepped{0}, unkTicks{0};   // S2.2: children whose camY was stepped, and those with an input that is not self-derived (a 1914 static camera, a Camera Edge object, no group input)
    // S2.4 (search_dp.hpp, main thread, before a group's dedupe): per dedupe key of the group's live children, whether their camY agree. keys = distinct keys, mixed = keys whose camY spread is over 1e-3 (mixed05: over 0.5), kids = children in a mixed key
    std::atomic<long long> mxGroups{0}, mxKeys{0}, mxMixed{0}, mxMixed05{0}, mxKids{0}, mxKids05{0};
    // S3-0 (a): the same keys, counting only children whose camera is a 1914 static with a known start (camPortal 254, camTan known): the band's lower edge (camY - 1 / zoom) spreads exactly as camY does
    std::atomic<long long> staticTicks{0}, staticTgtUnk{0}, bandWrites{0}, bandWritesB{0}, anchorExits{0}, trigRewrites{0};   // --camy observation: 1914-static ticks seen by the child step, of them with a target the model could not place; bands --camyband wrote
    std::atomic<long long> stChildren{0}, stKeys{0}, stMixed005{0}, stMixed025{0}, stMixed1{0}; std::atomic<int> stMaxMilli{0};
    std::atomic<int> mxMaxSpreadMilli{0};   // the largest spread seen, in 1/1000 (an atomic int: main thread only, kept atomic for the one print)
    std::atomic<bool> printed{false};
    void reset() { for (auto& w : dp::gdcam::g_camLiveWhy) w = 0; staticTicks = 0; staticTgtUnk = 0; bandWrites = 0; bandWritesB = 0; anchorExits = 0; trigRewrites = 0; dp::gdcam::g_camLiveAsked = 0; dp::gdcam::g_camLiveGot = 0; stChildren = 0; stKeys = 0; stMixed005 = 0; stMixed025 = 0; stMixed1 = 0; stMaxMilli = 0; mxGroups = 0; mxKeys = 0; mxMixed = 0; mxMixed05 = 0; mxKids = 0; mxKids05 = 0; mxMaxSpreadMilli = 0; portalEnter = 0; portalLeave = 0; shPortalOld = 0; shPortalNew = 0; shEnter = 0; shLeave = 0; kidSteps = 0; noTick = 0; stampMiss = 0; tickOk = 0; tick1914 = 0; evUsed = 0; evStale = 0; stepped = 0; unkTicks = 0; printed = false; }
};
inline CamyObs g_camyObs;

// what the group of a layer hands its states for the camera Y: the timeline's inputs at tick t (cam_model.hpp), made by trigshadow_live.hpp at the end of group() from a COPY of the parent timeline's camera state, with the stamp
// of the group it was made for. Passed to the phase-1 lambda by argument (a local of the group's scope), never through a global.
struct CamYGroup {
    bool valid = false;
    long long t = -1;
    uint64_t key = 0;
    gdcam::CamYTick tick;
    float zoomPrev = 1.0f, offYPrev = 0.0f;   // the Y side's zoom / offset of the previous tick (the parent timeline's camera state before this step)
};

inline std::string camySummary() {
    char b[256];
    std::snprintf(b, sizeof b, "static_ticks=%lld tgt_unknown=%lld live_asked=%lld live_answered=%lld band_writes=%lld band_writes_b=%lld why_nocov=%lld why_nomember=%lld why_blind=%lld", g_camyObs.staticTicks.load(), g_camyObs.staticTgtUnk.load(), dp::gdcam::g_camLiveAsked.load(), dp::gdcam::g_camLiveGot.load(), g_camyObs.bandWrites.load(), g_camyObs.bandWritesB.load(), dp::gdcam::g_camLiveWhy[2].load(), dp::gdcam::g_camLiveWhy[3].load(), dp::gdcam::g_camLiveWhy[4].load());
    return b;
}

inline void camyPrint() {
    if (!g_camy) return;
    g_camyObs.printed = true;
    if (g_camyObs.kidSteps.load() > 0)
        std::printf("camy: search children %lld: group input handed %lld, none %lld, stamp mismatches %lld, 1914 static ticks %lld; portal events used %lld, stale %lld; camera stepped %lld, with an input not self-derived %lld\n", g_camyObs.kidSteps.load(), g_camyObs.tickOk.load(), g_camyObs.noTick.load(), g_camyObs.stampMiss.load(), g_camyObs.tick1914.load(), g_camyObs.evUsed.load(), g_camyObs.evStale.load(), g_camyObs.stepped.load(), g_camyObs.unkTicks.load());
    if (g_camyObs.mxGroups.load() > 0)
        std::printf("camy: dedupe keys over %lld groups (children before the dedupe, camY seeded only): keys %lld, mixed (camY spread > 1e-3) %lld, mixed > 0.5 %lld; children in mixed keys %lld, in mixed > 0.5 keys %lld; largest spread %.3f\n", g_camyObs.mxGroups.load(), g_camyObs.mxKeys.load(), g_camyObs.mxMixed.load(), g_camyObs.mxMixed05.load(), g_camyObs.mxKids.load(), g_camyObs.mxKids05.load(), g_camyObs.mxMaxSpreadMilli.load() / 1000.0);
    if (g_camyObs.stChildren.load() > 0)
        std::printf("camy: moving static targets read live from the shadow: asked %lld, answered %lld\n", dp::gdcam::g_camLiveAsked.load(), dp::gdcam::g_camLiveGot.load());
        std::printf("camy: static-1914 children before the dedupe %lld: keys with 2+ of them %lld, band lower edge spread > 0.05 px %lld, > 0.25 px %lld, > 1 px (the key's rounding) %lld; largest spread %.3f\n", g_camyObs.stChildren.load(), g_camyObs.stKeys.load(), g_camyObs.stMixed005.load(), g_camyObs.stMixed025.load(), g_camyObs.stMixed1.load(), g_camyObs.stMaxMilli.load() / 1000.0);
    if (g_camyBand) std::printf("camy: --camyband branch A writes %lld, branch B (offY) writes %lld, anchor exits %lld, zoom / Free Mode trigger rewrites %lld\n", g_camyObs.bandWrites.load(), g_camyObs.bandWritesB.load(), g_camyObs.anchorExits.load(), g_camyObs.trigRewrites.load());
    std::printf("camy: portal enters %lld, leaves %lld (step.hpp); shadow portal ticks: with the old tick %lld, without %lld (enters %lld, leaves %lld) (observation only: camPortal / camTwStart set, nothing read)\n", g_camyObs.portalEnter.load(), g_camyObs.portalLeave.load(), g_camyObs.shPortalOld.load(), g_camyObs.shPortalNew.load(), g_camyObs.shEnter.load(), g_camyObs.shLeave.load());
}

// a replay / section run never reaches the search's print: report once at exit when the flag was on and nothing printed it
struct CamyAtExit { CamyAtExit() { std::atexit([] { if (g_camy && !g_camyObs.printed) camyPrint(); }); } };
inline CamyAtExit g_camyAtExit;

}  // namespace dp
