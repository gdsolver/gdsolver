// The camera's X as the game computes it, for the screen-type Areas (an Area whose centre is a point of the screen reads the camera's left edge; a shift of 0.01 px in it moves the objects it acts on, a shift in Y moves
// none: notes camera_model_1105_gd04.md "11-06 early"). Observation only (TrigLive); not read by the search.
//
//   camPX = max(15, (x - 284.5f / zoom) + 75.0f / zoom + offsetX)      in float, in this order (bit-exact against the recorded camera on custom level A and custom level B, 99-100% elsewhere; cam_x_bits5.py)
//   zoom, offsetX: GJValueTween channels (a Camera Zoom 1913 and a Camera Offset 1916 start one when the player's x first reaches the trigger's x; one step of 1 / (60 N) s per tick, N = the substeps of the time warp)
//
// Not modelled -- the camera is then `uncertain` for the rest of the lineage: a Static Camera 1914 or a Camera Edge 2062 that has been crossed (they clamp or replace the follow), a spawn- or touch-driven zoom (the
// shadow's spawn chain does not reach them), the first 180 ticks and the last ticks of a level.
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "dp/trigshadow.hpp"   // gdtrig::eased: GameToolbox::getEasedValue in float
#include "dp/cam_y_static.hpp"   // camYStaticPointAt: the static camera's point, the same closed form for the X axis (updateCamera iterates both axes in one loop)

namespace dp::gdcam {

struct CamTrig {
    int id = 0, ord = 0;
    double x = 0.0;

