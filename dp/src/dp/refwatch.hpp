#pragma once
// Reference watch (--refwatch): follow ONE known-good trajectory through the
// search and name the gate that drops it.
//
// Why it exists. brief-017's reach sweep found windows where the model REPLAYS
// the verified solution with zero divergence for thousands of ticks and its own
// reachability still dies inside the window (lv22 windows at t=6,970 and 7,190:
// replay survives to 11,596, frontier dies at 7,338 / 7,355). The physics is
// right and the SEARCH drops the passage. "Which gate dropped it" is not
// answerable from the outside: the PARTIAL line gives a tick and an x, and
// every discard site in the layer -- kill, out-of-play, deadband, dedupe merge,
// cap eviction -- removes a state with no per-state record.
//
// This is the DP-side counterpart of the section solver's spine (secsolve.hpp),
// and it is deliberately the WEAKER of the two. The spine PINS its rollout,
// exempting it from dedupe and cap; the watch pins nothing and changes nothing.
// It only asks, at each layer, whether the reference is still there, and the
// first time it is not, says what happened to it. A watch that altered the
// search would answer a question about a different search.
//
// The reference is a leveldp .trace.csv: the model's own replay of the solution
// from the same anchor the search starts at. It already exists wherever this is
// useful -- the reach probe and the anchored diff build one per window.
#include <cstring>
#include <map>

#include "dp/frames.hpp"
#include "dp/state.hpp"

