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
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "dp/constants.hpp"

namespace dp {

struct SearchProgress {
    // `from` is the layer the search RESUMES AT, not zero. A tail solved from a re-anchor at
    // t=18,656 starts there, so a bar drawn as tick/horizon would open at 36% and a watcher
    // would read it as "already a third of the way". The span is horizon - from.
    std::atomic<long long> from{0};
    std::atomic<long long> tick{0};      // the layer being processed
    // The last layer the loop CAN process, so that tick/horizon is a fraction that reaches 1
    // exactly when the search runs out of layers. It is not `--horizon`: that bounds the PLAN,
    // and the loop deliberately runs on to t0 + 2*horizon before taking a survivor, or stops
    // earlier at the end of the level. A search that solves, empties its frontier or trips the
    // memory budget still ends before this -- the bar is an upper bound, not a promise.
    std::atomic<long long> horizon{0};
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

// ---- checkpoints: candidate lineages for the caller to fly, and the caller's off switch ----
//
// A caller that can fly a plan while the search runs (the mod's repair loop) asks the game early
// whether the model is right, instead of waiting for the whole search and flying its answer.
// At fixed LAYERS of the search (kCheckpointOffsets below, counted from the anchor) the search
// publishes the lineage of its frontier's first state, cut at that layer. The caller flies the
// checkpoints IN ORDER and judges each one: it survived to its last tick, or the game killed it
// strictly inside. A kill means the model was wrong somewhere on that lineage, so the caller
// cancels the search, learns from the death, and solves the same question again.
//
// WHY IT IS DETERMINISTIC. Everything that decides the outcome is a function of the search and
// the game, never of the clock:
//
//   * a checkpoint is a LAYER, not a moment, and its lineage is read on the search thread at that
//     layer -- the same call publishes the same checkpoints every time;
//   * the caller judges checkpoints in order, one flight each, and never skips one because it is
//     behind -- skipping on a backlog is exactly how wall time would get back in;
//   * the search does not finish until every checkpoint it published has been judged
//     (waitForJudgement in cli.hpp). A search that reaches the goal while flights are still in the
//     air waits for them, so "the search finished first" and "the game refuted it first" cannot
//     race. A cancelled search is thrown away whole, so how far it had got when the cancel landed
//     changes nothing but the wall clock.
//
// An earlier version published the frontier's lowest common ancestor instead (every plan the call
// can still emit starts with it, so a death inside it is a death of the call's answer). It was
// sound but slow: on lv22 the frontier stayed forked for 1,450-11,600 ticks past the deaths it
// needed to cover, and a cancelled call had no verdict for the ladder to book (2026-09-17). A
// checkpoint makes the weaker claim -- "the model is wrong on this lineage" -- and the caller acts
// on it by learning and re-solving rather than by standing in for the call's answer.
//
// Rules the publisher keeps:
//
//   * NO ARENA INDEX CROSSES THIS BOUNDARY. The lineage is materialised into `input=` edges on the
//     search thread before the lock is taken; the mark-compact GC renumbers nodes.
//   * `enabled` keeps this free when nobody is listening: with no subscriber the search publishes
//     nothing, waits for nothing and prints nothing, so the CLI's work and output are unchanged.

// Layers past the anchor at which a checkpoint is published: doubling to 4,800, then every 4,800.
// Deaths cluster within a few hundred ticks of the anchor, and every flight replays the level
// from its start, so a dense late schedule would cost game time and buy little.
inline bool isCheckpointLayer(long long d) {
    if (d == 150 || d == 300 || d == 600 || d == 1200 || d == 2400) return true;
    return d >= 4800 && d % 4800 == 0;
}

struct SearchCheckpoints {
    struct Point {
        long long t0 = 0;      // the anchor the search runs from (--start's tick)
        long long tick = 0;    // the checkpoint layer: the last tick the lineage carries
        // `input=press,level`, through the plan writer's own latency conversion (planEdges)
        std::vector<std::pair<long long, int>> edges;
    };
    std::atomic<bool> enabled{false};   // a subscriber exists
    std::atomic<bool> cancel{false};    // the caller has stopped caring about this search
    // WHICH CALL OWNS THE CHANNEL. Bumped by reset(), which cliMain runs at the start of every
    // call AND on the way out of it. A judgement names the call it is about, and one for a call
    // that has already returned is refused.
    std::atomic<uint64_t> call{0};
    // How many of this call's checkpoints the caller has judged as surviving. Reset with the call.
    std::atomic<size_t> judged{0};
    std::mutex m;
    std::vector<Point> points;          // this call's checkpoints, in publication order
    uint64_t seq = 0;                   // never restarts, so two calls never share an id

    void reset() {
        std::lock_guard<std::mutex> g(m);
        points.clear();
        ++seq;
        call.store(seq, std::memory_order_release);
        judged.store(0, std::memory_order_release);
    }
    void publish(Point p) {
        std::lock_guard<std::mutex> g(m);
        points.push_back(std::move(p));
    }
    size_t published() {
        std::lock_guard<std::mutex> g(m);
        return points.size();
    }
    // The caller's verdict that checkpoint `index` of call `c` survived. Accepted only in order and
    // only for the call that owns the channel; returns whether it was accepted.
    bool pass(uint64_t c, size_t index) {
        std::lock_guard<std::mutex> g(m);
        if (call.load(std::memory_order_acquire) != c) return false;
        if (index != judged.load(std::memory_order_acquire) || index >= points.size()) return false;
        judged.store(index + 1, std::memory_order_release);
        return true;
    }
};

inline SearchCheckpoints g_check;
// --phaseprof (cli.hpp): per-phase wall time of the layer loop. Print only.
inline bool g_phaseProf = false;

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
// VerdictCancelled is not a failure: the caller asked for the search to stop (g_check.cancel)
// because the game refuted one of its checkpoints. No plan file is written for it, and a caller
// must not read it as "this anchor has nothing".
enum Verdict {
    VerdictFailed = 0,
    VerdictPartial = 1,
    VerdictSolved = 2,
    VerdictCancelled = 3
};

struct SearchOutcome {
    int verdict = VerdictFailed;
    long long deepT = -1;    // where the frontier died (PARTIAL / FAILED); -1 = never reported
    double deepX = -1.0;
    long long capHits = -1;  // -1 = no capstat line, i.e. the layer loop never ran
    // VerdictCancelled only: the layer the search was on when it noticed the cancel. -1 = the
    // call was never cancelled. The caller logs it so a cancelled round says how much of the
    // search was paid for before the game refuted it.
    long long cancelT = -1;
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
    // --rejoinuse: the tick the search joined the old plan at, -1 = it did not.
    long long rejoinT = -1;
    // ...and the first tick past the join where the joined plan's walk stops retracing the old
    // plan's (a field differs, or it dies where the old walk did not). -1 = it never does.
    long long rejoinBadT = -1;
    const char* rejoinBadWhy = nullptr;   // string literal
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
        deepT = -1; deepX = -1.0; capHits = -1; replayDiedT = -1; cancelT = -1; rejoinT = -1;
        rejoinBadT = -1; rejoinBadWhy = nullptr;
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
