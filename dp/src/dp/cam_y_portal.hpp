// The static camera that a mode portal starts (Y only), as 22091 produces it. Pure and in float; observation only, like cam_y.hpp / cam_y_static.hpp.
//   [R] portal hit FUN_14031a210 -> GJBaseGameLayer::updateDualGround (0x31a4a0, player, mode, 0) -> the "enter the ground-limited camera" function (0x31a920) -> prepareMoveActions (0x3490a0).
//   [R] getGroundHeightForMode (0x318c30): modes 5, 19, 26, 41 -> 300; mode 16 -> 240; modes 23 / 24 -> the previous real mode (+0x540) judged again; every other mode 270.
//   [R] updateDualGround: dual (+0x422) and h == 240 -> 270, then h = max(h, the other player's h) (its mode: 5 when it is ship / ufo / wave / swing, 16 ball, else 6).
//        Enter when +0x311 (free mode) == 0 and (h != 270 or mode == 0x21 or dual); otherwise leave: +0x310 = 0 and, if +0x2a0 is set, the camera's static state is cleared (+0x23c = 0).
//   [R] enter: bottom = portalY - h/2, floor(bottom / 30) * 30 unless the portal's flag (+0x312, key 370 in the level file) is set; target = max(90, bottom) + h/2; duration 0.5 s, 0.4 s while dual (+0x422)
//        or +0x310 is already set; it starts the tween only when +0x23c == 0 or +0x2a0 is set (so a 1914 static camera is not restarted by a portal).  The tween is ease 1 (EaseInOut, rate 2) on Y.
//   [M] checked against the dump (tools: t2\portalcam2.py, portalcam3.py; 74 levels): target 147 / 159 portal-started events, the tween shape 271 / 312 events (96576 / 101081 ticks), the 0.4 / 0.5 choice 294 / 312.
//        portalY is the level file's y + 90 (world y); a portal that a move trigger has moved needs its live y.  The mode ids of ufo / wave (19 / 26) and ball (16) are fitted from the data, not named in the code.
//   Tween step: dp/cam_y_static.hpp (CamYTween with follow = bezier = false, ease = 1, easeRate = 2, dur = the value here, start = the camera centre when the portal was hit, target = this target).
// NOT self-derived inputs (to be marked at the import): the mode and the portal's live y (from the portal event the state carries), the previous mode, +0x310 (ground-limited already) and +0x2a0 (this static camera was
//   started by a portal), the tween's start point (+0x1dc = the camera centre).
#pragma once
#include <algorithm>
#include <cmath>

namespace dp::gdcam {

inline float groundHeightForMode(int mode, int prevRealMode) {
    for (int guard = 0; guard < 4; ++guard) {
        switch (mode) {
            case 5: case 19: case 26: case 41: return 300.0f;
            case 16: return 240.0f;
            case 23: case 24:
                if (prevRealMode == 23 || prevRealMode == 24) return 270.0f;
                mode = prevRealMode; continue;
            default: return 270.0f;
        }
    }
    return 270.0f;
}

struct CamYPortalIn {
    int mode = 6, prevRealMode = 6;   // portal's mode id (vtable +0x670); +0x540
    bool dual = false;
    int otherMode = 6;                // the other player's mode class (5 / 16 / 6), used when dual
    bool freeMode = false;            // +0x311
    bool limitedBefore = false;       // +0x310
    float portalY = 0.0f;             // world y (file y + 90)
    bool noSnap = false;              // portal key 370
};

struct CamYPortalOut {
    bool enter = false;
    float h = 270.0f, target = 0.0f, dur = 0.5f;
};

inline CamYPortalOut camYPortal(const CamYPortalIn& in) {
    CamYPortalOut o;
    float h = groundHeightForMode(in.mode, in.prevRealMode);
    if (in.dual) {
        if (h == 240.0f) h = 270.0f;
        h = std::max(h, groundHeightForMode(in.otherMode, in.prevRealMode));
    }
    o.h = h;
    o.enter = !in.freeMode && (h != 270.0f || in.mode == 0x21 || in.dual);
    if (!o.enter) return o;
    float bottom = in.portalY - h * 0.5f;
    if (!in.noSnap) bottom = std::floor(bottom / 30.0f) * 30.0f;
    o.target = std::max(90.0f, bottom) + h * 0.5f;
    o.dur = (in.dual || in.limitedBefore) ? 0.4f : 0.5f;
    return o;
}

}  // namespace dp::gdcam