namespace dp {

struct RefRow {
    double y = 0.0, vy = 0.0, x = 0.0;
    int mode = -1;
    // The reference's OWN record of the input level that stepped this tick, and
    // of the gravity it was in. `act` is what makes the choice of child a
    // reading rather than an inference: on a swing's press tick the two children
    // are identical in y, vy and mode, so nothing observable distinguishes them
    // and any rule based on the trajectory alone would be guessing.
    int act = -1;
    int flip = -1;
    // The TURNED FRAME. In a rotation section y is measured in the frame's own
    // axes, so comparing a state in one frame with a reference in another is
    // comparing two different quantities -- measured, it produces differences of
    // thousands of px and it accounted for most of the first sweep's
    // `cannot-reproduce` count.
    int frame = -1;
    int dead = -1;   // the witness walk's own kill on this tick (1/0); -1 = no such column
    // The search's own dedupe key of the walk's state on this tick (keyOf), and whether the trace
    // carried it. --rejoinfull requires it to match: y, vy, mode, flip and frame are what the trace
    // shows, and a state equal on those can still differ in what the key holds (held, grounded,
    // the trigger and orb bits) -- lv20's t=17137 join left the old walk the very next tick.
    uint64_t key = 0;
    bool haveKey = false;
    bool have = false;
};

inline bool g_refWatch = false;
inline std::map<long long, RefRow> g_refRows;
// Matching tolerance. The reference comes out of the same stepOne as the
// search, so this is float noise, not physics: 0.01 px is already three orders
// of magnitude looser than the agreement actually observed (both sides print
// seven significant figures and match on all of them).
inline double g_refEps = 0.01;
// Per-layer working state, all reset each tick.
//
// THE WATCH STOPS AT THE FIRST GATE and does not carry on past it. Following the
// survivor of a merge was tried and removed: past a merge the carrier is not on
// the reference's trajectory, so nothing after that point can be matched against
// the trace, and every further line would be about a different path while
// looking like it was about this one.
//
// How far the kept representative is, though, is recorded -- because that is the
// whole question about a merge. A cell standing in for a state 0.19 px/tick of
// vy away is doing its job; one standing in for a state much further away is
// representing a passage with something that may not make it.
inline double g_refDriftVy = 0.0;
inline double g_refDriftY = 0.0;
inline int g_refParent = -1;      // index into `cur` of the reference's parent
// BOTH children of the tracked state, indexed by action. Which one continues
// the reference is decided BY THE REFERENCE -- the child that matches the next
// trace row -- rather than by reading an input out of the plan.
//
// That is not a convenience. Which tick's plan input a layer applies is exactly
// the off-by-one that cost the section solver's spine a day (secspineoff, swept
// rather than derived because two derivations disagreed), and a watch that got
// it wrong would follow a DIFFERENT trajectory and report an honest-looking
// kill on it. Letting the trace pick removes the question: if neither child
// matches, that is itself the finding and it is reported as one.
inline int g_refKidFate[2] = {-1, -1};   // -1 unknown, 0 not expanded, 1 died, 2 alive
inline const char* g_refKidWhy[2] = {"", ""};
inline uint64_t g_refKidKey[2] = {0, 0};
inline State g_refKidState[2]{};
inline long long g_refLostAt = -1;

// --rejoinwatch <trace.csv> --rejoinafter <tick> (print only): the model's trace of
// the plan the game last flew, and the tick it died on. Past that tick, how soon does the new
// search's frontier come back onto the old trajectory -- exactly, within 2 px / 0.5 vy, within
// 8 px / 2 vy? The earliest such tick bounds how much of the old plan's tail a search that joined
// it could have kept instead of solving again. Same mode, flip and frame only; y and vy compared
// as refMatches compares them.
inline bool g_rjOn = false;
inline std::map<long long, RefRow> g_rjRows;
inline long long g_rjAfter = -1;
inline long long g_rjFirst[3] = {-1, -1, -1};   // exact, near2, near8
inline int g_rjMode[3] = {-1, -1, -1};
inline long long g_rjLayers = 0, g_rjLayersNear[3] = {0, 0, 0};
inline uint32_t g_rjOldNode = 0xffffffffu;   // arena node of the old path at the death tick
// --rejoinuse: stop at the first exact rejoin and emit the new prefix followed by
// the old trace's own inputs (its `act` column) from the next tick on.
inline bool g_rjUse = false;
inline bool g_rjFull = false;   // --rejoinfull: an exact rejoin also needs the dedupe key to match
inline long long g_rjJoinT = -1;

// Read the model's own trace of the reference replay.
inline bool loadTraceInto(const char* path, std::map<long long, RefRow>& rows);
inline bool loadRefTrace(const char* path) { return loadTraceInto(path, g_refRows); }
inline bool loadTraceInto(const char* path, std::map<long long, RefRow>& rows) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    if (!std::getline(f, line)) return false;   // header
    std::vector<std::string> cols;
    {
        std::stringstream ss(line);
        std::string c;
        while (std::getline(ss, c, ',')) cols.push_back(c);
    }
    auto col = [&](const char* name) -> int {
        for (size_t i = 0; i < cols.size(); ++i)
            if (cols[i] == name) return (int)i;
        return -1;
    };
    const int cT = col("tick"), cX = col("x"), cY = col("y"),
              cV = col("vy"), cM = col("mode"), cA = col("act"),
              cF = col("flip"), cFr = col("frame"), cD = col("dead"), cK = col("key");
    if (cT < 0 || cY < 0 || cV < 0) return false;
    while (std::getline(f, line)) {
        std::vector<std::string> v;
        std::stringstream ss(line);
        std::string c;
        while (std::getline(ss, c, ',')) v.push_back(c);
        if ((int)v.size() <= cT) continue;
        RefRow r;
        r.have = true;
        r.y = std::atof(v[(size_t)cY].c_str());
        r.vy = std::atof(v[(size_t)cV].c_str());
        r.x = cX >= 0 && (int)v.size() > cX ? std::atof(v[(size_t)cX].c_str()) : 0.0;
        r.mode = cM >= 0 && (int)v.size() > cM ? std::atoi(v[(size_t)cM].c_str()) : -1;
        r.act = cA >= 0 && (int)v.size() > cA ? std::atoi(v[(size_t)cA].c_str()) : -1;
        r.flip = cF >= 0 && (int)v.size() > cF ? std::atoi(v[(size_t)cF].c_str()) : -1;
        r.frame = cFr >= 0 && (int)v.size() > cFr ? std::atoi(v[(size_t)cFr].c_str()) : -1;
        r.dead = cD >= 0 && (int)v.size() > cD ? std::atoi(v[(size_t)cD].c_str()) : -1;
        if (cK >= 0 && (int)v.size() > cK && !v[(size_t)cK].empty()) {
            r.key = std::strtoull(v[(size_t)cK].c_str(), nullptr, 10);
            r.haveKey = true;
        }
        rows[std::atoll(v[(size_t)cT].c_str())] = r;
    }
    return !rows.empty();
}