    float to = 1.0f, dur = 0.0f, rate = 0.0f;
    int ease = 0;
    int mode = 0;          // 1916: key 101 (1 = Y only, 2 = X only)
    float offX = 0.0f, offY = 0.0f;
    float tw = 1.0f;       // 1935: key 120
    int grp = 0;           // 2062: key 51 (the group whose object is the limit; 0 clears it); mode = key 164 (1 left, 2 right, 3 top, 4 bottom); 1914: mode = key 101, grp = key 110 (exit)
    // 1914, the static camera's tween as the Y timeline (cam_y_static.hpp) needs it: target group (key 71), follow (212), follow speed (213, at least 1), bezier (453)
    int tgt = 0; bool follow = false, bez = false; float speed = 1.0f;
    // 2.2 trigger channels (key 170): the crossing array of the active channel is the only one walked (checkSpawnObjects); a Rotate Gameplay (2900) with m_changeChannel (171) switches the channel to key 173 (the shadow's trigshadow.hpp does the same)
    int chan = 0, order = 0; double y = 0.0;   // key 170, the trigger's order (always 0 here: its level-string key is not verified; an ordered trigger sorts behind the unordered ones in the game), y + 90 (the object's y in the game)
    std::vector<int> groups; bool yUnk = false;   // key 57; yUnk: a 1914 the Y side cannot vouch for -- its group is named by a Toggle (the game passes over a trigger whose object is switched off, the camera side does not look) or it has a trigger order (key 115)
    bool swarm = false, chanOnly = false; int swch = 0, dir = 0;   // 2900: 171 changeChannel, 172 channel only, 173 target channel, 167 gnddir
    float bz = 0.0f;       // 1914 key 454 (+0x26c: the bezier's second control point's weight)
};

// GJValueTween: elapsed += dt; u = elapsed < dur ? elapsed / dur : 1; eased when easing > 0; value = from + (to - from) * u
struct Chan {
    float v = 0.0f;
    bool on = false;
    float from = 0.0f, to = 0.0f, dur = 0.0f, el = 0.0f, rate = 0.0f;
    int ease = 0;
    void start(float to_, float dur_, int ease_, float rate_) {
        if (dur_ <= 0.0f) { v = to_; on = false; return; }
        from = v; to = to_; dur = dur_; el = 0.0f; ease = ease_; rate = rate_; on = true;
    }
    float step(float sec) {
        if (!on) return v;
        el += sec;
        const float u = el < dur ? el / dur : 1.0f;
        v = from + (to - from) * (ease > 0 ? gdtrig::eased(u, ease, rate) : u);
        if (el >= dur) on = false;
        return v;
    }
};

// --camy: the live y of a moving static target, read from the trigger shadow the tick AFTER the Camera Static fired -- the game reads the target group's first object after that tick's movement, and
// the shadow handed to the model between two ticks is in that state (trigshadow_live.hpp camYTickFor). ok = a value for group g.
struct CamLive {
    bool ok = false; int g = 0; float y = 0.0f;
    // the follow target of the Static Camera in force AFTER this tick's movement (the first object of group pg; py without the +90 of the world): handed to CamModel::resolveLive once the shadow has stepped the tick
    bool okPos = false; int pg = 0; float px = 0.0f, py = 0.0f;
};
inline bool g_camNoLive = false;   // --camynolive (observation arm): the moving static targets are not read live (they stay unknown, as before)
inline std::atomic<long long> g_camLiveAsked{0}, g_camLiveGot{0}, g_camLiveWhy[5] = {{0}, {0}, {0}, {0}, {0}};   // observation: moving static targets asked of the shadow / answered (printed with the camy counters)

struct CamState {
    Chan zoom, offX;
    double runMax = -1e18;
    int active = 0;         // the active trigger channel (a dense index)
    bool yUnk = false;      // a 1914 with yUnk fired: the Y side's static point is not to be trusted from here on (sticky)
    bool vertical = false, vertUnk = false;   // a Rotate Gameplay turn of up / down travel: the crossing arrays compare y there; with no y handed in the channel cannot be walked (vertUnk: the camera is unknown)
    static constexpr int kMaxChan = 32;   // the level's distinct trigger channels, as dense indices (CamModel::chIdx); more than this and the camera is unknown from the start (no heap: a CamState is copied per timeline per layer)
    uint32_t cur[kMaxChan] = {};   // each channel's cursor into its crossing array (kept while another channel is active)
    uint32_t revMask = 0;          // each channel's reverse bit (gnddir - 2 <u 2)
    float tw = 1.0f;
    bool staticX = false;   // a Static Camera that moves X is active (its object is the camera: not modelled)
    bool edgeX[3] = {false, false, false};   // a left (1) / right (2) Camera Edge is set
    int edgeG[3] = {0, 0, 0};                // ...and its group (key 51): the limit is the x of the group's first object
    long long n = 0;        // ticks stepped (the clock of an exit tween)
    double lastX = -1e18;   // the player's x of the previous tick (a jump of more than 20 px is a teleport: the camera lags behind it)
    long long tpUntil = -1; // a teleport / reverse: the camera X returns by a lerp (not modelled) until this tick
    long long exitUntil = -1;   // a Static Camera was left: the camera X returns to the follow position by a tween (cand + d0 * (1 - ease(u)), 1 / (60 N) s a tick) until this tick: not modelled
    bool uncertain = false; // staticX, an X edge, the return from a static camera, or the level end's blend zone: the camera X of this tick is not the follow formula, not modelled
    // the Y side (observation only, read by cam_y.hpp's callers; nothing above reads it): the timeline-level inputs of updateCamera's y iteration
    Chan offY;               // a Camera Offset's Y (key 29), the same tween as offX
    bool staticY = false;    // a Static Camera that moves Y is active (+0x23c)
    Chan zoomY;              // the zoom tween as the Y side reads it (see CamModel::step)
    int edgeTGrp = 0, edgeBGrp = 0;   // a top / bottom Camera Edge's group (key 51; 0 = none)
    struct YStaticEv { long long n = -1; int tgt = 0; bool follow = false, bez = false; float dur = 0.0f, speed = 1.0f, easeRate = 2.0f, bz = 0.0f; int ease = 0; bool tgtKnown = false; float tgtY = 0.0f; bool liveWait = false; float startC = 0.0f; bool liveFollow = false, startOk = false; float cur = 0.0f, tprev = 0.0f; } yev;   // the last static trigger fired (n = the tick it took effect on)
    // the X side of the static camera ([R] GJBaseGameLayer::updateCamera base+0x345c60: one do-while over both axes, +0x238 / +0x23c flags, point +0x1e0 / +0x1e4, start +0x1d8 / +0x1dc): the tween of the last static trigger that moved X
    // (YStaticEv's fields; tgtY holds the target's x here), and the previous tick's camera centre it starts from
    YStaticEv xev;
    // a follow Static with a moving target is in force: step() left this tick uncertain and resolveLive() finishes it (the half window, the offset and the other reasons for an uncertain camera of this tick)
    bool liveNeed = false, liveOther = false, liveYNeed = false; float liveHalf = 0.0f, liveOff = 0.0f;
    // a touch-triggered Zoom may have fired (the player is, or has been, where it can touch the trigger: CamModel::touchZooms): the zoom is not known until an x-fired Zoom placed past that stretch has finished its tween
    bool zoomUnk = false, zoomClr = false; float zoomHold = -1e30f;
    bool cenYOk = false; float cenY = 0.0f;   // the camera centre Y of this tick, known only where resolveLive placed it (a follow Static past its tween)
    float cenX = -1e30f;     // the previous tick's camera centre (world): left edge + 284.5 / zoom; -1e30 = not known (the previous tick was not certain)
    CamState() { zoom.v = 1.0f; zoomY.v = 1.0f; }
};

// what a tick of the timeline hands to the camera's Y (CamModel::step's optional output)
struct CamYTick {
    float zoom = 1.0f, offY = 0.0f, tw = 1.0f;
    bool staticY = false; int edgeTGrp = 0, edgeBGrp = 0;
    bool yev = false;        // a static trigger took effect on this very tick (CamState::yev describes it)
    // the static tween in force (valid while staticY): the last trigger's keys, whether its target point is known here (the target group's first object, not moved by any trigger of the level), and the tick of the tween (k = 1 on the tick it fired)
    CamState::YStaticEv st; long long k = 0;
    bool unkOff = false;     // a spawn- or touch-triggered camera trigger that changes the Y side (a Camera Offset not Y-free, a Zoom, a Static, a top / bottom Edge, a time warp) may have fired by now: the Offset Y / zoom of the timeline cannot be trusted (--camyband branch B)
    bool unkCam = false;     // a Camera Static trigger fired by a spawn or a touch (a time the model cannot know) may have fired by now: the static point of the timeline cannot be trusted (it may be another trigger's); a spawned zoom or a touched Camera Edge does not move the point
};

class CamModel {
public:
    std::vector<CamTrig> trigs;         // the x-driven camera triggers and Rotate Gameplay triggers, in (x, file) order
    std::vector<std::vector<size_t>> chanArr;   // per trigger channel: the indices into trigs, in the crossing array's order
    bool loaded = false;
    bool useUnknown = true;             // false (a measurement): the touch- / spawn-triggered camera triggers are ignored instead of marking the camera uncertain
    double unknownStaticMinX = 1e18;    // the same for a spawn- or touch-fired Camera Static (1914) trigger only: the earliest x its point may be another trigger's
    double unknownYMinX = 1e18;         // the same for the triggers that change the Y side (CamYTick::unkOff)
    double unknownMinX = 1e18;          // the smallest x from which a camera trigger that changes X may have fired at a time the model cannot know (touch-triggered: when the player touches it; spawn-triggered: when its chain runs): the camera is uncertain from there on
    // A touch-triggered Zoom (1913, key 11) fires when the player's box meets the trigger's, so only while the player is within reach of it along the travel axis: [lo, hi] = the trigger's x -+ (its box at its
    // scale, any rotation, plus the player's). It leaves the zoom at its own value, which the next x-fired Zoom replaces when its tween ends. Kept apart from unknownMinX (which makes the rest of the level
    // unknown) only for a trigger no trigger moves, in a level with one trigger channel.
    struct TouchZoom { double lo, hi; };
    std::vector<TouchZoom> touchZooms;
    std::map<int, float> gfirst;        // group -> the y of its first object
    std::map<int, float> gfirstX;       // group -> the x of its first object
    std::set<int> parented, movedByTrigger;   // diagnosis: why a group is in `moved`
    int groupWhy(int g) const { return g <= 0 ? 4 : !gfirst.count(g) ? 1 : movedByTrigger.count(g) ? 2 : parented.count(g) ? 3 : 0; }   // 0 static, 1 no object, 2 a trigger moves it, 3 a parent group moves it, 4 no target
    std::set<int> toggled;              // groups a Toggle trigger (1049, key 51) names
    std::set<int> moved;                // groups a Move / Rotate / Follow / Scale-like trigger of the level targets (key 51 or 71)
    double levelMaxX = 0.0;             // the game's levelMaxX = the largest object x + 340 (measured on six levels); the level end is a right camera edge at levelMaxX - 569 / zoom with a 60 px blend zone

