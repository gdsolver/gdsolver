// The static camera's Y point (+0x1e4) and rate as updateCamera / FUN_140348d60 (22091) produce them while a Camera Static trigger (1914) tween runs. Pure and in float; observation only, like cam_y.hpp.
//   [R] FUN_140348d60: a follow (+0x22c) or bezier (+0x264) trigger starts the tween value +0x214 (t) from 0 (t = 1 at once when the duration is 0); +0x224 = max(1, key 213). A plain one is moved by a move action.
//   [R] updateCamera 761-834: t >= 1 -> point = target; bezier -> B(t) with P0 = start (+0x1dc), P1 = P0 + duration * tangent (+0x20c * +0x204) / 3, P2 = (P1 - T) * +0x26c + T, P3 = T; else the incremental
//        point += (T - point) / ((1 - tprev) / (t - tprev)); T = the live y of the group in +0x234 when following (+90 included in the object's position), else the stored target +0x1ec.
//   [R] 835-851: the rate of the follow smoothing = +0x224 * t when that is >= 1, else 1.
//   [M] t = ease(kind, k / D), D = duration * 240 (not rounded), k = ticks since the trigger fired (the first tick of the tween is k = 1); the plain point = start + (target - start) * t.
// NOT self-derived inputs (to be marked at the import): the start point (+0x1dc = the previous tick's camera centre), the bezier's start tangent (+0x204 = the point's speed when the trigger fired), +0x26c, the live
// group y (the timeline's moving group), and the trigger itself (its fire tick and keys 10 / 30 / 85 / 71 / 212 / 213 / 453): all four come from the timeline in the dp.
// Checked against the dump: tools/cam_y_static_check.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include "dp/trigshadow.hpp"   // gdtrig::eased

namespace dp::gdcam {

struct CamYTween {
    bool follow = false, bezier = false;
    float dur = 0.0f;          // key 10 (s) = +0x20c
    float speed = 1.0f;        // +0x224 = max(1, key 213)
    float tangent = 0.0f;      // +0x204 (bezier)
    float bz = 0.0f;           // +0x26c (bezier)
    float target = 0.0f;       // +0x1ec (the stored point; the live group y when following)
    int ease = 0; float easeRate = 2.0f;   // keys 30 / 85
};

struct CamYStatic {
    bool on = false;
    CamYTween tw;
    long long k = 0;
    float tprev = 0.0f, start = 0.0f, cur = 0.0f;
    void fire(const CamYTween& t, float startPoint) { tw = t; k = 0; tprev = 0.0f; start = startPoint; cur = startPoint; on = true; }
    // one tick; liveY = the y of the follow group (ignored unless following); returns +0x1e4, rate = the lerp rate the static branch hands to the camera
    float step(float liveY, float* rate) {
        ++k;
        const double D = (double)tw.dur * 240.0;
        const float t = D <= 0.0 ? 1.0f : gdtrig::eased((float)std::min(1.0, (double)k / D), tw.ease, tw.easeRate);
        const float T = tw.follow ? liveY : tw.target;
        float nw;
        if (t >= 1.0f) nw = T;
        else if (tw.bezier) {
            const float u = 1.0f - t, p0 = start;
            const float p1 = tw.dur * tw.tangent / 3.0f + p0;
            const float p2 = (p1 - T) * tw.bz + T;
            nw = p1 * 3.0f * t * u * u + u * p0 * u * u + p2 * 3.0f * t * t * u + t * T * t * t;
        } else if (tw.follow) {
            nw = t != tprev ? (T - cur) / ((1.0f - tprev) / (t - tprev)) + cur : cur;
            tprev = t;
        } else nw = start + (T - start) * t;
        cur = nw;
        float r = 1.0f;
        if (tw.follow && t * tw.speed >= 1.0f) r = tw.speed * t;
        if (rate) *rate = r;
        return nw;
    }
};

// The point of the tween at its tick k (k = 1 on the tick the trigger fired), in closed form: what the incremental CamYStatic::step gives for a plain or a bezier tween, and for a follow tween whose target does not move
// (the incremental follow update point += (T - point) * (t - tprev) / (1 - tprev) from point = start + (T - start) * tprev gives start + (T - start) * t: [R] by induction, the float rounding differs at ~1e-5).
// start = the previous tick's camera centre ([M] 7/7), T = the target group's first object's y + 90 ([M] 20/20), bezier tangent = the camera's speed one tick before the fire, (c414[t-1] - c414[t-2]) * 240 ([M] 8/8).
inline float camYStaticPointAt(const CamYTween& tw, float start, long long k, float T, float* rate) {
    const double D = (double)tw.dur * 240.0;
    const float t = D <= 0.0 ? 1.0f : gdtrig::eased((float)std::min(1.0, (double)k / D), tw.ease, tw.easeRate);
    float nw;
    if (t >= 1.0f) nw = T;
    else if (tw.bezier) {
        const float u = 1.0f - t, p0 = start;
        const float p1 = tw.dur * tw.tangent / 3.0f + p0;
        const float p2 = (p1 - T) * tw.bz + T;
        nw = p1 * 3.0f * t * u * u + u * p0 * u * u + p2 * 3.0f * t * t * u + t * T * t * t;
    } else nw = start + (T - start) * t;
    if (rate) *rate = (tw.follow && t * tw.speed >= 1.0f) ? tw.speed * t : 1.0f;
    return nw;
}

}  // namespace dp::gdcam
