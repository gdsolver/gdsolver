// --camy S1: the camera Y of GD's updateCamera along ONE path (the replay), every input the dp's own: the player's y and mode from the State, the zoom / offset / time warp / Camera Edge ids from the level's camera triggers
// (cam_model.hpp, run on this path's x), a mode portal's ground-limited static camera from the portal event step.hpp leaves (cam_y_portal.hpp, cam_y_static.hpp). OBSERVATION ONLY: nothing reads the result but the trace columns
// and the State::camY / camPortal / camTwStart that the replay writes back.
// Owner of every value: a CamyRun is a local of the replay function; the portal event is a thread_local that the shadow's tick (shadow_step.hpp) writes and the replay reads and clears right after the same step (stamped with the tick; a
// stamp that is not this tick is counted, never used). No global carries a camera value from one place to another.
// Inputs NOT self-derived here (each one is marked in the trace's `unk` column when it matters): the 1914 static camera's point (the target group's live position: an object the replay does not place for the camera) -> `src` 2 ticks
// are not computed; a top / bottom Camera Edge's object y -> `unk`; the exit offset tween after a 1914 exit (offApplied) -> 0; +0x284 (the rotated frame's tween) -> 0 and the frame is read as "rotated" from State::frame != 0;
// the player's camera point y (pointY, bVar5) -> the player's own y; k2cc / rate2d0 (+0x2cc / +0x2d0) -> the level-start defaults 0.5 / 10 (a free-mode portal / camera-mode trigger changes them); the post500 timer -> 0.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include "dp/cam_model.hpp"
#include "dp/cam_y.hpp"
#include "dp/cam_y_obs.hpp"
#include "dp/cam_y_portal.hpp"
#include "dp/cam_y_static.hpp"

namespace dp {

struct CamyTickIn {
    long long t = 0;
    double x = 0.0, y = 0.0;   // the player's world x and y after the tick's step
    int mode = 0;              // State::mode: 0 cube, 1 ship, 2 ball, 3 ufo, 4 wave, 5 robot, 6 spider, 7 swing
    bool flip = false, dual = false;
    int frame = 0;             // State::frame (0 = the level's own orientation)
};

struct CamyOut {
    float camY = 0.0f, zoom = 1.0f, offY = 0.0f;
    int src = 0;               // 0 no static camera, 1 a mode portal's ground-limited camera, 2 a Camera Static trigger (1914: not computed)
    bool unk = false;          // an input this tick is not self-derived (src 2, an Edge object, ...): the value is a hold, not a model
};

class CamyRun {
public:
    gdcam::CamModel M;
    gdcam::CamState cs;
    bool haveLevel = false;
    float maxPlayY = 0.0f;
    float camY = 0.0f, zoomPrev = 1.0f, offYPrev = 0.0f, prevCand = 0.0f;
    bool first = true, seeded = false;   // seeded: an anchor camera; the first tick takes the model's zoom / offset as the previous ones (the tweens before the anchor are not replayed)
    double lastY = 0.0;
    int src = 0;               // see CamyOut
    bool exitSnap = false;     // the tick a 1914 static camera ends: +0x244 snaps the camera
    float camDy = -1e30f;      // this run's last camera step (camY after - before; -1e30 = none yet: the first tick snaps): the speed a static trigger takes as its bezier tangent
    float st1914Start = 0.0f, st1914Tan = 0.0f; bool st1914Known = false;   // the 1914 tween's start point and bezier tangent, taken on the tick the run saw it fire (not at an anchor: then the tick is `unk`)
    gdcam::CamYStatic pst;     // the portal's tween
    float pTarget = 0.0f, pH = 270.0f;
    bool pDual = false;
    long long evUsed = 0, evStampMiss = 0, ticks = 0, unkTicks = 0, srcTicks[3] = {0, 0, 0};
    gdcam::CamYTick dbgYt; float dbgZp = 1.0f, dbgOp = 0.0f;   // the timeline input of the last step and the previous zoom / offset it used (cli.hpp hands them to the state-encoded step beside this one)

