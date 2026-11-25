#pragma once
// Reference watch (--refwatch): follow ONE known-good trajectory through the
// search and name the gate that drops it.
//
// Why it exists. A reach sweep found windows where the model REPLAYS
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

// What differs between two shadow blocks, by name (design v4 / Orch 11-13: the dedupe gate merged two states of one key, and the State's own bytes differ only in the handle `slRef`). The carried words by their
// image offset (+0x...: the GD object the shadow images, PlayerObject unless the number is past 0xc40's neighbours), the lists and scalars by member name. Only for --refwatch's print.
inline std::string slDiffNames(const gdcol::ShadowLite& a, const gdcol::ShadowLite& b) {
    std::string s;
    char buf[48];
    int k = 0;
    for (const gdcol::SlRange& r : gdcol::kSlRanges)
        for (size_t o = r.lo; o < r.hi; o += 4, ++k)
            if (a.w[k] != b.w[k]) { std::snprintf(buf, sizeof buf, " +0x%zx(%08x>%08x)", o, a.w[k], b.w[k]); s += buf; }
    auto list = [&](const char* name, const int32_t* x, const int32_t* y, int nx, int ny) {
        if (nx != ny || std::memcmp(x, y, sizeof(int32_t) * (size_t)std::max(0, std::min(nx, ny))) != 0) { s += " "; s += name; }
    };
    list("gate", a.gate, b.gate, a.nGate, b.nGate); list("used", a.used, b.used, a.nUsed, b.nUsed); list("arr", a.arr, b.arr, a.nArr, b.nArr);
    list("now", a.now, b.now, a.nNow, b.nNow); list("ever", a.ever, b.ever, a.nEver, b.nEver); list("act", a.act, b.act, a.nAct, b.nAct);
    list("fired", a.fired, b.fired, a.nFired, b.nFired);
#define SLD(m) if (std::memcmp(&a.m, &b.m, sizeof a.m) != 0) { s += " "; s += #m; }
    SLD(layerTime) SLD(everW) SLD(pruneW) SLD(dashRot) SLD(dashElapsed) SLD(dashSpinRate) SLD(pendingSpeed) SLD(rng) SLD(lyCentre) SLD(lyHeight) SLD(lyLastPortalY) SLD(lyW764) SLD(lyFlipAmt) SLD(lyFlipFrom) SLD(lyFlipTo)
    SLD(lyFlipEl) SLD(lyLastMode) SLD(lyLastPortalUid) SLD(flags) SLD(flags2) SLD(hold) SLD(frameSteps) SLD(prevFrameSteps) SLD(ly) SLD(ly2) SLD(seedId0) SLD(seedId1)
#undef SLD
    return s.empty() ? " (none)" : s;
}

// --refadoptsl (with --refadopt; refwatch's own, observation only): a cell-mate whose shadow block differs from the reference's only in the words below is taken as the reference's stand-in (the watch goes on with
// it), so the watch ends at the first difference that is NOT one of these -- the question "which merge is the first that changes anything" instead of "which is the first merge". The words are the ones the
// first census saw (lv5 / lv14 / lv16 / lv19, design v4): the sprite's rotation pair (+0x38, +0x3c), the previous position (+0x4d4, +0xa9c) and three words that move with the rotation (+0xa10, +0xabc, +0xb40),
// and the press-state copies the key leaves out (the high byte of +0x98c: Hp; +0x990: Rp, Hm, Sm, slot A). Whether they are really harmless is what the stand-in's later ticks say (the reference trace keeps matching or not).
inline bool g_refAdoptSl = false;
inline bool slDiffBenign(const gdcol::ShadowLite& a, const gdcol::ShadowLite& b, std::string& names) {
    int k = 0;
    bool ok = true;
    char buf[40];
    for (const gdcol::SlRange& r : gdcol::kSlRanges)
        for (size_t o = r.lo; o < r.hi; o += 4, ++k) {
            if (a.w[k] == b.w[k]) continue;
            const bool benign = o == 0x38 || o == 0x3c || o == 0x4d4 || o == 0xa9c || o == 0xa10 || o == 0xabc || o == 0xb40 || o == 0x990 || (o == 0x98c && ((a.w[k] ^ b.w[k]) & 0x00ffffffu) == 0);
            std::snprintf(buf, sizeof buf, " +0x%zx", o);
            names += buf;
            if (!benign) ok = false;
        }
    // everything else must be equal: the lists, the scalars, the flags
    auto same = [&](const int32_t* x, const int32_t* y, int nx, int ny) { return nx == ny && std::memcmp(x, y, sizeof(int32_t) * (size_t)std::max(0, nx)) == 0; };
    if (!same(a.gate, b.gate, a.nGate, b.nGate) || !same(a.used, b.used, a.nUsed, b.nUsed) || !same(a.arr, b.arr, a.nArr, b.nArr) || !same(a.now, b.now, a.nNow, b.nNow)
        || !same(a.ever, b.ever, a.nEver, b.nEver) || !same(a.act, b.act, a.nAct, b.nAct) || !same(a.fired, b.fired, a.nFired, b.nFired)) ok = false;
    if (std::memcmp(&a.layerTime, &b.layerTime, sizeof a.layerTime) != 0 || a.flags != b.flags || a.flags2 != b.flags2 || a.hold != b.hold || a.frameSteps != b.frameSteps || a.ly != b.ly || a.ly2 != b.ly2
        || a.rng != b.rng || a.dashRot != b.dashRot || a.dashElapsed != b.dashElapsed || a.dashSpinRate != b.dashSpinRate || a.pendingSpeed != b.pendingSpeed || a.everW != b.everW || a.pruneW != b.pruneW) ok = false;
    return ok;
}

