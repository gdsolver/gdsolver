// The layer words the self-running shadow (selfshadow.hpp) reads, worked out by the shadow instead of copied from GD (S2 stage 2 preparation).
// Only tools include this (tools/self_test.cpp with ST_OWNLAYER=1); leveldp does not.
//
// What is ported (2.209b / 22091 addresses):
//   - GJBaseGameLayer::playerWillSwitchMode (FUN_14031a210): every mode portal copies its Free Mode byte (+0x6e4 -> layer +0x311) and its
//     +0x6f0 (-> +0x312, the band centre's grid snap off) into the layer, records itself as the last mode portal (+0x400), then calls
//   - GJBaseGameLayer::updateDualGround 0x31a4a0: +0x540 := the mode (not for a dual portal); H := getGroundHeightForMode 0x318c30 (300 ship / ufo /
//     wave / swing, 240 ball, the last mode's for a dual portal, 270 else); in a dual H is the larger of the two players' (not ported: player 2);
//     if !freeMode && (H != 270 || spider || dual): animateInDualGroundNew 0x31a690, else the band is retracted (+0x310 := 0, values kept)
//   - animateInDualGroundNew 0x31a690: centre +0x308 := max(90, floor((y - H/2) / 30) * 30) + H/2 (no floor when +0x312), y = the portal's start
//     position (+0x400, or +0x408 in a dual); +0x2ec := H; +0x310 := 1; +0x2a0 := 1 only when branch A was not open, so a band portal never changes the branch: only the Static Camera trigger does (+0x23c)
//   - GJBaseGameLayer::toggleDualMode 0x31de10: +0x422 := on / off; off: Free Mode / +0x312 from the portal, updateDualGround(player 1's mode)
//   - getMinPortalY 0x31a980 / getMaxPortalY 0x31aa60, branch B (+0x23c == 0 or +0x2a0 != 0):
//       min = max(90, (+0x308 - (+0x2ec * 0.5) / zoom) + camOffY), max = min + +0x2ec / zoom   (zoom = +0x1a8, camOffY = +0x1b4, float)
//   - GJBaseGameLayer::update 0x3406d0: at the end of every step +0x764 := getMaxPortalY()
// Inputs it takes per step (not ported: they need the trigger or the camera shadow): the zoom +0x1a8 / +0x1ac (Zoom trigger, a tween),
// camOffY +0x1b4 (Camera Offset trigger, a tween), the static camera's +0x23c (Static Camera trigger; branch A reads the ground sprites
// and the camera instead), and the Camera Mode trigger's Free Mode write. A portal's +0x6f0 (-> +0x312) is not in objrects.txt: taken as 0 (it is 0 in
// every capture). The mirror portal's tween of +0x41c is ported below (toggleFlipped).
#pragma once
#include "dp/collide.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace dp {
namespace gdcol {

struct ShadowLayer {
    // the layer's own words the shadow carries
    bool freeMode = false;     // +0x311
    bool noSnap = false;       // +0x312
    bool dual = false;         // +0x422
    bool bandOn = false;       // +0x310 (the band is animated in)
    bool animEvent = false, animIn = false;   // this call's updateDualGround ran: the ground band's tween restarted (in / out); not carried between calls (State::bandAnim is the old model's clock of it, P9 band family)
    float centre = 0.0f;       // +0x308
    float height = 0.0f;       // +0x2ec
    bool heightKnown = false;  // the seed's +0x2ec is not in the capture: known once inferred from the seed's band, or set by a portal
    int lastMode = -1;         // +0x540
    int lastPortalUid = -1;    // +0x400 (its start y: lastPortalY)
    float lastPortalY = 0.0f;
    bool lastPortalKnown = false;
    float w764 = 0.0f;         // +0x764
    bool w764Known = false;

