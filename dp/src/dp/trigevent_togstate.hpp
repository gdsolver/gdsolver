#pragma once
#include <atomic>
// P4 / P5 wiring of restructure draft section 7.7 (the world's tog, kept in every run): the world's group on / off bits (State::tog) and the x-crossing gate digest (State::xgate), updated once per tick from the
// parent's and the child's x and the button's edge. The pure parts (TogBits, xgateAdd) are in trigevent_world.hpp; the Tap rule (an edge after the crossing acts, a press applies ttog, a release with thold the opposite,
// a Stop ends it) is trigtouch.hpp's, written here as a function of the two x values so that nothing about the Tap has to be stored: "armed" is "the parent's x is already at or past the Tap's cx" and "stopped" is "the
// parent's x is already at or past the Stop's cx" (the crossing x, which is the farthest x reached when --tpback has moved the player back). The dedupe key reads State::tog / xgate (search_key.hpp).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "dp/trigevent_orb.hpp"
#include "dp/trigevent_world.hpp"

namespace dp {
namespace gdtrig {

inline int g_togTrace = 0;     // --togtrace: print every change of a group and every x-gate crossing (the replay walk only)

struct TogTap { int uid = 0, idx = 0, target = 0, thold = 0, ttog = 0; double cx = 0.0, stopCx = 1e300; bool chain = false; int slot = -1, cap = 255; bool perPress = false; int sat = 0; bool narrow = false; };   // narrow: a chain Tap whose only effect on the world is a plain Move spawned at every press (TapMove): State::tapN / tapWin count its presses, the timeline does not run it, and shadowApplyTapMoves puts the moved objects where the presses took them   // perPress: the group's chain does something at every press (the window of edges tells it); else it only feeds Counts: cap = what they read, sat = the steps what they start keeps running   // chain: the Tap's group holds a trigger the shadow runs from its presses (TrigLive, game_event.hpp kTapTok)
// The plain Moves a narrow Tap spawns at every press (trigshadow_live.hpp builds, shadow_step.hpp shadowApplyTapMoves reads): the offset of a press and its ease, and the uids of the objects of the Move's target group.
struct TapMove { int tapUid = 0; double dx = 0, dy = 0, dur = 0, rate = 2.0; int kind = 0; std::vector<int> uids; };
inline std::vector<TapMove> g_tapMoves;
// What the narrow Taps' offsets did in the call (printed with the call's stages): the movers an offset was laid on, and how many of those were laid while the state's player was past the Tap's past x -- there the count has left the key, so such a mover is an unsound merge (it stays 0 when the bound is right).
inline std::atomic<long long> g_tapMoveApplied{0}, g_tapMoveUnsound{0};
struct TogOrb { int uid = 0, idx = 0, target = 0; bool on = false; OrbRect r; bool multi = false; };   // a Toggle Orb 1594: a hit switches its group on (actgrp 1) or off (actgrp 0)
struct TogGate { int uid = 0; double cx = 0.0; std::vector<int> idx; };   // idx: the toggleable groups the trigger sits in
// A level object (not a trigger) in a group that only a Tap switches (buildTogTable): the shadow's world keeps it off while any of these groups is off (shadow_step.hpp shadowApplyTapGates).
struct TogObj { int uid = 0; std::vector<int> idx; };
struct TogTable {
    std::vector<int> groups;         // groups[i] = the group id of bit i (ascending)
    std::vector<uint8_t> initialOn;  // groups start on; kept per bit so a level that says otherwise has a place
    std::vector<TogTap> taps;        // ascending uid (the order TapDecider's std::map gives)
    std::vector<TogGate> gates;      // ascending cx
    std::vector<TogOrb> orbs;        // ascending uid
    std::vector<TogObj> objs;        // ascending uid
    std::vector<int> tapMembers;     // ascending: every object (not a trigger) in a group some Tap targets, whatever else switches that group (the recording's on / off of these is the Tap's as well as the Toggles')
    const TogOrb* findOrb(int uid) const {
        const auto it = std::lower_bound(orbs.begin(), orbs.end(), uid, [](const TogOrb& o, int u) { return o.uid < u; });
        return (it != orbs.end() && it->uid == uid) ? &*it : nullptr;
    }
    bool empty() const { return taps.empty() && gates.empty(); }
};
inline TogTable g_togTable;
inline uint64_t g_togTableGen = 0;   // bumped at every rebuild of g_togTable (a thread's cache of the gated objects' world indices is stamped with it)

struct TogChange { int uid = 0, group = 0; bool on = false; bool gate = false; bool fired = false; };

inline bool togGroupOn(const TogTable& T, const TogBits& tog, int i) { return (T.initialOn[(size_t)i] != 0) != tog.differs(i); }

// The Toggle Orbs a button press hits at this tick, by the PUSH path only (trigevent_orb.hpp orbDecide: the player's box at the row BEFORE the call row against the orb's rect; the contact path and the once-only
// rule are not modelled -- on c133224341 all six clicks were pushes, and a second click of the same orb sets the same value). Ascending uid.
inline void togOrbPushHits(const TogTable& T, const OrbPlayer& prevRow, bool pushNow, std::vector<int>& hits) {
    hits.clear();
    if (!pushNow) return;
    OrbInput in; in.pushNow = true; in.contactPath = false;
    for (const TogOrb& o : T.orbs)
        if (orbDecide(o.r, prevRow, prevRow, in, false, true).fire) hits.push_back(o.uid);
}

// One tick. px / kx: the crossing x before and after the tick (State::xAbs, or the farthest x when a teleport moved the player back: crossXOf). prevHeld / input: the button before and during the tick.
// The button is applied first (the toggle is logged at the tick of the push), then the crossings of this tick. Returns the changes (for --togtrace) only when `out` is given.
inline void togStep(const TogTable& T, TogBits& tog, uint64_t& xgate, double px, double kx, int prevHeld, int input, std::vector<TogChange>* out = nullptr, const std::vector<int>* orbHits = nullptr) {
    const int edge = (input && !prevHeld) ? 1 : (!input && prevHeld) ? 2 : 0;
    if (edge) {
        for (const TogTap& t : T.taps) {
            if (!(px >= t.cx)) continue;            // not armed: the crossing's own tick does not act
            if (px >= t.stopCx) continue;           // a Stop already crossed: the Tap has ended
            const bool pressOn = t.ttog == 1;
            bool on;
            if (edge == 1) on = pressOn;
            else if (t.thold) on = !pressOn;
            else continue;
            tog.set(t.idx, on, T.initialOn[(size_t)t.idx] != 0);
            if (out) { TogChange c; c.uid = t.uid; c.group = t.target; c.on = on; out->push_back(c); }
        }
    }
    // the Toggle Orbs the player hit this tick (the orb producer's result: trigevent_orb.hpp orbDecide), ascending uid; last write wins
    if (orbHits)
        for (int u : *orbHits)
            if (const TogOrb* o = T.findOrb(u)) {
                tog.set(o->idx, o->on, T.initialOn[(size_t)o->idx] != 0);
                if (out) { TogChange c; c.uid = u; c.group = o->target; c.on = o->on; out->push_back(c); }
            }
    for (const TogGate& g : T.gates) {
        if (g.cx > kx) break;                       // sorted by cx
        if (!(px < g.cx)) continue;                 // crossed in an earlier tick
        bool fired = true;
        for (int i : g.idx) fired = fired && togGroupOn(T, tog, i);
        xgate = xgateAdd(xgate, g.uid, fired);
        if (out) { TogChange c; c.uid = g.uid; c.gate = true; c.fired = fired; out->push_back(c); }
    }
}

}  // namespace gdtrig
}  // namespace dp
