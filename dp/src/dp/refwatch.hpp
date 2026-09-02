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
#include <map>

#include "dp/state.hpp"

namespace dp {

struct RefRow {
    double y = 0.0, vy = 0.0, x = 0.0;
    int mode = -1;
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

// Read the model's own trace of the reference replay.
inline bool loadRefTrace(const char* path) {
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
              cV = col("vy"), cM = col("mode");
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
        g_refRows[std::atoll(v[(size_t)cT].c_str())] = r;
    }
    return !g_refRows.empty();
}

// Is this state the reference row? Mode is compared when the trace carries it:
// two states can share y and vy across a mode change, and calling that a match
// would report the reference as alive in a frontier it has left.
inline bool refMatches(const State& s, const RefRow& r) {
    if (!r.have) return false;
    if (r.mode >= 0 && (int)s.mode != r.mode) return false;
    return std::fabs((double)s.y - r.y) <= g_refEps
        && std::fabs((double)s.vy - r.vy) <= g_refEps;
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
inline int refFindExact(const std::vector<State>& v, const State& s) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].y == s.y && v[i].vy == s.vy && v[i].mode == s.mode
            && v[i].grounded == s.grounded && v[i].flip == s.flip)
            return (int)i;
    return -1;
}

// The survivor of a merged state's own cell, nearest in (y, vy).
inline int refNearestInCell(const std::vector<State>& v, uint64_t key,
                            const State& want,
                            uint64_t (*keyFn)(const State&)) {
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
