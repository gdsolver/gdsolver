#pragma once
// Helpers for the PlayLayer hook (visibility-crash log, checkpoint notes).
#include "mod/render_trace.hpp"
#include "solver/attempt_end.hpp"

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

// Publish the recording before the outcome line lets readers consume this attempt.
inline void rollGroupTrace(long long endTick = -1) {
    if (!grouptrace::g_on) return;
    if (cpflight::g_probeFlying || cpflight::g_controlFlying) return;
    const long long tick = endTick >= 0 ? endTick : g_tick;
    auto r = grouptrace::roll(tick);
    writeResult("gt_last: attempt=" + std::to_string(g_attempt)
        + " rows=" + std::to_string(r.rows)
        + " depth=" + std::to_string(r.depth)
        + " end=" + std::to_string(tick));
}

// Commit the attempt once, before any reset, whether GD killed a body or a guard forced it over.
inline void bookAttemptEnd(const cocos2d::CCPoint& pos,
                           solver::AttemptEndKind kind = solver::AttemptEndKind::Collision,
                           const char* reason = "death",
                           long long endTick = -1) {
    if (solver::g_deathBooked) return;
    solver::g_deathBooked = true;
    solver::g_unbookedP2Death = {};
    // Explicit endTick is already a state label (including a deferred p2 verdict).
    const long long tick = endTick >= 0 ? endTick : solver::attemptStateTick(g_tick);
    ev("destroyPlayer", pos.x, pos.y);
    clearance::observe(solver::g_log, tick);
    if (!g_started) return;
    ++g_finishedAttempts;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_attemptStart).count();
    const double speed = ms > 0 ? (tick / 240.0) / (ms / 1000.0) : 0;
    rollGroupTrace(tick);
    flushAll();
    cpflight::noteEnd(g_tick);
    writeResult("death: attempt=" + std::to_string(g_attempt)
        + " tick=" + std::to_string(tick) + " x=" + std::to_string(pos.x)
        + " counter=" + std::to_string(g_tick) + " phase=state"
        + " wallMs=" + std::to_string(ms) + " speedX=" + std::to_string(speed)
        + (kind == solver::AttemptEndKind::Forced ? " forced=1 reason=" + std::string(reason)
           : kind == solver::AttemptEndKind::ConfirmedP2Reset
               ? " resetConfirmed=1 collisionEvidence=0 reason=" + std::string(reason) : ""));
    if (g_serveMode) g_serveWait = true;
    if (g_secRung && secsolve::g_on && !g_ckpt && g_cfg.checkpointAt >= 0
        && tick < g_cfg.checkpointAt)
        dpsolve::secRungPrefixFailed(tick, pos.x,
            kind == solver::AttemptEndKind::Collision ? "died" : reason);
    if (g_cfg.dpSolve) dpsolve::onDeath(tick, pos.x, kind);
}

// A kill call can be refused by GD; a forced end must still reach the repair loop before reset.
inline void forceAttemptEnd(PlayLayer* layer, const char* reason) {
    if (!layer || layer != PlayLayer::get() || !layer->m_player1
        || secsolve::g_active || secsolve::g_noKill || g_cfg.noDeath) return;
    g_stallResetPending = true;   // classify a synchronous death callback as forced too
    layer->destroyPlayer(layer->m_player1, layer->m_player1);
    if (!solver::g_deathBooked && g_started && !g_sessionOver) {
        writeResult("attempt_end: GD did not book the forced kill; handing "
                    + std::string(reason) + " to the repair loop before reset");
        bookAttemptEnd(layer->m_player1->getPosition(), solver::AttemptEndKind::Forced, reason);
    }
}

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