// Is this state the reference row? Mode is compared when the trace carries it:
// two states can share y and vy across a mode change, and calling that a match
// would report the reference as alive in a frontier it has left.
// THE TRACE IS IN WORLD COORDINATES AND THE STATE IS NOT. The replay writes
// wX/wY (see where it composes a row) and `vy * -1 in frame 3`, because GD's
// dump reports the player's velocity in the current gameplay frame. Comparing
// the raw fields is comparing two different quantities: measured, it made 15 of
// this sweep's 26 `cannot-reproduce` cases, with dy in the thousands of px and
// both sides reporting the SAME frame -- the giveaway that the mismatch was in
// the units, not the physics.
//
// (The one place this is still not exact is a dash-ring tick, where the trace
// deliberately emits GD's vy while the state keeps 0. That is a handful of ticks
// per level and it is visible as a vy-only mismatch.)
inline void refWorldOf(const State& s, double& wy, double& wvy) {
    wy = (double)s.y;
    wvy = (double)s.vy;
    if (s.frame & 3) {
        double wx = 0.0;
        fromFrame((int)s.frame, (double)s.xAbs, (double)s.y, wx, wy);
        if (s.frame == 3) wvy = -wvy;
    }
}

inline bool refMatches(const State& s, const RefRow& r) {
    if (!r.have) return false;
    if (r.mode >= 0 && (int)s.mode != r.mode) return false;
    if (r.flip >= 0 && (int)s.flip != r.flip) return false;
    if (r.frame >= 0 && (int)s.frame != r.frame) return false;
    // WHICH COORDINATES THE REFERENCE IS IN. A GD dump is world -- it has no
    // notion of the model's turned frames -- so a state in frame 1 or 3 has to
    // be converted before it can be compared. A reference that CARRIES a frame
    // column is a model trace instead, written in that frame's own axes
    // (cli.hpp's witness resim), and converting the state would then compare a
    // world y against a frame-local one. Measured: on lv22 from t=5,600 the two
    // agreed to seven figures on y, vy, flip and frame and the match was still
    // refused, because one side had been turned and the other had not.
    //
    // Gated on `r.frame >= 0`, so a reference without the column behaves exactly
    // as before -- which is every GD-derived reference, including the ones
    // py/secqueue.py's refwatch_sweep feeds in.
    double wy = (double)s.y, wvy = (double)s.vy;
    if (r.frame < 0) refWorldOf(s, wy, wvy);
    return std::fabs(wy - r.y) <= g_refEps
        && std::fabs(wvy - r.vy) <= g_refEps;
}

