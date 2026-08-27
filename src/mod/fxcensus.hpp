#pragma once
// Census of live effect nodes and sweeping of leaked circle waves (fast-mode hygiene).
#include "mod/config.hpp"

namespace p1 {

// ============================================================
// Census of visual-effect nodes (fxcensus). Counts, per type, what is alive on screen
// (a tool for naming what is piling up before adding a suppression)
// ============================================================
inline void censusInto(cocos2d::CCNode* n, int depth, std::map<std::string, int>& out) {
    if (!n || depth > 3) return;
    const char* kind = "other";
    // Test the most-derived types first (a CCParticleSystemQuad is also a CCParticleSystem)
    if (geode::cast::typeinfo_cast<PlayerObject*>(n))                      kind = "PlayerObject";
    else if (geode::cast::typeinfo_cast<GameObject*>(n))                   kind = "GameObject";
    else if (geode::cast::typeinfo_cast<cocos2d::CCParticleSystem*>(n))    kind = "Particle";
    else if (geode::cast::typeinfo_cast<CCCircleWave*>(n))                 kind = "CircleWave";
    else if (geode::cast::typeinfo_cast<cocos2d::CCSpriteBatchNode*>(n))   kind = "BatchNode";
    else if (geode::cast::typeinfo_cast<cocos2d::CCLabelBMFont*>(n))       kind = "Label";
    else if (geode::cast::typeinfo_cast<cocos2d::CCSprite*>(n))            kind = "Sprite";
    else if (geode::cast::typeinfo_cast<cocos2d::CCLayer*>(n))             kind = "Layer";
    ++out[kind];
    auto* kids = n->getChildren();
    if (!kids) return;
    for (unsigned i = 0; i < kids->count(); ++i)
        censusInto(static_cast<cocos2d::CCNode*>(kids->objectAtIndex(i)), depth + 1, out);
}

// One line of "what is alive right now". tag names the moment it was taken (switch /
// session_end etc.)
inline std::string fxCensus(const char* tag) {
    std::map<std::string, int> c;
    if (auto* pl = PlayLayer::get()) censusInto(pl, 0, c);
    std::string s = std::string("fxcensus[") + tag + "]";
    for (auto& kv : c) s += " " + kv.first + "=" + std::to_string(kv.second);
    return s;
}

// Sweep the CCCircleWaves that accumulated while not rendering (cfg `fxsweep=0` disables, A/B
// only). A CCCircleWave removes itself via the scheduler, not an action, so
// purgeDanglingActions cannot reach it and it piles up without bound during the fast loop
// (the family GD spawns directly on orb firing and death).
// Removal follows the same order as the game itself: notify the delegate first, then detach
// (skipping that leaves a dangling pointer)
inline bool g_fxSweep = true;
inline long long g_fxSwept = 0;
inline void collectCircleWaves(cocos2d::CCNode* n, int depth,
                               std::vector<CCCircleWave*>& out) {
    if (!n || depth > 3) return;
    auto* kids = n->getChildren();
    if (!kids) return;
    for (unsigned i = 0; i < kids->count(); ++i) {
        auto* k = static_cast<cocos2d::CCNode*>(kids->objectAtIndex(i));
        if (auto* cw = geode::cast::typeinfo_cast<CCCircleWave*>(k)) out.push_back(cw);
        else collectCircleWaves(k, depth + 1, out);
    }
}
inline void sweepCircleWaves() {
    auto* pl = PlayLayer::get();
    if (!pl) return;
    std::vector<CCCircleWave*> waves;
    collectCircleWaves(pl, 0, waves);
    for (auto* cw : waves) {
        // Do not drive the refcount down to zero ourselves: if someone releases mid-sweep it
        // goes below 0 and the autorelease pool steps on a freed pointer. retain before
        // touching it, and at the end autorelease to defer the release to a safe point of the
        // pool (always balanced)
        cw->retain();
        if (cw->m_delegate) cw->m_delegate->circleWaveWillBeRemoved(cw);
        cw->m_delegate = nullptr;   // cut off further callbacks (the other side may die first)
        cw->removeFromParentAndCleanup(true);
        cw->autorelease();
        ++g_fxSwept;
    }
}

// Swallow log on the render (visit) side. Split out because a function containing an SEH block
// cannot construct C++ objects. Counter and log are separate (the log is cut off at 5 entries,
// so it cannot be used to count)
inline long long g_visitAVs = 0;
inline long long g_visAVs = 0;
// Times a re-entrantly called update was passed through (so that "zero" is visible)
inline long long g_reentrantUpdates = 0;
// Times the auto-pause on focus loss was swallowed (the main cause of frozen workers)
inline long long g_unfocusPauseBlocked = 0;
inline void logVisitCrashSwallowed() {
    static int s_count = 0;
    ++g_visitAVs;
    if (++s_count <= 5)
        log::warn("GJBaseGameLayer::visit access violation swallowed ({}), frame skipped",
                  s_count);
}

// Mode of the on-screen panel. The panel itself is in ui_panel.hpp; the mode lives here because
// both loadConfig (cfg `uimode=`) and the session setup, which come earlier in the chain, need it.
//
// The numbering is append-only: `uimode=1` has meant Replay since before Solve existed.
inline constexpr int UI_MODE_NORMAL = 0;   // the game as it is; the mod only watches
inline constexpr int UI_MODE_REPLAY = 1;   // replay a stored solution file for this level
inline constexpr int UI_MODE_SOLVE = 2;    // solve the level in-process, then replay the result
inline constexpr int UI_MODE_COUNT = 3;

inline const char* uiModeName(int m) {
    switch (m) {
        case UI_MODE_REPLAY: return "Replay";
        case UI_MODE_SOLVE: return "Solve";
        default: return "Normal";
    }
}

inline int g_uiMode = UI_MODE_NORMAL;

// F8: temporarily toggle between fast mode and realtime (spectating hotkey)
inline bool g_realtimeOverride = false;
// Trigger the F8 equivalent mechanically (cfg `watchat=<tick>`). Hotkeys only work in the
// foreground window, so headless reproduction/diagnosis walks the same path through this
inline long long g_watchAtTick = 0;
// Variant fired on the player's x (cfg `watchatx=<x>`). tick resets to 0 every attempt, so use
// this to target a deep position (the condition where the visible-section catch-up is large)
inline float g_watchAtX = 0.f;
// Number of ticks after returning to realtime before going back to frame skip (cfg
// `watchback=<ticks>`). The CTD happens on the return leg (realtime -> frame skip), not the
// outbound one
inline long long g_watchBackTicks = 0;
inline long long g_watchStartTick = -1;   // tick at which we switched to realtime
// Variant fired on the wall clock (cfg `watchafter=<seconds>`). The condition "after skipping for
// a long time" cannot be expressed in tick / x
inline double g_watchAfterSec = 0.0;
// Variant that keeps going back and forth (cfg `watchcycle=<seconds>`). Flips realtime <-> fast
// every N wall-clock seconds
inline double g_watchCycleSec = 0.0;
inline double g_watchCycleLast = -1.0;   // wall-clock second of the last flip
inline long long g_watchFlips = 0;

// Measurement of the "idle spinning" from death to the start of the next attempt

}  // namespace p1
