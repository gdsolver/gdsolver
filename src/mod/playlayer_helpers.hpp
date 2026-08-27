#pragma once
// Helpers for the PlayLayer hook (visibility-crash log, checkpoint notes).
#include "mod/render_trace.hpp"

using namespace p1;

// Swallow log for visibility crashes (split out because a function containing an SEH block
// cannot construct C++ objects. The log flood when it fires often is cut off at 5)
inline void logVisibilityCrashSwallowed() {
    static int s_count = 0;
    ++g_visAVs;   // the real count is this one (the log is cut off at 5 entries, so it cannot
                  // be used to count)
    if (++s_count <= 5)
        log::warn("updateVisibility access violation swallowed ({}), frame skipped", s_count);
}

// ---- Progress recording: use GD's own "do not record" path ----
// PlayLayer::destroyPlayer and PlayLayer::levelComplete write the level's record -- percentage,
// attempts, orbs, diamonds, the completion flag -- and BOTH gate that whole block on
// `m_isTestMode` (GJBaseGameLayer +0x3230, the flag GD sets when a level is played from a start
// position). Setting it around the original call makes GD skip the recording itself, which
// covers every path inside those functions instead of the handful a hook list can enumerate.
//
// IT IS SCOPED ON PURPOSE. `m_isTestMode` is not only read by the recording code:
// `GJBaseGameLayer::updateCamera` reads it too and skips a camera clamp when it is set. The
// camera is not decoration here -- the flight band and camera-driven triggers follow it, and a
// changed camera would change the simulation. So the flag is raised for the duration of the
// original call and put straight back; updateCamera runs in the tick loop and never sees it.
struct NoRecordGuard {
    GJBaseGameLayer* layer;
    bool saved;
    explicit NoRecordGuard(GJBaseGameLayer* l)
        : layer(progressBlockActive() ? l : nullptr), saved(l && l->m_isTestMode) {
        if (layer) layer->m_isTestMode = true;
    }
    ~NoRecordGuard() {
        if (layer) layer->m_isTestMode = saved;
    }
    NoRecordGuard(const NoRecordGuard&) = delete;
    NoRecordGuard& operator=(const NoRecordGuard&) = delete;
};

// ---- Attempt boundaries / end conditions ----
// Common output for observing automatic checkpoint placement (cfg `ckpttrace=1`). Always prints
// the tick together with the clock candidates (decides by the numbers whether placement is a
// function of tick or of real time). solved = placement tick looked up from x (-1=not computed)
inline void noteCkpt(const char* what, PlayerObject* p, void* obj, long long solved) {
    char cb[256];
    snprintf(cb, sizeof(cb),
        "ckpt: %-6s tick=%lld solved=%lld x=%.2f obj=%p try=%d timeout=%d "
        "lastCkptT=%.6f totalT=%.6f",
        what, (long long)g_tick, solved,
        p ? p->getPositionX() : -1.f, obj,
        p ? (p->m_shouldTryPlacingCheckpoint ? 1 : 0) : -1,
        p ? (p->m_checkpointTimeout ? 1 : 0) : -1,
        p ? p->m_lastCheckpointTime : -1.0,
        p ? p->m_totalTime : -1.0);
    writeResult(cb);
}