// The old plan's own path is usually still IN the new search -- the game killed it, the model
// did not -- so the frontier "matches" the old trace right after the death by simply being it.
// A rejoin is a state that was NOT on the old path at the death tick and comes back onto the old
// trace later. `fromOld(s)` answers "does s descend from the old path's state at the death tick"
// (an arena walk, cli.hpp); only the first few candidates per tier are walked, and only within
// kRjWindow ticks of the death, to bound that walk.
constexpr long long kRjWindow = 3000;
constexpr int kRjWalks = 4;
constexpr long long kRjMinTail = 600;   // --rejoinuse: ticks the old plan must go on past a join
// Returns the index in `v` of the first exact rejoin found on this layer, -1 if none.
template <class FromOld, class SameKey>
inline int rejoinLayer(const std::vector<State>& v, long long t, FromOld fromOld, SameKey sameKey) {
    if (!g_rjOn || t <= g_rjAfter || t > g_rjAfter + kRjWindow) return -1;
    const auto it = g_rjRows.find(t);
    if (it == g_rjRows.end()) return -1;
    int exactIdx = -1;
    const RefRow& r = it->second;
    ++g_rjLayers;
    int walks[3] = {0, 0, 0};
    bool hit[3] = {false, false, false};
    for (size_t si = 0; si < v.size(); ++si) {
        const State& s = v[si];
        if (r.mode >= 0 && (int)s.mode != r.mode) continue;
        if (r.flip >= 0 && (int)s.flip != r.flip) continue;
        if (r.frame >= 0 && (int)s.frame != r.frame) continue;
        double wy = (double)s.y, wvy = (double)s.vy;
        if (r.frame < 0) refWorldOf(s, wy, wvy);
        const double dy = std::fabs(wy - r.y), dv = std::fabs(wvy - r.vy);
        const bool in[3] = {dy <= g_refEps && dv <= g_refEps && sameKey(s, r),
                            dy <= 2.0 && dv <= 0.5, dy <= 8.0 && dv <= 2.0};
        int k = 0;
        while (k < 3 && !in[k]) ++k;          // the tightest tier this state is in
        if (k == 3 || hit[k] || walks[k] >= kRjWalks) continue;
        ++walks[k];
        if (fromOld(s)) continue;
        for (int j = k; j < 3; ++j) hit[j] = true;
        if (hit[0]) { exactIdx = (int)si; break; }
    }
    for (int k = 0; k < 3; ++k)
        if (hit[k]) {
            ++g_rjLayersNear[k];
            if (g_rjFirst[k] < 0) { g_rjFirst[k] = t; g_rjMode[k] = r.mode; }
        }
    return exactIdx;
}

inline int refFind(const std::vector<State>& v, const RefRow& r) {
    for (size_t i = 0; i < v.size(); ++i)
        if (refMatches(v[i], r)) return (int)i;
    return -1;
}

// The tracked state, once the watch is following the search's own carrier
// rather than the trace: matched by value, because it IS the state that was
// pushed (no tolerance needed, and a tolerance here would silently adopt a
// neighbour when the real one was dropped).
//
// rHover AND held ARE PART OF THE IDENTITY, not decoration. On a swing's press
// tick the two children are IDENTICAL in y, vy and mode -- the tap only sets a
// pending bit and the flip lands next tick -- so a match on the visible fields
// alone adopts whichever comes first and the watch carries the wrong one. That
// is not hypothetical: it is what made the watch report `cannot-reproduce` two
// ticks after a flip that the search had in fact modelled correctly.
// THE WHOLE STRUCT, minus the search's own bookkeeping. Listing fields by hand
// was wrong three times in a row -- first rHover (the swing's pending flip),
// then boost (a pad pinning vy), and each time the watch adopted a twin that
// was identical in everything it happened to compare and one gravity step apart
// on the tick after. The child was pushed into the layer verbatim, so an exact
// comparison is available and there is no reason to choose which fields matter:
// anything added to State later is part of the identity automatically.
//
// `parent` and `action` are excluded because the layer overwrites them after the
// push; they say where the state came from, not what it is.
inline bool refSameState(const State& a, const State& b) {
    State x = a, y = b;
    x.parent = y.parent = 0;
    x.action = y.action = 0;
    return std::memcmp(&x, &y, sizeof(State)) == 0;
}

inline int refFindExact(const std::vector<State>& v, const State& s) {
    for (size_t i = 0; i < v.size(); ++i)
        if (refSameState(v[i], s)) return (int)i;
    return -1;
}

// The survivor of a merged state's own cell, nearest in (y, vy).
// keyFn is a template parameter rather than a function pointer because keyOf
// takes the layer's tick now, so the caller passes a lambda that captures it --
// and a capturing lambda does not convert to a plain pointer. The tick must be
// the one the compared keys were built at: a key from another layer silently
// fails to match and reads as "the reference left the frontier".
template <class KeyFn>
inline int refNearestInCell(const std::vector<State>& v, uint64_t key,
                            const State& want, KeyFn keyFn) {
    int best = -1;
    double bd = 1e18;
    for (size_t i = 0; i < v.size(); ++i) {
        if (keyFn(v[i]) != key) continue;
        const double d = std::fabs((double)v[i].y - (double)want.y)
                       + std::fabs((double)v[i].vy - (double)want.vy);
        if (d < bd) { bd = d; best = (int)i; }
    }
    return best;
}

}  // namespace dp
