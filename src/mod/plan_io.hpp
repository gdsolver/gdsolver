#pragma once
// Per-session state reset, plan_in.txt input loader, inject=/stopat= extras.
#include "mod/speedgate.hpp"
#include "mod/plan_edges.hpp"

namespace p1 {

// The level slice (mod/level_slice.hpp): where this session stands. Declared here, ahead of the
// loop and the session reset that ask it.
namespace levelslice {
enum Phase {
    Off = 0,     // the level itself, as chosen (no slice, or not decided yet)
    Sliced,      // solving on a copy without the objects the run cannot depend on
    Verifying,   // back on the level itself, flying the plan that cleared the copy
    Whole,       // the plan did not hold there: solving on the level itself from here on
    RefFlight,   // on a fresh copy, flying the plan a wall check just flew on the level, as the
                 // copy's first attempt (cfg slicefirstref, level_slice.hpp onRefFlightEnd)
};
inline int g_phase = Off;
inline void reset();
inline bool maybeSlice(GJBaseGameLayer* l);
inline bool takeCarry(std::vector<InputCmd>& out);
inline bool takeLight(std::vector<InputCmd>& out);
inline bool onVerifyDeath(long long dt, float deathX);
inline bool maybeCheckWall();
inline bool onGiveUp(const char* reason);
inline std::string summary();
inline size_t heldBytes();   // what the slice holds allocated (cfg memcensus)
}  // namespace levelslice

// ATTEMPT SEQUENCE. A plan file may hold several attempts: `attempt=<N>` starts block N, its `input=` lines are
// relative to that attempt's own tick 0, and `until=<tick>` ends a block that does not end in a death by
// restarting the level at that tick (a person pressing restart). A level that is not the same on its first
// attempt (an Item Persistence / attempt-count start, see level_warnings) is cleared by running the earlier
// blocks and then the last one in ONE layer, in order, with nothing pinned. `# ...` lines (a lineage id) are
// comments. A file without `attempt=` lines is one attempt, as before: out gets its inputs and the globals
// below are not touched. With them, out gets the LAST block (what every consumer already flies) and
// g_prelude the blocks before it.
struct PreludeAttempt {
    std::vector<InputCmd> in;
    long long until = -1;   // restart the level when this attempt's tick reaches it (-1: only a death ends it)
};
inline std::vector<PreludeAttempt> g_prelude;
inline std::vector<InputCmd> g_finalInputs;   // the last block, kept while a prelude swaps g_cfg.inputs
inline long long g_preludePendingAttempt = -1;   // the attempt the restart was asked for: a stale one is dropped
inline bool g_preludeResetPending = false;     // asked at an `until`; the level restarts at the next safe point
inline long long g_preludeResets = 0;

// A solve has run since its memory was last let go (dpsolve::releaseSolveMemory, repair.hpp):
// raised by dpsolve::start, lowered by the release, so leaving the level after a session end that
// has already released does not do it again.
inline bool g_solveMemHeld = false;
namespace dpsolve {
inline void releaseSolveMemory(const char* where);
inline void memCensus(const char* where);   // cfg memcensus (print only), repair.hpp
}

inline void resetSessionState() {
    g_solveMemHeld = false;   // (onQuit releases first, then calls this)
    g_prelude.clear();
    g_finalInputs.clear();
    g_preludeResetPending = false;
    g_preludeResets = 0;
    g_started = false; g_sessionOver = false; g_uiSession = false;
    g_attempt = 0; g_finishedAttempts = 0;
    g_nextInput = 0; g_nextToggle = 0;
    g_frame = 0; g_tick = 0; g_traceLines = 0; g_gameFrame = 0;
    g_endzoneBurn = false;   // carried into the next session, rendering would stay stopped
    g_endzoneLockStart = {};
    g_secRenderWas = -1;     // ...and the same for a section solve's hold (secRenderHold)
    g_secRealtimeWas = -1;
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
    // The raw jump button, as handleButton last saw it. Nothing physical reads
    // it -- its only consumer is the slope trace's `held` column
    // (hooks_player.cpp:664) -- so this is not a physics leak. It is worse in
    // one narrow way: it is the INSTRUMENT that leaks. Left behind, the first
    // rows of a level in a one-session run carry the LAST level's button state,
    // and a `held` column read that way looks exactly like a measurement. A
    // launch per level cannot reach it, so no per-level regression will.
    g_btnDown = false;
    g_practiceOn = false; g_restoreDone = false; g_restorePending = false;
    g_restoreLoopDone = false;
    solver::g_log.clear();
    orbtrace::reset();
    padtrace::reset();
    // ...and orbtrace's 400-line cap, which its reset() leaves (it runs per attempt; padtrace's clears
    // its own). Left behind, once one level had printed 400 lines no later level printed any.
    orbtrace::g_lines = 0;
    // The dead-tick counters printed at every session's end (endSession) are the session's.
    deadstat::g_total = deadstat::g_dead = deadstat::g_streak = 0;
    deadstat::g_maxStreak = deadstat::g_episodes = 0;
    // ...and the activations the anchor payload is built from. Left behind,
    // one level's touched triggers would seed the next level's anchors, which
    // is the same shape as the recording leak that hid under a green 22/22.
    touchseed::reset();
    touchfire::reset();
    portalseed::reset();
    padseed::reset();
    ringseed::reset();
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
    // solving a custom level after another level started the bar at 54.3% with iter 1 not yet run.
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
    // The coin tables belong to one level, and --one-session runs every level in
    // one process. buildPois rebuilds them, but only when it runs -- a session
    // that never reaches it would otherwise report the PREVIOUS level's coins,
    // which is the leak class CLAUDE.md names (one level's recording adopted by
    // the next, green for months under a per-level launch).
    solver::g_coins.clear();
    solver::g_coinPickupTick.clear();
    solver::g_coinGdTick.clear();
    solver::g_coinGdUnmatched = 0;
    solver::g_coinMissFired = false;
    solver::g_coinMissIdx = -1;
    solver::g_endTriggers = 0;
    solver::g_endTriggerFiredTick = -1;
    solver::g_hasRotGameplay = false;
    solver::g_itemCounts.clear();
    solver::g_coinGates.clear();
    solver::g_coinNoMiss.clear();
    solver::g_coinLogLines = 0;
    route::clear();   // cfg routeprereq: one level's prerequisites, built with its coins
    // The iteration map belongs to one level's run. Left behind, the next session's F10 would
    // draw the PREVIOUS level's rounds over this one -- the same family of bug as the grouptrace
    // and HUD leaks above, and just as convincing to look at.
    itermap::clear();
    levelslice::reset();
    // ...and the human takeover: its checkpoint belongs to this level's layer, and a mode left
    // live would lift the next session's input block.
    takeover::resetState();
    // The last real reset's layer and processCommands count. The skip asks only
    // whether both are unchanged, and a new level's layer can stand where the last one stood, so
    // left behind they could make its first reset read as a second. The count of skips is printed
    // at every session's end, so it is the session's, and so is its print cap.
    g_resetMarkLayer = nullptr;
    g_resetMarkPc = -1;
    g_resetsSkipped = 0;
    // cfg actcensus: the summary at the session's end is this level's objects, not the run's.
    itermap::g_actCensusState = itermap::ActCensus{};
    // The PREFERENCE is deliberately not reset here. Until someone expresses one it is not a
    // stored value at all -- mapWanted() reads it off the session's mode, so a solve draws the map
    // and a replay does not without anything having to be reset between them. Once F10 or cfg
    // `itermap` has answered, that answer is theirs and outlives the session: resetting it would
    // mean the cfg key worked for a worker-driven replay and silently did nothing for a
    // panel-driven one, and that turning the map on and replaying (F7 does not come through here,
    // level exit does) gave two different answers depending on how the level was re-entered.
    probe::reset(); g_probeRequest = 0;
    g_blockedAch = 0; g_blockedStat = 0; g_blockedCoin = 0; g_blockedProgress = 0;
    g_blockedSend = 0; g_blockedSave = 0; g_rolloverSkipped = 0;
    g_progressRestores = 0;
    g_soundWhileSolving = 0;
    g_soundBlockedWhileSolving = 0;
    g_secSongSeeksSkipped = 0;
    g_silenceReasserts = 0;
    notify::g_forced = 0;
    if (g_trace.is_open()) g_trace.close();
    if (g_dump.is_open()) g_dump.close();
}

// initialMask: the buttons held at the first line of the file. A solver's tail is the buttons held from each tick, starting from the state the anchor left, so
// its first line is read against that state: a tail that begins by releasing player 2's button, which the anchor holds, is a release only if the reader knows the button
// is down. Read against 0 the release was no edge, and the tail's later press became a press of a button still held.
inline bool loadInputsFile(const std::string& path, std::vector<InputCmd>& out, bool keepPrelude = false, int initialMask = 0) {
    std::ifstream f(path);
    if (!f) return false;
    out.clear();
    std::vector<PreludeAttempt> blocks;   // only when the file has attempt= lines
    std::string line;
    bool first = true;
    int prevMask = initialMask;   // the buttons held after the last line of the block: player 2's edge is written only where bit 1 changes
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
        if (line.rfind("attempt=", 0) == 0) {
            blocks.emplace_back();
            prevMask = 0;
            continue;
        }
        if (line.rfind("until=", 0) == 0) {
            if (!blocks.empty()) blocks.back().until = atoll(line.c_str() + 6);
            continue;
        }
        if (line.rfind("input=", 0) != 0) continue;
        int t = 0, d = 0;
        if (sscanf(line.c_str() + 6, "%d,%d", &t, &d) == 2) {
            // The level is a bitmask of the buttons held from that tick: bit 0 is player 1's jump,
            // bit 1 player 2's (a two-player level's second button), and every other bit is reserved
            // for buttons this version does not drive (platformer moves). A plan that uses one is
            // refused, not replayed with those buttons dropped.
            if (d & ~3) {
                writeResult("plan: " + path + " holds buttons this version does not drive (input="
                            + std::to_string(t) + "," + std::to_string(d) + ") - not loaded");
                out.clear();
                return false;
            }
            pushInputMask(blocks.empty() ? out : blocks.back().in, t, d, prevMask);
        }
    }
    if (!blocks.empty()) {
        // the last block is the clearing attempt; the ones before it are the prelude
        out = blocks.back().in;
        blocks.pop_back();
        if (!keepPrelude) {
            g_prelude = blocks;
            g_finalInputs = out;
        }
    } else if (!keepPrelude) {
        g_prelude.clear();   // a plain plan: the session has no prelude (nothing left from the file before)
        g_finalInputs.clear();
    }
    return !out.empty();
}

