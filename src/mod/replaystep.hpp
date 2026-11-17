// cfg `replaystep=<n>` (0 = off): a replay the bot drives at the screen's speed advances in fixed steps.
//
// The game's update is given the frame's wall-clock dt, so one update covers 4, 5 or 6 ticks as the frames run long or short, and on a screen faster than
// 60 Hz it covers 1 or 2. What a replay does then depends on where the update boundaries fall (a portal's tick, a ball's rotation, a cocos action read
// on a tick), and the solver's fast loop, which calls the update with exactly 1/60 s each time, does not see that. Here the time the frames bring is
// kept (a remainder carries over) and the update is called once for every 1/n s that has gathered, with the cocos actions given the same 1/n s, scaled
// by the scheduler's time scale (GD's TimeWarp), before every call but the first (the scheduler's own pass gave the first). A frame that has gathered
// less than 1/n s runs neither the update nor the actions (the scheduler is given dt 0), so a screen faster than 60 Hz does not run the animations fast.
// What it does not do is draw in between: the picture changes n times a second.
//
// Off for the fast loop (a solve, and a replay run fast), for the person's own play (botDriving is false), and for anything that sets its own speed
// (the notches, slow motion, a seek, the takeover's approach, a pause).
#pragma once
#include "mod/playlayer_helpers.hpp"
#include <cmath>

namespace replaystep {

inline double g_acc = 0.0;        // seconds gathered and not yet run
inline int g_calls = -1;          // updates this frame's scheduler pass allows (-1: none was decided for the update that asks)
inline long long g_lastTick = -1;
constexpr int kMaxCalls = 8;      // the most one frame runs; what has gathered beyond it is dropped

inline bool active() {
    return g_replayStep > 0 && g_started && !g_sessionOver && g_cfg.fastdt <= 0.f && !g_cfg.dpSolve && botDriving()
           && g_watchSpeed == 1.f && g_slowmo == 1.f && g_cfg.framedt <= 0.f && !g_paused && !probe::g_pause && !itermap::seeking()
           && !takeover::personDriving() && !g_endzoneBurn;
}

inline float stepSeconds() { return 1.f / (float)g_replayStep; }

// Called with the frame's dt where the scheduler is about to run; returns the dt the scheduler is to run with.
inline float feed(float dt) {
    const double step = 1.0 / (double)g_replayStep;
    // Nothing is gathered at the first frame this runs and when the tick falls back (a new attempt), each ONCE: a test of "the tick is 0" here
    // would drop the frame's time at every frame of the level's start, and on a screen whose frames are shorter than one step (> 60 Hz for
    // n = 60) nothing would ever gather enough to run the first tick.
    if (g_lastTick < 0 || (long long)g_tick < g_lastTick) g_acc = 0.0;
    g_lastTick = (long long)g_tick;
    if (g_replayStepDiagDt > 0.f) dt = g_replayStepDiagDt;   // diagnostic: every frame brings this much, whatever the screen does
    g_acc += (double)dt;
    if (g_acc > step * kMaxCalls) g_acc = step * kMaxCalls;
    const int n = (int)std::floor(g_acc / step + 1e-9);
    g_acc -= n * step;
    if (g_acc < 0.0) g_acc = 0.0;
    g_calls = n;
    return n > 0 ? stepSeconds() : 0.f;
}

inline void reset() {
    g_acc = 0.0;
    g_calls = -1;
    g_lastTick = -1;
}

}  // namespace replaystep