    float seedZoom = -1.0f, seedOff = 0.0f;   // S3-2: the anchor row's zoom / Y offset (--camyzoom), put into the timeline's tweens before its catch-up call
    float restPt = -1e30f; bool restOn = false;   // S3-2: a 1914 at rest at the anchor (--camyrest): its point, and whether it is still the camera's (a new trigger or the exit ends it)
    bool seedStatic = false; float sdStart = 0.0f, sdTarget = 0.0f, sdCur = 0.0f, sdDur = 0.5f, sdH = 270.0f;
    bool prerolled = false;
    // The camera timeline walked from the level's first tick along --xtrack's rows (GD's x per tick, the ticks before the anchor), as the search's shadow does (TrigLive::replayInto), instead of the one catch-up call at the anchor
    // that fires every earlier Zoom / Offset trigger at once and restarts its tween from rest (a custom level t=11800: the model's Offset Y 15 and zoom 1 at the anchor, GD's 0 and 0.9, for 250 ticks). The anchor row's zoom / Offset
    // seeds (--camyzoom) are not applied then: the timeline is already where GD's is. [M] a custom level:11800: Offset Y / zoom error max 15 / 0.1 -> 0 / 0; over the 591 valid sections of the 604 (229,170 ticks) the replay's zoom is within
    // 0.001 of GD on 92% of the ticks (64% with the catch-up) and its Offset Y within 0.05 on 97% (86%). Only a replay given a camera (--camylevel) has a timeline at all: the loop passes none, so nothing in it changes.
    void preroll(const std::vector<std::pair<int, double>>& track, long long t0) {
        if (!haveLevel || track.empty()) return;
        float zx = 1.0f; long long n = 0;
        for (const auto& r : track) { if (r.first >= t0) break; M.step(cs, r.second, &zx, nullptr, r.second, std::nan("")); ++n; }
        if (n > 0) prerolled = true;
    }
    bool begin(const std::string& levelFile, double maxPlayYv, double startCamY = -1e30, const std::string& staticSeed = std::string()) {
        if (!staticSeed.empty() && std::sscanf(staticSeed.c_str(), "%f,%f,%f,%f,%f", &sdStart, &sdTarget, &sdCur, &sdDur, &sdH) == 5) seedStatic = true;
        maxPlayY = (float)maxPlayYv;
        if (startCamY > -1e29) { camY = (float)startCamY; first = false; seeded = true; }
        haveLevel = !levelFile.empty() && M.load(levelFile);
        return haveLevel;
    }

    static int portalModeId(int mode) {   // State::mode -> the portal's mode id (getGroundHeightForMode's switch; ufo / wave / swing 19 / 26 / 41 are fitted, not named in the code)
        switch (mode) { case 1: return 5; case 2: return 16; case 3: return 19; case 4: return 26; case 5: return 27; case 6: return 33; case 7: return 41; default: return 6; }
    }

