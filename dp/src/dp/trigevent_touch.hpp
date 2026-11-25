#pragma once
// The producer of the Touch-Triggered events for the live search (restructure draft P4): the same decision as trigtouch.hpp's TouchDecider -- a touch trigger is called on the rising edge of the player's
// overlap, once per touch, again after the player left only if it is Multi Activate -- but from what the SEARCH has (the set of touch boxes the state overlaps this tick and the tick before, State::trig)
// instead of the body shadow's pass (SelfOut::fired). It returns the starting Event only ({kEvTouch, step = call tick + 1, uid}); what the called trigger does (a Spawn's delay, remap and next Spawns, a
// Move, a Toggle) is the trigger shadow's own chain walk (TrigShadow::addTouchEvent -> fireTouch).
//
// A pure function over its inputs; the state it keeps between ticks (`calledEver`) is the caller's, one per world line, so two branches never share it.
#include <set>
#include <vector>
#include "dp/trigevent.hpp"

namespace dp {
namespace gdtrig {

struct TouchEventRules {
    std::set<int> multi;        // the trigger uids that may be called again after the player left (Multi Activate); empty = every rising edge counts
    bool onceOnly = false;      // true: a trigger not in `multi` is called at most once per attempt
};

// `callTick`: the game tick of the pass whose overlap is `cur` (the shadow applies it at step callTick + 1).
// `prev`: the uids overlapped by the previous tick's pass. `cur`: the uids overlapped by this tick's pass, in the pass's order (the order the calls are applied in).
// `calledEver`: the uids already called this attempt (updated). Returns the calls of this tick, in `cur`'s order.
inline std::vector<Event> touchEvents(long long callTick, const std::set<int>& prev, const std::vector<int>& cur,
                                      const TouchEventRules& rules, std::set<int>& calledEver) {
    std::vector<Event> out;
    std::set<int> seen;
    for (int u : cur) {
        if (!seen.insert(u).second) continue;                    // one call per uid per tick
        if (prev.count(u)) continue;                             // still inside: not a rising edge
        if (rules.onceOnly && !rules.multi.count(u) && calledEver.count(u)) continue;
        Event e;
        e.kind = kEvTouch;
        e.step = callTick + 1;
        e.uid = u;
        out.push_back(e);
        calledEver.insert(u);
    }
    return out;
}

// The live search's form: the state carries a LATCH (State::trig, bit b = the b-th touch box of the window, State::fireB[b] = the game tick it was entered on), so a call is a bit that is set in
// the state and not in its parent: `{kEvTouch, step = fireB[b] + 1, uid = boxUid[b]}`, in ascending bit order (the latch does not keep the pass's order). The latch holds the FIRST entry of a box
// only, which is touchEvents() with onceOnly = true and no Multi Activate trigger. `nBits` <= 64 (the window's width); `boxUid[b]` is the trigger uid of bit b (-1: no trigger, skipped).
inline std::vector<Event> touchEventsFromLatch(uint64_t parentBits, uint64_t curBits, const uint16_t* fireB, const std::vector<int>& boxUid) {
    std::vector<Event> out;
    const uint64_t fresh = curBits & ~parentBits;
    for (size_t b = 0; b < boxUid.size() && b < 64; ++b) {
        if (!((fresh >> b) & 1ull) || boxUid[b] < 0) continue;
        Event e;
        e.kind = kEvTouch;
        e.step = (long long)fireB[b] + 1;
        e.uid = boxUid[b];
        out.push_back(e);
    }
    return out;
}

}  // namespace gdtrig
}  // namespace dp
