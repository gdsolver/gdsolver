#pragma once
// Session lifecycle: configuration from the UI, live commands, loadConfig, endSession.
#include "mod/gd_compat.hpp"
#include "mod/dp_bridge.hpp"
#include "mod/plan_io.hpp"
#include "mod/profsample.hpp"
#include "mod/record_gate.hpp"

namespace p1 {

inline void resetSecsolveSession();   // below, with the handoff's globals

// Everything a panel-started session has in common, whichever mode it is.
inline void uiSessionBase(int levelId) {
    resetSessionState();
    resetSecsolveSession();
    g_cfg = Config{};
    g_cfg.enabled = true;
    g_cfg.levelId = levelId;
    g_cfg.quitWhenDone = false; // a UI-started session does not quit the app
    g_cfg.noTrace = true;
    g_cfg.cbs = 0; g_cfg.cos = 1;
    g_cfg.blockInput = true;    // the mod drives; the keyboard does not
    g_cfg.delaySec = 0.f;
}

// The stored solution Replay plays for a level: the plain one, or with Coins on the coin one.
// The play menu asks the same question before offering Replay, so both read it from here.
inline std::string uiSolutionPath(int levelId, bool coins) {
    return std::string(DATA_DIR) + "/solution_lv" + std::to_string(levelId)
           + (coins ? "_coins.txt" : "_dp.txt");
}

inline bool uiConfigureSession(int levelId) {
    if (g_uiMode == UI_MODE_SOLVE) {
        // Solve mode: no plan yet -- the mod is about to make one. The level is held still
        // while the solver runs (dpsolve::start pauses it) and released when the plan lands.
        uiSessionBase(levelId);
        g_cfg.dpSolve = true;
        // The overlays shown: F1 does not hide them while solving, so a solve must not inherit them
        // hidden from a replay (session_hotkeys.hpp, F1).
        if (g_overlayHidden) {
            g_overlayHidden = false;
            log::info("panel: overlays shown again for the solve (F1 had hidden them)");
        }
        grouptrace::g_on = true;   // see the cfg key of the same name
        // Give the session a fast loop to fall back on, so the render key has somewhere to
        // send the frames. Without one, turning the screen off just makes the game invisible
        // at the same speed, which is all cost and no gain
        g_cfg.fastdt = 1.f / 60.f;   // 4 physics ticks per call
        g_cfg.fastloops = 1800;      // ...and this many calls per frame once the screen is off
        // ...but never more than 30 ms of them: the window has to keep answering on a heavy
        // level, where 1,800 calls took 8-18 s (Config::frameBudgetMs), and the replay panel
        // (drawn once a frame) keeps moving through a flight. It was 100 ms; on a heavy custom
        // level the same solve took 238.6 s at 100 ms and 231.5 s at 30 ms, to the same plan
        // (clear tick 15,444, 27 attempts) -- a smaller batch costs nothing measurable.
        g_cfg.frameBudgetMs = 30.0;
        // The section search hands the frame back every 20 ms of its own work (cfg secslicems; 50 by default): with the panel on
        // screen its frames came at 54-66 ms, 15 a frame a second, and at 20 ms they come at 30-39 ms. The same search, to the
        // same answer (restores 26,298, steps 39,777, depth 254/304), took 49.2 s against 48.6 s at 50 ms: the frames between
        // slices cost the same 1.4 s a search whichever way it is cut.
        secsolve::g_sliceMs = 20;
        // Start FAST, with the screen off -- the same state the render key produces, so the key
        // toggles out of it and back. A solve is searching (the level frozen, nothing to look at)
        // and testing candidates (replays that mostly die); at 1x each candidate costs the length
        // of the song. The overlay is still drawn, so the run is legible while the level is not.
        // [2026-08-23, user direction] This was briefly changed to "visible at 16x" on the theory
        // that a black screen reads as a crash. It does not, given the overlay -- what read as a
        // crash was the session closing itself afterwards, which is fixed at its own end.
        g_renderOff = true;
        g_realtimeOverride = false;
        // ...and the visibility pass off with it, as a worker's solve (skiprender=1) has always run it: g_renderOff
        // stops the drawing only, so with the pass left on every fast-loop call walked all the level's objects
        // (Grief, 190,000: the replays of a solve took 134 s against 21 s). skipRender holds only while
        // g_realtimeOverride is off, so the render key's ON (reset, then real time) puts the pass and the drawing
        // back with no further switch, and its OFF takes them away again.
        g_cfg.skipRender = true;
        // A plan that does not clear ends in a death, and the session has to survive that to be
        // repaired and tried again. The loop closes the session itself, on a clear or when it
        // gives up
        g_cfg.maxAttempts = 1000000;
        // Coins on: the goal is the end AND every coin (cfg coinroute, which implies GD's own
        // pickup detection), and the result is filed as the coin solution.
        if (g_uiCoins) {
            g_cfg.coinRoute = true;
            g_cfg.coinMode = true;
            g_cfg.coinFiles = true;
        }
        // Avoid glitches: the searches drop the frame-level routes (Config::glitchPortal).
        if (g_uiGlitch) {
            g_cfg.glitchPortal = kGlitchPortalDefault;
            g_cfg.glitchWave = kGlitchWaveDefault;
            g_cfg.glitchEmbed = kGlitchEmbedDefault;
            g_cfg.glitchDeco = true;
        }
        // ...and, as the menu's third choice, the wave's behaviour on top (Config::wavePulse / waveCentre).
        if (g_uiGlitchMode >= UI_GLITCH_WAVE) {
            g_cfg.wavePulse = kWavePulseDefault;
            g_cfg.waveCentre = kWaveCentreDefault;
            g_cfg.waveTidy = kWaveTidyDefault;
            g_cfg.waveToggle = kWaveToggleDefault;
        }
        // Section search: on unless Options turned it off (cfg dpsecauto=0, the same switch).
        if (!g_uiSecSolve) g_cfg.dpSecAuto = false;
        // The search (Config::dpSearch): the layer DP unless Options chose an experimental one.
        g_cfg.dpSearch = uiSearchCfg(g_uiSearch);
        log::info("panel: solve lv{} in-process{}{}{}{}, search {}", levelId,
                  g_uiCoins ? " with coins" : "", g_uiGlitch ? " avoiding glitches" : "",
                  g_uiGlitchMode >= UI_GLITCH_WAVE ? " (wave behaviour)" : "",
                  g_uiSecSolve ? "" : ", no section search", g_cfg.dpSearch);
    } else {
        const std::string name = uiSolutionPath(levelId, g_uiCoins);
        std::vector<InputCmd> plan;
        if (!loadInputsFile(name, plan)) {
            // Say so on screen as well: silently falling back to normal play looks exactly
            // like "the mod did nothing", which is what it looked like the first time. The menu
            // does not start Replay without the file, so what reaches here is a file that exists
            // and does not load.
            log::info("panel: no solution file for lv{} ({}) -> normal play", levelId, name);
            notify::show(g_uiCoins ? "gdsolver: no coin solution file for this level - "
                                     "choose Solve with Coins on"
                                   : "gdsolver: no solution file for this level - "
                                     "choose Solve",
                         NotificationIcon::Warning, 4.f);
            return false;
        }
        uiSessionBase(levelId);
        loadInputsFile(name, plan);   // again: the session base cleared the attempt sequence the first read set
        g_cfg.inputs = std::move(plan);
        g_cfg.maxAttempts = 1;
        // Coins on: GD's own pickups are reported (`coingd:`, and the count at the clear), so the
        // replay says in the log what the eye sees on screen.
        if (g_uiCoins) {
            g_cfg.coinMode = true;
            g_cfg.coinFiles = true;
        }
        g_replayStep = g_replayStepCfg >= 0 ? g_replayStepCfg : (g_uiReplayStep ? 60 : 0);
        log::info("panel: replay lv{} from {}{}", levelId, name, g_replayStep > 0 ? ", fixed replay step 1/" + std::to_string(g_replayStep) + " s" : std::string());
    }
    openFiles();
    stallwatch::start();   // so that on a stall we can say "where"
    updateWindowTitle();   // show the role in the title (reflected the moment the session starts)
    writeResult("session_start (panel)", true);
    dpsolve::memCensus("session start");   // cfg memcensus (print only)
    // Say once, plainly, whether the research trail (dp_band_itN/dp_groups_itN and the
    // recorder's dp_fixin_*/dp_attempt_itN files) is being kept this session -- see
    // Config::researchCapture for why it defaults off. A panel session never turns it on.
    writeResult(g_cfg.researchCapture
        ? "session: research capture on (per-attempt band/groups/recorder-input copies are kept)"
        : "session: research capture off (per-attempt band/groups/recorder-input copies are not kept)");
    g_started = true;
    g_uiSession = true;
    g_forceCleanStart = true;
    solver::g_solveStart = std::chrono::steady_clock::now();
    return true;
}

inline void endSession(const std::string& why);
// The session-end line of cfg cpflight (mod/cp_flight.hpp, included after this file): set by every
// solve's start, the flag on or off (off, its checkpoint and restore counts are 0), printed and cleared
// by endSession.
inline std::string (*g_cpFlightSummary)() = nullptr;

// ---- section-solver handoff (cmd `secsolve ...`) ---------------------------
// Written by pollCommandFileImpl below, consumed by dpsolve::poll() (repair.hpp) at a frame
// boundary. A pending-request pair of globals rather than a direct call, because the command
// can arrive while a solver worker thread is in flight and only the loop knows when it is
// safe to stop (a detached worker cannot be cancelled).
inline bool g_secReqPending = false;
inline long long g_secReqStart = -1;
inline double g_secReqTarget = 0.0;
inline long long g_secReqHorizon = -1;   // -1 = keep secsolve's current value
inline long long g_secReqCap = -1;       // -1 = keep
// A depth target instead of (or with) the x one: success = alive this many ticks past the head.
// -1 = keep the session's sectargetdepth. Only the loop's own rung sets it (dpsecauto).
inline long long g_secReqDepth = -1;
// cfg dpseccoinrung: the coin a rung fired at the coin wall has to take (-1 = none).
inline int g_secReqCoin = -1;
// The tick the rung's plan is known to live to in the game (its verified death), -1 = not known.
// The search's spine follows that plan, so on a faithful search it lives as long; a spine that dies
// earlier says the player snapshot parts from the game in this window (secsolve::g_spineUntil).
inline long long g_secReqSpineUntil = -1;
// ...and a rung asked again on checkpoints after its snapshot search was caught that way.
inline bool g_secReqForceCp = false;
inline bool g_secCpRedo = false;   // the rung in flight is such a redo
// ...and a rung asked again with its wsnap set after the plain snapshot search found nothing
// (cfg secwsnapgate; secsolve::g_wsnapForce is the rung in flight being one).
inline bool g_secReqForceWsnap = false;
// The last rung's window as handed over, for asking it again (a rung is consumed by the handoff).
struct SecLastReq {
    long long start = -1, depth = -1, horizon = -1, cap = -1;
    double target = 0.0;
    int coin = -1;
};
inline SecLastReq g_secLastReq;
// cfg dpsecstate (repair.hpp stateFire): what a rung asks of player 1's size. The kind is 0 for an
// ordinary window, 1 for the probe -- its answer is a verdict, never spliced -- 2 for a kept window
// over the decision, and 3 for an ordinary window that keeps the size the run needs; the size (0
// normal, 1 mini) is the one the probe forces or the window keeps.
inline int g_secReqState = 0, g_secReqSize = -1;
inline int g_secState = 0;           // ...the kind of the rung in flight

// ---- section solve as a RUNG (cmd `secrung ...`) ---------------------------
// The same search as the one-way `secsolve` handoff above, except that the
// answer comes BACK: the inputs it finds are spliced into the plan, the loop
// resumes, and GD verifies the new plan the way it verifies every other one.
//
// Measured on a level the model of 2026-09-22 could not pass: the loop died
// 60 times at one tick while writing
// 200 fixups around it; a 120-tick search over the stretch where those fixups
// sit crossed it in 61 s, and with the answer spliced in the loop was past the
// wall on its next iteration.
//
// THE SPLICED STRETCH HAS TO BE PINNED. The ladder's backoff grows while the
// next wall holds, and once it reaches back past the splice the stretch is
// re-planned by the model that could not do it: measured on the same level, a
// pin that was dropped that way cost 3 of the next 15 rounds, each dying on the
// wall the splice had crossed. `g_secPin` is the tick the ladder may not reach
// back past (runLadder). A pin does not rescue a splice that dies by itself: a
// window that ended before the death was solved in 5 s and its splice died on
// the wall (the model could not see what killed it), so the exit goes past the
// death.
inline bool g_secReqRung = false;    // the queued request is a rung
inline bool g_secRung = false;       // ...and the search in flight is one
inline long long g_secPin = -1;      // no anchor before this tick (-1 = none)
// The death the rung was fired at. The pin is placed past it as well as past the
// spliced ticks: an anchor between the two hands the crossing back to the model
// that could not do it. This is the reasoning, not a measurement -- the run first
// quoted for it (the loop back at the wall while its best stayed past it) was a
// bookkeeping fault, the splice never reaching g_plan. With the window's exit
// past the death, as dpsecauto draws it, the two ticks coincide anyway.
inline long long g_secPinWall = -1;
// What the handoff overwrote, so the resume can put it back.
inline int g_secSavePractice = -1, g_secSaveCkpt = -1, g_secSaveFastloops = 1;
inline int g_secSaveTargetDepth = 0;
inline long long g_secSaveMaxAttempts = 0;
// A rung whose prefix never reaches its head. The handoff replays the deepest plan to the head
// and places the checkpoint there; if GD does not replay that plan the way it once flew -- a
// custom level teleports the player 1,528 px ahead on some attempts and not on others -- the prefix
// dies before the head, markCheckpoint refuses a dead player, and nothing ended the rung: the
// attempt cap is the non-rung handoff's (g_cfg.maxAttempts), and the loop is stopped while a rung
// is in flight. 687 attempts in a row on the user's own game. Attempts (by g_attempt) that failed
// to reach the head, the last one counted, and kSecRungPrefixTries of them abandon the rung.
inline int g_secRungPrefixFails = 0;
inline long long g_secRungFailAttempt = -1;
constexpr int kSecRungPrefixTries = 2;

// The section search's own settings as the session's cfg left them. A rung changes them for its
// window -- the handoff sets the start tick, target, depth, horizon, cap and per-layer log, and
// autoFire the branching primitive -- and nothing put them back, so the next level of a suite
// started with the last rung's window and primitive. A run's `cfgdiff:` lines showed it on
// levels that set no sec* key (after the spin-off 1003's rungs, on every level that followed).
// Inert there -- the same [fp] as a process per level -- but a depth target the next handoff
// does not set would have carried over. Taken when a solve starts (dpsolve::start), put back
// when the session ends.
struct SecSettings {
    bool valid = false;
    long long startTick = -1;
    double targetX = 0.0;
    int targetDepth = 0;
    int horizon = 300;
    size_t cap = 100;
    bool log = false;
    int snapMode = 0;
    int verifyEvery = 0;
};
inline SecSettings g_secSettings;
inline void captureSecsolveSettings() {
    g_secSettings = {true, secsolve::g_startTick, secsolve::g_targetX, secsolve::g_targetDepth,
                     secsolve::g_horizon, secsolve::g_cap, secsolve::g_log, secsolve::g_snapMode,
                     secsolve::g_verifyEvery};
}

// The handoff's request and the rung's state belong to the session that raised them. Left
// behind by a player leaving the level mid-rung, they handed the next session a pending handoff
// at the old level's tick, a pin, and the search's own depth target (with secsolve::reset for
// the search itself). Called by endSession and at a panel session's start.
inline void resetSecsolveSession() {
    if (g_secRung) secsolve::g_targetDepth = g_secSaveTargetDepth;
    if (g_secSettings.valid) {
        secsolve::g_startTick = g_secSettings.startTick;
        secsolve::g_targetX = g_secSettings.targetX;
        secsolve::g_targetDepth = g_secSettings.targetDepth;
        secsolve::g_horizon = g_secSettings.horizon;
        secsolve::g_cap = g_secSettings.cap;
        secsolve::g_log = g_secSettings.log;
        secsolve::g_snapMode = g_secSettings.snapMode;
        secsolve::g_verifyEvery = g_secSettings.verifyEvery;
        g_secSettings.valid = false;
    }
    g_secReqPending = false;
    g_secReqRung = false;
    g_secRung = false;
    g_secPin = -1;
    g_secPinWall = -1;
    g_secReqCoin = -1;
    g_secReqDepth = -1;
    g_secReqSpineUntil = -1;
    g_secReqForceCp = false;
    g_secCpRedo = false;
    g_secReqForceWsnap = false;
    secsolve::g_wsnapForce = false;
    secsolve::g_wsnapHeld = 0;
    g_secLastReq = SecLastReq{};
    secsolve::g_spineUntil = -1;
    if (g_secState) {
        secsolve::g_forceP1Size = -1;
        secsolve::g_keepP1Size = -1;
    }
    g_secReqState = 0;
    g_secReqSize = -1;
    g_secState = 0;
}

// ---- level swap (cmd `swaplevel <path>`) -----------------------------------
// Replace the level THIS session is playing with the one in a file, without
// leaving the game. A section solve only needs the geometry its window can
// reach, and a clone cut down to that restores far faster (the restore is
// 86-89% of a section search and its cost follows the level's object count) --
// so the search wants a different level than the loop around it.
//
// The request is a pair of globals, consumed one frame later
// (queueInMainThread): the poll runs inside GJBaseGameLayer::update, and
// replacing the scene there would destroy the layer the call is standing in.
inline std::string g_swapReq;                   // "" = nothing pending
inline std::chrono::steady_clock::time_point g_swapT0;
inline bool g_swapTiming = false;               // print the cost of the swap

// Read one cfg value as a number, without raising if it is not one.
//
// These all went through std::stoi / std::stof, which throw on malformed input, and nothing above
// them catches: loadConfig runs at startup and pollCommandFileImpl runs inside
// GJBaseGameLayer::update, so a single unparseable number in autorun.cfg took the game down on
// launch and one in cmd.txt took it down mid-frame. Geode's own pitfalls page names those two
// functions for this reason -- try/catch does not work on every platform, so the fix is to stop
// generating the exception rather than to catch it.
//
// A value that will not parse leaves the setting at whatever it had and says so, which is what an
// unrecognised key already does. Reporting rather than defaulting silently matters here: a rig
// that quietly measures something other than what was asked for is the failure this project keeps
// having to detect afterwards.
template <class T>
inline bool cfgNum(const std::string& key, const std::string& val, T& out) {
    auto r = geode::utils::numFromString<T>(val);
    if (!r) {
        writeResult("cfg: " + key + "=" + val + " is not a number - ignored");
        return false;
    }
    out = r.unwrap();
    return true;
}

// The same, for the few keys that clamp or combine the parsed value instead of storing it as-is.
template <class T>
inline T cfgNumOr(const std::string& key, const std::string& val, T fallback) {
    T v{};
    return cfgNum(key, val, v) ? v : fallback;
}

inline void pollCommandFileImpl(const std::string& cmd) {
    if (cmd == "pause") { g_paused = true; log::info("phase2: paused at tick {}", g_tick); }
    else if (cmd == "resume") { g_paused = false; log::info("phase2: resumed"); }
    // `swaplevel <path>`: play a different level from here on (see g_swapReq). The swap
    // itself happens a frame later, off the update stack.
    else if (cmd.rfind("swaplevel ", 0) == 0) {
        g_swapReq = cmd.substr(10);
        g_swapT0 = std::chrono::steady_clock::now();
        g_swapTiming = true;
        writeResult("swaplevel: requested " + g_swapReq);
    }
    else if (cmd == "rerun") {
        // Serve mode: swap in data/plan_in.txt, reset immediately and run the next attempt from
        // the head of the new plan. Waiting for the attempt boundary via pause does not work
        // (with fastloops a whole attempt completes within one frame and never hits the
        // "stop at tick==0" check)
        std::vector<InputCmd> plan;
        // The analysis directives ride in the same file as the plan. Read them regardless of
        // whether the plan loads — to allow a zero-input run of "inject and just watch N ticks"
        loadPlanExtras(std::string(DATA_DIR) + "/plan_in.txt");
        bool haveInputs = loadInputsFile(std::string(DATA_DIR) + "/plan_in.txt", plan);
        bool haveExtras = !g_injects.empty() || g_stopAt >= 0;
        if (haveInputs || haveExtras) {
            std::sort(plan.begin(), plan.end(),
                      [](const InputCmd& a, const InputCmd& b) { return a.step < b.step; });
            g_cfg.inputs = std::move(plan);
            g_serveWait = false;
            g_paused = false;
            writeResult("serve: loaded " + std::to_string(g_cfg.inputs.size())
                + " inputs (attempt " + std::to_string(g_attempt) + ")"
                + (haveExtras ? " inject=" + std::to_string(g_injects.size())
                              + " stopat=" + std::to_string(g_stopAt) : ""));
            // Truncate dump/trace on every feed (in a resident session they pile up without
            // bound)
            if (g_dump.is_open()) {
                g_dump.close();
                g_dump.open(std::string(DATA_DIR) + "/dump.csv", std::ios::trunc);
                g_dump.precision(9);   // x precision for re-anchoring. Reason is in openFiles
                g_dump << "frame,attempt,tick,x,y,yvel,rot,mode,upsideDown,onGround,"
                          "onGround2,dead,speed,gravityMod,platXVel,vsize,gy1,gy2,"
                          "dual,p2y,p2vy,p2up,p2ground,p2dead,pmin,pmax,"
                          "snapuid,snapdist,camscale,gframe,ctrlOff,camx,camy,"
                          "p2ground2,p2mode,p2vsize,p2x,rotch,rotidx,rotrev,firedw,"
                          "bandst,bandmode,camoffy,freemode,bandforce,p2rot,p2ctrlOff\n";
            }
            if (g_trace.is_open()) {
                g_trace.close();
                g_trace.open(std::string(DATA_DIR) + "/trace.csv", std::ios::trunc);
                g_trace << "frame,attempt,tick,event,a,b,c\n";
            }
            // The reset is done on the physics-loop side (inside fastloop). Calling resetLevel
            // directly from the polling site crashes the worker
            g_serveReset = true;
        } else {
            writeResult("serve: FAILED to load plan_in.txt");
        }
    }
    // `seekfrom <cp tick> <target>` (cfg seekcp's instrument): a seek to <target> that starts from the
    // checkpoint at or before <cp tick>, whatever is nearer.
    else if (cmd.rfind("seekfrom ", 0) == 0) {
        long long cp = -1, t = -1;
        std::istringstream ss(cmd.substr(9));
        if (ss >> cp >> t && cp >= 0 && t >= 0) {
            itermap::seekToTick(t);
            itermap::g_seekFromCp = cp;
            itermap::g_seekRestart = true;
        } else {
            writeResult("seekfrom: usage: seekfrom <cp tick> <target>");
        }
        return;
    }
    // `seek <tick>`: what clicking the seek bar asks for, for a driver with no mouse on the worker.
    else if (cmd.rfind("seek ", 0) == 0) {
        long long t = -1;
        if (cfgNum("seek", cmd.substr(5), t)) itermap::seekToTick(t);
        return;
    }
    else if (cmd.rfind("step ", 0) == 0) {
        if (cfgNum("step", cmd.substr(5), g_stepTicks))
            log::info("phase2: stepping {} ticks", g_stepTicks);
    }
    else if (cmd == "quit") {
        writeResult("user quit via cmd");
        endSession("user_quit");
        return;
    }
    // The leave-the-level key's twin for a driver with no keyboard: ends the session and leaves the
    // level through PlayLayer::onQuit, as F9 does -- the path on which the game writes what it keeps
    // when a level is left (the platformer save), which `quit` above never reaches.
    else if (cmd == "leave") {
        writeResult("user left the level via cmd");
        endSession("user_quit_hotkey");
        return;
    }
    // The human takeover's end key, for a driver or an agent that has no keyboard on the worker
    // (mod/takeover.hpp): the attempt in progress and the summary are closed as the key closes them.
    else if (cmd == "takeoverend") {
        if (takeover::armed()) takeover::endByKey();
        else writeResult("takeoverend: this session has no takeover");
        return;
    }
    // The key's other half for a driver (cfg takeoverkey): take the input over where the replay
    // stands, as the first press of the key does.
    else if (cmd == "takeovernow") {
        takeover::takeNow("the command");
        return;
    }
    // ...and the replay key's half (F7 while a person plays): leave the mode, replay from the start.
    else if (cmd == "takeoverreset") {
        if (takeover::resetByReplayKey()) {
            // the key's own replay restart follows the reset; a command has to ask for it
            if (auto* pl = PlayLayer::get()) pl->resetLevel();
        } else {
            writeResult("takeoverreset: nothing to leave (only a takeover begun by the key can be)");
        }
        return;
    }
    else if (cmd == "diag") {
        std::string s = "diag: lastDeath=" + std::to_string(solver::g_lastDeathTick)
            + " groundGap=" + std::to_string(solver::g_lastGroundGap)
            + " injAtDeath=" + std::to_string(solver::g_injAtDeath)
            + " planSize=" + std::to_string(g_cfg.inputs.size())
            + " nextInputIdx=" + std::to_string(g_nextInput)
            + " restoreTick=" + std::to_string(solver::g_restoreTickDbg);
        writeResult(s);
        // Trajectory right before death (tick, x, y, grounded)
        std::string tr = "traj:";
        long long d = solver::g_lastDeathTick;
        for (long long t = std::max(1LL, d - 90); t <= d && t < (long long)solver::g_log.size(); t += 6) {
            tr += " " + std::to_string(t) + ":(" + std::to_string((int)solver::g_log[t].x)
                + "," + std::to_string((int)solver::g_log[t].y)
                + (solver::g_log[t].grounded ? ",G)" : ")");
        }
        writeResult(tr);
        return;
    }
    else if (cmd.rfind("secsolve", 0) == 0 || cmd.rfind("secrung", 0) == 0) {
        // Hand the in-process solve session over to the section solver (src/solver/secsolve.hpp):
        //   secsolve <startTick> <targetX> [horizon] [cap]   -- one way, the session ends
        //   secrung  <startTick> <targetX> [horizon] [cap]   -- a RUNG: the answer is spliced
        //                                                       into the plan and the loop resumes
        // Queued here, performed by dpsolve::poll() at an idle frame boundary: the loop stops,
        // its deepest VERIFIED plan replays to <startTick>, a practice checkpoint is dropped
        // there and the section search takes over. Reporting and the session end ("secsolve")
        // are the search's own, exactly as on the cfg-driven served path. Tuning knobs
        // (secyq/secvq/secgrace/...) can be given in the session cfg at open -- without
        // `secsolve=1` and `checkpointat` they are inert until this command fires.
        const bool rung = cmd.rfind("secrung", 0) == 0;
        const std::string what = rung ? "secrung" : "secsolve";
        long long st = -1, hz = -1, cp = -1;
        double tx = 0.0;
        {
            std::istringstream ss(cmd.substr(what.size()));
            ss >> st >> tx;
            if (!(ss >> hz)) hz = -1;
            if (!(ss >> cp)) cp = -1;
        }
        if (st <= 0 || tx <= 0.0) {
            writeResult(what + " cmd: usage: " + what + " <startTick> <targetX> [horizon] [cap]");
            return;
        }
        if (!g_started || g_sessionOver || !g_cfg.dpSolve) {
            writeResult(what + " cmd: refused - no in-process solve session (dpsolve) is open");
            return;
        }
        g_secReqStart = st;
        g_secReqTarget = tx;
        g_secReqHorizon = hz;
        g_secReqCap = cp;
        g_secReqDepth = -1;
        g_secReqRung = rung;
        g_secReqPending = true;
        writeResult(what + " cmd: queued start=" + std::to_string(st)
            + " target=" + std::to_string(tx)
            + " - takes over at the next idle frame boundary");
        return;
    }
    else if (cmd == "status") {
        auto* pl = PlayLayer::get();
        writeResult("status: attempt=" + std::to_string(g_attempt)
            + " tick=" + std::to_string(g_tick)
            + " x=" + (pl && pl->m_player1 ? std::to_string(pl->m_player1->getPositionX()) : "?")
            + " dead=" + (pl && pl->m_player1 ? std::to_string(pl->m_player1->m_isDead) : "?"));
        return;
    }
    writeResult("cmd_ack: " + cmd + " tick=" + std::to_string(g_tick));
}

// The solve loop's own keys, lifted out of loadConfig's chain.
//
// Not a tidy-up: MSVC counts each `else if` as a nesting level and stops at 128, and the chain
// reached it (`error C1061: blocks nested too deeply`, on the line the NEXT key would have been
// written). One `if` per key is the shape the file wants, so the answer is a second chain rather
// than a table -- and `dp*` is the group that grows.
// Returns whether the key was one of these; loadConfig moves on if so, so a name here shadows the
// same name in the main chain (there are none).
inline bool loadDpCfg(const std::string& key, const std::string& val) {
    if (key == "dpselftest") g_cfg.dpSelfTest = (val == "1");
    else if (key == "dpsolve") {
        g_cfg.dpSolve = (val == "1");
        // A solve needs to know where the moving geometry went, and the only source is its
        // own replays. Turned on with the solve rather than left to a separate key: a run
        // without it plans against a level frozen at its entry positions, and on the levels
        // built out of moving parts it cannot get past the first one.
        if (g_cfg.dpSolve) grouptrace::g_on = true;
    }
    else if (key == "dphorizon") cfgNum(key, val, g_cfg.dpHorizon);
    else if (key == "dpstephorizon") cfgNum(key, val, g_cfg.dpStepHorizon);
    else if (key == "dphorizongrow") cfgNum(key, val, g_cfg.dpHorizonGrow);
    else if (key == "dphorizonmax") cfgNum(key, val, g_cfg.dpHorizonMax);
    else if (key == "dpretirestall") cfgNum(key, val, g_cfg.dpRetireStall);
    else if (key == "dpseed") cfgNum(key, val, g_cfg.dpSeed);
    else if (key == "dpsearch") {
        if (val == "dp" || val == "beam" || val == "montecarlo") g_cfg.dpSearch = val;
        else writeResult("cfg: dpsearch=" + val + " is not dp, beam or montecarlo - ignored (dp)");
    }
    else if (key == "dpfastvetoall") g_cfg.dpFastVetoAll = (val == "1");
    else if (key == "dprejoinfull") g_cfg.dpRejoinFull = (val == "1");
    else if (key == "dpoffboardkill") g_cfg.dpOffBoardKill = (val == "1");
    else if (key == "offboardfree") g_cfg.offBoardFree = (val == "1");
    else if (key == "offboardband") g_cfg.offBoardBand = (val == "1");
    else if (key == "dpmaxiters") cfgNum(key, val, g_cfg.dpMaxIters);
    else if (key == "dptopstop") cfgNum(key, val, g_cfg.dpTopStop);
    else if (key == "dpcapenvrepeat") cfgNum(key, val, g_cfg.dpCapEnvRepeat);
    else if (key == "dpmissedportalrepeat") cfgNum(key, val, g_cfg.dpMissedPortalRepeat);
    else if (key == "coinmissrev") g_cfg.coinMissRev = (val == "1");
    else if (key == "coinmisspost") g_cfg.coinMissPost = (val == "1");
    else if (key == "coinmissearly") g_cfg.coinMissEarly = (val == "1");
    else if (key == "coinoverdepth") g_cfg.coinOverDepth = (val == "1");
    else if (key == "coinapproachoff") g_cfg.coinApproachOff = (val == "1");
    else if (key == "coinapproachreach") g_cfg.coinApproachReach = (val == "1");
    else if (key == "routeprereq") g_cfg.routePrereq = (val == "1");
    else if (key == "routeprereqafter") g_cfg.routePrereqAfter = std::max(0, std::atoi(val.c_str()));
    else if (key == "coinmissmove") g_cfg.coinMissMove = (val == "1");
    else if (key == "groupsretime") g_cfg.groupsRetime = (val == "1");
    else if (key == "dpsecauto") g_cfg.dpSecAuto = (val == "1");
    else if (key == "dpsecstall") cfgNum(key, val, g_cfg.dpSecStall);
    else if (key == "dpsecnorec") cfgNum(key, val, g_cfg.dpSecNoRec);
    else if (key == "dpsecmargin") cfgNum(key, val, g_cfg.dpSecMargin);
    else if (key == "dpsecchain") g_cfg.dpSecChain = (val == "1");
    else if (key == "secredoreach") g_cfg.secRedoReach = (val == "1");
    else if (key == "seccpfallback") g_cfg.secCpFallback = (val == "1");
    else if (key == "secthrow") {
        // <length|alloc|both>@<depth>; anything else leaves it off
        const size_t at = val.find('@');
        const std::string kind = val.substr(0, at);
        g_cfg.secThrowKind = kind == "length" ? 1 : kind == "alloc" ? 2 : kind == "both" ? 3 : 0;
        g_cfg.secThrowDepth = at == std::string::npos ? 0 : std::atoi(val.c_str() + at + 1);
        if (g_cfg.secThrowDepth <= 0) g_cfg.secThrowKind = 0;
    }
    else if (key == "dpsecchainspan") cfgNum(key, val, g_cfg.dpSecChainSpan);
    else if (key == "dpsecpinback") cfgNum(key, val, g_cfg.dpSecPinBack);
    else if (key == "dpsecstateprice") cfgNum(key, val, g_cfg.dpSecStatePrice);
    else if (key == "dpseccallprice") cfgNum(key, val, g_cfg.dpSecCallPrice);
    else if (key == "dpseccap") cfgNum(key, val, g_cfg.dpSecCap);
    else if (key == "dpseccaptiers") cfgNum(key, val, g_cfg.dpSecCapTiers);
    else if (key == "dpsecstate") g_cfg.dpSecState = (val == "1");
    else if (key == "dpneedspeed") g_cfg.dpNeedSpeed = (val == "1");
    else if (key == "dpsecsolved") g_cfg.dpSecSolved = (val == "1");
    else if (key == "dpsecrecaway") g_cfg.dpSecRecAway = (val == "1");
    else if (key == "dpsecreuse") g_cfg.dpSecReuse = (val == "1");
    else if (key == "dpsecrent") g_cfg.dpSecRent = (val == "1");
    else if (key == "dpsecboxwait") g_cfg.dpSecBoxWait = (val == "1");
    else if (key == "dpsecrungprior") cfgNum(key, val, g_cfg.dpSecRungPrior);
    else if (key == "dpsecwallbudget") cfgNum(key, val, g_cfg.dpSecWallBudget);
    else if (key == "dpunseenfirst") g_cfg.dpUnseenFirst = (val == "1");
    else if (key == "dpseedpickups") g_cfg.dpSeedPickups = (val == "1");
    else if (key == "obbnolatch") g_cfg.obbNoLatch = (val == "1");
    else if (key == "exportdigits") cfgNum(key, val, solver::g_exportDigits);
    else if (key == "dpseccoinrung") g_cfg.dpSecCoinRung = (val == "1");
    else if (key == "dpendtrigclear") g_cfg.dpEndTrigClear = (val == "1");
    else if (key == "slice") g_cfg.slice = (val == "1");
    else if (key == "slicemin") cfgNum(key, val, g_cfg.sliceMin);
    else if (key == "sliceaddbacks") cfgNum(key, val, g_cfg.sliceAddBacks);
    else if (key == "stagecuts") g_cfg.stageCuts = val;
    else if (key == "stagecutcheck") cfgNum(key, val, g_cfg.stageCutCheck);
    else if (key == "stagecutwindow") cfgNum(key, val, g_cfg.stageCutWindow);
    else if (key == "stagebreak") cfgNum(key, val, g_cfg.stageBreak);
    else if (key == "slicegiveupcopy") g_cfg.sliceGiveUpCopy = (val == "1");
    else if (key == "slicecount") g_cfg.sliceCount = (val == "1");
    else if (key == "slicenoposition") g_cfg.sliceNoPosition = (val == "1");
    else if (key == "sliceverifyrec") g_cfg.sliceVerifyRec = (val == "1");
    else if (key == "sliceverifybreak") cfgNum(key, val, g_cfg.sliceVerifyBreak);
    else if (key == "dpseedplan") g_cfg.dpSeedPlan = val;
    else if (key == "dpshow") cfgNum(key, val, g_cfg.dpShow);
    else if (key == "dpcoinwin") g_cfg.dpCoinWin = (val == "0") ? std::string() : val;
    else if (key == "glitchavoid") {
        const bool on = (val == "1");
        g_cfg.glitchPortal = on ? kGlitchPortalDefault : 0.0;
        g_cfg.glitchWave = on ? kGlitchWaveDefault : 0.0;
        g_cfg.glitchEmbed = on ? kGlitchEmbedDefault : 0.0;
        g_cfg.glitchDeco = on;
    }
    else if (key == "avoidwave") {
        const bool on = (val == "1");
        g_cfg.wavePulse = on ? kWavePulseDefault : 0;
        g_cfg.waveCentre = on ? kWaveCentreDefault : 0.0;
        g_cfg.waveTidy = on ? kWaveTidyDefault : 0.0;
        g_cfg.waveToggle = on ? kWaveToggleDefault : 0;
    }
    else if (key == "wavepulse") cfgNum(key, val, g_cfg.wavePulse);
    else if (key == "wavecentre") cfgNum(key, val, g_cfg.waveCentre);
    else if (key == "wavetidy") cfgNum(key, val, g_cfg.waveTidy);
    else if (key == "wavetoggle") cfgNum(key, val, g_cfg.waveToggle);
    else if (key == "glitchdeco") g_cfg.glitchDeco = (val == "1");
    else if (key == "glitchportal") cfgNum(key, val, g_cfg.glitchPortal);
    else if (key == "glitchwave") cfgNum(key, val, g_cfg.glitchWave);
    else if (key == "glitchembed") cfgNum(key, val, g_cfg.glitchEmbed);
    else if (key == "dpbandtrack") g_cfg.dpBandTrack = (val == "1");
    else if (key == "dprotseed") {
        g_cfg.dpRotSeed = val == "A" ? 1 : val == "E" ? 2 : val == "F" ? 3 : val == "S" ? 4 : 0;
        if (g_cfg.dpRotSeed == 0 && val != "off" && val != "0")
            writeResult("cfg: dprotseed=" + val + " is not one of off|A|E|F|S - left off");
    }
    else if (key == "dprotseedanchor") g_cfg.dpRotSeedAnchor = (val != "0");
    else if (key == "dpsnapshot") g_cfg.dpSnapshot = (val == "1");
    else if (key == "dpcheck") g_cfg.dpCheck = (val == "1");
    else if (key == "dpcheckobs") g_cfg.dpCheckObs = (val == "1");
    else if (key == "dpcheckfirst") g_cfg.dpCheckFirst = (val == "1");
    else if (key == "dpcontenthorizon") cfgNum(key, val, g_cfg.dpContentHorizon);
    else if (key == "dpcapladder") cfgNum(key, val, g_cfg.dpCapLadder);
    else if (key == "dpsigcheck") cfgNum(key, val, g_cfg.dpSigCheck);
    else if (key == "gtrollcheck") cfgNum(key, val, g_cfg.gtRollCheck);
    else if (key == "gtdeathrow") cfgNum(key, val, g_cfg.gtDeathRow);
    else if (key == "gtrestrow") cfgNum(key, val, g_cfg.gtRestRow);
    else if (key == "recenteredfire") cfgNum(key, val, g_cfg.recEnteredFire);
    else if (key == "windowguard") cfgNum(key, val, g_cfg.windowGuard);
    else if (key == "dpplainbeside") cfgNum(key, val, g_cfg.dpPlainBeside);
    else if (key == "dpinputgrid") cfgNum(key, val, g_cfg.dpInputGrid);
    else if (key == "dpphaseprof") g_cfg.dpPhaseProf = (val == "1");
    else if (key == "memcensus") g_cfg.memCensus = (val == "1");
    else if (key == "dpmodelreanchor") g_cfg.dpModelReanchor = std::atoi(val.c_str());
    else if (key == "dpspentorb") {}   // always on since 2026-09-26; accepted so old cfgs parse
    else if (key == "dpwatchfired") {
        g_cfg.dpWatchFired.clear();
        size_t p = 0;
        while (p <= val.size()) {
            const size_t q = val.find(',', p);
            const std::string tok = val.substr(p, q == std::string::npos ? std::string::npos : q - p);
            if (!tok.empty()) g_cfg.dpWatchFired.push_back(std::atoi(tok.c_str()));
            if (q == std::string::npos) break;
            p = q + 1;
        }
    }
    else if (key == "dpfingerprint") g_cfg.dpFingerprint = (val == "1");
    else if (key == "dptouchseednow") g_cfg.dpTouchSeedNow = (val == "1");
    else if (key == "dparg") g_cfg.dpArgs.push_back(val);
    else return false;
    return true;
}

// Trace-only keys. They live outside loadConfig's else-if chain because that chain is at
// MSVC's block-nesting limit (C1061): one more `else if` there does not compile.
inline bool loadTraceCfg(const std::string& key, const std::string& val) {
    if (key == "presstrace") {
        auto c = val.find(',');
        if (c != std::string::npos) {
            long long a = 0, b = -1;
            if (cfgNum(key, val.substr(0, c), a) && cfgNum(key, val.substr(c + 1), b)) {
                g_pressT0 = a;
                g_pressT1 = b;
            }
        }
        return true;
    }
    if (key == "rngfresh") {
        g_rngFresh = (val == "1");
        return true;
    }
    if (key == "rngfix") {
        g_rngFix = (val != "0");
        return true;
    }
    if (key == "attemptone") {
        g_attemptOne = (val == "1");
        return true;
    }
    if (key == "attemptcount") {
        long long k = -1;
        g_attemptCount = (cfgNum(key, val, k) && k >= 0) ? k : -1;
        return true;
    }
    if (key == "rotdbg") {
        const auto c = val.find(',');
        if (c != std::string::npos) { g_rotDbg0 = std::atoll(val.c_str()); g_rotDbg1 = std::atoll(val.c_str() + c + 1); }
        return true;
    }
    if (key == "replaystepdt") {
        g_replayStepDiagDt = (float)std::atof(val.c_str());
        return true;
    }
    if (key == "replaystep") {
        g_replayStep = std::max(0, std::atoi(val.c_str()));
        g_replayStepCfg = g_replayStep;
        return true;
    }
    if (key == "stopfollowers") {
        g_stopFollowers = val != "0";
        return true;
    }
    if (key == "vtscale") {
        g_vtScale = val != "0";
        return true;
    }
    if (key == "nodedbg") {
        const auto c = val.find(',');
        if (c != std::string::npos) { g_nodeDbg0 = std::atoll(val.c_str()); g_nodeDbg1 = std::atoll(val.c_str() + c + 1); }
        return true;
    }
    if (key == "camrand") {
        g_camRand = val != "0";
        return true;
    }
    if (key == "vtime") {
        g_vtime = (float)std::atof(val.c_str());
        return true;
    }
    if (key == "attemptauto") {
        g_attemptAuto = (val != "0");
        return true;
    }
    if (key == "rngseed") {
        auto c = val.find(',');
        long long a = 0, b = 0;
        if (c != std::string::npos && cfgNum(key, val.substr(0, c), a)
            && cfgNum(key, val.substr(c + 1), b)) {
            g_rngSeedSet = true;
            g_rngSeedEE0 = a;
            g_rngSeedEF8 = b;
        }
        return true;
    }
    if (key == "areatrace") {
        // t0,t1,uid[,uid...]
        std::vector<long long> v;
        std::stringstream ss(val);
        for (std::string item; std::getline(ss, item, ',');) {
            long long n = 0;
            if (!cfgNum(key, item, n)) return true;
            v.push_back(n);
        }
        if (v.size() >= 3) {
            g_areaT0 = v[0];
            g_areaT1 = v[1];
            g_areaUids.clear();
            for (size_t i = 2; i < v.size(); ++i) g_areaUids.push_back((int)v[i]);
        }
        return true;
    }
    if (key == "sticktrace") {
        auto c = val.find(',');
        if (c != std::string::npos) {
            long long a = 0, b = -1;
            if (cfgNum(key, val.substr(0, c), a) && cfgNum(key, val.substr(c + 1), b)) {
                g_stickT0 = a;
                g_stickT1 = b;
            }
        }
        return true;
    }
    if (key == "slopetrace") {
        auto c = val.find(',');
        if (c != std::string::npos) {
            long long a = 0, b = -1;
            if (cfgNum(key, val.substr(0, c), a) && cfgNum(key, val.substr(c + 1), b)) {
                g_slopeT0 = a;
                g_slopeT1 = b;
            }
        }
        return true;
    }
    return false;
}

// The solver-facing cfg lines as read (every key starting with "dp", `dparg` included), for the
// `dpconfig:` line at session start.
inline std::vector<std::string> g_dpCfgSeen;

// One line naming the solver configuration this session runs: the dp cfg lines as read, in file
// order, and the loaded package's size/FNV-1a (the solver is linked into it). A batch run under a
// hand-written set of experiment flags checks its first run's line against the set it meant before
// it widens to the rest.
inline std::string dpConfigLine() {
    std::string line = "dpconfig:";
    for (const std::string& s : g_dpCfgSeen) line += " " + s;
    std::string sig = "-";
    const std::string pkg = geode::Mod::get()->getPackagePath().string();
    std::ifstream f(pkg, std::ios::binary);
    if (f) {
        uint64_t h = 1469598103934665603ULL;
        size_t n = 0;
        char buf[65536];
        while (f.read(buf, sizeof(buf)) || f.gcount()) {
            const size_t got = (size_t)f.gcount();
            n += got;
            for (size_t i = 0; i < got; ++i) {
                h ^= (uint8_t)buf[i];
                h *= 1099511628211ULL;
            }
        }
        char b[48];
        snprintf(b, sizeof(b), "%zu/%08x", n, (unsigned)(h & 0xffffffffULL));
        sig = b;
    }
    return line + " package=" + sig;
}

// Every cfg key with the value of what it sets (tools/gen_effective_cfg.py writes it from the
// parsers above into cfg_effective.gen.hpp, included at the end of this file), and that line as
// it stood before the first autorun.cfg was applied: what a key nobody wrote runs with.
namespace effcfg { inline std::string modCfg(); }
inline std::string g_cfgDefaults;

// A test instrument (config.hpp, kTestInstruments) asked for by autorun.cfg: honoured only by a build made for measuring; any other says so in
// the log and ignores the key.
inline bool testInstrumentAvailable(const std::string& key) {
    if (kTestInstruments) return true;
    writeResult("cfg: " + key + " is a test instrument and this build does not carry them; ignored", false);
    return false;
}

inline void loadConfig() {
    std::ifstream f(std::string(DATA_DIR) + "/autorun.cfg");
    if (!f.is_open()) return;
    if (g_cfgDefaults.empty()) g_cfgDefaults = effcfg::modCfg();
    g_cfgRemoved.clear();
    g_dpCfgSeen.clear();
    std::string line;
    int inputMask = 0;   // the buttons held after the last `input=` line (plan_io.hpp pushInputMask)
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = line.substr(0, eq);
        auto val = line.substr(eq + 1);
        if (key.rfind("dp", 0) == 0) g_dpCfgSeen.push_back(key + "=" + val);
        // KEYS REMOVED IN A RELEASE CLEAN-UP. Each switched the loop back to a behaviour that
        // had been measured and replaced. Columns: the key, the commit that removed it, and the
        // earlier one that folded it into the default. A file
        // that still names one is asking for something that no longer exists, so it is
        // refused by name (dpsolve::start will not solve) rather than ignored.
        {
            static const char* const kRemovedCfg[][3] = {
                {"dpadaptivehorizon", "9cb6587", "7613353"},
                {"dpbandend", "9cb6587", "7613353"},
                {"dpctrlwin", "9cb6587", "7613353"},
                {"dpfastveto", "9cb6587", "7613353"},
                {"dpfixp2", "9cb6587", "7613353"},
                {"dphoverstrict", "9cb6587", "7613353"},
                {"dprejoinchain", "9cb6587", "7613353"},
                {"dprejoinuse", "9cb6587", "7613353"},
                {"dprejoinwatch", "9cb6587", "7613353"},
                {"dpswingpending", "9cb6587", "7613353"},
                {"histpayload", "9cb6587", "7613353"},
                // the 0.2.0 clean-up
                {"dptouchentered", "36b14f7", "671e60d"},
                {"dptouchenteredtick", "36b14f7", "671e60d"},
                {"dpgroupholddeath", "36b14f7", "671e60d"},
                // the 0.4.0 clean-up
                {"dpladderback", "ff5d7a4", "9aed903"},
                {"dpladderenv", "ff5d7a4", "f95fba6"},
                {"dpwalkgates", "ff5d7a4", "6cf416c"},
                {"dprotextend", "ff5d7a4", "d008ee2"},
                {"dpslideparts", "ff5d7a4", "d008ee2"},
                {"dpgroupfire", "ff5d7a4", "c6ddea5"},
                {"dprotqtoggle", "ff5d7a4", "856a833"},
                // the 0.6.0 clean-up
                {"offboardtp", "96efcf7", "d008ee2"},
                {"dpxtrack", "96efcf7", "d008ee2"},
                {"portalpayloadlt", "96efcf7", "d008ee2"},
                {"dpanchorrotstep", "96efcf7", "d008ee2"},
                {"dpanchorrot2", "96efcf7", "d008ee2"},
                {"dpfixups", "96efcf7", "a6dd210"},
                {"dpworld", "96efcf7", "a6dd210"},
                {"dpgroups", "96efcf7", "a6dd210"},
                {"anchorboardarm", "96efcf7", "8c8c034"},
                {"headbandfresh", "96efcf7", "8c8c034"},
                {"needpinrelease", "96efcf7", "7c739a6"},
                {"rotseedpre", "96efcf7", "ff34af8"},
                {"dpspentpad", "96efcf7", "dfb5c64"},
                {"dpfineretry", "96efcf7", "27ac5dd"},
                {"anticheatpass", "96efcf7", "d55b964"},
                {"singlereset", "96efcf7", "0fee83c"},
                {"snapplayer", "96efcf7", "bd08fca"},
            };
            bool removed = false;
            for (const auto& r : kRemovedCfg)
                if (key == r[0]) {
                    // Said by dpsolve::start: result.txt is started afresh after this file
                    // is read, so a line written here would not survive.
                    g_cfgRemoved.push_back(key + " was removed in " + std::string(r[1])
                                           + "; the behaviour is fixed (since "
                                           + std::string(r[2]) + ")");
                    removed = true;
                }
            if (removed) continue;
        }
        if (loadDpCfg(key, val)) continue;
        if (loadTraceCfg(key, val)) continue;
        // Not in the chain below, which is at MSVC's block-nesting ceiling (C1061).
        if (key == "areaenv") { g_cfg.areaEnv = (val == "1"); continue; }
        if (key == "framebudgetms") { cfgNum(key, val, g_cfg.frameBudgetMs); continue; }
        if (key == "snapnodesync") { g_cfg.snapNodeSync = (val == "1"); continue; }
        if (key == "snapnodelog") { g_cfg.snapNodeLog = (val == "1"); continue; }
        if (key == "resetlog") { g_cfg.resetLog = (val == "1"); continue; }
        if (key == "resetdump") { g_cfg.resetDump = (val == "1"); continue; }
        if (key == "resetdumpuids") {
            g_cfg.resetDumpUids.clear();
            size_t p = 0;
            while (p < val.size()) {
                size_t q = val.find(',', p);
                if (q == std::string::npos) q = val.size();
                const int u = std::atoi(val.substr(p, q - p).c_str());
                if (u > 0) g_cfg.resetDumpUids.push_back(u);
                p = q + 1;
            }
            continue;
        }
        if (key == "persistworld") { cfgNum(key, val, g_cfg.persistWorld); continue; }
        if (key == "persistitems") { g_cfg.persistItems = val; continue; }
        if (key == "voidband") { g_cfg.voidBand = (val == "1"); continue; }
        if (key == "voidcredit") { g_cfg.voidCredit = (val == "1"); continue; }
        if (key == "stallbound") { g_cfg.stallBound = (val == "1"); continue; }
        if (key == "uishow") { g_cfg.uiShow = val; continue; }
        // Test hook, cfg `f10auto=<s>,<s>,...`: the mod presses F10 itself at those seconds after the showing starts (a replay's
        // first frame, or the solve's clear), through the key's own poll (session_hotkeys.hpp), for the checks that cannot send a key.
        if (key == "f10auto") {
            itermap::g_f10Auto.clear();
            size_t p = 0;
            while (p < val.size()) {
                size_t q = val.find(',', p);
                if (q == std::string::npos) q = val.size();
                itermap::g_f10Auto.push_back(std::atof(val.substr(p, q - p).c_str()));
                p = q + 1;
            }
            continue;
        }
        // Test hook, cfg `panelmouse=<s>:<f>[-<f2>],...`: the replay panel's bar is pressed at fraction <f> of its length <s> seconds after the
        // panel first shows (dragged to <f2> and released there when one is given, a click when not): a mouse the check cannot send.
        if (key == "panelmouse") {
            itermap::g_panelMouseAuto.clear();
            size_t p = 0;
            while (p < val.size()) {
                size_t q = val.find(',', p);
                if (q == std::string::npos) q = val.size();
                const std::string e = val.substr(p, q - p);
                p = q + 1;
                const size_t c = e.find(':');
                if (c == std::string::npos) continue;
                const size_t d = e.find('-', c);
                itermap::g_panelMouseAuto.push_back(std::atof(e.substr(0, c).c_str()));
                itermap::g_panelMouseAuto.push_back(std::atof(e.substr(c + 1, d == std::string::npos ? std::string::npos : d - c - 1).c_str()));
                itermap::g_panelMouseAuto.push_back(d == std::string::npos ? -1.0 : std::atof(e.substr(d + 1).c_str()));
            }
            continue;
        }
        if (key == "dpspeedfirst") { g_cfg.dpSpeedFirst = (val == "1"); continue; }
        if (key == "replaypanel") { cfgNum(key, val, g_cfg.replayPanel); continue; }
        if (key == "replaypanelspan") { cfgNum(key, val, g_cfg.replayPanelSpan); continue; }
        if (key == "replaypanelplans") { cfgNum(key, val, g_cfg.replayPanelPlans); continue; }
        if (key == "replaypanelspeed") { cfgNum(key, val, g_cfg.replayPanelSpeed); continue; }
        if (key == "dphorizongrowmin") { cfgNum(key, val, g_cfg.dpHorizonGrowMin); continue; }
        if (key == "recprovchain") { g_cfg.recProvChain = (val == "1"); continue; }
        if (key == "cpfollowsection") { g_cfg.cpFollowSection = (val == "1"); continue; }
        if (key == "cpflight") { cfgNum(key, val, g_cfg.cpFlight); continue; }
        if (key == "cpflightprobe") { g_cfg.cpFlightProbe = (val == "1"); continue; }
        if (key == "cpflightprobecarry") { g_cfg.cpFlightProbeCarry = (val == "1"); continue; }
        if (key == "cpflightgaps") { g_cfg.cpFlightGaps = (val == "1"); continue; }
        if (key == "cpdisabled") { g_cfg.cpDisabled = (val == "1"); continue; }
        if (key == "cpfollowfloats") { g_cfg.cpFollowFloats = (val == "1"); continue; }
        if (key == "cprotnode") { g_cfg.cpRotNode = (val == "1"); continue; }
        if (key == "cppersistguard") { g_cfg.cpPersistGuard = (val == "1"); continue; }
        if (key == "cpcarryparts") { cfgNum(key, val, g_cfg.cpCarryParts); continue; }
        if (key == "cpflightobj") { cfgNum(key, val, g_cfg.cpFlightObj); continue; }
        if (key == "cpflightobjfrom") { cfgNum(key, val, g_cfg.cpFlightObjFrom); continue; }
        if (key == "cpflightobjto") { cfgNum(key, val, g_cfg.cpFlightObjTo); continue; }
        if (key == "takeover") { cfgNum(key, val, g_cfg.takeoverAt); continue; }
        if (key == "takeoverx") { cfgNum(key, val, g_cfg.takeoverX); continue; }
        if (key == "takeoverwait") { cfgNum(key, val, g_cfg.takeoverWait); continue; }
        if (key == "takeoverkey") { g_cfg.takeoverKey = (val == "1"); continue; }
        if (key == "cpflightprobeat") { cfgNum(key, val, g_cfg.cpFlightProbeAt); continue; }
        if (key == "cpflightlayerdiff") { g_cfg.cpFlightLayerDiff = (val == "1"); continue; }
        if (key == "cpflightcarrylog") { g_cfg.cpFlightCarryLog = (val == "1"); continue; }
        if (key == "cpflightnodewatch") { g_cfg.cpFlightNodeWatch = val; continue; }
        if (key == "slicenotouch") { g_cfg.sliceNoTouch = (val == "1"); continue; }
        if (key == "slicetriggers") { g_cfg.sliceTriggers = (val == "1"); continue; }
        if (key == "slicekeepgroups") { g_cfg.sliceKeepGroups = (val == "1"); continue; }
        if (key == "sliceuids") { g_cfg.sliceUids = (val == "1"); continue; }
        if (key == "slicefirstref") { g_cfg.sliceFirstRef = (val == "1"); continue; }
        if (key == "dplearnresets") { cfgNum(key, val, g_cfg.dpLearnResets); continue; }
        if (key == "secsnapact") { psnap::g_snapAct = (val == "1"); continue; }
        if (key == "secsnapem") { psnap::g_snapEM = (val == "1"); continue; }
        if (key == "secsnapemfrom") { cfgNum(key, val, psnap::g_snapEMFrom); continue; }
        if (key == "secactkey") { psnap::g_actKey = (val == "1"); continue; }
        if (key == "seccpplayer") { secsolve::g_cpPlayer = (val == "1"); continue; }
        if (key == "seccpcheck") { psnap::g_cpCheck = (val == "1"); continue; }
        if (key == "seccover") { secsolve::g_cover = (val == "1"); continue; }
        if (key == "secsizefam") { secsolve::g_sizeFam = (val == "1"); continue; }
        if (key == "secp2key") { secsolve::g_p2Key = (val == "1"); continue; }
        if (key == "secforcesize") { cfgNum(key, val, secsolve::g_forceP1Size); continue; }
        if (key == "seckeepsize") { cfgNum(key, val, secsolve::g_keepP1Size); continue; }
        if (key == "secp2extras") { secsolve::g_p2Extras = (val == "1"); continue; }
        if (key == "secdriftwhere") { g_cfg.secDriftWhere = (val == "1"); continue; }
        if (key == "dpcaptiers") { cfgNum(key, val, g_cfg.dpCapTiers); continue; }
        if (key == "dpsectierfirst") { cfgNum(key, val, g_cfg.dpSecTierFirst); continue; }
        if (key == "dpsecmaxback") { cfgNum(key, val, g_cfg.dpSecMaxBack); continue; }
        if (key == "sectrail") { secsolve::g_trailClear = (val == "1"); continue; }
        if (key == "secgracefly") { cfgNum(key, val, secsolve::g_graceFly); continue; }
        if (key == "secleafmid") { secsolve::g_leafMid = (val == "1"); continue; }
        if (key == "secleaflife") { cfgNum(key, val, secsolve::g_leafLife); continue; }
        if (key == "secdrift") { secsolve::g_driftLog = (val == "1"); continue; }
        if (key == "seekvisevery") { cfgNum(key, val, itermap::g_seekVisEvery); continue; }
        if (key == "seekblind") { itermap::g_seekBlind = (val == "1"); continue; }
        if (key == "profsample") { itermap::g_profSample = (val == "1"); continue; }
        if (key == "sectionhooklazy") { itermap::g_sectionHookLazy = (val == "1"); continue; }
        if (key == "seekcp") { cfgNum(key, val, itermap::g_seekCpEvery); continue; }
        if (key == "seekcpfrom") { cfgNum(key, val, itermap::g_seekCpFrom); continue; }
        if (key == "seekcpto") { cfgNum(key, val, itermap::g_seekCpTo); continue; }
        if (key == "seekcpkeep") { itermap::g_seekCpKeep = (val == "1"); continue; }
        if (key == "latchat") { cfgNum(key, val, itermap::g_latchAt); continue; }
        if (key == "headcheck") { itermap::g_headCheck = (val == "1"); continue; }
        if (key == "regionat") { cfgNum(key, val, itermap::g_regionAt); continue; }
        if (key == "pauseattick") { cfgNum(key, val, itermap::g_pauseAtTick); continue; }
        if (key == "groupcheck") { itermap::g_groupCheck = (val == "1"); continue; }
        if (key == "actcensus") { itermap::g_actCensus = (val == "1"); continue; }
        if (key == "cpactivation") { cfgNum(key, val, g_cfg.cpActivation); continue; }
        if (key == "cpvisafter") { g_cfg.cpVisAfter = (val == "1"); continue; }
        if (key == "objwatch") {
            itermap::g_objWatch.clear();
            std::stringstream ss(val);
            for (std::string u; std::getline(ss, u, ',');)
                if (!u.empty()) itermap::g_objWatch.push_back(std::atoi(u.c_str()));
            continue;
        }
        if (key == "batchlog") {
            if (std::sscanf(val.c_str(), "%lld:%lld", &itermap::g_batchFrom, &itermap::g_batchTo) != 2)
                writeResult("cfg: batchlog=" + val + " is not from:to - ignored");
            continue;
        }
        if (key == "regionbox") {
            float* b = itermap::g_regionBox;
            if (std::sscanf(val.c_str(), "%f:%f:%f:%f", &b[0], &b[1], &b[2], &b[3]) != 4)
                writeResult("cfg: regionbox=" + val + " is not x0:y0:x1:y1 - ignored");
            continue;
        }
        if (key == "secbound") { secsolve::g_boundOn = (val == "1"); continue; }
        if (key == "secshaderskip") { secsolve::g_shaderSkip = (val == "1"); continue; }
        if (key == "secshadersig") { secsolve::g_shaderSig = (val == "1"); continue; }
        if (key == "seccoins") { secsolve::g_secCoins = (val == "1"); continue; }
        if (key == "secrungcoinoff") { secsolve::g_rungCoinOff = (val == "1"); continue; }
        if (key == "secbookfirst") { secsolve::g_bookFirst = (val == "1"); continue; }
        if (key == "secjbkeep") { secsolve::g_jbKeep = (val == "1"); continue; }
        if (key == "secheaddash") { secsolve::g_headDash = (val == "1"); continue; }
        if (key == "secheadaccel") { secsolve::g_headAccel = (val == "1"); continue; }
        // (not on the else-if chain below: one more link there exceeds MSVC's nesting limit, C1061)
        if (key == "secsnapring") { psnap::g_snapRing = (val == "1"); continue; }
        if (key == "secgracedash") { secsolve::g_graceDash = (val == "1"); continue; }
        if (key == "secwsnapauto") { secsolve::g_wsnapAuto = (val == "1"); continue; }
        if (key == "secwsnapgate") { secsolve::g_wsnapGate = (val == "1"); continue; }
        if (key == "seclieskippsnap") { secsolve::g_lieSkipPsnap = (val == "1"); continue; }
        if (key == "secwsnapset") { cfgNum(key, val, secsolve::g_wsnapSet); continue; }
        if (key == "secwsnapany") { secsolve::g_wsnapAny = (val == "1"); continue; }
        if (key == "secwsnaplayer") { cfgNum(key, val, psnap::g_layerInput); continue; }
        if (key == "secwsnapgroups") {
            secsolve::g_wsnapGroups.clear();
            std::stringstream gs(val);
            for (std::string tok; std::getline(gs, tok, ',');)
                if (!tok.empty()) secsolve::g_wsnapGroups.push_back(std::atoi(tok.c_str()));
            continue;
        }
        if (key == "levelfiletype") { g_cfg.levelFileMain = (val == "main"); continue; }
        if (key == "leveldir") { suite::g_levelDir = val; continue; }
        if (key == "suiteplans") { suite::g_planDir = val; continue; }
        if (key == "enabled") g_cfg.enabled = (val == "1");
        else if (key == "level") cfgNum(key, val, g_cfg.levelId);
        // `levels=1,2,3`: solve these in this order, in ONE game (see suite:: in config.hpp).
        // Re-read on every level of the suite, so it must be idempotent -- the list is rebuilt
        // rather than appended to, and g_at is left where the advance put it.
        else if (key == "levels") {
            suite::g_levels.clear();
            size_t i = 0;
            while (i < val.size()) {
                size_t j = val.find(',', i);
                if (j == std::string::npos) j = val.size();
                const std::string one = val.substr(i, j - i);
                if (!one.empty()) {
                    int lv = 0;
                    if (cfgNum(key, one, lv) && lv > 0) suite::g_levels.push_back(lv);
                }
                i = j + 1;
            }
            if (suite::g_at >= suite::g_levels.size()) suite::g_at = 0;
            if (suite::active()) g_cfg.levelId = suite::current();
        }
        else if (key == "levelfile") g_cfg.levelFile = val;
        else if (key == "attempts") cfgNum(key, val, g_cfg.maxAttempts);
        else if (key == "delay") cfgNum(key, val, g_cfg.delaySec);
        else if (key == "quitwhendone") g_cfg.quitWhenDone = (val == "1");
        else if (key == "fps") cfgNum(key, val, g_cfg.fps);
        else if (key == "cbs") cfgNum(key, val, g_cfg.cbs);
        else if (key == "cos") cfgNum(key, val, g_cfg.cos);
        else if (key == "framedt") cfgNum(key, val, g_cfg.framedt);
        else if (key == "slowmo") g_slowmo = std::max(1.f, cfgNumOr(key, val, g_slowmo));
        else if (key == "blockinput") g_cfg.blockInput = (val == "1");
        else if (key == "progblock") g_cfg.progressBlock = (val == "1");
        else if (key == "fastdt") cfgNum(key, val, g_cfg.fastdt);
        else if (key == "fastloops") cfgNum(key, val, g_cfg.fastloops);
        else if (key == "skiprender") g_cfg.skipRender = (val == "1");
        // The spectating notch, as an INDEX into WATCH_SPEEDS (config.hpp) -- the keys move by
        // notch, so a rig that reproduces them has to as well. Through watchSpeedSet, which is
        // the setter the arrow keys use: poking g_watchSpeed directly would let the harness
        // reach a state the keys cannot, which is the mistake the note on renderTogglePress
        // records for the render key.
        //
        // It exists because the audio bug of 2026-08-31 could not be reproduced without it. The
        // case needs the notch held above kPitchMax while the loop keeps resetting the level,
        // and there was no way to ask for that except by holding down an arrow key.
        else if (key == "watchspeed") {
            int idx = g_watchIdx;
            if (cfgNum(key, val, idx)) watchSpeedSet(idx);
        }
        else if (key == "watchat") cfgNum(key, val, g_watchAtTick);
        else if (key == "watchatx") cfgNum(key, val, g_watchAtX);
        else if (key == "watchback") cfgNum(key, val, g_watchBackTicks);
        else if (key == "watchafter") cfgNum(key, val, g_watchAfterSec);
        else if (key == "watchcycle") cfgNum(key, val, g_watchCycleSec);
        else if (key == "fxsweep") g_fxSweep = (val == "1");
        else if (key == "visrefresh") g_visRefreshOn = (val == "1");
        else if (key == "watchpurge") g_watchPurge = (val == "1");
        else if (key == "endpurge") g_endPurge = (val == "1");
        else if (key == "endburn") g_endzoneBurnOn = (val == "1");
        else if (key == "syncprobe") g_syncProbe = (val == "1");
        // Start with the iteration map already drawn, or explicitly not. For filming and for the
        // replay-watching tools, which cannot press a key. The seek bar under it is always there
        // and has no key of its own.
        //
        // Either value is an ANSWER, including 0: a run that says itermap=0 has asked for it off
        // and gets it off, even in the solve session that would otherwise have drawn it (see
        // mapWanted). Only the absence of the key leaves the mode to decide.
        else if (key == "itermap") itermap::g_mapPref = (val == "1") ? 1 : 0;
        else if (key == "trigtrace") g_trigTrace = (val == "1");
        // Observation of moving gates: gatetrace=x0,x1 is the target x window, gatetick=t0,t1
        // the recording tick window
        else if (key == "gatetrace") {
            auto c = val.find(',');
            if (c != std::string::npos) {
                if (cfgNum(key, val.substr(0, c), solver::g_gateX0)
                    && cfgNum(key, val.substr(c + 1), solver::g_gateX1))
                    solver::g_gateTrace = true;
            }
        }
        else if (key == "gatestride")
            solver::g_gateStride = std::max(1LL, cfgNumOr(key, val, solver::g_gateStride));
        else if (key == "servemode") g_serveMode = (val == "1");
        // State-injection probe: colprobe=T,y0,ystep,vy,n (explained in solver.hpp)
        else if (key == "colprobe") {
            std::vector<std::string> f;
            size_t p = 0;
            while (true) {
                auto c = val.find(',', p);
                f.push_back(val.substr(p, c == std::string::npos ? c : c - p));
                if (c == std::string::npos) break;
                p = c + 1;
            }
            if (f.size() == 5
                && cfgNum(key, f[0], solver::g_cpT) && cfgNum(key, f[1], solver::g_cpY0)
                && cfgNum(key, f[2], solver::g_cpYStep) && cfgNum(key, f[3], solver::g_cpVy)
                && cfgNum(key, f[4], solver::g_cpN)) {
                solver::g_colProbe = true;
            }
        }
        else if (key == "gatetick") {
            auto c = val.find(',');
            if (c != std::string::npos) {
                cfgNum(key, val.substr(0, c), solver::g_gateT0);
                cfgNum(key, val.substr(c + 1), solver::g_gateT1);
            }
        }
        else if (key == "lagms") cfgNum(key, val, g_lagMs);
        else if (key == "lagat") cfgNum(key, val, g_lagAtTick);
        // Kill attempts that missed a speed portal (default OFF=detect and report only. In levels
        // with portals on another lane it can kill legitimate routes too, so enable it
        // explicitly per level)
        else if (key == "speedgate") speedgate::g_gate = (val == "1");
        else if (key == "deathfx") g_deathFx = (val == "1");
        else if (key == "hud") g_hudOn = (val == "1");
        else if (key == "retryafter") cfgNum(key, val, g_retryAfterSec);
        else if (key == "notrace") g_cfg.noTrace = (val == "1");
        // cfg `hitboxes=<0|1|2>`: start with GD's hitbox drawing off / on / on-and-nothing-else.
        // The same state F6 cycles through, reachable without a keyboard (for filming, and for
        // checking the display from a headless harness). Clamped and applied right away -- an
        // out-of-range value used to leave g_mode disagreeing with what showArt() had actually
        // hidden, the same desync cycle() now guards against on its own hotkey path.
        else if (key == "hitboxes") {
            hitbox::g_mode = std::clamp(cfgNumOr(key, val, hitbox::g_mode), 0, 2);
            hitbox::apply();
        }
        else if (key == "dumpearly") g_cfg.dumpEarly = (val == "1");
        else if (key == "endtrace") g_cfg.endTrace = (val == "1");
        else if (key == "orbtrace") g_cfg.orbTrace = (val == "1");
        else if (key == "orbtracex") cfgNum(key, val, g_cfg.orbTraceX);
        else if (key == "subringspent") g_cfg.subRingSpent = (val == "1");
        else if (key == "padtrace") g_cfg.padTrace = (val == "1");
        // Five keys, ONE branch, and not for tidiness: this else-if chain is at
        // MSVC's block-nesting ceiling, and adding a plain `else if` here is
        // C1061 "nesting level too deep" -- the whole mod stops building. The
        // next key added has to share a branch the same way, or the chain has
        // to be broken into a second function.
        else if (key == "touchpayload" || key == "portalpayload" || key == "capture"
                 || key == "histride" || key == "anchorframe2dir")
            (key == "touchpayload" ? g_cfg.touchPayload
             : key == "portalpayload" ? g_cfg.portalPayload
             : key == "histride" ? g_cfg.histRide
             : key == "anchorframe2dir" ? g_cfg.anchorFrame2Dir
             : g_cfg.researchCapture)
                = (val == "1");
        else if (key == "snaptrace") g_cfg.snapTrace = (val == "1");
        // `fieldprobe` shares this branch rather than taking one of its own: the chain is at
        // MSVC's C1061 nesting ceiling (see the note above at "touchpayload"), so a plain
        // `else if` here does not fail this key -- it fails the whole mod build.
        // killersite rides this branch rather than adding another `else if`:
        // the chain is at MSVC's block-nesting limit (C1061), which is what the
        // note above means by "fails the whole mod build".
        // dppin rides it for the same reason.
        else if (key == "hitboxtrace" || key == "fieldprobe"
                 || key == "killersite" || key == "dppin" || key == "dptap" || key == "dpswitch"
                 || key == "dpincanchor" || key == "dprlspread0" || key == "dpstatecross"
                 || key == "dpareaadd" || key == "dpcrosscx" || key == "dpcrossbehind" || key == "dpdualbornlinked" || key == "dpbirthrung" || key == "dpwaveflipcap" || key == "dpportalobbmode" || key == "dprampseatall" || key == "dpslopelagplain" || key == "dpwaveflipcapfloor" || key == "dpwaveflipceilup" || key == "dpwaveportalrotmini" || key == "dpballseatground" || key == "dprobotslopenohover" || key == "dpridesupgrace")
            (key == "hitboxtrace" ? g_cfg.hitboxTrace
             : key == "fieldprobe" ? g_cfg.fieldProbe
             : key == "dppin" ? g_cfg.dpPin
             : key == "dptap" ? g_cfg.dpTap
             : key == "dpswitch" ? g_cfg.dpSwitch
             : key == "dpincanchor" ? g_cfg.dpIncAnchor
             : key == "dprlspread0" ? g_cfg.dpRlSpread0
             : key == "dpstatecross" ? g_cfg.dpStateCross
             : key == "dpareaadd" ? g_cfg.dpAreaAdd
             : key == "dpcrosscx" ? g_cfg.dpCrossCx
             : key == "dpcrossbehind" ? g_cfg.dpCrossBehind
             : key == "dpdualbornlinked" ? g_cfg.dpDualBornLinked
             : key == "dpbirthrung" ? g_cfg.dpBirthRung
             : key == "dpwaveflipcap" ? g_cfg.dpWaveFlipCap
             : key == "dpportalobbmode" ? g_cfg.dpPortalObbMode
             : key == "dprampseatall" ? g_cfg.dpRampSeatAll
             : key == "dpslopelagplain" ? g_cfg.dpSlopeLagPlain
             : key == "dpwaveflipcapfloor" ? g_cfg.dpWaveFlipCapFloor
             : key == "dpwaveflipceilup" ? g_cfg.dpWaveFlipCeilUp
             : key == "dpwaveportalrotmini" ? g_cfg.dpWavePortalRotMini
             : key == "dpballseatground" ? g_cfg.dpBallSeatGround
             : key == "dprobotslopenohover" ? g_cfg.dpRobotSlopeNoHover
             : key == "dpridesupgrace" ? g_cfg.dpRideSupGrace
             : g_cfg.killerSite) = (val == "1");
        // dprlbound shares hbfrom's branch (same chain limit)
        else if (key == "hbfrom" || key == "dprlbound" || key == "dpcrossminlag")
            (key == "hbfrom" ? g_cfg.hbFrom : key == "dpcrossminlag" ? g_cfg.dpCrossMinLag : g_cfg.dpRlBound) = std::atoll(val.c_str());
        else if (key == "hbto") g_cfg.hbTo = std::atoll(val.c_str());
        else if (key == "watchuid") g_cfg.watchUid = std::atoi(val.c_str());
        else if (key == "standtrace") g_cfg.standTrace = (val == "1");
        // Timeline of moving geometry (src/solver/grouptrace.hpp). Required for levels with moving
        // objects
        else if (key == "grouptrace") grouptrace::g_on = (val == "1");
        // Measuring mode: track EVERY object (see g_all's note there). For finding
        // movers the normal filters miss; not for solving runs, where rows matter.
        else if (key == "grouptraceall") grouptrace::g_all = (val == "1");
        else if (key == "groupstride")
            grouptrace::g_stride = std::max(1LL, cfgNumOr(key, val, grouptrace::g_stride));
        // Swallow deaths and keep running (observation only: take the recording to the end in a
        // single run). Hitboxes are untouched, so portals and triggers fire as usual
        // 1 both bodies, 2 only player 1's deaths, 3 only player 2's (config.hpp noDeathCode)
        else if (key == "nodeath") g_cfg.noDeath = ((g_cfg.noDeathCode = std::atoi(val.c_str())) == 1);
        else if (key == "clearance") clearance::g_on = (val == "1");
        else if (key == "practiceat") cfgNum(key, val, g_cfg.practiceAt);
        else if (key == "checkpointat") cfgNum(key, val, g_cfg.checkpointAt);
        else if (key == "restoreat") cfgNum(key, val, g_cfg.restoreAt);
        // The restore bench's four keys share one branch of the chain. MSVC's
        // nesting limit for an else-if chain is 128 and this parser is at it:
        // adding a fifth key as its own `else if` fails the build outright
        // (C1061, 2026-09-02). Any further key here goes inside this block.
        else if (key.rfind("restoreloop", 0) == 0) {
            if (key == "restoreloopkeep") g_cfg.restoreLoopKeep = (val == "1");
            else if (key == "restoreloophold") cfgNum(key, val, g_cfg.restoreLoopHold);
            else if (key == "restoreloopcycle") g_cfg.restoreLoopCycle = (val == "1");
            else if (key == "restoreloopclearq") g_cfg.restoreLoopClearQ = (val == "1");
            else if (key == "restoreloopdrain") g_cfg.restoreLoopDrain = (val == "1");
            else if (key == "restoreloopheap") cfgNum(key, val, g_cfg.restoreLoopHeapMB);
            else if (key == "restoreloopdrainat") cfgNum(key, val, g_cfg.restoreLoopDrainAt);
            else if (key == "restoreloop") cfgNum(key, val, g_cfg.restoreLoop);
        }
        else if (key == "oobtest") g_cfg.oobTest = (val == "1");
        else if (key == "vytest") { g_cfg.vyTest = std::atof(val.c_str());
                                    g_cfg.vyTestOn = true; }
        else if (key == "snapverify") cfgNum(key, val, g_cfg.snapVerify);
        else if (key == "robodbg") {
            const size_t c = val.find(',');
            if (c != std::string::npos) {
                g_cfg.roboDbg0 = cfgNumOr<long long>(key, val.substr(0, c), -1);
                g_cfg.roboDbg1 = cfgNumOr<long long>(key, val.substr(c + 1), -1);
            } else {
                writeResult("cfg: robodbg=" + val + " wants t0,t1 - ignored");
            }
        }
        else if (key == "snapat") {
            g_cfg.snapAt.clear();
            std::stringstream ss(val);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                if (tok.empty()) continue;
                g_cfg.snapAt.push_back(std::atoi(tok.c_str()));
            }
            std::sort(g_cfg.snapAt.begin(), g_cfg.snapAt.end());
        }
        // Section solver (src/solver/secsolve.hpp)
        else if (key == "secsolve") secsolve::g_on = (val == "1");
        else if (key == "secstart") cfgNum(key, val, secsolve::g_startTick);
        else if (key == "sectarget") cfgNum(key, val, secsolve::g_targetX);
        else if (key == "sechorizon") cfgNum(key, val, secsolve::g_horizon);
        else if (key == "seccap") cfgNum(key, val, secsolve::g_cap);
        else if (key == "secverify") secsolve::g_verify = (val == "1");
        else if (key == "secspine") secsolve::g_spineOn = (val == "1");
        else if (key == "secdeadline") cfgNum(key, val, secsolve::g_deadlineSec);
        else if (key == "secspineoff") cfgNum(key, val, secsolve::g_spineOff);
        else if (key == "seclog") secsolve::g_log = (val == "1");
        else if (key == "secdt") cfgNum(key, val, secsolve::g_dt);
        else if (key == "secoff") cfgNum(key, val, secsolve::g_off);
        else if (key == "secpsnap") cfgNum(key, val, secsolve::g_snapCmp);
        else if (key == "secpsnapreps") cfgNum(key, val, secsolve::g_snapReps);
        else if (key == "secsnap") cfgNum(key, val, secsolve::g_snapMode);
        else if (key == "secrephase") cfgNum(key, val, secsolve::g_rephase);
        else if (key == "secworld") cfgNum(key, val, secsolve::g_worldDiff);
        else if (key == "secmaxmov") cfgNum(key, val, secsolve::g_maxMoving);
        else if (key == "secnokill") cfgNum(key, val, secsolve::g_killOverride);
        else if (key == "seckilllog") secsolve::g_killLog = (val == "1");
        else if (key == "secvis") secsolve::g_visRefresh = (val == "1");
        else if (key == "secpos") psnap::g_keepSnapPos = (val == "1");
        else if (key == "secoverlay") cfgNum(key, val, secsolve::g_overlay);
        else if (key == "secmasklo") cfgNum(key, val, psnap::g_maskLo);
        else if (key == "secmaskhi") cfgNum(key, val, psnap::g_maskHi);
        else if (key == "secskipextras") psnap::g_skipExtras = (val == "1");
        else if (key == "secmemat") cfgNum(key, val, secsolve::g_memAt);
        else if (key == "secmaskexlo") cfgNum(key, val, psnap::g_maskExLo);
        else if (key == "secmaskexhi") cfgNum(key, val, psnap::g_maskExHi);
        else if (key == "secsnapobj") psnap::g_snapCollideObj = (val == "1");
        else if (key == "secwinshift") cfgNum(key, val, secsolve::g_winShift);
        else if (key == "seccollog") secsolve::g_colLog = (val == "1");
        else if (key == "secverifyevery") cfgNum(key, val, secsolve::g_verifyEvery);
        else if (key == "secverifytol") cfgNum(key, val, secsolve::g_verifyTol);
        else if (key == "secanchor") secsolve::g_anchor = (val == "1");
        else if (key == "secyq") cfgNum(key, val, secsolve::g_yq);
        else if (key == "secvq") cfgNum(key, val, secsolve::g_vq);
        // The same meaning as leveldp's --deadband, for the section solver. Format is
        // "x0,x1[,mode];x0,x1[,mode];..." (duplicate cfg keys can collapse, so packed with ;).
        //
        // What happens without it is measured (2026-08-12): the arc of a gravity-flipped ball
        // rising off-screen at x=2,812 survives for 1,200 ticks, so it passes straight through
        // secgrace (600) and the section solver buys it as "a solution that survives past
        // targetX". The driver's deepest-first puts it on the throne, and since the DP correctly
        // rejects it with the band it can never win — 61 iterations of idling at x=4,911.
        else if (key == "secdeadband") {
            size_t pos = 0;
            while (pos < val.size()) {
                size_t semi = val.find(';', pos);
                if (semi == std::string::npos) semi = val.size();
                const std::string one = val.substr(pos, semi - pos);
                double x0 = 0, x1 = 0, y0 = -1e18, y1 = 1e18; int m = -1;
                const int n = std::sscanf(one.c_str(), "%lf,%lf,%d,%lf,%lf",
                                          &x0, &x1, &m, &y0, &y1);
                if (n >= 2)
                    secsolve::g_secBands.push_back(
                        {x0, x1, n >= 3 ? m : -1,
                         n >= 4 ? y0 : -1e18, n >= 5 ? y1 : 1e18});
                pos = semi + 1;
            }
        }
        else if (key == "secxq") cfgNum(key, val, secsolve::g_xq);
        else if (key == "secjumpbuf") secsolve::g_jumpBuf = (val == "1");
        else if (key == "sectargety") cfgNum(key, val, secsolve::g_targetY);
        else if (key == "sectargetydir") cfgNum(key, val, secsolve::g_targetYDir);
        else if (key == "sectargetdepth") cfgNum(key, val, secsolve::g_targetDepth);
        else if (key == "secgrace") cfgNum(key, val, secsolve::g_grace);
        else if (key == "secmaxdoomed") cfgNum(key, val, secsolve::g_maxDoomed);
        else if (key == "secmaxy") cfgNum(key, val, secsolve::g_maxY);
        // Wall-clock budget per slice before the search hands the frame back (0 = the old
        // single-frame search). See the note on secsolve::SecTask.
        else if (key == "secslicems") cfgNum(key, val, secsolve::g_sliceMs);
        // Notification self-test (see notify.hpp): show one, purge under it, report.
        else if (key == "notifytest") notify::g_test = (val == "1");
        else if (key == "notifyfix") notify::g_fix = (val == "1");
        else if (key == "music") g_cfg.music = val;
        else if (key == "heapcheck") cfgNum(key, val, g_heapCheckEvery);
        // The record gate's layers (record_gate.hpp). A chain of their own: the one above was at the
        // compiler's limit when these were added (see the note below).
        if (key == "netblock") g_cfg.netBlock = (val == "1");
        else if (key == "saveblock") g_cfg.saveBlock = (val == "1");
        else if (key == "memaudit") g_cfg.memAudit = (val == "1");
        else if (key == "nettest") g_cfg.netTest = (val == "1");
        else if (key == "savetest") g_cfg.saveTest = (val == "1");
        // TEST ONLY (see g_netCatchTestOnly): 1 drops every request the process makes, a player's too;
        // "deathdata" drops only the death-data upload
        else if (key == "netcatch_testonly") g_netCatchTestOnly = testInstrumentAvailable(key) ? ((val == "1") ? 1 : (val == "deathdata") ? 2 : 0) : 0;
        // TEST ONLY (see g_netTraceTestOnly): names requests and their answers, never their text
        else if (key == "nettrace_testonly") g_netTraceTestOnly = testInstrumentAvailable(key) && (val == "1");
        // TEST ONLY (see g_deathTestTestOnly): makes the game build a death-data upload for the level entered
        else if (key == "deathtest_testonly") g_deathTestTestOnly = testInstrumentAvailable(key) && (val == "1");
        // TEST ONLY (see g_heapPhaseTestOnly): a heap check at each step of the section search
        else if (key == "heapphase_testonly") g_heapPhaseTestOnly = testInstrumentAvailable(key) ? std::max(0, std::atoi(val.c_str())) : 0;
        else if (key == "phasefile_testonly") g_phaseFileTestOnly = testInstrumentAvailable(key) && (val == "1");
        // Break the chain here (`if`, not `else if`). A huge else-if chain hits MSVC's nesting
        // limit (C1061). The keys are mutually exclusive, so the meaning is unchanged.
        // If a new key triggers C1061, cut one more level the same way
        if (key == "ckpttrace") solver::ckpttrace::g_on = (val == "1");
        // Pause once the player's x passes this (a tool to freeze and film the screen right
        // before a wall)
        else if (key == "pauseatx") cfgNum(key, val, g_pauseAtX);
        else if (key == "clearmargin") cfgNum(key, val, g_clearMargin);
        else if (key == "uisim") g_uiSession = (val == "1"); // for tests: treat autorun as panel
        // For tests: preset g_uiMode for the session setup (0=Normal 1=Replay 2=Solve); the play menu itself opens on what the level has stored
        else if (key == "uimode") cfgNum(key, val, g_uiMode);
        // One branch for both keys: this else-if chain is at MSVC's block-nesting limit (C1061),
        // so a new key joins an existing branch. coinroute implies coins whatever the order.
        else if (key == "coins" || key == "coinroute" || key == "coinwatch" || key == "worldgates") {
            if (key == "coinroute") g_cfg.coinRoute = (val == "1");
            else if (key == "coinwatch") g_cfg.coinWatch = (val == "1");
            else if (key == "worldgates") g_cfg.worldGates = (val == "1");
            else g_cfg.coinMode = (val == "1");
            if (g_cfg.coinRoute) g_cfg.coinMode = true;
        }
        else if (key == "toggle") {
            auto comma = val.find(',');
            int at = 0;
            if (comma != std::string::npos && cfgNum(key, val.substr(0, comma), at)) {
                g_cfg.toggles.push_back({ at, val.substr(comma + 1) });
            }
        }
        else if (key == "planfile") {
            // A plan file (plan_io.hpp loadInputsFile), an attempt sequence included: its last block is
            // g_cfg.inputs, the blocks before it the prelude.
            std::vector<InputCmd> p;
            if (loadInputsFile(val, p)) g_cfg.inputs = std::move(p);
            else writeResult("planfile: could not read " + val);
        }
        else if (key == "input") {
            auto comma = val.find(',');
            int at = 0;
            if (comma != std::string::npos && cfgNum(key, val.substr(0, comma), at)) {
                // The level is a mask of the buttons held (bit 0 player 1's jump, bit 1 player 2's), read as
                // a plan file reads it. A level that is not a number is a release, as it was.
                const int mask = atoi(val.c_str() + comma + 1);
                std::vector<InputCmd> edges;
                if (pushInputMask(edges, at, mask, inputMask))
                    for (const InputCmd& e : edges) g_cfg.inputs.push_back(e);   // (tools/gen_effective_cfg.py reads the target off this line)
                else
                    writeResult("cfg: input=" + val + " holds buttons this version does not drive - not used");
            }
        }
    }
    // cfg suiteplans: the plan of the level the suite is at (config.hpp suite::g_planDir)
    if (!suite::g_planDir.empty() && suite::active()) {
        const std::string planPath = suite::g_planDir + "/solution_lv" + std::to_string(suite::current()) + "_dp.txt";
        std::vector<InputCmd> sp;
        if (loadInputsFile(planPath, sp)) {
            g_cfg.inputs = std::move(sp);
            writeResult("suiteplans: level=" + std::to_string(suite::current()) + " plan=" + planPath
                        + " inputs=" + std::to_string(g_cfg.inputs.size()));
        } else {
            g_cfg.inputs.clear();
            writeResult("suiteplans: could not read " + planPath + " - level " + std::to_string(suite::current()) + " plays no input");
        }
    }
    log::info("phase1 config: enabled={} level={} attempts={} inputs={}",
        g_cfg.enabled, g_cfg.levelId, g_cfg.maxAttempts, g_cfg.inputs.size());
}