    CamyOut step(const CamyTickIn& in, CamyPortalEv* ev) {
        ++ticks;
        gdcam::CamYTick yt; float zx = 1.0f;
        if (haveLevel && seeded && !prerolled && seedZoom > 0.0f) { cs.zoom.v = seedZoom; cs.zoomY.v = seedZoom; cs.offY.v = seedOff; }
        if (haveLevel) M.step(cs, in.x, &zx, &yt, in.x, in.y);
        const bool anchorTick = seeded;
        const float zpBefore = zoomPrev, opBefore = offYPrev;
        if (seeded) { zoomPrev = yt.zoom; offYPrev = yt.offY; lastY = in.y; seeded = false; yt.yev = false; src = yt.staticY ? 2 : 0; }   // an anchor: the triggers before it were caught up in this one call, so their start / exit events are not events of this tick
        bool unk = false;
        dbgYt = yt; dbgZp = anchorTick ? yt.zoom : zpBefore; dbgOp = anchorTick ? yt.offY : opBefore;
        if (seedStatic) {   // --camystatic: the portal's tween as the game had it at the anchor (it is not replayed from its start); a 1914 static camera of the timeline wins (src 2)
            seedStatic = false;
            if (!yt.staticY) {
                gdcam::CamYTween tw; tw.follow = false; tw.bezier = false; tw.dur = sdDur; tw.target = sdTarget; tw.ease = 1; tw.easeRate = 2.0f;
                pst.fire(tw, sdStart); pst.cur = sdCur;
                const double D = (double)sdDur * 240.0, span = (double)sdTarget - (double)sdStart;
                double e = std::fabs(span) < 1e-6 ? 1.0 : ((double)sdCur - (double)sdStart) / span; e = e < 0 ? 0 : (e > 1 ? 1 : e);
                const double u = e < 0.5 ? std::sqrt(e / 2.0) : 1.0 - std::sqrt(2.0 * (1.0 - e)) / 2.0;   // the inverse of ease 1 (in / out, rate 2)
                pst.k = (long long)std::llround(u * D);
                pTarget = sdTarget; pH = sdH; src = 1;
            }
        }
        // the camera centre of the previous tick = the start point of a static tween ([M] c1dc = c414 + 160 / zoom - offset)
        const float centrePrev = camY + 160.0f / zoomPrev - offYPrev;
        // 1914: the timeline's static (the target group's point is not available here)
        if (yt.yev) { restOn = false; src = 2; st1914Known = !anchorTick && yt.tw == 1.0f && camDy > -1e29f; st1914Start = centrePrev; st1914Tan = camDy * 240.0f; }   // [M] start = the previous tick's centre, tangent = the camera's speed one tick before (per second)
        else if (!anchorTick && src == 2 && !yt.staticY) { src = 0; exitSnap = true; restOn = false; }
        if (anchorTick && src == 2 && restPt > -1e29f && !yt.st.follow) restOn = true;
        // a mode portal's ground-limited camera ([R] updateDualGround 0x31a4a0 -> 0x31a920 -> prepareMoveActions 0x3490a0)
        if (ev && ev->valid) {
            if (ev->t != in.t) { ++evStampMiss; }
            else {
                ++evUsed;
                if (ev->leave) { if (src == 1) src = 0; }
                else if (src != 2) {
                    gdcam::CamYPortalIn pi; pi.mode = portalModeId(ev->mode); pi.prevRealMode = pi.mode; pi.dual = ev->dual; pi.otherMode = 6;
                    pi.freeMode = ev->freeMode; pi.limitedBefore = (src == 1); pi.portalY = ev->y; pi.noSnap = false;
                    const gdcam::CamYPortalOut po = gdcam::camYPortal(pi);
                    if (po.enter) {
                        gdcam::CamYTween tw; tw.follow = false; tw.bezier = false; tw.dur = po.dur; tw.target = po.target; tw.ease = 1; tw.easeRate = 2.0f;
                        pst.fire(tw, centrePrev);
                        pTarget = po.target; pH = po.h; pDual = ev->dual; src = 1;
                    } else if (src == 1) src = 0;
                }
            }
            ev->valid = false;
        }
        gdcam::CamYIn ci;
        ci.y = (float)in.y; ci.pointY = (float)in.y;
        ci.upsideDown = in.flip && in.frame == 0; ci.rotated = in.frame != 0;
        const bool f1 = in.mode == 1 || in.mode == 3 || in.mode == 4 || in.mode == 7;
        ci.flierNo9c3_9c6 = !f1; ci.flier = f1 || in.mode == 2 || in.mode == 6;
        ci.zoom = yt.zoom; ci.zoomPrev = zoomPrev; ci.offY = yt.offY; ci.offYPrev = offYPrev;
        ci.tw = yt.tw; ci.k2cc = 0.5f; ci.rate2d0 = 10.0f;
        ci.xgate = true; ci.defY = 0.0f; ci.top37a8 = maxPlayY;
        ci.edgeT = yt.edgeTGrp; ci.edgeB = yt.edgeBGrp;
        if (yt.edgeTGrp > 0 || yt.edgeBGrp > 0 || yt.unkCam) unk = true;   // the Edge objects' y are not placed here; a Camera Static the model cannot time may have changed the camera
        ci.prevCand = prevCand;
        bool snap = first || exitSnap;
        // [R] checkCameraLimitAfterTeleport (0x3427b0): after a teleport with no static camera, a player beyond the window snaps the camera ([M] a jump of y by more than 20 in one tick; margin 90)
        if (!first && src == 0 && in.frame == 0 && std::fabs(in.y - lastY) > 20.0) {
            const double cy = camY, py = in.y; if (320.0 + cy + 90.0 < py || cy - 90.0 > py) snap = true;
        }
        ci.snap = snap;
        float rate = 1.0f;
        if (src == 1) {
            ci.staticOn = true; ci.staticY = pst.step(0.0f, &rate); ci.staticStart = pst.start; ci.staticTarget = pTarget; ci.mode2ec = pH; ci.b310 = true; ci.staticRate = 1.0f;
        } else if (src == 2) {
            ci.staticOn = true;
            if (restOn && yt.tw == 1.0f && !yt.unkCam) {   // a static at rest: the point is the anchor row's, constant
                ci.staticY = restPt; ci.staticStart = restPt; ci.staticTarget = restPt; ci.staticRate = 1.0f;
            } else if (st1914Known && yt.st.tgtKnown && yt.tw == 1.0f && !yt.unkCam) {   // the point of the tween ([M] cam_y_static.hpp camYStaticPointAt): its target is a static group's object
                gdcam::CamYTween tw; tw.follow = yt.st.follow; tw.bezier = yt.st.bez; tw.dur = yt.st.dur; tw.speed = yt.st.speed; tw.tangent = st1914Tan; tw.bz = yt.st.bz; tw.target = yt.st.tgtY; tw.ease = yt.st.ease; tw.easeRate = yt.st.easeRate;
                float r = 1.0f;
                ci.staticY = gdcam::camYStaticPointAt(tw, st1914Start, yt.k, yt.st.tgtY, &r); ci.staticStart = st1914Start; ci.staticTarget = yt.st.tgtY; ci.staticRate = r;
            } else {
                unk = true; ci.staticY = centrePrev; ci.staticStart = centrePrev; ci.staticTarget = centrePrev; ci.staticRate = 1.0f;   // a hold at the previous centre: the point is not known here (a follow of a moving group, a seeded anchor)
            }
        }
        CamyState_ cst; cst.camY = camY;
        float cand = 0.0f;
        const float out = gdcam::camYStep(cst, ci, &cand);
        camDy = first ? -1e30f : out - camY; camY = out; prevCand = cand; zoomPrev = yt.zoom; offYPrev = yt.offY; lastY = in.y; first = false; exitSnap = false;
        ++srcTicks[src]; if (unk) ++unkTicks;
        CamyOut o; o.camY = out; o.zoom = yt.zoom; o.offY = yt.offY; o.src = src; o.unk = unk;
        return o;
    }
private:
    using CamyState_ = gdcam::CamYState;
};

}  // namespace dp
