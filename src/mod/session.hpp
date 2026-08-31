#pragma once
// Session lifecycle: configuration from the UI, live commands, loadConfig, endSession.
#include "mod/plan_io.hpp"

namespace p1 {

// Everything a panel-started session has in common, whichever mode it is.
inline void uiSessionBase(int levelId) {
    resetSessionState();
    g_cfg = Config{};
    g_cfg.enabled = true;
    g_cfg.levelId = levelId;
    g_cfg.quitWhenDone = false; // a UI-started session does not quit the app
    g_cfg.noTrace = true;
    g_cfg.cbs = 0; g_cfg.cos = 1;
    g_cfg.blockInput = true;    // the mod drives; the keyboard does not
    g_cfg.delaySec = 0.f;
}

inline bool uiConfigureSession(int levelId) {
    if (g_uiMode == UI_MODE_SOLVE) {
        // Solve mode: no plan yet -- the mod is about to make one. The level is held still
        // while the solver runs (dpsolve::start pauses it) and released when the plan lands.
        uiSessionBase(levelId);
        g_cfg.dpSolve = true;
        grouptrace::g_on = true;   // see the cfg key of the same name
        // Give the session a fast loop to fall back on, so the render key has somewhere to
        // send the frames. Without one, turning the screen off just makes the game invisible
        // at the same speed, which is all cost and no gain
        g_cfg.fastdt = 1.f / 60.f;   // 4 physics ticks per call
        g_cfg.fastloops = 1800;      // ...and this many calls per frame once the screen is off
        // Start FAST, with the screen off -- the same state the render key produces, so the key
        // toggles out of it and back. A solve is searching (the level frozen, nothing to look at)
        // and testing candidates (replays that mostly die); at 1x each candidate costs the length
        // of the song. The overlay is still drawn, so the run is legible while the level is not.
        // [2026-08-23, user direction] This was briefly changed to "visible at 16x" on the theory
        // that a black screen reads as a crash. It does not, given the overlay -- what read as a
        // crash was the session closing itself afterwards, which is fixed at its own end.
        g_renderOff = true;
        g_realtimeOverride = false;
        // A plan that does not clear ends in a death, and the session has to survive that to be
        // repaired and tried again. The loop closes the session itself, on a clear or when it
        // gives up
        g_cfg.maxAttempts = 1000000;
        log::info("panel: solve lv{} in-process", levelId);
    } else {
        char name[128];
        snprintf(name, sizeof(name), "solution_lv%d_dp.txt", levelId);
        std::vector<InputCmd> plan;
        if (!loadInputsFile(std::string(DATA_DIR) + "/" + name, plan)) {
            // Say so on screen as well: silently falling back to normal play looks exactly
            // like "the mod did nothing", which is what it looked like the first time
            log::info("panel: no solution file for lv{} ({}) -> normal play", levelId, name);
            notify::show("gdsolver: no solution file for this level - "
                         "switch the panel to Solve", NotificationIcon::Warning, 4.f);
            return false;
        }
        uiSessionBase(levelId);
        g_cfg.inputs = std::move(plan);
        g_cfg.maxAttempts = 1;
        log::info("panel: replay lv{} from {}", levelId, name);
    }
    openFiles();
    stallwatch::start();   // so that on a stall we can say "where"
    updateWindowTitle();   // show the role in the title (reflected the moment the session starts)
    writeResult("session_start (panel)", true);
    g_started = true;
    g_uiSession = true;
    g_forceCleanStart = true;
    solver::g_solveStart = std::chrono::steady_clock::now();
    return true;
}

inline void endSession(const std::string& why);

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
                          "p2ground2,p2mode,p2vsize,p2x\n";
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
    else if (cmd.rfind("step ", 0) == 0) {
        if (cfgNum("step", cmd.substr(5), g_stepTicks))
            log::info("phase2: stepping {} ticks", g_stepTicks);
    }
    else if (cmd == "quit") {
        writeResult("user quit via cmd");
        endSession("user_quit");
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
    else if (cmd.rfind("secsolve", 0) == 0) {
        // Hand the in-process solve session over to the section solver (src/solver/secsolve.hpp):
        //   secsolve <startTick> <targetX> [horizon] [cap]
        // Queued here, performed by dpsolve::poll() at an idle frame boundary: the loop stops,
        // its deepest VERIFIED plan replays to <startTick>, a practice checkpoint is dropped
        // there and the section search takes over. Reporting and the session end ("secsolve")
        // are the search's own, exactly as on the cfg-driven served path. Tuning knobs
        // (secyq/secvq/secgrace/...) can be given in the session cfg at open -- without
        // `secsolve=1` and `checkpointat` they are inert until this command fires.
        long long st = -1, hz = -1, cp = -1;
        double tx = 0.0;
        {
            std::istringstream ss(cmd.substr(8));
            ss >> st >> tx;
            if (!(ss >> hz)) hz = -1;
            if (!(ss >> cp)) cp = -1;
        }
        if (st <= 0 || tx <= 0.0) {
            writeResult("secsolve cmd: usage: secsolve <startTick> <targetX> [horizon] [cap]");
            return;
        }
        if (!g_started || g_sessionOver || !g_cfg.dpSolve) {
            writeResult("secsolve cmd: refused - no in-process solve session (dpsolve) is open");
            return;
        }
        g_secReqStart = st;
        g_secReqTarget = tx;
        g_secReqHorizon = hz;
        g_secReqCap = cp;
        g_secReqPending = true;
        writeResult("secsolve cmd: queued start=" + std::to_string(st)
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
    else if (key == "dpmaxiters") cfgNum(key, val, g_cfg.dpMaxIters);
    else if (key == "dpseedplan") g_cfg.dpSeedPlan = val;
    else if (key == "dpshow") cfgNum(key, val, g_cfg.dpShow);
    else if (key == "dpfixups") g_cfg.dpFixups = (val == "1");
    else if (key == "dpfixp2") g_cfg.dpFixP2 = (val == "1");
    else if (key == "dpworld") g_cfg.dpWorld = (val == "1");
    else if (key == "dpgroups") g_cfg.dpGroups = (val == "1");
    else if (key == "dpbandtrack") g_cfg.dpBandTrack = (val == "1");
    else if (key == "dpfingerprint") g_cfg.dpFingerprint = (val == "1");
    else if (key == "dparg") g_cfg.dpArgs.push_back(val);
    else return false;
    return true;
}

inline void loadConfig() {
    std::ifstream f(std::string(DATA_DIR) + "/autorun.cfg");
    if (!f.is_open()) return;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = line.substr(0, eq);
        auto val = line.substr(eq + 1);
        if (loadDpCfg(key, val)) continue;
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
        else if (key == "padtrace") g_cfg.padTrace = (val == "1");
        else if (key == "snaptrace") g_cfg.snapTrace = (val == "1");
        else if (key == "hitboxtrace") g_cfg.hitboxTrace = (val == "1");
        else if (key == "hbfrom") g_cfg.hbFrom = std::atoll(val.c_str());
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
        else if (key == "nodeath") g_cfg.noDeath = (val == "1");
        else if (key == "clearance") clearance::g_on = (val == "1");
        else if (key == "practiceat") cfgNum(key, val, g_cfg.practiceAt);
        else if (key == "checkpointat") cfgNum(key, val, g_cfg.checkpointAt);
        else if (key == "restoreat") cfgNum(key, val, g_cfg.restoreAt);
        else if (key == "restoreloop") cfgNum(key, val, g_cfg.restoreLoop);
        // Section solver (src/solver/secsolve.hpp)
        else if (key == "secsolve") secsolve::g_on = (val == "1");
        else if (key == "secstart") cfgNum(key, val, secsolve::g_startTick);
        else if (key == "sectarget") cfgNum(key, val, secsolve::g_targetX);
        else if (key == "sechorizon") cfgNum(key, val, secsolve::g_horizon);
        else if (key == "seccap") cfgNum(key, val, secsolve::g_cap);
        else if (key == "secverify") secsolve::g_verify = (val == "1");
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
        // Break the chain here (`if`, not `else if`). A huge else-if chain hits MSVC's nesting
        // limit (C1061). The keys are mutually exclusive, so the meaning is unchanged.
        // If a new key triggers C1061, cut one more level the same way
        if (key == "ckpttrace") solver::ckpttrace::g_on = (val == "1");
        // Pause once the player's x passes this (a tool to freeze and film the screen right
        // before a wall)
        else if (key == "pauseatx") cfgNum(key, val, g_pauseAtX);
        else if (key == "clearmargin") cfgNum(key, val, g_clearMargin);
        else if (key == "uisim") g_uiSession = (val == "1"); // for tests: treat autorun as panel
        // For tests: supply the panel mode from cfg (0=Normal 1=Replay)
        else if (key == "uimode") cfgNum(key, val, g_uiMode);
        else if (key == "coins") g_cfg.coinMode = (val == "1");
        else if (key == "toggle") {
            auto comma = val.find(',');
            int at = 0;
            if (comma != std::string::npos && cfgNum(key, val.substr(0, comma), at)) {
                g_cfg.toggles.push_back({ at, val.substr(comma + 1) });
            }
        }
        else if (key == "input") {
            auto comma = val.find(',');
            int at = 0;
            if (comma != std::string::npos && cfgNum(key, val.substr(0, comma), at)) {
                g_cfg.inputs.push_back({ at, val.substr(comma + 1) == "1" });
            }
        }
    }
    log::info("phase1 config: enabled={} level={} attempts={} inputs={}",
        g_cfg.enabled, g_cfg.levelId, g_cfg.maxAttempts, g_cfg.inputs.size());
}

inline void endSession(const std::string& why) {
    if (g_sessionOver) return;
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
        + " notifyForced=" + std::to_string(notify::g_forced));
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
        + " progress=" + std::to_string(g_blockedProgress));
    // ...and the proof: the level's own record, compared with the sample taken before the
    // level had run a tick. "changed: none" is the only acceptable outcome for a solver
    // session; anything else names the field that leaked. `restored` counts the writes GD
    // made outside the guarded block (the inline attempt counter) and that were put back --
    // it is what stops "none" from being vacuous.
    restoreProgress();
    writeResult("level record changed: "
        + progressDiff(g_progressAtStart, sampleProgress(g_progressLevel))
        + " (restored " + std::to_string(g_progressRestores) + ")");
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
    if (g_renderOff) {
        g_renderOff = false;
        writeResult("session_end: rendering restored (the solve had the screen off)");
    }
    g_realtimeOverride = true;
    watchSpeedSet(WATCH_SPEED_1X);
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
    // resident poll enter the next once the scene is clear (SuiteKeeper, ui_panel.hpp).
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
            utils::game::exit(false);
        });
    }
}

}  // namespace p1
