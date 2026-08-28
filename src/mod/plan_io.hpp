#pragma once
// Per-session state reset, plan_in.txt input loader, inject=/stopat= extras.
#include "mod/speedgate.hpp"

namespace p1 {

inline void resetSessionState() {
    g_started = false; g_sessionOver = false; g_uiSession = false;
    g_attempt = 0; g_finishedAttempts = 0;
    g_nextInput = 0; g_nextToggle = 0;
    g_frame = 0; g_tick = 0; g_traceLines = 0; g_gameFrame = 0;
    g_endzoneBurn = false;   // carried into the next session, rendering would stay stopped
    g_endzoneLockStart = {};
    speedgate::reset();      // the level changes, so rebuild
    g_paused = false; g_stepTicks = 0; g_realtimeOverride = false;
    g_dpShowSolution = false;   // a finished solve must not make the next session start as one
    // ...and neither must its SOLVING flag. It is raised when a search starts and lowered when
    // that search returns; a session that ends in between (F9, leaving the level, a give-up that
    // beat the worker home) left it raised, and solvingNow() then answered yes for the NEXT
    // session. Measured 2026-08-28: after one Solve, a Replay had no music (audio::silent asks
    // solvingNow) and a dead seek bar (it refuses to drive a level the loop owns).
    g_dpSolving = false;
    watchSpeedSet(WATCH_SPEED_1X); g_visRefresh = false;
    g_pauseAtXFired = false;   // stop once again in the next session
    g_ckpt = nullptr; g_ckptTick = -1; g_headHeld = 0;
    g_practiceOn = false; g_restoreDone = false; g_restorePending = false;
    g_restoreLoopDone = false;
    solver::g_log.clear();
    orbtrace::reset();
    padtrace::reset();
    // Only a panel Solve session turns this on (session.hpp), and nothing ever turned it back
    // off: g_objs kept pointing at the outgoing level's GameObjects, and the next session --
    // Replay, Normal, another Solve -- inherited them. Measured (2026-08-24): solving lv16, then
    // replaying lv20, crashed in grouptrace::tick() on a dangling lv16 GameObject*.
    grouptrace::g_on = false;
    grouptrace::reset();
    // A suspended section search holds the OUTGOING level in its coroutine frame (the same
    // dangling-pointer family as grouptrace's g_objs above). The session it belonged to is over
    // either way -- a search ends its own session -- so it is dropped here rather than left for
    // the next one to resume into a level it never ran in.
    secsolve::reset();
    // The HUD's progress numbers. g_hudVerifiedX is a running maximum (onDeath only ever raises
    // it) and nothing lowered it again, so a second solve in the same process opened showing the
    // PREVIOUS level's deepest x measured against the NEW level's length. Measured (2026-08-24):
    // solving 1474319 after another level started the bar at 54.3% with iter 1 not yet run.
    // giveUp()'s "stopped at N%" reads the same field, so it reported that figure too.
    g_hudIter = 0;
    g_hudFixups = 0; g_hudFixupCap = 0;
    g_hudVerifiedX = 0.f; g_hudAnchorX = 0.f;
    g_hudAnchorT = 0;
    g_hudPhase.clear();
    solver::g_levelMaxX = 0;
    solver::g_prevTickX = -1e9f;
    solver::g_totalAttempts = 0;
    solver::g_lastDeathTick = 0; solver::g_lastGroundGap = 0;
    solver::g_injThisAttempt = 0; solver::g_injAtDeath = 0;
    solver::g_restoreTickDbg = -1;
    solver::g_pois.clear(); solver::g_poisBuilt = false;
    // The iteration map belongs to one level's run. Left behind, the next session's F10 would
    // draw the PREVIOUS level's rounds over this one -- the same family of bug as the grouptrace
    // and HUD leaks above, and just as convincing to look at.
    itermap::clear();
    // The PREFERENCE is deliberately not reset here. Until someone expresses one it is not a
    // stored value at all -- mapWanted() reads it off the session's mode, so a solve draws the map
    // and a replay does not without anything having to be reset between them. Once F10 or cfg
    // `itermap` has answered, that answer is theirs and outlives the session: resetting it would
    // mean the cfg key worked for a worker-driven replay and silently did nothing for a
    // panel-driven one, and that turning the map on and replaying (F7 does not come through here,
    // level exit does) gave two different answers depending on how the level was re-entered.
    probe::reset(); g_probeRequest = 0;
    g_blockedAch = 0; g_blockedStat = 0; g_blockedCoin = 0; g_blockedProgress = 0;
    g_progressRestores = 0;
    g_soundWhileSolving = 0;
    g_soundBlockedWhileSolving = 0;
    g_silenceReasserts = 0;
    notify::g_forced = 0;
    if (g_trace.is_open()) g_trace.close();
    if (g_dump.is_open()) g_dump.close();
}

inline bool loadInputsFile(const std::string& path, std::vector<InputCmd>& out) {
    std::ifstream f(path);
    if (!f) return false;
    out.clear();
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        // PowerShell's `Set-Content -Encoding utf8` writes with a BOM. With a BOM only the first
        // line fails the "input=" test and the first input silently disappears (the log looks
        // normal)
        if (first) {
            first = false;
            if (line.size() >= 3 && (unsigned char)line[0] == 0xEF
                && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
                line.erase(0, 3);
        }
        if (line.rfind("input=", 0) != 0) continue;
        int t = 0, d = 0;
        if (sscanf(line.c_str() + 6, "%d,%d", &t, &d) == 2)
            out.push_back({t, d != 0});
    }
    return !out.empty();
}

// Write a plan back out in the same format loadInputsFile reads. Used to keep a solution the
// mod solved and then SAW CLEAR THE LEVEL -- a plan that has not cleared is not a solution and
// must not be filed as one (the repository already carries 72 dead lineages from the days when
// anything got saved).
inline bool writeInputsFile(const std::string& path, const std::vector<InputCmd>& plan) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    for (const InputCmd& c : plan)
        f << "input=" << c.step << "," << (c.down ? 1 : 0) << "\n";
    return true;
}