inline void endSession(const std::string& why) {
    if (g_sessionOver) return;
    // The loop's rate as a section handoff saved it: only a handoff in THIS session may use it
    // for the prefix replay (the fast loop in hooks_gamelayer), not a cfg-driven section run in
    // the next one.
    g_secSaveFastloops = 1;
    resetSecsolveSession();
    // The search's run ends with the session, HOWEVER it ended, and says so (dp_bridge runEnd): a
    // run left open is the carry-over the next level's start refuses.
    writeResult(dpbridge::runEnd());
    // The iteration map, HOWEVER the session ended.
    //
    // It used to be written from two places only -- the clear, and giveUp -- which between them
    // cover a solve that ran to one end or the other and nothing else. Leave the level while the
    // loop is still working and onQuit calls this and then resetSessionState(), which drops the
    // records; every round the run had recorded, tails included, went with them. That is the
    // ordinary way to end a solve you are watching, so in practice no run ever produced a file
    // with tails in it, and every map in data/ was a log rebuild that cannot have them. Reported
    // 2026-08-29 as "the trajectories are not drawn".
    //
    // This is the one funnel every ending goes through, so it is the place. save() writes nothing
    // when there is nothing (a plain replay records nothing), and the two callers above stay: they
    // file the map at the moment it is complete, next to the solution, rather than at teardown.
    if (itermap::save(g_cfg.levelId, why == "level_complete"))
        writeResult("dpsolve: itermap saved -> " + itermap::pathFor(g_cfg.levelId));
    // Where a slice (level_slice.hpp) left the session: which level was in play at the end.
    {
        const std::string s = levelslice::summary();
        if (!s.empty()) writeResult(s);
    }
    // Do not call purgeDanglingActions() here: when endSession runs on a clear, the actions of
    // GD's result visual effects are alive, and wiping them makes it look stuck with the result
    // screen never appearing. `endpurge=1` is the old behaviour, kept only for disproof
    if (g_endPurge) purgeDanglingActions();
    // Make visible every session whether the achievement/statistics blocking was in effect
    writeResult("deathfx: suppressed=" + std::to_string(g_deathFxSkipped)
        + " circleWavesSwept=" + std::to_string(g_fxSwept));
    writeResult("deaths: p1=" + std::to_string(solver::g_deathsP1)
        + " p2=" + std::to_string(solver::g_deathsP2)
        + " other=" + std::to_string(solver::g_deathsOther));
    writeResult("anticheat: destroyPlayer calls with GD's anti-cheat spike="
        + std::to_string(solver::g_anticheatCalls) + " (counted, not logged per call)");
    solver::g_anticheatCalls = 0;
    solver::g_killTick = -1;   // cfg voidband: the last killer belongs to this session
    solver::g_killUid = -1;
    solver::g_killX = solver::g_killY = 0.f;
    writeResult("avswallowed: visit=" + std::to_string(g_visitAVs)
        + " updateVisibility=" + std::to_string(g_visAVs)
        + " rendertoggles=" + std::to_string(g_watchFlips)
        + " reentrantUpdates=" + std::to_string(g_reentrantUpdates));
    {
        char d[256];
        hookdepth::format(d, sizeof(d));
        writeResult(std::string("hookdepth:") + (d[0] ? d : " (all 1)"));
    }
    writeResult(fxCensus("session_end"));
    // Sound and notifications: both are things the operator reports as "it did X", and both are
    // otherwise invisible in a log. `whileSolving` counts what audio::silent() let through before
    // the showing began; `notifyForced` counts the notifications the mod had to re-arm or take
    // off the screen itself (the purge, see notify.hpp).
    writeResult("audio: soundWhileSolving=" + std::to_string(g_soundWhileSolving)
        + " blocked=" + std::to_string(g_soundBlockedWhileSolving) + " reasserts=" + std::to_string(g_silenceReasserts)
        + " notifyForced=" + std::to_string(notify::g_forced)
        + " secSongSeeksSkipped=" + std::to_string(g_secSongSeeksSkipped));
    // NOT notify::clear() here. giveUp() raises its "could not solve, stopped at N%" and then
    // ends the session, and the level is deliberately left standing so the operator can see
    // where the bot stopped -- clearing here would wipe the one message that explains the
    // screen they are looking at. Ours is let go when the LEVEL is left (PlayLayer::onQuit).
    writeResult("focus: blocked background=" + std::to_string(g_bgBlocked)
        + " resignActive=" + std::to_string(g_resignBlocked)
        + " unfocusPause=" + std::to_string(g_unfocusPauseBlocked));
    writeResult("blocked during this session: achievements=" + std::to_string(g_blockedAch)
        + " stats=" + std::to_string(g_blockedStat)
        + " coins=" + std::to_string(g_blockedCoin)
        + " progress=" + std::to_string(g_blockedProgress)
        + " send=" + std::to_string(g_blockedSend)
        + " saves=" + std::to_string(g_blockedSave)
        + " rollover=" + std::to_string(g_rolloverSkipped));
    // Touch triggers the SECOND player set off. dp's markTouched reads p1's
    // position alone, so any count above zero names a trigger the model can
    // never fire -- it would plan against a world where that trigger does not
    // exist. It is reported and never carried into an anchor, because seeding
    // a bit the step function cannot set forward would make an anchor claim
    // what a whole run of the same plan would not.
    //
    // This line is the ONLY observer of that case. The model-side check
    // compares against the model's own whole run, and lv20 -- the one level
    // with both a dual portal and touch boxes -- dies at t=1,157 while its
    // dual section starts near t=17,119. GD replays lv20 to the end, so this
    // counter reaches what that check cannot.
    //
    // The p1 count and the hook's call count travel WITH the zero, because a
    // zero alone cannot separate "watched and saw none" from "never watched" --
    // the reading that stalled three measurements the day this was written.
    // The claim "p2 set off no trigger" is only worth anything when the
    // numbers beside it are non-zero.
    writeResult("p2 activated trigger boxes: " + std::to_string(touchseed::g_p2)
        + " (p1: " + std::to_string((int)touchseed::g_first.size())
        + ", hook calls: " + std::to_string(touchseed::g_calls)
        + ") -- non-zero p2 means the model cannot fire a trigger this run did");
    // ...and the map itself, which is what the payload is cut from. A payload is
    // per-anchor (the entries at or before t0) and the anchors only exist inside
    // a solve, so checking the seeding through them would cost a solve per
    // measurement. The whole map comes out of a PLAIN REPLAY -- half a minute --
    // and every anchor's payload is a prefix of it, so the offline check can
    // build the payload for any t0 itself. The maps and these counts are per
    // attempt (cleared when one starts), so after a solve they are the last one's.
    {
        std::string m = "touchseed map:";
        bool first = true;
        for (const auto& kv : touchseed::g_first) {
            m += (first ? " " : ",") + std::to_string(kv.first) + ":"
               + std::to_string(kv.second);
            first = false;
        }
        writeResult(m);
    }
    // ...and the touch triggers GD actually fired (touchfire, playerTouchedTrigger), uid order, with the
    // hook's own counts: the map above holds orbs and portals, never these.
    {
        std::vector<std::pair<int, int>> v(touchfire::g_first.begin(), touchfire::g_first.end());
        std::sort(v.begin(), v.end());
        std::string m = "touchfire map (p1 " + std::to_string(v.size()) + ", p2 calls "
                        + std::to_string(touchfire::g_p2) + ", calls " + std::to_string(touchfire::g_calls) + "):";
        bool first = true;
        for (const auto& kv : v) {
            m += (first ? " " : ",") + std::to_string(kv.first) + ":" + std::to_string(kv.second);
            first = false;
        }
        writeResult(m);
    }
    // ...and the same for the gravity portals (see portalseed). Same shape and
    // the same reason: a plain replay writes the whole map, and any anchor's
    // portal mask is the prefix of it at or before t0.
    // The FIRST thing this line answers is whether activatedByPlayer is the
    // right source at all. lv22 uid 13833 must read 6,300 -- the tick the ccl
    // probe watched hasBeenActivated go up -- and if it is absent or reads
    // something else, the flag those virtual methods return is not this one and
    // the mask has to come from the collision path instead.
    {
        std::string m = "portalseed map:";
        bool first = true;
        for (const auto& kv : portalseed::g_first) {
            m += (first ? " " : ",") + std::to_string(kv.first) + ":"
               + std::to_string(kv.second);
            first = false;
        }
        writeResult(m);
        std::string m2 = "portalseed map2:";
        first = true;
        for (const auto& kv : portalseed::g_first2) {
            m2 += (first ? " " : ",") + std::to_string(kv.first) + ":"
                + std::to_string(kv.second);
            first = false;
        }
        writeResult(m2 + " (hook calls: "
                    + std::to_string(portalseed::g_calls) + ")");
        // ...and the pads the LAST attempt latched (padseed is per attempt), for dp --spentpad.
        std::string m3 = "padseed map:";
        first = true;
        for (const auto& kv : padseed::g_first) {
            m3 += (first ? " " : ",") + std::to_string(kv.first) + ":"
                + std::to_string(kv.second);
            first = false;
        }
        writeResult(m3 + " (hook calls: " + std::to_string(padseed::g_calls) + ")");
        // ...and the rings it fired, oldest first, for dp --spentorb.
        std::string m4 = "ringseed map:";
        first = true;
        for (const auto& f : ringseed::g_fired) {
            m4 += (first ? " " : ",") + std::to_string(f.first) + ":" + std::to_string(f.second);
            first = false;
        }
        writeResult(m4 + " (hook calls: " + std::to_string(ringseed::g_calls) + ")");
    }
    // ...and the proof: the level's own record, compared with the sample taken before the
    // level had run a tick. "changed: none" is the only acceptable outcome for a solver
    // session; anything else names the field that leaked. `restored` counts the writes GD
    // made outside the guarded block (the inline attempt counter) and that were put back --
    // it is what stops "none" from being vacuous.
    // The Area Move envelope's own check (solver/areaenv.hpp): printed whenever an Area Move ran,
    // so a level that has one says whether its prediction matched the game.
    if (areaenv::g_objects > 0 || areaenv::g_unenveloped > 0 || areaenv::solverKills() > 0)
        writeResult(areaenv::summary());
    restoreProgress();
    writeResult("level record changed: "
        + progressDiff(g_progressAtStart, sampleProgress(g_progressLevel))
        + " (restored " + std::to_string(g_progressRestores) + ")");
    // ...and the same question asked of everything the game would save, not just the level's own
    // counters: a record kept under a key nothing here knows about shows up in this one.
    gate::memaudit::report("session end");
    // rngfix: resets that had already drawn from the trigger seed before it was put back (see
    // rngFixAfterReset). Non-zero means some trigger fired inside the reset itself.
    if (g_rngFix) writeResult("rngfix: trigger seed drawn inside a reset " +
                              std::to_string(g_rngDrawnInReset) + " times");
    g_rngDrawnInReset = 0;
    // attemptone: how often the pin answered (see g_attemptOne). Both 0 on a level that reads
    // neither the attempt count nor a kept item.
    writeResult("singlereset: " + std::to_string(g_resetsSkipped) + " resets skipped (no physics since the last reset)");
    if (g_attemptOne)
        writeResult("attemptone: attempt count read as on attempt 1 " + std::to_string(g_attemptReadsPinned)
                    + " times, kept items not carried over " + std::to_string(g_persistTransfersSkipped)
                    + " times");
    else if (g_attemptCount >= 0 || g_attemptAutoCount >= 0)
        writeResult("attemptcount: attempt count read as "
                    + std::to_string(g_attemptCount >= 0 ? g_attemptCount : g_attemptAutoCount)
                    + (g_attemptCount >= 0 ? " " : " (attemptauto) ")
                    + std::to_string(g_attemptReadsPinned) + " times");
    g_attemptReadsPinned = 0;
    g_persistTransfersSkipped = 0;
    g_attemptReadsSeen = 0;
    // cfg cpflight: its tally (mod/cp_flight.hpp comes after this file; the solve's start sets this).
    if (g_cpFlightSummary) writeResult(g_cpFlightSummary());
    if (itermap::g_actCensus) writeResult(itermap::actCensusLine());   // cfg actcensus (print only)
    g_cpFlightSummary = nullptr;
    // Corridor clearance table (cfg clearance=1). Written out from the samples gathered during
    // the run
    clearance::write(g_cfg.levelId);
    // Tally of idle spinning while dead
    if (deadstat::g_total > 0) {
        char buf[224];
        snprintf(buf, sizeof(buf),
            "deadticks: total=%lld dead=%lld (%.2f%%) episodes=%lld "
            "meanStreak=%.1f maxStreak=%lld",
            deadstat::g_total, deadstat::g_dead,
            100.0 * deadstat::g_dead / deadstat::g_total, deadstat::g_episodes,
            deadstat::g_episodes ? (double)deadstat::g_dead / deadstat::g_episodes : 0.0,
            deadstat::g_maxStreak);
        writeResult(buf);
    }
    // Give the screen and the clock back. A solve turns rendering off for speed, and a session
    // that ends while it is off -- it gave up, it ran out of iterations, the plan ended -- used
    // to leave the player looking at a black window with no sign the game was still there and no
    // key listed to bring it back. Mod state must not outlive the mod's session; this is the
    // same duty audio::neutral() has for the sound.
    g_secRenderWas = -1;   // a section solve's hold ends with the session too (secRenderHold)
    g_secRealtimeWas = -1;
    if (g_renderOff) {
        g_renderOff = false;
        writeResult("session_end: rendering restored (the solve had the screen off)");
    }
    g_realtimeOverride = true;
    watchSpeedSet(WATCH_SPEED_1X);
    // A search waiting for checkpoint judgements (cfg `dpcheck`) would wait forever once nothing
    // flies them: release it. Harmless when nothing is waiting.
    dpbridge::checkSubscribe(false);
    // ...and stop a search still running, subscribed or not. Its answer belongs to a session that
    // is over, and while it runs it holds the job slot that the next level's Solve queues behind
    // (dpsolve::start).
    dpbridge::cancelSearch(true);
    // ...and the memory the solving held, which nothing reads from here on (repair.hpp
    // releaseSolveMemory; a search that has not stopped yet keeps it, and leaving the level asks
    // again).
    if (g_solveMemHeld) dpsolve::releaseSolveMemory("session end");
    dpsolve::memCensus("session end");   // cfg memcensus (print only)
    // cfg profsample: the report is written here, not at the next frame -- a session that ends the process
    // (quitwhendone) has no next frame
    profsample::stopAndReport("session end");
    g_sessionOver = true;
    writeResult("session_end: " + why);
    flushAll();
    // Close the files: left open, the write checks stay live during subsequent normal play
    // (performance), and the next session's openFiles fails
    if (g_trace.is_open()) g_trace.close();
    if (g_dump.is_open()) g_dump.close();
    // A manual end via F9 does not close the game: we only want to stop the analysis, and
    // taking the whole process down would require a restart. Return to the menu via GD's own
    // exit path (onQuit).
    //
    // ONLY F9. A solve that gives up briefly did this too, and it was wrong: the level vanishes
    // the instant the run fails, so the player is thrown back to the menu with no chance to see
    // where the bot stopped or what the level looked like there. Leaving is the player's call,
    // and F9 is on the overlay. Nothing leaks by staying -- the recording suppression is tied to
    // the session having been opened, not to it still running, so the level's record cannot be
    // credited from wherever the bot got to.
    if (why == "user_quit_hotkey") {
        Loader::get()->queueInMainThread([] {
            if (auto* pl = PlayLayer::get()) pl->onQuit();
        });
        return;
    }
    // A suite has another level to solve: leave this one the way a player does and let the
    // resident poll enter the next once the scene is clear (SuiteKeeper, level_entry.hpp).
    //
    // AFTER the F9 branch above, which is a human stopping the run and must not step the
    // suite on, and BEFORE quitWhenDone, which is what ends the process on the last level.
    // Whether the level cleared or gave up makes no difference here: the suite is measuring
    // every level in the list, and a level that fails is a result, not a reason to stop.
    if (suite::hasNext()) {
        ++suite::g_at;
        suite::g_advance = true;
        suite::g_settle = 0;
        suite::g_waited = 0;
        writeResult("suite: next level=" + std::to_string(suite::current())
                    + " (" + std::to_string(suite::g_at + 1) + "/"
                    + std::to_string(suite::g_levels.size()) + ")");
        Loader::get()->queueInMainThread([] {
            if (auto* pl = PlayLayer::get()) pl->onQuit();
        });
        return;
    }
    // The marker the reader waits for. `session_end:` cannot serve: a suite writes one per
    // level, and a reader that stops at the first would take level 1's log for the whole run.
    if (suite::active())
        writeResult("suite: done " + std::to_string(suite::g_levels.size()) + " levels");
    if (g_cfg.quitWhenDone) {
        Loader::get()->queueInMainThread([] {
            gdcompat::exitGame();
        });
    }
}

}  // namespace p1

#include "mod/cfg_effective.gen.hpp"