    // the level string: objects separated by ';', "key,value,key,value"; the first record is the level header
    bool load(const std::string& path) {
        levelMaxX = 0.0; rel.clear(); trigs.clear(); chanArr.clear(); chIdx.clear(); chanOverflow = false; unknownMinX = 1e18; unknownYMinX = 1e18; unknownStaticMinX = 1e18; opaqueActivator = false; actNamed.clear(); touchZooms.clear(); gfirst.clear(); gfirstX.clear(); moved.clear(); toggled.clear(); parented.clear(); movedByTrigger.clear();
        std::ifstream f(path, std::ios::binary);
        if (!f) return false;
        std::stringstream ss; ss << f.rdbuf();
        const std::string raw = ss.str();
        size_t p = raw.find(';');
        int ord = 0;
        while (p != std::string::npos) {
            const size_t q = raw.find(';', p + 1);
            const std::string obj = raw.substr(p + 1, q == std::string::npos ? std::string::npos : q - p - 1);
            ++ord;
            parseObject(obj, ord - 1);
            p = q;
        }
        std::stable_sort(trigs.begin(), trigs.end(), [](const CamTrig& a, const CamTrig& b) { return a.x != b.x ? a.x < b.x : a.ord < b.ord; });
        // (a Toggle naming a camera trigger's group is NOT held against the trigger: S3-0 measured 9,284 ticks it would drop, 99.89% of them exact; `toggled` stays as a diagnosis)
        buildChannels();
        resolveUnknown();
        loaded = true;
        return true;
    }

    // [M] the static camera's stored target (+0x1ec) = the y of the target group's first object (the level string's order) + 90, while no trigger of the level moves, rotates, follows or scales that group (then its live y is not known here)
    bool groupY(int g, float* y) const {
        if (g <= 0) return false;
        const auto it = gfirst.find(g);
        if (it == gfirst.end() || moved.count(g)) return false;
        *y = it->second + 90.0f; return true;
    }
    // the same for the x: a static camera's X point = the x of the target group's first object ([M] dump.csv: the camera's centre is that x at rest, custom level A x=22275 / 39615, custom level B x=11145, custom level C x=30165 within 0.05 px)
    bool groupX(int g, float* x) const {
        if (g <= 0) return false;
        const auto it = gfirstX.find(g);
        if (it == gfirstX.end() || moved.count(g)) return false;
        *x = it->second; return true;
    }
    bool groupYKnownNow(const CamState& s) const { return s.yev.tgtKnown; }   // (diagnosis: whether the static trigger in force has a target the model can place)
    static int substeps(float tw) { return std::max(1, (int)std::floor(4.0f / std::min(tw, 1.0f) + 0.5f)); }