// ============================================================
// Analysis directives that piggyback on the plan (`inject=` / `stopat=` lines in plan_in.txt)
//
//   inject=<tick>[,x=<v>][,y=<v>][,vy=<v>]  overwrite the state after that tick's physics update
//   stopat=<tick>                           report the state at that tick and end the attempt
//
// They piggyback on plan_in.txt because cmd.txt (~5Hz polling) cannot name a physics tick.
// loadInputsFile ignores lines other than "input=", so compatibility is not broken.
// Only scalars can be injected (position and y velocity). Mode/size/gravity carry a lot of
// attached state and a raw assignment is inconsistent (a wholesale copy of PlayerObject stops
// orbs from firing). Takes effect from the T+1 transition (same contract as colprobe).
// ============================================================
struct InjectSpec {
    long long tick = -1;
    bool hasX = false, hasY = false, hasVy = false;
    double x = 0, y = 0, vy = 0;
};
inline std::vector<InjectSpec> g_injects;
inline size_t g_injectNext = 0;
inline long long g_stopAt = -1;
inline bool g_stopFired = false;   // once per attempt (reset to false at the attempt head)

inline void loadPlanExtras(const std::string& path) {
    // Clear every time. A leftover injection from last time shows up as "same plan, different
    // result"
    g_injects.clear();
    g_injectNext = 0;
    g_stopAt = -1;
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    bool first = true;
    while (std::getline(f, line)) {
        if (first) {
            first = false;
            if (line.size() >= 3 && (unsigned char)line[0] == 0xEF
                && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
                line.erase(0, 3);
        }
        if (line.rfind("stopat=", 0) == 0) {
            g_stopAt = std::atoll(line.c_str() + 7);
            continue;
        }
        if (line.rfind("inject=", 0) != 0) continue;
        InjectSpec s;
        const char* p = line.c_str() + 7;
        s.tick = std::atoll(p);
        // The rest is a sequence of ",key=value". Any order, each optional
        size_t pos = line.find(',', 7);
        while (pos != std::string::npos) {
            size_t next = line.find(',', pos + 1);
            std::string kv = line.substr(pos + 1,
                next == std::string::npos ? std::string::npos : next - pos - 1);
            auto eq = kv.find('=');
            if (eq != std::string::npos) {
                auto key = kv.substr(0, eq);
                double val = std::atof(kv.c_str() + eq + 1);
                if (key == "x")  { s.hasX = true;  s.x = val; }
                else if (key == "y")  { s.hasY = true;  s.y = val; }
                else if (key == "vy") { s.hasVy = true; s.vy = val; }
            }
            pos = next;
        }
        if (s.tick >= 0 && (s.hasX || s.hasY || s.hasVy)) g_injects.push_back(s);
    }
    std::sort(g_injects.begin(), g_injects.end(),
              [](const InjectSpec& a, const InjectSpec& b) { return a.tick < b.tick; });
}

// Configure the session on level entry (level selection is left to the game's own UI).
// The panel only does Replay: replays data/solution_lv{N}_dp.txt at realtime with rendering.
// Return value false = no session started, normal play (including when there is no solution
// file)

}  // namespace p1