    static float heightFor(int type, int lastMode) {
        switch (type) {
        case 5: case 0x13: case 0x1a: case 0x29: return 300.0f;
        case 0x10: return 240.0f;
        case 0x17: case 0x18:
            if (lastMode == 0x17 || lastMode == 0x18 || lastMode < 0) return 270.0f;
            return heightFor(lastMode, -1);
        default: return 270.0f;
        }
    }
    // branch B of getMinPortalY / getMaxPortalY
    static void bandB(float centre, float H, float zoom, float camOffY, float& mn, float& mx) {
        float v = (centre - (H * 0.5f) / zoom) + camOffY;
        mn = 90.0f;
        if (90.0f <= v) mn = v;
        mx = mn + H / zoom;
    }
    // the seed: the layer bytes the capture has at the first pass, and the band GD returned there (branch B only: H is the value of 0, 240, 270, 300
    // that gives the same two floats; branch A or no such value leaves H unknown until a portal sets it)
    void seed(bool fm, bool ns, bool du, bool on, float c308, float w, float zoom, float camOffY, bool branchA, float gdMin, float gdMax) {
        freeMode = fm; noSnap = ns; dual = du; bandOn = on; centre = c308; w764 = w; w764Known = true;
        heightKnown = false;
        if (!branchA) {
            for (float H : {300.0f, 270.0f, 240.0f, 0.0f}) {
                float a, b;
                bandB(c308, H, zoom, camOffY, a, b);
                if (std::memcmp(&a, &gdMin, 4) == 0 && std::memcmp(&b, &gdMax, 4) == 0) { height = H; heightKnown = true; break; }
            }
        }
    }
    void animateIn(float H, float y) {
        float v = y - H * 0.5f;
        if (!noSnap) v = std::floor(v / 30.0f) * 30.0f;
        float lo = 90.0f;
        if (90.0f <= v) lo = v;
        centre = lo + H * 0.5f;
        height = H; heightKnown = true;
        bandOn = true;
    }
    // updateDualGround for player 1. `y` is the start y of the portal animateInDualGroundNew reads (+0x400), `yKnown` false when that portal is not known
    // (a dual-off portal before the window saw any mode portal). In a dual the second player's mode decides too: not ported (heightKnown := false).
    void updateDualGround(int type, float y, bool yKnown) {
        if (type != 0x17 && type != 0x18) lastMode = type;
        const float H = heightFor(type, lastMode);
        if (!freeMode && (H != 270.0f || type == 0x21 || dual)) {
            animEvent = true; animIn = true;
            if (dual || !yKnown) { heightKnown = false; bandOn = true; return; }
            animateIn(H, y);
        } else {
            animEvent = true; animIn = false;
            bandOn = false;
        }
    }
    // a mode portal of player 1 (collisionCheckObjects cases 5, 6, 0x10, 0x13, 0x1a, 0x1b, 0x21, 0x29 on a fresh contact): playerWillSwitchMode
    void modePortal(int type, int uid, float startY, bool free, bool noSnapByte) {
        freeMode = free; noSnap = noSnapByte;
        lastPortalUid = uid; lastPortalY = startY; lastPortalKnown = true;
        updateDualGround(type, startY, true);
    }
    // a dual portal (0x17 on / 0x18 off): toggleDualMode
    void dualPortal(bool on, float startY, bool free, bool noSnapByte, int p1ModeType) {
        if (dual == on) return;
        dual = on;
        if (on) {   // playerWillSwitchMode with the dual portal: the band then depends on player 2 (not ported)
            freeMode = free; noSnap = noSnapByte;
            heightKnown = false;
            (void)startY;
            return;
        }
        freeMode = free; noSnap = noSnapByte;
        updateDualGround(p1ModeType, lastPortalY, lastPortalKnown);   // +0x408 is cleared: animateInDualGroundNew reads the last mode portal (+0x400)
    }
    // A Zoom trigger's fire (GJBaseGameLayer::updateZoom, 2.209b base+0x33eac0): after it sets the zoom (+0x1a8 at once, or a tween) it runs updateDualGround(player 1, a type read from player 1's CURRENT mode: ship /
    // ufo / wave / swing 5, ball 0x10, spider 0x21, cube / robot 6, instant = (duration == 0)) when the band is animated in (+0x310) and +0x331d is 0 (a byte not carried here, taken as 0). updateDualGround then
    // writes the band again (animateInDualGroundNew: the current mode's H, the centre from the last mode portal's y) or, for a 270-high mode, only tweens the ground band out. [M] a custom level t=4799 / 4800: the
    // zoom 0.4 -> 0.6 tick moved the band's centre 3360 -> 3330 and its height 500 = 300 / 0.6.
    void zoomTrigger(int p1ModeType) {
        if (!bandOn) return;
        updateDualGround(p1ModeType, lastPortalY, lastPortalKnown);
    }
    // A Camera Mode trigger's fire (id 2925, CameraTriggerGameObject::triggerObject, 2.209b base+0x65e8c0..): +0x311 := the trigger's Free Mode (property 111); a CHANGED byte runs updateDualGround(+0x540 = the last
    // type, ..., 0). (+0x312 := property 370 and the padding / ease words are not carried: noSnap is left as it was.) Nothing when the last type is not known (lastMode < 0): the flag is set, the band left.
    void freeModeTrigger(bool free) {
        const bool changed = freeMode != free;
        freeMode = free;
        if (changed && lastMode >= 0) updateDualGround(lastMode, lastPortalY, lastPortalKnown);
    }
    // player 1's mode as toggleDualMode names it (0x31de10, after the dual is off)
    static int modeTypeOf(const Mem& P) {
        if (P.u8(po::ModeShip) || P.u8(po::ModeUfo) || P.u8(po::ModeWave) || P.u8(po::ModeSwing)) return 5;
        if (P.u8(po::ModeBall)) return 0x10;
        if (P.u8(po::ModeSpider)) return 0x21;
        return 6;
    }
    // ---- the mirror: GJBaseGameLayer::toggleFlipped 0x350b00 and its tween of +0x41c ----
    // A mirror portal (collisionCheckObjects case 0xe: on, 0xf: off, both gated on +0x317c and the once-gate) calls toggleFlipped(on, portal +0x41c "instant"):
    // nothing when +0x418 already says so; else +0x418 := on and, not instant, +0x41c starts from 1e-5 (on) / 0.99999 (off) when it was settled (0 or 1),
    // and GJGameState::tweenValue(+0x41c -> on ? 1 : 0, id 7, 0.4 s, easing 1 (ease in-out), rate 1.2). The tween is advanced once per step before the next
    // pass: elapsed += the step's seconds (float), u = elapsed / 0.4, e = in-out(u, 1.2) in float powf, value = from + (to - from) * e (o_lv13_9210: 60 of
    // 60 steps bit for bit). The end (u >= 1 -> the target) is assumed, not measured: no capture reaches it.
    bool flipped = false;      // +0x418
    float flipAmt = 0.0f;      // +0x41c
    bool flipKnown = false;    // false: the seed is mid-tween (its elapsed time is not in the capture)
    bool flipTween = false;
    float flipFrom = 0.0f, flipTo = 0.0f, flipEl = 0.0f;
    void seedFlip(bool on, float amt) {
        flipped = on; flipAmt = amt; flipTween = false;
        flipKnown = amt == 0.0f || amt == 1.0f;
    }
    void mirrorPortal(bool on, bool instant) {
        if (flipped == on) return;
        flipped = on;
        const float target = on ? 1.0f : 0.0f;
        if (instant) { flipAmt = target; flipTween = false; return; }
        if (flipAmt == 0.0f || flipAmt == 1.0f) flipAmt = on ? 1e-05f : 0.99999f;
        // (a reversal mid-tween: the game also raises +0x420; the tween restarts from the current value)
        flipFrom = flipAmt; flipTo = target; flipEl = 0.0f; flipTween = true;
    }
    static float easeInOut(float u, float rate) {
        u = u * 2.0f;
        if (u < 1.0f) return 0.5f * std::pow(u, rate);
        return 1.0f - 0.5f * std::pow(2.0f - u, rate);
    }
    void advanceFlip(float stepSeconds) {
        if (!flipTween) return;
        flipEl = flipEl + stepSeconds;
        float u = flipEl / 0.4f;
        if (u >= 1.0f) { flipAmt = flipTo; flipTween = false; return; }
        flipAmt = flipFrom + (flipTo - flipFrom) * easeInOut(u, 1.2f);
    }

    // the band for a step: false when the shadow cannot give it (branch A, a dual, H not known)
    bool band(float zoom, float camOffY, bool branchA, float& mn, float& mx) const {
        if (branchA || dual || !heightKnown) return false;
        bandB(centre, height, zoom, camOffY, mn, mx);
        return true;
    }
};

}  // namespace gdcol
}  // namespace dp
