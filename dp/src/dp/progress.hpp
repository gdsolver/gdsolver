#pragma once
// Live progress of the layer loop, for whoever is watching from another thread.
//
// The CLI reports progress by printing a line every 500 ticks; in the mod the solver runs on a
// worker thread with the game's own frame carrying on beside it, and a printed line goes
// nowhere. So the same three numbers -- how far the layer loop has got, where the frontier's
// leader is, and how many states are alive -- are published here as well.
//
// Written by the search, read by anyone. Nothing in the search ever reads them back, so the
// stores are relaxed: a UI that samples a tick late is not wrong in any way that matters.
#include "dp/constants.hpp"

namespace dp {

struct SearchProgress {
    // `from` is the layer the search RESUMES AT, not zero. A tail solved from a re-anchor at
    // t=18,656 starts there, so a bar drawn as tick/horizon would open at 36% and a watcher
    // would read it as "already a third of the way". The span is horizon - from.
    std::atomic<long long> from{0};
    std::atomic<long long> tick{0};      // the layer being processed
    std::atomic<long long> horizon{0};   // the last layer it will process
    std::atomic<double> x{0.0};          // frontier leader's x
    std::atomic<size_t> alive{0};        // states in the frontier
    std::atomic<bool> running{false};

    void begin(long long f, long long h) {
        from.store(f, std::memory_order_relaxed);
        horizon.store(h, std::memory_order_relaxed);
        tick.store(f, std::memory_order_relaxed);
        x.store(0.0, std::memory_order_relaxed);
        alive.store(0, std::memory_order_relaxed);
        running.store(true, std::memory_order_release);
    }
    void layer(long long t, double leadX, size_t n) {
        tick.store(t, std::memory_order_relaxed);
        x.store(leadX, std::memory_order_relaxed);
        alive.store(n, std::memory_order_relaxed);
    }
    void end() { running.store(false, std::memory_order_release); }
};

inline SearchProgress g_progress;

// What the search concluded, for a caller that has no pipe to read.
//
// The CLI says this in three printed lines -- `PARTIAL: frontier died at ...`, `FAILED: ...`,
// `SOLVED at ...` -- and the Python driver parses them back out of stdout. Inside the game the
// solver is a function call on a worker thread and its printf goes to a console nobody reads,
// so the same facts are published here, written at exactly the sites that print them. Neither
// the wording nor the order of the printed lines changes: this is a second copy of what is
// already said, not a replacement.
//
// Read after the call returns (the worker thread's completion flag is the synchronisation), so
// plain fields are enough -- unlike SearchProgress above, nothing samples these while the
// search is running.
enum Verdict { VerdictFailed = 0, VerdictPartial = 1, VerdictSolved = 2 };

struct SearchOutcome {
    int verdict = VerdictFailed;
    long long deepT = -1;    // where the frontier died (PARTIAL / FAILED); -1 = never reported
    double deepX = -1.0;
    long long capHits = -1;  // -1 = no capstat line, i.e. the layer loop never ran
    // --replay only: the tick the model died on, or -1 if it survived the plan. The fixup
    // recorder needs it for the case where the two agree all the way and only the MODEL kills:
    // there is no divergence to scan for, and the record to make is a revival of the last
    // common transition.
    long long replayDiedT = -1;
    // Touch boxes this call REQUIRED the route to enter, and which of them the anchor is
    // already past. A requirement the anchor cannot possibly satisfy empties the frontier
    // before a single tick runs, and from outside that is indistinguishable from a physics
    // wall -- so the caller needs to be able to tell the two apart and drop the box.
    unsigned needTrigMask = 0;    // bit b = box b was required
    unsigned needTrigPassed = 0;  // bit b = the anchor starts past box b

    void reset() {
        verdict = VerdictFailed;
        deepT = -1; deepX = -1.0; capHits = -1; replayDiedT = -1;
        needTrigMask = 0; needTrigPassed = 0;
    }
};

inline SearchOutcome g_outcome;

}  // namespace dp
