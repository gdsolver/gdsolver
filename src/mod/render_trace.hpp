#pragma once
// Render-suppression predicate and modifier-counter tracing.
#include "mod/hud.hpp"

using namespace p1;

// ---- State dump + physics-update trace ----
// Whether rendering is stopped (= nobody sees the visual effects even if created). Uses the same
// condition as the visit() skip. If only one of them changed we would get "visible but no
// effects"
inline bool renderSuppressed() {
    if (g_renderOff) return true;      // F8
    return g_started && !g_sessionOver && g_cfg.skipRender && !g_realtimeOverride;
}

// Record only the rising edges of the modifier counters
// (m_stateNoAutoJump/DartSlide/HitHead/FlipGravity/Force) (cfg `hitboxtrace=1`, `mod:` lines).
//
// collisionCheckObjects sets 2 based on "the id of the touched object", and the tail of
// PlayerObject::update unconditionally decrements all 5 together. So "reading 2 = touching it
// on that tick" should hold. Yet at lv22 t=2,748 a 2 was read 40px away from the box of 2866
// (pinned at 3735,255). The box cannot be identified without measuring when, and by whom, it
// was raised.
inline void traceModifierCounters(PlayerObject* p) {
    if (!g_cfg.hitboxTrace || !g_started || g_sessionOver) return;
    const char* pb = reinterpret_cast<const char*>(p);
    int v[5];
    memcpy(&v[0], pb + 0xb74, 4);   // NoAutoJump
    memcpy(&v[1], pb + 0xb78, 4);   // DartSlide
    memcpy(&v[2], pb + 0xb7c, 4);   // HitHead
    memcpy(&v[3], pb + 0xb80, 4);   // FlipGravity
    memcpy(&v[4], pb + 0xb88, 4);   // Force
    static int prev[5] = {0, 0, 0, 0, 0};
    bool up = false;
    for (int i = 0; i < 5; ++i) {
        // Both the rising edge (touched) and the falling edge (effect expired).
        // With rising edges alone, the end of a "kept touching, pinned at 1" interval never
        // shows, and "permanent once touched" vs "interval-limited" cannot be decided
        if (v[i] > 0 && v[i] > prev[i]) up = true;
        if (v[i] <= 0 && prev[i] > 0) up = true;
    }
    for (int i = 0; i < 5; ++i) prev[i] = v[i];
    // With rising edges alone, a "kept touching, pinned at 1" interval produces not a single
    // line (2->dec 1->2->dec 1 ...). Within the hbfrom..hbto window emit every tick
    const bool inWindow = g_tick >= g_cfg.hbFrom
                          && (g_cfg.hbTo <= 0 || g_tick <= g_cfg.hbTo);
    if (!up && !inWindow) return;
    static int lines = 0;
    if (++lines > 400) return;
    char b[192];
    snprintf(b, sizeof(b),
             "mod: t=%lld x=%.3f y=%.3f noAuto=%d dart=%d hitHead=%d "
             "flipGrav=%d force=%d",
             (long long)g_tick, p->getPositionX(), p->getPositionY(),
             v[0], v[1], v[2], v[3], v[4]);
    writeResult(b);
}

// Observation of pad activation (cfg `padtrace=1`). The pad's one-shot is held by
// EnhancedGameObject::m_activatedByPlayer1 (the GameObject-side activatedByPlayer is inline on
// win and cannot be hooked). To make GD name the boundary of grazing activation, only pad types
// (m_objectType 8/9/10) are recorded. A line re-touching with `used` still raised is evidence of
// "already consumed, will not fire"