inline bool writeInputsFile(const std::string& path, const std::vector<InputCmd>& plan) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    writeInputLines(f, plan);
    return true;
}

// An attempt sequence out: the prelude blocks, then the clearing one, each as `attempt=<N>`, an optional
// `until=<tick>`, and its inputs. `lineage` is written as a comment next to each block (the start state's id).
inline bool writeAttemptSequence(const std::string& path, const std::vector<PreludeAttempt>& prelude,
                                 const std::vector<InputCmd>& clearing, const std::string& lineage = "") {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    int n = 0;
    auto block = [&](const std::vector<InputCmd>& in, long long until) {
        f << "attempt=" << ++n << "\n";
        if (!lineage.empty()) f << "# lineage=" << lineage << "\n";
        if (until >= 0) f << "until=" << until << "\n";
        writeInputLines(f, in);
    };
    for (const PreludeAttempt& p : prelude) block(p.in, p.until);
    block(clearing, -1);
    return true;
}

// Which version of the mod solved a stored solution: one line ("v0.3.2") in a file beside it
// (<solution>.ver), so the solution itself keeps the format every reader of it expects. A
// solution filed before the versions were written has none, and reads as "".
inline std::string solvedWithPath(const std::string& solutionPath) { return solutionPath + ".ver"; }
inline void writeSolvedWith(const std::string& solutionPath, const std::string& version) {
    std::ofstream f(solvedWithPath(solutionPath), std::ios::trunc);
    if (f) f << version << "\n";
}
inline std::string readSolvedWith(const std::string& solutionPath) {
    std::ifstream f(solvedWithPath(solutionPath));
    std::string v;
    if (f) std::getline(f, v);
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
    return v;
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
// The panel's Replay plays data/solution_lv{N}_dp.txt -- or solution_lv{N}_coins.txt with its
// Coins switch on -- at realtime with rendering (session.hpp, uiConfigureSession).
// Return value false = no session started, normal play (including when there is no solution
// file)

}  // namespace p1
