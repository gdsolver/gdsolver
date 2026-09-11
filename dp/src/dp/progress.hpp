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
#include <string>

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
    // Ticks of the emitted plan on which the model fired a kill, counted on the
    // witness resim -- the ONE walk of the final plan (cli.hpp). -1 means the
    // walk never ran, which is not the same as 0 and must not read as clean.
    // The CLI prints this as `resimdie:` and the Python driver parses it back
    // out of stdout; the mod has no pipe (dp_bridge.hpp:57), so a plan that
    // dies in its own walk is invisible to the repair loop unless it travels
    // here. Same reason capHits is in this struct.
    long long resimDead = -1;
    long long resimFirst = -1;   // first such tick, -1 = none
    const char* resimWhy = nullptr;  // cause at that tick; a string literal
    // ...and WHICH object, at that same tick. The cause is a category --
    // `cube/hazard` is true of every hazard on the level -- so two walks that
    // die at different ticks for the same reason cannot be told apart from the
    // cause alone, and neither can "the same object, moved" be told from "a
    // different object". That distinction is the whole question on lv20 (same
    // mask, different position) and it is now the question on lv22, where the
    // loop's walk dies at 1813 and a rebuild of the same solve dies at 1837.
    // stdout already carries it as `resimwho:`, but the mod has no pipe
    // (dp_bridge.hpp:57), so without this the loop's own rows cannot be joined
    // to an object at all.
    int resimUid = -1;               // the LEVEL's uid, not this build's ordinal
    float resimObjX = 0.f, resimObjY = 0.f;
    unsigned resimTrig = 0;          // the walk's trigger mask at that tick
    int resimFrame = -1;             // the frame resimObjX/Y are expressed in
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
    // --seeddump only: the ready-made `--startrotq` argument for the dumped
    // tick, exactly as stdout carries it on the `seedrotq:` line. The mod has
    // no pipe (dp_bridge.hpp:57), so a caller in-process cannot read that line;
    // without this the loop would have to re-derive the seed, and the only
    // derivation available to it (spentRotArg's gframe walk) cannot see the
    // entries that change no frame -- 11 of lv22's 30. One producer, one
    // string, whichever side is reading.
    std::string seedRotQ;
    // The rotation queue exactly as buildRotQueue ordered it, for a caller that
    // derives its own --startrotq (the mod's cfg dprotseed). The mod cannot
    // re-sort it without keeping a second copy of the ordering rule, which only
    // dp should hold. One queue slot per `;`-separated entry, each
    // `ch,uid,px,py,swarm,swch,chanOnly,gnddir,id`; empty when no queue was
    // loaded. Printed nowhere, so no existing output changes.
    std::string rotQOrder;
    // --startrotq's own read-back, for the same caller: how many of the seed's
    // uids bound to a queue slot, out of how many were given, and the ones that
    // did not. -1/-1 = this call carried no --startrotq.
    int startRotHit = -1, startRotGiven = -1;
    std::string startRotMiss;

    void reset() {
        verdict = VerdictFailed;
        deepT = -1; deepX = -1.0; capHits = -1; replayDiedT = -1;
        resimDead = -1; resimFirst = -1; resimWhy = nullptr;
        resimUid = -1; resimObjX = 0.f; resimObjY = 0.f; resimTrig = 0;
        resimFrame = -1;
        needTrigMask = 0; needTrigPassed = 0;
        seedRotQ.clear();
        rotQOrder.clear();
        startRotHit = -1; startRotGiven = -1;
        startRotMiss.clear();
    }
};

inline SearchOutcome g_outcome;

}  // namespace dp