inline bool g_refWatch = false;
inline std::map<long long, RefRow> g_refRows;
// Matching tolerance. The reference comes out of the same stepOne as the
// search, so this is float noise, not physics: 0.01 px is already three orders
// of magnitude looser than the agreement actually observed (both sides print
// seven significant figures and match on all of them).
inline double g_refEps = 0.01;
// --tracestate (off): the replay's trace also carries `svy,sflip`, the state's own vy and flip.
// Its `vy` and `flip` are what GD's dump shows, which the state is not on two kinds of row: the
// tick a dash engages (the state keeps 0, the row shows GD's vy) and the row before a ship's or
// UFO's fly ring is applied (the row shows the ring's vy and gravity a tick early, cli.hpp rowVy).
// A search child is a state, so on those rows it never matches and the watch reported a stepping
// difference that is not one (a custom level's UFO ring: the row 3.773, the state and the child
// 1.595, all three equal on the next row). A trace that carries the columns is read through them.
inline bool g_traceState = false;
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
// --rejoinprobe N (default 5 since 2026-10; --no-rejoinprobe or N = 0 turns it off): a join whose own witness walk dies within N ticks of the join is
// not the call's answer; the call searches again without --rejoinuse (cli.hpp, after the witness
// walk). The old trace is the walk of an older model, and past the old plan's death it is only the
// model's guess. A custom level: a search joins the old plan 2 ticks past its death, the joined walk
// leaves the old one on the next tick and dies there, GD dies on that same tick, and the next search
// joins again a little further on -- six rounds of one tick's progress each. The same search without
// the join is SOLVED with a walk that does not die. Refusing every join whose walk dies anywhere was
// measured and dropped (repair.hpp's dprejoinuse note): the old trace departs as a matter of course
// and the refusals cost a second search each; this refuses only the joins that die at once.
// A walk that dies before it reaches the join is not the join's doing (the search and its own walk
// disagree on that stretch, and a search without the join meets the same stretch): it is left alone.
// [2026-11-19] N is now only on / off (N > 0): a join whose walk dies at or past the join is refused at
// whatever distance, and it is refused right after the walk, before the resim ladder is spent on a tail
// whose premise the walk has just refuted. "Within 5 ticks" was a count nobody could derive: on a heavy
// two-player custom a join's walk left the old one 1 tick past the join and died 42 ticks past it, stayed
// dead for 1,592 ticks, and the call then paid four probes, a re-search and three re-anchor rounds of the
// loop (441-512 s) for a plan the walk had refuted. In that level's cold 3 of 8 joins had a walk that left
// the old one, and exactly those 3 died in their own walk; the 5 that retraced it were clean. The
// 2026-09-19 measurement above (every refusal a second search; lv22 249 -> 462 s) was of an older model
// and of a loop that flew the dying plan instead; it is the reason to watch the cold's time, not a proof
// that this loses: the official 22 flew no join at all in the cold of 2026-11-19 (trunk 38736ad9's code).
inline long long g_rejoinProbe = 5;
inline thread_local bool t_rejoinRetry = false;   // the retry's own call (not reset per invocation)
inline bool g_rjFull = false;   // --rejoinfull: an exact rejoin also needs the dedupe key to match
inline long long g_rjJoinT = -1;
// What --rejoinprobe refused and what rejoinTailReaches declined, over the whole CALL. cliMain clears this and writes it into the outcome it returns:
// cliMainOnce resets the outcome for every attempt of the cap ladder, so a note made inside an attempt the ladder did not return was lost (a cold of a
// heavy custom lost every one of its refusals that way: two dying joins gone, no line). refusedT / refusedDieT = the last refusal, shortT / shortEndT =
// the first join not taken.
struct RejoinCallNote { long long refusedN = 0, refusedT = -1, refusedDieT = -1, shortN = 0, shortT = -1, shortEndT = -1; };
inline RejoinCallNote g_rjCall;

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
    // --tracestate's columns, when the trace has them: the state's own vy and flip
    const int cSv = col("svy"), cSf = col("sflip");
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
        if (cSv >= 0 && (int)v.size() > cSv) r.vy = std::atof(v[(size_t)cSv].c_str());
        if (cSf >= 0 && (int)v.size() > cSf) r.flip = std::atoi(v[(size_t)cSf].c_str());
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
// per level and it is visible as a vy-only mismatch. The row before a fly ring is
// the other; a trace written with --tracestate carries the state's own values.)
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
// --rejoinuse: does the old plan's tail, taken from the tick after `join`, reach where a plan of this call has to end -- the goal, or the call's own
// last planned tick (t0 + horizon; 0 = the call has no horizon and only the goal counts)? The tail is what cli.hpp appends: the rows join + 1 .. while each
// exists and carries an input. `endT` / `endX` = its last row. An old plan that was a PARTIAL, or was itself cut short, ends before either: joining it
// turns the place the old search stopped at into this call's SOLVED. (This was `the trace goes on for 600 ticks past the join`, kRjMinTail: a count that
// let a 3,742-tick tail ending at the old plan's own end through, on a heavy two-player custom -- the plan ended there, its walk had no death to find,
// the game died one tick on, and every anchor after it was a PARTIAL at that tick until the budget ran out.)
inline bool rejoinTailReaches(long long join, double goalX, long long callEndT, long long* endT = nullptr, double* endX = nullptr) {
    long long k = join;
    const RefRow* last = nullptr;
    for (auto it = g_rjRows.find(join + 1); it != g_rjRows.end() && it->first == k + 1 && it->second.act >= 0; ++it) { k = it->first; last = &it->second; }
    if (endT) *endT = k;
    if (endX) *endX = last ? last->x : 0.0;
    return last && (last->x >= goalX || (callEndT > 0 && k >= callEndT));
}
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