    // one tick: the player is at x; returns the camera's left edge (the zoom of this tick in zoomOut)
    // crossX / crossY: the coordinates the crossing loop compares (trigshadow's step(t, px, py): in a rotated frame the travel coordinate is on y); default x and none -- a vertical channel then cannot be walked (vertUnk)
    float step(CamState& s, double x, float* zoomOut = nullptr, CamYTick* yOut = nullptr, double crossX = std::nan(""), double crossY = std::nan(""), const CamLive* live = nullptr) const {
        s.runMax = std::max(s.runMax, x);
        // a static target the model could not place at the fire (a group a trigger moves) is read ONCE, on the very next tick, from the live shadow; any other call drops the wait (the target stays unknown)
        if (s.yev.liveWait) { if (live && live->ok && live->g == s.yev.tgt && s.n == s.yev.n + 1) { s.yev.tgtY = live->y + 90.0f; s.yev.tgtKnown = true; } s.yev.liveWait = false; }
        bool yev = false;
        const double rx = std::isnan(crossX) ? x : crossX;
        for (const TouchZoom& z : touchZooms) if (x >= z.lo && x <= z.hi) { s.zoomUnk = true; s.zoomClr = false; s.zoomHold = std::max(s.zoomHold, (float)z.hi); }
        // the crossing loop (GJBaseGameLayer::checkSpawnObjects, as trigshadow.hpp's): walk the ACTIVE channel's array from its cursor and fire each trigger the player has reached (a reversed channel: the one it has passed),
        // stopping at the first it has not; a Rotate Gameplay among them switches the channel for the rest of the loop
        for (;;) {
            if (s.active < 0 || (size_t)s.active >= chanArr.size()) break;
            const std::vector<size_t>& arr = chanArr[(size_t)s.active];
            uint32_t& cu = s.cur[(size_t)s.active];
            if (cu >= arr.size()) break;
            const CamTrig& t = trigs[arr[cu]];
            const bool rv = ((s.revMask >> s.active) & 1u) != 0;
            if (s.vertical && std::isnan(crossY)) { s.vertUnk = true; break; }
            const double ref = s.vertical ? crossY : rx, at = s.vertical ? t.y : t.x;
            if (rv ? (at < ref) : (ref < at)) break;
            ++cu;
            if (t.yUnk) s.yUnk = true;
            if (t.id == 2900) { if (t.swarm && t.swch >= 0) { const int di = denseOf(t.swch); if (di >= 0) { s.active = di; if ((unsigned)(t.dir - 2) < 2u) s.revMask |= 1u << di; else s.revMask &= ~(1u << di); } } if (!t.chanOnly) s.vertical = t.dir == 1 || t.dir == 2; continue; }
            if (t.id == 1913) { s.zoom.start(t.to, t.dur, t.ease, t.rate); s.zoomY.start(t.to, t.dur, t.ease, t.rate); if (s.zoomUnk) s.zoomClr = t.x > (double)s.zoomHold; }
            else if (t.id == 1916) { if (t.mode != 2) s.offX.start(t.offX, t.dur, t.ease, t.rate); if (t.mode != 1) s.offY.start(t.offY, t.dur, t.ease, t.rate); }   // (key 101: 1 = Y only, 2 = X only per the model's fit: X moves unless it is 2; Y: the Y-side reading is checked by tools/cam_y_timeline_check.cpp)
            else if (t.id == 1935) s.tw = t.tw;
            else if (t.id == 2062) { if (t.mode == 1 || t.mode == 2) { s.edgeX[t.mode] = t.grp != 0; s.edgeG[t.mode] = t.grp; } else if (t.mode == 3) s.edgeTGrp = t.grp; else if (t.mode == 4) s.edgeBGrp = t.grp; }
            else if (t.id == 1914) {
                if (t.grp != 0) { if (s.staticX) s.exitUntil = s.n + (long long)std::ceil((double)t.dur * 60.0 * (double)substeps(s.tw)) + 2; s.staticX = false; if (t.mode != 1) s.staticY = false; }   // [R] FUN_140349410 (the exit) takes the axis key like the start: key 101 = 1 leaves Y alone (the Y side only; X is as before)
                else {
                    if (t.mode != 2) {   // key 101: 1 = X only moves, 2 = Y only, 0 = both (the model's fit: X static unless 2)
                        s.staticX = true;
                        // the X tween ([R] the same as the Y's): known when the target group is a still object, the camera was certain the tick before (its centre is the tween's start), no time warp and no bezier
                        s.xev.n = s.n; s.xev.tgt = t.tgt; s.xev.follow = t.follow; s.xev.bez = t.bez; s.xev.dur = t.dur; s.xev.speed = t.speed; s.xev.ease = t.ease; s.xev.easeRate = t.rate; s.xev.bz = t.bz;
                        float tx = 0.0f; s.xev.tgtKnown = !t.bez && s.tw == 1.0f && s.cenX > -1e29f && groupX(t.tgt, &tx); s.xev.tgtY = tx;
                        s.xev.liveWait = false; s.xev.startC = s.cenX;
                        // a FOLLOW Static (key 212) whose target a trigger moves: the point is read from the live target every tick ([R] updateCamera 761-834: T = the live position of the group in +0x234; t >= 1 -> point = T,
                        // else point += (T - point) * (t - tprev) / (1 - tprev)). [M] a custom level (Static at x=4155, target group 1085 moved by nine Moves): from the tween's end on the camera centre is the
                        // target's (x, y) of the same tick to 0.000 px on 1,085 ticks. The tween itself needs the start (the centre of the tick before, when that was certain); a follow speed above 1 smooths, not modelled.
                        s.xev.liveFollow = !s.xev.tgtKnown && t.follow && !t.bez && t.speed == 1.0f && s.tw == 1.0f && t.tgt > 0 && gfirstX.count(t.tgt) && !parented.count(t.tgt);
                        s.xev.startOk = s.cenX > -1e29f; s.xev.cur = s.cenX; s.xev.tprev = 0.0f;
                    }
                    if (t.mode != 1) { s.staticY = true; s.yev.n = s.n; s.yev.tgt = t.tgt; s.yev.follow = t.follow; s.yev.bez = t.bez; s.yev.dur = t.dur; s.yev.speed = t.speed; s.yev.ease = t.ease; s.yev.easeRate = t.rate; s.yev.bz = t.bz; s.yev.tgtKnown = groupY(t.tgt, &s.yev.tgtY); s.yev.liveWait = !s.yev.tgtKnown && t.tgt > 0 && gfirst.count(t.tgt) && !parented.count(t.tgt); s.yev.liveFollow = !s.yev.tgtKnown && t.follow && !t.bez && t.speed == 1.0f && s.tw == 1.0f && t.tgt > 0 && gfirst.count(t.tgt) && !parented.count(t.tgt); yev = true; }
                }
            }
        }
        const int N = substeps(s.tw);
        const float sec = 1.0f / (60.0f * (float)N);
        const float z = s.zoom.step(sec), ox = s.offX.step(sec);
        // [M] the Y side's copies of the tweens do not step on the very first call: the game's first tick takes the triggers at x 0 but its tweens first move on the second (the dump of custom level A / custom level B: zoom 38889 -> 100% of the ticks)
        float zy = s.zoomY.v, oy = s.offY.v; if (s.n > 0) { zy = s.zoomY.step(sec); oy = s.offY.step(sec); }
        if (s.zoomUnk && s.zoomClr && !s.zoom.on && !s.zoomY.on) { s.zoomUnk = false; s.zoomClr = false; }   // the x-fired Zoom past the touch stretch has ended: the zoom is its value whatever it started from
        if (zoomOut) *zoomOut = z;
        if (yOut) { yOut->zoom = zy; yOut->offY = oy; yOut->tw = s.tw; yOut->staticY = s.staticY; yOut->edgeTGrp = s.edgeTGrp; yOut->edgeBGrp = s.edgeBGrp; yOut->yev = yev; yOut->st = s.yev; yOut->k = s.n - s.yev.n + 1; yOut->unkCam = unknownStaticMinX <= s.runMax || s.vertUnk || s.yUnk; yOut->unkOff = (useUnknown && (unknownYMinX <= s.runMax || s.zoomUnk)) || s.vertUnk; }
        const float xf = (float)x;
        const float base = (xf - 284.5f / z) + 75.0f / z;
        float cand = base + ox;
        // a Static Camera that moves X, its point known: the camera's left edge is the tween's point less half the window ([M] the same 0.05 px match as the Y side; the offset still adds, as the follow's does)
        bool staticXKnown = false;
        if (s.staticX && s.xev.tgtKnown && s.tw == 1.0f) {
            CamYTween xTween;
            xTween.follow = s.xev.follow; xTween.bezier = false; xTween.dur = s.xev.dur; xTween.speed = s.xev.speed; xTween.ease = s.xev.ease; xTween.easeRate = s.xev.easeRate;
            float rr = 1.0f;
            const float P = camYStaticPointAt(xTween, s.xev.startC, s.n - s.xev.n + 1, s.xev.tgtY, &rr);
            cand = P - 284.5f / z + ox; staticXKnown = true;
        }
        s.liveNeed = s.staticX && !staticXKnown && s.xev.liveFollow && s.tw == 1.0f; s.liveHalf = 284.5f / z; s.liveOff = ox;
        s.liveYNeed = s.staticY && s.yev.liveFollow && s.tw == 1.0f; s.cenYOk = false;
        // A left / right Camera Edge. A LEFT edge holds the camera's left at the limit object's x: [R] updateCamera (2.2081 base+0x23bcf0, the X pass): with the edge set (the sentinel -99999 = none) a left
        // that falls short of the limit is replaced by it; [M] dump.csv a custom level (left edge at x 2940 set at x 3015): 81 ticks held by the limit and the 20 after it within 0.05 px of the dump's camx.
        // A RIGHT edge's clamp is not this: within 60 px of the limit the game blends the camera ([R] the same pass: a smoothing value pulled toward 24 by (1 - distance / 60)); [M] dump.csv a custom level (right
        // edge at x 15510, set at x 14655): from x 15152 the model's clamp is 45 px off the game's camx. So a right edge leaves the camera uncertain from 60 px before its limit on, and the limit is the
        // x of the group's first object while no trigger moves that group (groupX); a limit the model cannot place leaves the camera uncertain whichever side.
        bool edgeUnk = false;
        for (int m = 1; m <= 2; ++m) {
            if (!s.edgeX[m]) continue;
            float ex = 0.0f;
            if (!groupX(s.edgeG[m], &ex)) { edgeUnk = true; continue; }
            if (m == 1) { if (cand < ex) cand = ex; }
            else if (cand > ex - 569.0f / z - 60.0f) edgeUnk = true;
        }
        ++s.n;
        const bool endZone = levelMaxX > 0.0 && (double)cand > levelMaxX - 569.0 / (double)z - 60.0;
        if (s.lastX > -1e17 && std::fabs(x - s.lastX) > 20.0) s.tpUntil = s.n + 400;
        s.lastX = x;
        const bool behind = x < s.runMax - 1.0;
        const bool unk = useUnknown && (unknownMinX <= s.runMax || s.zoomUnk);
        s.liveOther = edgeUnk || s.edgeX[1] || s.edgeX[2] || s.n <= s.exitUntil || behind || s.n <= s.tpUntil || unk || s.vertUnk;
        s.uncertain = (s.staticX && !staticXKnown) || edgeUnk || s.n <= s.exitUntil || endZone || behind || s.n <= s.tpUntil || unk || s.vertUnk;
        const float leftOut = std::max(15.0f, cand);
        s.cenX = s.uncertain ? -1e30f : leftOut + 284.5f / z;   // the next static trigger starts its tween from this centre (only when this tick was certain)
        return leftOut;
    }
    // The second half of a tick under a follow Static whose target a trigger moves (liveNeed / liveYNeed after step()): the game's camera of tick t reads the target as tick t's movement left it, so the caller steps
    // the shadow first and hands the target here. X: the point is the target once the tween value is 1, before that the incremental tween from the centre of the tick before; the left edge is returned and the
    // tick is certain unless another reason holds (liveOther). Y: the centre is the target (+90) once the tween value is 1; during the tween it is not placed (its start is not carried on the Y side).
    // Returns false when nothing was placed on X (the tick stays uncertain).
    bool resolveLive(CamState& s, const CamLive& live, float* leftOut) const {
        const bool needX = s.liveNeed, needY = s.liveYNeed;
        s.liveNeed = false; s.liveYNeed = false;
        bool placed = false;
        if (needX) {
            const bool have = live.okPos && live.pg == s.xev.tgt;
            const double D = (double)s.xev.dur * 240.0;
            const float tt = D <= 0.0 ? 1.0f : gdtrig::eased((float)std::min(1.0, (double)(s.n - s.xev.n) / D), s.xev.ease, s.xev.easeRate);   // (s.n is already the next tick's: k = n - fire)
            float P = 0.0f; bool known = false;
            if (!have) s.xev.startOk = false;   // a tick without the target breaks the incremental tween (the point is the target again once t >= 1)
            else if (tt >= 1.0f) { P = live.px; known = true; }
            else if (s.xev.startOk) {
                if (tt != s.xev.tprev) s.xev.cur = (live.px - s.xev.cur) / ((1.0f - s.xev.tprev) / (tt - s.xev.tprev)) + s.xev.cur;
                s.xev.tprev = tt; P = s.xev.cur; known = true;
            }
            if (known) {
                const float cand = P - s.liveHalf + s.liveOff, left = std::max(15.0f, cand);
                const bool endZone = levelMaxX > 0.0 && (double)cand > levelMaxX - 2.0 * (double)s.liveHalf - 60.0;
                s.uncertain = s.liveOther || endZone;
                s.cenX = s.uncertain ? -1e30f : left + s.liveHalf;
                if (leftOut) *leftOut = left;
                placed = !s.uncertain;
            }
        }
        if (needY) {
            const bool have = live.okPos && live.pg == s.yev.tgt;
            const double D = (double)s.yev.dur * 240.0;
            const float tt = D <= 0.0 ? 1.0f : gdtrig::eased((float)std::min(1.0, (double)(s.n - s.yev.n) / D), s.yev.ease, s.yev.easeRate);
            if (have && tt >= 1.0f && !s.yUnk && !s.vertUnk && !(useUnknown && (unknownYMinX <= s.runMax || s.zoomUnk))) { s.cenY = live.py + 90.0f; s.cenYOk = true; }
        }
        return placed;
    }

private:
    // LevelTools::sortChannelOrderObjects as trigshadow.hpp has it: each channel's array is sorted by the trigger's order, then (int)(order + the axis coordinate) -- the axis the direction of the FIRST Rotate Gameplay that switches to the
    // channel gives (1: y, 2: -y, 3: -x, else x) -- then file order
    void buildChannels() {
        chIdx.clear();
        auto dense = [&](int ch) { if (ch < 0) return -1; const auto it = chIdx.find(ch); if (it != chIdx.end()) return it->second; const int d = (int)chIdx.size(); chIdx[ch] = d; return d; };
        dense(0);
        for (const CamTrig& t : trigs) { dense(t.chan); if (t.id == 2900 && t.swarm) dense(t.swch); }
        const int nCh = (int)chIdx.size();
        chanArr.assign((size_t)std::max(1, std::min(nCh, CamState::kMaxChan)), std::vector<size_t>());
        if (nCh > CamState::kMaxChan) { chanOverflow = true; return; }   // too many channels to carry a cursor each: resolveUnknown() makes the camera unknown from x 0
        std::vector<int> dir((size_t)nCh, 4); std::vector<char> have((size_t)nCh, 0);
        std::vector<size_t> byFile(trigs.size()); for (size_t i = 0; i < trigs.size(); ++i) byFile[i] = i;
        std::stable_sort(byFile.begin(), byFile.end(), [&](size_t a, size_t b) { return trigs[a].ord < trigs[b].ord; });
        for (size_t i : byFile) { const CamTrig& t = trigs[i]; if (t.id == 2900 && t.swarm && t.swch >= 0) { const int di = chIdx[t.swch]; if (!have[(size_t)di]) { have[(size_t)di] = 1; dir[(size_t)di] = t.dir; } } }
        for (size_t i = 0; i < trigs.size(); ++i) { const int di = chIdx[trigs[i].chan]; chanArr[(size_t)di].push_back(i); }
        for (int di = 0; di < nCh; ++di) {
            const int d = dir[(size_t)di];
            auto axis = [d](const CamTrig& t) { return d == 1 ? t.y : d == 2 ? -t.y : d == 3 ? -t.x : t.x; };
            std::sort(chanArr[(size_t)di].begin(), chanArr[(size_t)di].end(), [&](size_t a, size_t b) {
                const CamTrig &x = trigs[a], &z = trigs[b];
                if (x.order != z.order) return x.order < z.order;
                const int ka = (int)((float)x.order + (float)axis(x)), kb = (int)((float)z.order + (float)axis(z));
                return ka != kb ? ka < kb : x.ord < z.ord;
            });
        }
    }
    std::map<int, int> chIdx;   // trigger channel number -> dense index (channel 0 is 0)
    bool chanOverflow = false;
    int denseOf(int ch) const { const auto it = chIdx.find(ch); return it == chIdx.end() ? -1 : it->second; }
    struct Rel { double x = 0.0; bool spawn = false; int target = 0; bool camera = false, isStatic = false, onDeath = false; std::vector<int> groups; double fx = 1e18; bool camY = false; bool tz = false; double reach = 0.0; };
    std::set<int> actNamed;         // groups named by an activator the model does not time (Count, Instant Count, Collision, Pickup, Random, Event, Item Compare, State)
    bool opaqueActivator = false;   // a trigger that names its groups in a list the model does not read (Advanced Random 2068, Sequence 3607): a spawn-triggered trigger nothing else names may still be one of them
    std::vector<Rel> rel;
    static std::vector<int> groupsOf(const std::vector<std::pair<int, std::string>>& kv) {
        std::vector<int> g;
        for (const auto& e : kv) if (e.first == 57) { size_t p = 0; while (p < e.second.size()) { size_t q = e.second.find('.', p); if (q == std::string::npos) q = e.second.size(); if (q > p) g.push_back(std::atoi(e.second.substr(p, q - p).c_str())); p = q + 1; } }
        return g;
    }
    // the earliest x a spawn-triggered trigger can fire from: the smallest firing x of a Spawn trigger that targets one of its groups (itself x-driven or spawned in turn); no spawner found: from the start of the level
    void resolveUnknown() {
        for (Rel& r : rel) r.fx = r.spawn ? 1e18 : r.x;
        for (int it = 0; it < 60; ++it) {
            bool ch = false;
            for (Rel& r : rel) {
                if (!r.spawn) continue;
                double best = 1e18; bool any = false, sawDeath = false;
                for (const Rel& s : rel) {
                    if (s.camera || s.target == 0) continue;
                    if (std::find(r.groups.begin(), r.groups.end(), s.target) != r.groups.end()) { if (s.onDeath) sawDeath = true; else { any = true; best = std::min(best, s.fx); } }
                }
                for (int g : r.groups) if (actNamed.count(g)) sawDeath = false;   // some other activator names it too: unknown
                // [R] 2.209b: an On Death trigger (1812) only REGISTERS its group action when it is triggered (EffectGameObject::triggerObject case 0x714 pushes a 0x30-byte record on the effect manager's vector, +0x260 / +0x268);
                // PlayLayer::destroyPlayer is what runs the records, at the death; the next attempt starts from GJBaseGameLayer::resetCamera -> resetStaticCamera(1). So a spawn-triggered trigger whose only activator is an On Death
                // trigger never fires on a path that stays alive (the model's timeline ends at a death): it is not "from the start of the level" but never. Any other trigger the model cannot read keeps the old answer.
                const double v = any ? best : (sawDeath && !opaqueActivator) ? 1e18 : -1e18;
                if (v < r.fx) { r.fx = v; ch = true; }
            }
            if (!ch) break;
        }
        touchZooms.clear();
        auto bounded = [&](const Rel& r) {   // a touch Zoom the model bounds (touchZooms) instead of giving up the rest of the level
            if (!r.tz || chanOverflow || chanArr.size() != 1) return false;
            for (int g : r.groups) if (moved.count(g)) return false;
            return true;
        };
        for (const Rel& r : rel) if (bounded(r)) touchZooms.push_back({r.x - r.reach, r.x + r.reach});
        unknownMinX = 1e18;
        for (const Rel& r : rel) if (r.camera && !bounded(r)) unknownMinX = std::min(unknownMinX, r.fx);
        unknownYMinX = 1e18;
        for (const Rel& r : rel) if (r.camY && !bounded(r)) unknownYMinX = std::min(unknownYMinX, r.fx);
        unknownStaticMinX = 1e18;
        for (const Rel& r : rel) if (r.isStatic) unknownStaticMinX = std::min(unknownStaticMinX, r.fx);
        if (chanOverflow) { unknownMinX = -1e18; unknownYMinX = -1e18; unknownStaticMinX = -1e18; }   // more trigger channels than a cursor each: no trigger of the level can be placed
    }
    static std::string parentsOf(const std::vector<std::pair<int, std::string>>& kv) {
        for (const auto& e : kv) if (e.first == 274 && !e.second.empty() && e.second != "0") return e.second;
        return std::string();
    }
    static float fkey(const std::vector<std::pair<int, std::string>>& kv, int key, float d) {
        for (const auto& e : kv) if (e.first == key) { char* end = nullptr; const double v = std::strtod(e.second.c_str(), &end); return end == e.second.c_str() ? d : (float)v; }
        return d;
    }
    void parseObject(const std::string& obj, int ord) {
        std::vector<std::pair<int, std::string>> kv;
        size_t p = 0;
        while (p <= obj.size()) {
            size_t a = obj.find(',', p); if (a == std::string::npos) break;
            size_t b = obj.find(',', a + 1); if (b == std::string::npos) b = obj.size();
            kv.push_back({std::atoi(obj.substr(p, a - p).c_str()), obj.substr(a + 1, b - a - 1)});
            p = b + 1;
        }
        const int id = (int)fkey(kv, 1, 0.0f);
        if (id > 0) levelMaxX = std::max(levelMaxX, (double)fkey(kv, 2, 0.0f) + 340.0);
        for (int g : groupsOf(kv)) if (!gfirst.count(g)) { gfirst[g] = fkey(kv, 3, 0.0f); gfirstX[g] = fkey(kv, 2, 0.0f); if (!parentsOf(kv).empty()) { moved.insert(g); parented.insert(g); } }   // (an object with parent groups (key 274) moves with them: its y is not its own)
        // any trigger that names the group (key 51 target, 71 follow target) and is not known to leave a position alone: Move 901, Rotate 1346, Follow 1347, Follow Player Y 1814, Scale 2067, Advanced Follow 3016, Keyframe 3032 ... (the harmless: Color 899, Pulse 1006, Alpha 1007, Toggle 1049, Spawn 1268, Count 1611, Camera Static 1914, Camera Edge 2062)
        if (id >= 899 && id != 899 && id != 1006 && id != 1007 && id != 1049 && id != 1268 && id != 1611 && id != 1914 && id != 2062 && id != 1913 && id != 1916 && id != 1935) {
            const int a = (int)fkey(kv, 51, 0.0f), b = (int)fkey(kv, 71, 0.0f); if (a > 0) { moved.insert(a); movedByTrigger.insert(a); } if (b > 0) { moved.insert(b); movedByTrigger.insert(b); }
        }
        if (id == 1049) { const int a = (int)fkey(kv, 51, 0.0f); if (a > 0) toggled.insert(a); }
        const bool spawnTrig = fkey(kv, 62, 0.0f) != 0.0f, touchTrig = fkey(kv, 11, 0.0f) != 0.0f;
        if (id == 2068 || id == 3607) opaqueActivator = true;
        if (id == 1812) { Rel r; r.x = (double)fkey(kv, 2, 0.0f); r.spawn = spawnTrig; r.target = (int)fkey(kv, 51, 0.0f); r.camera = false; r.onDeath = true; r.groups = groupsOf(kv); rel.push_back(r); return; }
        if (id == 1611 || id == 1811 || id == 1815 || id == 1817 || id == 1912 || id == 3604 || id == 3620 || id == 3640) {   // other activators the model does not time: the groups they name (keys 51 / 71) keep the old answer below
            for (int key : {51, 71}) { const int g = (int)fkey(kv, key, 0.0f); if (g > 0) actNamed.insert(g); }
            return;
        }
        if (id == 1268) {   // a Spawn trigger: its target group (key 51) and when it can fire
            Rel r; r.x = (double)fkey(kv, 2, 0.0f); r.spawn = spawnTrig; r.target = (int)fkey(kv, 51, 0.0f); r.camera = false; r.groups = groupsOf(kv); rel.push_back(r);
            return;
        }
        if (id != 1913 && id != 1916 && id != 1935 && id != 1914 && id != 2062 && id != 2900) return;
        const double x = (double)fkey(kv, 2, 0.0f);
        const bool xAffects = id == 1913 || id == 1935 || id == 2900 || (id == 1916 && (int)fkey(kv, 101, 0.0f) != 2) || (id == 1914 && ((int)fkey(kv, 101, 0.0f) != 2 || fkey(kv, 110, 0.0f) != 0.0f))
                              || (id == 2062 && ((int)fkey(kv, 164, 0.0f) == 1 || (int)fkey(kv, 164, 0.0f) == 2));
        if (spawnTrig || touchTrig) {   // fires when its chain runs / when the player touches it: a time the model cannot know (a touch one is placed at its own x, a spawned one at its earliest spawner)
            const bool yAffects = id == 1913 || id == 1914 || id == 1935 || id == 2900 || (id == 1916 && (int)fkey(kv, 101, 0.0f) != 1) || (id == 2062 && ((int)fkey(kv, 164, 0.0f) == 3 || (int)fkey(kv, 164, 0.0f) == 4));
            if (xAffects || yAffects || id == 1914) { Rel r; r.x = x; r.spawn = spawnTrig; r.target = 0; r.camera = xAffects; r.camY = yAffects; r.isStatic = id == 1914; r.groups = groupsOf(kv);
                if (id == 1913 && touchTrig && !spawnTrig && (int)fkey(kv, 170, 0.0f) == 0) {   // the reach of its touch: half the diagonal of the 30 x 30 box at the largest of its scales (keys 32 / 128 / 129), the player's half box and one step of travel
                    const float sc = std::max(1.0f, std::max(std::fabs(fkey(kv, 32, 1.0f)), std::max(std::fabs(fkey(kv, 128, 1.0f)), std::fabs(fkey(kv, 129, 1.0f)))));
                    r.tz = true; r.reach = 21.25 * (double)sc + 15.0 + 12.0;
                }
                rel.push_back(r); }
            return;
        }
        CamTrig t; t.id = id; t.ord = ord; t.x = x; t.chan = (int)fkey(kv, 170, 0.0f); t.y = (double)fkey(kv, 3, 0.0f) + 90.0; t.groups = groupsOf(kv); t.yUnk = fkey(kv, 115, 0.0f) != 0.0f;
        if (id == 2900) { t.swarm = fkey(kv, 171, 0.0f) != 0.0f; t.chanOnly = fkey(kv, 172, 0.0f) != 0.0f; t.swch = (int)fkey(kv, 173, 0.0f); t.dir = (int)fkey(kv, 167, 0.0f); trigs.push_back(t); return; }
        t.dur = fkey(kv, 10, 0.0f); t.ease = (int)fkey(kv, 30, 0.0f); t.rate = fkey(kv, 85, 0.0f);
        if (id == 2062) { t.mode = (int)fkey(kv, 164, 0.0f); t.grp = (int)fkey(kv, 51, 0.0f); trigs.push_back(t); return; }
        if (id == 1914) {
            t.mode = (int)fkey(kv, 101, 0.0f); t.grp = fkey(kv, 110, 0.0f) != 0.0f ? 1 : 0;
            t.bz = fkey(kv, 454, 0.0f); t.tgt = (int)fkey(kv, 71, 0.0f); t.follow = fkey(kv, 212, 0.0f) != 0.0f; t.bez = fkey(kv, 453, 0.0f) != 0.0f; t.speed = std::max(1.0f, fkey(kv, 213, 1.0f));
            trigs.push_back(t); return;
        }
        t.dur = fkey(kv, 10, 0.0f); t.ease = (int)fkey(kv, 30, 0.0f); t.rate = fkey(kv, 85, 0.0f);
        if (id == 1913) { float to = fkey(kv, 371, 1.0f); to = to <= 0.0f ? 1.0f : std::min(3.0f, std::max(0.4f, to)); t.to = to; }
        else if (id == 1916) { t.mode = (int)fkey(kv, 101, 0.0f); t.offX = fkey(kv, 28, 0.0f); t.offY = fkey(kv, 29, 0.0f); }
        else if (id == 1935) t.tw = fkey(kv, 120, 1.0f);
        trigs.push_back(t);
    }
};

}  // namespace dp::gdcam
