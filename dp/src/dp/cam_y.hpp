// The camera's Y as GJBaseGameLayer::updateCamera (22091, base+0x345c60) computes it: a port of the y iteration of the function, pure (no state of its own) and in float in the order of the decompile.
// Observation only, like cam_model.hpp (not read by the search yet). Every rule says where it comes from: [R] = read in the decompile (line numbers of dec_updateCamera_c.c in the private lab,
// t2\dis\), [M] = measured in the game, nothing is [F] here. Checked tick by tick against the game's own camera (dumpcamedge dump.csv, gd05cam5/6): see t2\shadowy.py (the same rules in python, 0.9996 on
// 12 levels, 0.9997 on 52 more, 0.9998 on the 22 official ones) and tools/cam_y_check.cpp (this file against the same rows).
//
// Inputs are split by who owns them:
//   per tick, from the player / the search state:  y (player), the mode flags (flier / upside-down / rotated frame), the previous tick's camera Y (CamYState)
//   per timeline (trigger-driven, the same for every state at a tick):  zoom, offset Y, time warp, the static camera's point and rate, the Camera Edge limits, level end, gy1 / gy2 of the band
// NOT self-derived inputs (read from the game's state, to be marked at the import): staticRate (+0x508), post500 (+0x500 timer), rotTween (+0x284), prevCand (+0x1c4, only inside a Camera Edge soft zone).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dp::gdcam {

struct CamYIn {
    // player / state
    float y = 0.0f;               // the player's y at this tick (the value updateCamera reads, before the landing snap: +0xa38 is not it)
    float pointY = 0.0f;          // +0xa34's second float: the player's camera point y (bVar5 asks for 105)
    bool upsideDown = false;      // +0x9c7
    bool rotated = false;         // +0x9cb
    bool flier = false;           // any of +0x9c1 +0x9c2 +0x9c4 +0x9cc +0x9c3 +0x9c6
    bool flierNo9c3_9c6 = true;   // none of +0x9c1 +0x9c2 +0x9c4 +0x9cc (bVar5's set)
    // per timeline
    float zoom = 1.0f, zoomPrev = 1.0f;
    float offY = 0.0f, offYPrev = 0.0f;       // +0x1b4 now / previous tick
    float offApplied = 0.0f, offAppliedPrev = 0.0f;   // +0x1f4 (the static-exit tween's offset this tick) / +0x1fc (what was added last tick)
    float tw = 1.0f;                           // time warp (dt = 0.25 * min(tw, 1))
    float k2cc = 0.0f, rate2d0 = 0.0f;         // +0x2cc / +0x2d0: the flier's margin factor / rate
    bool snap = false;                         // +0x331d or +0x761 or +0x244 != 0
    bool staticOn = false;                     // +0x23c
    float staticY = 0.0f;                      // +0x1e4: the static camera's y point (statself: self-produced)
    float staticStart = 0.0f, staticTarget = 0.0f, mode2ec = 0.0f; bool b310 = false;   // +0x1dc, +0x1ec, +0x2ec, +0x310 byte (decompile 886-899)
    float staticRate = 1.0f;                   // +0x508 when a follow smoothing is on, else 1  (NOT self-derived: c224 * t when +0x22c, see CamYStatic)
    float rotTween = 0.0f;                     // +0x284 (NOT self-derived)
    // Camera Edge / level end
    int edgeT = 0, edgeB = 0; bool xgate = false;   // +0x470 / +0x474 ids, (a right Edge or the level end)
    float edgeTy = -99999.0f, edgeBy = -99999.0f;   // the T / B edge object's y (-99999: unset)
    float defY = 0.0f, top37a8 = 0.0f;
    // carried
    float post500Prev = 0.0f;                  // +0x500 of the previous tick (NOT self-derived)
    float prevCand = 0.0f;                     // +0x1c4 of the previous tick (NOT self-derived; only read in an Edge soft zone)
};

struct CamYState { float camY = 0.0f; };       // +0x414 as of the previous tick (it includes offAppliedPrev)

namespace camy_detail {
inline float clampY(float v, float ymin, float ymax) {
    if (0.0f < ymax && ymax < v) v = ymax;     // [R] 1104-1110, 1139-1145
    if (v < ymin) v = ymin;                    // [R] 1111-1116, 1146-1151
    return v;
}
}

