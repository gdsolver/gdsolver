#pragma once
// Helpers for the PlayLayer hook (visibility-crash log, checkpoint notes).
#include "mod/render_trace.hpp"

using namespace p1;

// Swallow log for visibility crashes (split out because a function containing an SEH block
// cannot construct C++ objects. The log flood when it fires often is cut off at 5)
// WHERE it faulted, filled by the __except filter (visAvNote) before the handler runs: the
// instruction as an offset into the game module (0 = outside it, then the raw address) and the
// address it tried to read. A count alone said the pass broke, not on what.
inline uintptr_t g_visAvRip = 0, g_visAvRead = 0, g_visAvRet = 0;
#ifdef GEODE_IS_WINDOWS
inline int visAvNote(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord) {
        const uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
        auto* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        auto rel = [&](uintptr_t a) {
            return (a >= base && a - base < nt->OptionalHeader.SizeOfImage) ? a - base : a;
        };
        g_visAvRip = rel((uintptr_t)ep->ExceptionRecord->ExceptionAddress);
        g_visAvRead = ep->ExceptionRecord->NumberParameters >= 2
                          ? (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1] : 0;
        // The top of the stack: at a fault on a callee's first instructions (before it pushes
        // anything) this is the return address into its caller.
        g_visAvRet = 0;
        if (ep->ContextRecord) {
            __try {
                g_visAvRet = rel(*(uintptr_t*)ep->ContextRecord->Rsp);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif
inline void logVisibilityCrashSwallowed() {
    static int s_count = 0;
    ++g_visAVs;   // the real count is this one (the log is cut off at 5 entries, so it cannot
                  // be used to count)
    if (++s_count <= 5) {
        log::warn("updateVisibility access violation swallowed ({}), frame skipped", s_count);
        char b[160];
        snprintf(b, sizeof(b), "visav: n=%d t=%lld rip=GeometryDash.exe+0x%llx read=0x%llx "
                 "ret=GeometryDash.exe+0x%llx",
                 s_count, (long long)g_tick, (unsigned long long)g_visAvRip,
                 (unsigned long long)g_visAvRead, (unsigned long long)g_visAvRet);
        writeResult(b);
    }
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
//
// 2.209 also keeps the time spent on a level in its record -- first- and last-played timestamps and two
// play-time counters (save keys k138, k139, k142, k143, with k133 marking them valid). The function that
// commits them is called with no test-mode test, and destroyPlayer carries inline copies of the same
// writes, so the flag does not stop them. PlayTimeRecord209 is how they are undone: the five ints are
// read before the original runs and put back after it, the way restoreProgress treats the counters GD
// writes outside its guarded block. (Found by the memory audit: a driven session that died showed k133
// appear in the level's record.)
struct PlayTimeRecord209 {
    static constexpr std::size_t kOff[5] = {0x6b0, 0x738, 0x73c, 0x740, 0x744};
    int v[5] = {0, 0, 0, 0, 0};
    GJGameLevel* level = nullptr;
    // `layer` is the GJBaseGameLayer whose m_level is the record; the offset check keeps a layout this
    // code was not written for from being written through.
    void take(GJBaseGameLayer* layer) {
#if GEODE_COMP_GD_VERSION >= 22090 && defined(GEODE_IS_WINDOWS)
        level = nullptr;
        if (!layer || !layer->m_level) return;
        if (reinterpret_cast<const char*>(&layer->m_level) - reinterpret_cast<const char*>(layer) != 0x878) return;
        level = layer->m_level;
        auto* p = reinterpret_cast<const unsigned char*>(level);
        for (int i = 0; i < 5; ++i) v[i] = *reinterpret_cast<const int*>(p + kOff[i]);
#else
        (void)layer;
#endif
    }
    // True when something had moved and was put back.
    bool putBack() {
#if GEODE_COMP_GD_VERSION >= 22090 && defined(GEODE_IS_WINDOWS)
        if (!level) return false;
        bool moved = false;
        auto* p = reinterpret_cast<unsigned char*>(level);
        for (int i = 0; i < 5; ++i) {
            auto& x = *reinterpret_cast<int*>(p + kOff[i]);
            if (x != v[i]) { x = v[i]; moved = true; }
        }
        return moved;
#else
        return false;
#endif
    }
};

struct NoRecordGuard {
    GJBaseGameLayer* layer;
    bool saved;
    PlayTimeRecord209 time;
    explicit NoRecordGuard(GJBaseGameLayer* l)
        : layer(progressBlockActive() ? l : nullptr), saved(l && l->m_isTestMode) {
        if (layer) {
            layer->m_isTestMode = true;
            time.take(layer);
            static int s_said = 0;   // an original (destroyPlayer, levelComplete ...) run for an attempt the bot flew after its session was torn down: named, the first few times (config.hpp, recordsHeld)
            if (g_botAttempt && !g_started && ++s_said <= 10) writeResult("held attempt: GD's record block is skipped (test mode raised around the original)");
        }
    }
    ~NoRecordGuard() {
        if (layer) {
            layer->m_isTestMode = saved;
            if (time.putBack()) ++g_progressRestores;
        }
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

// Whether PlayLayer::updateVisibility is skipped outright: the fast loop with render skip, burning
// through the end-zone effects, and a section solve's search (the first three early-outs of the
// hook, hooks_playlayer), and a seek's screen-off stretch (cfg seekblind). The visibility pass is
// what puts a moved object's node at its real position, so with it off the node keeps where the
// object was when the pass last saw it.
inline bool visibilityPassOff() {
    if (g_started && !g_sessionOver && g_cfg.fastdt > 0 && g_cfg.skipRender && !g_realtimeOverride)
        return true;
    if (g_endzoneBurn) return true;
    if (itermap::g_seekBlindNow) return true;
    return g_secRenderWas >= 0;
}
