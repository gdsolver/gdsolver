#pragma once
// Death statistics, developer hotkeys (F8 realtime override, probe requests).
#include "mod/fxcensus.hpp"

namespace p1 {

namespace deadstat {
inline long long g_total = 0;      // total physics ticks advanced during the session
inline long long g_dead = 0;       // of which, ticks where the player was dead
inline long long g_streak = 0;     // current run of consecutive dead ticks
inline long long g_maxStreak = 0;  // longest
inline long long g_episodes = 0;   // number of death episodes (= effective attempt count)
inline void tick(bool dead) {
    ++g_total;
    if (dead) {
        ++g_dead;
        if (++g_streak > g_maxStreak) g_maxStreak = g_streak;
    } else {
        if (g_streak > 0) ++g_episodes;
        g_streak = 0;
    }
}
}

// --- Manual pause / step (F2 / F3 / F4) ---
// Hotkeys cannot touch PlayerObject from the key-polling context, so only a request is raised
// here and consumed in the update hook. 2=pause/resume 3=step 1 substep 4=step 10
inline int g_probeRequest = 0;

// Measurement of orb firing (cfg `orbtrace=1`). Because of the press buffer (a press before
// contact fires at the moment of contact), early presses within the window collapse onto the
// same firing point
#include "solver/sweep.hpp"

inline void endSession(const std::string& why);
// Release the manual (F2) pause. Defined further down the chain -- probe:: is not visible from
// here, and the keys have to be handled in this header
inline void clearManualPause();
// ...and whether it is on, and whether a step is still owed. All defined in helpers_game.hpp,
// where probe:: is visible.
inline bool manualPaused();
inline bool manualStepPending();

// Accept hotkeys only while the GD window is in the foreground. GetAsyncKeyState returns the
// physical key state regardless of focus = a key in another app would kill the session.
// Decide by whether the foreground window's owning process is ourselves
inline bool hotkeysFocused() {
#ifdef GEODE_IS_WINDOWS
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
#else
    return true;
#endif
}

// The keys that also work outside a session (plain manual play). pollHotkeys is only called
// inside the g_started guard, hence a separate function.
//
// [2026-08-23] The cheat keys were removed: F1 warp, F5 noclip, F6 force-ship, F10 infinite
// jump, and the F11 slow-motion cycle. They let a human do what the game does not allow, which
// is not what this project is for. Slow motion survives as cfg `slowmo=<N>`, which is how the
// replay-watching tools use it; it is no longer reachable from the keyboard mid-play.
// What is left only stops time (F2/F3/F4) or changes what is drawn (F1/F8 and the arrows).
inline void pollProbeHotkeys() {
#ifdef GEODE_IS_WINDOWS
    if (!hotkeysFocused()) return;
    // F1: show / hide the overlays. Two things it must not do (spec §9): it is refused while
    // solving, and it never hides the bot badge -- a replay has to be visibly a replay, so that
    // part is not the user's to turn off. Everything else (coordinates, progress, the key
    // legend) is. (Not F12: that is Steam's screenshot key by default.)
    static bool s_f1Down = false;
    bool f1 = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;
    if (f1 && !s_f1Down) {
        if (solvingNow()) {
            log::info("hotkey: F1 ignored (solving)");
        } else {
            g_overlayHidden = !g_overlayHidden;
            log::info("hotkey: F1 -> overlay {}", g_overlayHidden ? "hidden" : "shown");
        }
    }
    s_f1Down = f1;
    // F2: pause/resume (for manual experiments; also works outside a session)
    static bool s_f2Down = false;
    bool f2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
    if (f2 && !s_f2Down) g_probeRequest = 2;
    s_f2Down = f2;
    // F3: advance 1 substep / F4: advance 10 substeps (only meaningful while paused).
    // Held down they repeat: stepping 200 substeps by hand is 200 presses otherwise. The delay
    // before the repeat starts is what keeps a single press single.
    auto stepKey = [](int vk, int req, bool& down,
                      std::chrono::steady_clock::time_point& since,
                      std::chrono::steady_clock::time_point& last) {
        constexpr auto kRepeatAfter = std::chrono::milliseconds(500);
        constexpr auto kRepeatEvery = std::chrono::milliseconds(100);
        const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
        const auto t = std::chrono::steady_clock::now();
        if (now && !down) {            // the press itself
            g_probeRequest = req;
            since = t;
            last = t;
        } else if (now && t - since >= kRepeatAfter && t - last >= kRepeatEvery) {
            g_probeRequest = req;      // ...and every 100ms once it has been held for 500ms
            last = t;
        }
        down = now;
    };
    static bool s_f3Down = false, s_f4Down = false;
    static std::chrono::steady_clock::time_point s_f3Since, s_f3Last, s_f4Since, s_f4Last;
    stepKey(VK_F3, 3, s_f3Down, s_f3Since, s_f3Last);
    stepKey(VK_F4, 4, s_f4Down, s_f4Since, s_f4Last);
    // F6: GD's own hitbox drawing (see toggleHitboxes). The game only shows it in practice
    // mode, so the flag and the node's visibility are driven here instead
    static bool s_f6Down = false;
    bool f6 = (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
    if (f6 && !s_f6Down) g_probeRequest = 6;
    s_f6Down = f6;
    // F10: the iteration map (itermap.hpp). OFF by default -- it draws over the level, and the
    // point of a replay is the replay. It reads a file and a draw node and touches nothing the
    // physics can see, so unlike F1 it is not refused while solving; there is simply nothing to
    // look at until the run has produced some rounds.
    //
    // The level is taken from the session when there is one and from the game otherwise, so the
    // key also works while watching a level in plain manual play.
    static bool s_f10Down = false;
    bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (f10 && !s_f10Down) {
        int lv = g_cfg.levelId;
        if (lv <= 0)
            if (auto* pl = PlayLayer::get())
                if (pl->m_level) lv = pl->m_level->m_levelID.value();
        itermap::toggle(lv);
        log::info("hotkey: F10 -> iteration map {}", itermap::mapWanted() ? "on" : "off");
    }
    s_f10Down = f10;
#endif
}

// Whether anything is being drawn right now. Declared here because the hotkey needs it and
// renderSuppressed() lives further down the include chain (same condition, one definition).
inline bool renderingOn() {
    if (g_renderOff) return false;
    return !(g_started && !g_sessionOver && g_cfg.skipRender && !g_realtimeOverride);
}

// Exactly what happens when the render key is pressed once: rendering on <-> off, and nothing
// else. It used to be one key that cycled rendering AND the speed together, so "fast but
// visible" and "slow but blind" were both unreachable; the speed is on the arrow keys now.
// Every equivalent transition must go through this function — writing a separate path leads
// to the harness and the real key measuring different things (keep a single entry point).
// [2026-08-23] The key moved from F8 to F5 (user direction). cfg `watchat` / `watchcycle` still
// call this function, so they follow the key wherever it goes.
inline void renderTogglePress() {
        const bool hasFast = g_cfg.fastdt > 0;
        if (renderingOn()) {
            // Screen off. With a fast loop configured, hand the frames back to it as well --
            // stopping the drawing is the whole reason to do that.
            g_renderOff = true;
            if (hasFast) g_realtimeOverride = false;
        } else {
            g_renderOff = false;
            if (hasFast && !g_realtimeOverride && !g_visResetPending) {
                // Do not resume rendering here. Resume after resetLevel (the update hook).
                // In the reverse order the first updateVisibility after resuming steps on the
                // stale visibility state and crashes
                g_visResetPending = true;
                g_visRefresh = true;        // old method (only effective with visrefresh=1)
                if (g_watchPurge) purgeDanglingActions();
            }
        }
        log::info("hotkey: F5 -> render {}", renderingOn() ? "ON" : "OFF");
        // Record what was alive at the moment of the switch. The CTD shows up a few frames after
        // this, so after the fact "what was on screen right before the crash" can never be known
        writeResult(fxCensus("render-toggle"));
}

inline void pollHotkeys() {
#ifdef GEODE_IS_WINDOWS
    if (!hotkeysFocused()) return; // do not let key input in another app stop the analysis
    // F5: rendering on / off. Only in a solve session -- that is the only time there is anything
    // to gain by not drawing (the frames go back to the fast loop). In a plain replay it stays
    // dead: a replay that cannot be seen is not a replay, and turning the screen off by accident
    // mid-replay looks exactly like a crash.
    //
    // The whole SESSION, not only while the search runs: a solve alternates searching with
    // replaying its candidates, and it comes back to 1x for each replay. If the key only worked
    // while searching, the run could be slowed down but never sped up again.
    static bool s_f5Down = false, s_f9Down = false;
    bool f5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
    if (f5 && !s_f5Down && solveSession()) renderTogglePress();
    s_f5Down = f5;
    // F9: leave the level. The session is closed first if it is still open, and then GD's own
    // exit path runs, so this lands on the level screen exactly as pressing GD's own quit
    // button would. It is deliberately NOT a "stop the analysis but stay in the level" key any
    // more: staying behind with a finished session is the one state in which the mod is no
    // longer driving while the player is still mid-level, and the level's record would start
    // counting again from wherever the plan had got to.
    bool f9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (f9 && !s_f9Down) {
        log::info("hotkey: F9 -> quit to the level screen");
        if (!g_sessionOver) {
            endSession("user_quit_hotkey");   // ends with GD's onQuit (see endSession)
        } else {
            Loader::get()->queueInMainThread([] {
                if (auto* pl = PlayLayer::get()) pl->onQuit();
            });
        }
    }
    s_f9Down = f9;
    // F7: replay the current plan from the start. resetLevel() puts g_tick and g_nextInput back
    // to 0 and the plan streams from the beginning. Replay is launched with attempts=1, so each
    // press adds one more slot.
    //
    // It also works AFTER the run has finished, which is the case that matters: clearing the
    // level ends the session (`level_complete`), and without this the one key you want at the
    // moment the replay finishes was the one key that had stopped working. Reviving is for
    // replays only -- in serve mode the session belongs to the driver, and resurrecting it
    // behind the driver's back would hand it a run it never asked for.
    static bool s_f7Down = false;
    bool f7 = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
    if (f7 && !s_f7Down) {
        if (auto* pl = PlayLayer::get()) {
            if (g_sessionOver && !g_serveMode) {
                g_sessionOver = false;
                g_finishedAttempts = 0;
                writeResult("session_resume: F7 restarted the replay after the session ended");
            }
            ++g_cfg.maxAttempts;
            g_pauseAtXFired = false;   // stop short of the wall again on the next lap
            g_paused = false;          // release it so it does not start out stuck paused
            clearManualPause();        // ...and F2's, so the replay does not start frozen
            // GD leaves its end-of-level screen parented across resetLevel, so replaying a level
            // you have cleared stacks another one on every clear (see dismissEndScreen).
            itermap::dismissEndScreen();
            log::info("hotkey: F7 -> replay from the start");
            pl->resetLevel();
        }
    }
    s_f7Down = f7;
    // [2026-08-28, user direction] The arrows were remapped once the seek bar existed. They are
    // now the transport a person actually reaches for while watching a replay:
    //
    //   left / right   seek back / forward. A tap is five seconds; holding accelerates.
    //   up / down      the spectating speed, one notch (0.25 ... 16x).
    //
    // Up and down used to be deliberately unbound on the grounds that up is a jump in GD and a
    // spectator key that also jumps is a trap. That reasoning does not reach here: this whole
    // function only runs while the mod is DRIVING, where the plan owns the input and a keypress
    // cannot jump. (pollProbeHotkeys, the one that also works in plain manual play, binds
    // neither.)
    static bool s_upDown = false, s_downDown = false;
    const bool kUp   = (GetAsyncKeyState(VK_UP)   & 0x8000) != 0;
    const bool kDown = (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;
    int step = 0;
    if (kUp && !s_upDown) step = +1;
    if (kDown && !s_downDown) step = -1;
    s_upDown = kUp; s_downDown = kDown;
    if (step != 0) {
        watchSpeedStep(step);
        log::info("hotkey: {} -> {:g}x", step > 0 ? "faster" : "slower", g_watchSpeed);
    }
    // Seeking -- or STEPPING, while the game is stopped.
    //
    // A seek needs the game to run: it fast-forwards to the target and stops when it arrives.
    // While F2 has time frozen nothing arrives, so a forward seek issued there hung the level
    // behind its own blackout forever. Stopped, the arrows are frame-step keys instead:
    //
    //   right   exactly F4 -- ten substeps, same request, same timing
    //   left    ten substeps BACKWARDS, which physics cannot do, so it is a seek to
    //           tick-10 that puts the stop back when it lands (g_seekRepause)
    //
    // The step is refused while such a seek is still running, or a held key would keep moving
    // the target the in-flight one is chasing.
    auto transportKey = [](int vk, int dir, bool& down, int& repeats,
                           std::chrono::steady_clock::time_point& since,
                           std::chrono::steady_clock::time_point& last) {
        const bool stopped = g_paused || manualPaused();
        // Stopped: F3/F4's timing exactly, because that is the key this becomes.
        const auto repeatAfter = std::chrono::milliseconds(stopped ? 500 : 400);
        const auto repeatEvery = std::chrono::milliseconds(stopped ? 100 : 140);
        const bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
        const auto t = std::chrono::steady_clock::now();
        const bool press = now && !down;
        const bool repeat = now && !press && t - since >= repeatAfter
                            && t - last >= repeatEvery;
        down = now;
        if (!press && !repeat) return;
        // NOT WHILE THE LOOP OWNS THE LEVEL. Both halves of this key move it -- a seek restarts
        // it, a step advances it -- and during a solve the level IS the loop's verification replay,
        // held still between rounds by repair.hpp's spawn. Nudging it there corrupts the round the
        // recorder is about to be asked about. The bar is not drawn for the same reason and a press
        // on it is refused by barLive(); the keys never had the equivalent guard, and now that the
        // map is watchable live they are keys a person has a reason to be pressing.
        // The edge tracking above still runs, so a key held across the end of a solve is not
        // mistaken for a fresh press afterwards.
        if (showingSolve()) return;
        if (press) { repeats = 0; since = t; }
        last = t;
        if (stopped) {
            // ONE STEP AT A TIME, and a step is not over until the tick it owes has been walked.
            // A backward step aims short of its target and finishes with probe::g_step; a repeat
            // arriving in that gap sees a tick the run is not going to stay at, computes its own
            // target from it, and overwrites the finishing step -- two half-moves compounding
            // into a jump in the wrong direction.
            if (itermap::seeking() || manualStepPending()) return;
            if (dir > 0) g_probeRequest = 4;   // ...which IS F4
            else         itermap::stepBack(10);
            return;
        }
        // A BACKWARD repeat waits for the restart it already asked for. Rewinding runs the level
        // again from the top, and re-asking before that has even begun throws the work away and
        // starts over -- so a held key made no progress at all.
        if (dir < 0 && itermap::g_seekRestart) return;
        itermap::seekBy(dir * itermap::seekStep(press ? 0 : ++repeats));
    };
    static bool s_rightDown = false, s_leftDown = false;
    static int s_rightRep = 0, s_leftRep = 0;
    static std::chrono::steady_clock::time_point s_rSince, s_rLast, s_lSince, s_lLast;
    transportKey(VK_RIGHT, +1, s_rightDown, s_rightRep, s_rSince, s_rLast);
    transportKey(VK_LEFT,  -1, s_leftDown,  s_leftRep,  s_lSince, s_lLast);
#endif
}

// Body of the experimental-hotkey handling (called from update).
// Must be called before the pause check — if we return while paused the keys are never
// processed and the pause can never be released
inline void handleProbeRequest(GJBaseGameLayer* layer);

// Rollback-verification state

}  // namespace p1