// returns +0x414 of this tick
// candOut (optional): this tick's cand as [R] 1087 stores it into +0x1c4 (after the Edge limits inside the loop, before the clamps of 1104-1116): the next tick's prevCand
inline float camYStep(const CamYState& s, const CamYIn& in, float* candOut = nullptr) {
    using namespace camy_detail;
    const float H = 320.0f;                    // [R] 279
    float cam = s.camY - in.offAppliedPrev;
    cam = cam + (in.offY - in.offYPrev);       // [M] a change of the Y offset moves the kept camera 1:1
    const float z = in.zoom, zp = in.zoomPrev, P = in.y, off = in.offY;
    const float Hz = H / z, half = Hz * 0.5f;
    // bVar5 [R] 305-319
    const bool b5 = !in.staticOn && !in.rotated && !in.upsideDown && in.pointY == 105.0f && zp <= z && z != zp && in.flierNo9c3_9c6 && P <= Hz - 90.0f / z;
    if (z != zp && !b5) cam = cam + (H / zp * 0.5f - H / z * 0.5f);   // zoom compensation [R] 324-328
    float cand, rate;
    if (!in.staticOn && b5) { cand = off + 0.0f; rate = 1.0f; }
    else if (in.staticOn) {
        float v = in.staticY;                   // [R] 879-880, 901, 918-921
        if (in.b310) {                          // [R] 886-899
            const float tg = in.staticTarget, mn = (in.mode2ec * 0.5f) / z + 90.0f;
            if (tg < mn) {
                const float pg = tg == in.staticStart ? 0.0f : (tg - v) / (tg - in.staticStart);
                v = (mn - tg) * (1.0f - pg) + v;
            }
        }
        cand = v - half + off;
        rate = in.staticRate > 1.0f ? in.staticRate : 1.0f;   // [R] 835-851, 1126
    } else if (in.rotated) {
        cand = (P - half) + in.rotTween + off;  // [R] 587-590, 706-713, 321-323
        rate = 1.0f;
    } else {
        float top, bot;
        if (in.flier) {
            const float m = (160.0f - ((158.0f - 30.0f) * in.k2cc + 30.0f)) / z;   // [R] 280-284; the division by the zoom is [M]
            top = bot = m; rate = in.rate2d0;                                        // [R] 396-399
        } else {
            top = 70.0f; bot = 40.0f; rate = 10.0f;                                   // [R] 275-278, 303
            if (in.upsideDown) std::swap(top, bot);                                   // [R] 293-296
        }
        if (off + P <= cam + half + top) {
            if (off + P < cam + half - bot) cand = (P - half) + bot + off; else cand = cam;
        } else cand = (P - half) - top + off;
    }
    if (!b5) {
        if (in.snap) rate = 1.0f;                                                    // [R] 1095
        else if (in.post500Prev > 0.0f) rate = 30.0f;                                // [R] 1088-1092
    } else rate = 10.0f;
    // limits [R] 140-173
    const float defy = in.defY;
    float ymax, ymin;
    if (in.edgeT > 0) { float ty = in.edgeTy; if (ty == -99999.0f) ty = defy; ymax = ty - Hz; }
    else ymax = in.top37a8 - 150.0f - Hz;
    if (in.edgeB > 0) { float by = in.edgeBy; if (by == -99999.0f) by = defy; ymin = by >= 0.0f ? by : 0.0f; }
    else ymin = defy >= 0.0f ? defy : 0.0f;
    const float dt = 0.25f * std::min(in.tw, 1.0f);                                  // [M] only tw < 1 scales it
    const bool staticY = in.staticOn;
    const float pc = in.prevCand;
    if (!b5) {
        if (in.xgate) {                                                              // [R] 931, 938-1007
            if (cand > ymax) cand = ymax;
            if (in.edgeT > 0 && !(cand <= ymax - 60.0f)) {
                const bool skip = cand < pc && cand < cam;
                if (!skip && !(24.0f <= rate) && !(rate == 1.0f && staticY)) rate = (1.0f - (ymax - cand) / 60.0f) * (24.0f - rate) + rate;
            }
        }
        if (ymin > cand) cand = ymin;                                                // [R] 1009-1082
        if (in.edgeB > 0 && !(ymin + 60.0f <= cand)) {
            const bool skip = pc < cand && cam < cand;
            if (!skip && !(24.0f <= rate) && !(rate == 1.0f && staticY)) rate = (1.0f - (cand - ymin) / 60.0f) * (24.0f - rate) + rate;
        }
    }
    if (candOut) *candOut = cand;                                                  // [R] 1087
    cand = clampY(cand, ymin, ymax);
    float out;
    if (rate > 1.0f) { const float d = dt > 0.0f ? rate / dt : rate; out = (cand - cam) / d + cam; }   // [R] 1126-1131
    else out = cand;
    out = clampY(out, ymin, ymax);                                                   // [R] 1139-1150
    return clampY(out + in.offApplied, ymin, ymax);                                  // [R] 1349-1366
}

}  // namespace dp::gdcam
