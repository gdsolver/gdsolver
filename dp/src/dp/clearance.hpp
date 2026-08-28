#pragma once
#include "dp/step.hpp"

// ---------------------------------------------------------------------------
// CLEARANCE PROBE -- instrumentation only, off unless --clearprobe.
//
// The question it exists to answer: when the search commits to a state that
// sits a pixel from something that kills it, DID IT HAVE A SLACKER ALTERNATIVE
// IN HAND?  Measured, because the answer decides where (and whether) a
// clearance preference is worth writing:
//
//   M1  within one dedupe cell, how far apart are the children's clearances?
//       If that spread is ~0 there is nothing to prefer and the idea is dead.
//   M2  how often does the surviving representative have LESS room than one it
//       displaced, and by how much?  That is the headroom of the preference.
//   M3  how many states reach goalX?  The final pick is "smallest accepted
//       ordinal", which is arbitrary; if several reach the goal, choosing the
//       roomiest is free.
//
// Nothing here is read by the search.  It only ever looks at children that
// have already been stepped and keyed, so it cannot change a plan -- the
// acceptance test for that is bit-identical output with the flag off.
//
// TWO NUMBERS, NEVER ONE.  The player's box is not one box: the wave's solid
// half is 5.0 while its hazard test uses a different one, and the cube has an
// outer box for landing and an inner one for side kills.  A single "clearance"
// would correspond to neither boundary, so solid and hazard room are carried
// and reported separately and the reader decides which one carries the signal.
//
// Y ONLY.  x is fixed by the layer -- every state in a layer has travelled the
// same distance -- so the vertical gap is the only room the search can
// actually choose to keep.  (The census in the lab note measured x penetration
// too; that says where the state ended up, not what the DP could have done.)
// ---------------------------------------------------------------------------

namespace dp {

inline bool g_clearProbe = false;

// ---- the route score: the rule the final pick uses ------------------------
//
// Every state carries how many ticks its lineage spent with less room than
// kTightPx, and the FINAL PICK among the states that reached goalX is made on
// that count. What it replaced was "the smallest ACCEPTED ordinal", i.e. the
// child's index -- a number with no physical meaning at all, deciding between
// 2,000 complete solutions whose lineages stay distinct for 3,345 ticks (lv16,
// measured). Nothing else moves: the dedupe, the cap and reachability are
// untouched, and every candidate it chooses between is ALREADY a complete
// solution, so the worst case is a different valid plan, never a lost one.
//
// Was a flag (--clearscore) while it was being measured. Made unconditional on
// 2026-08-28 against a full 22-level cold A/B on one build:
//   iterations 294 -> 291, no level worse, 22/22 cleared in both arms
//   cost +1% on an isolated DP solve; undetectable in an isolated cold run
//   (lv20 395 s -> 371 s at identical iterations -- the +10% first seen came
//   from running four workers at once, and the easy levels moved -6..-13% in
//   the same column, which is how that was caught)
//
// The gain is small and it is one level's (lv16, -3). It is kept because the
// thing it replaced was arbitrary: there is no version of "pick by child index"
// that is worth defending, and this costs nothing to carry.
//
// "Less room than the model is accurate to." The archived fixups (51k records,
// 22 levels) put the model's disagreement with GD at p50 0.81 px and p99 3.72
// px at the ticks it has to be repaired on, so below ~4 px a model error of the
// size that actually occurs can flip a verdict, and above it cannot -- the
// threshold is read off that distribution, not picked round. Swept before it
// was fixed: 0.5 / 1 / 2 / 4 px all rank the same way (lv16 saves 3/9/11/19
// ticks of a 134/144/162/223 span), so the choice inside that range is not
// load-bearing.
constexpr double kTightPx = 4.0;

// Nothing is looked for beyond this; a state with more room than this is
// simply "clear" and the exact figure would not change any decision.
constexpr double kClearLimit = 64.0;

// The hazard test's own half for this mode (step.hpp uses the wave's separate
// one; every other mode tests hazards with the ordinary player half).
inline double hazardHalfFor(int mode, int mini) {
    if (mode == 4) return mini ? kWaveHazHalfMini : kWaveHalf;
    return playerHalf((uint8_t)mode, mini != 0);
}

// Signed vertical room to the nearest SOLID face: positive = free space,
// negative = already that deep inside one.  Slopes are skipped (their surface
// is a line, not this rect) and counted, and a one-way platform only counts as
// a floor because that is the only side it blocks.
inline float solidClearance(const std::vector<const Obj*>& near, double px,
                            double py, double half, long long* skipped) {
    const double pl = px - half, pr = px + half;
    const double pb = py - half, pt = py + half;
    double best = kClearLimit, worst = 0.0;
    bool embedded = false;
    for (const Obj* o : near) {
        if (o->type != 0) continue;
        if (o->slope) { if (skipped) ++*skipped; continue; }
        const double l = o->cx - o->hw, r = o->cx + o->hw;
        if (r <= pl || l >= pr) continue;              // no x overlap: not in the way
        const double b = o->cy - o->hh, tt = o->cy + o->hh;
        if (tt <= pb) {                                 // below the player
            best = std::min(best, pb - tt);
        } else if (b >= pt) {                           // above it
            if (o->oneway) continue;                    // does not block from below
            best = std::min(best, b - pt);
        } else {                                        // overlapping in y
            if (o->oneway) continue;                    // walks straight through
            embedded = true;
            worst = std::max(worst, std::min(tt - pb, pt - b));
        }
    }
    return (float)(embedded ? -worst : best);
}

// Vertical room before a HAZARD would take it.  Found by bisecting the model's
// OWN hazardHit rather than by measuring rects: that predicate already knows
// the saw discs, the turned boxes and the per-mode margins, and a second
// implementation here would drift away from the one the search actually uses.
inline float hazardClearance(const std::vector<const Obj*>& near, double px,
                             double py, double half, int mode, int mini) {
    auto hit = [&](const Obj* o, double y) {
        return hazardHit(o, px, y, half, kHazMargin, sawMarginFor((uint8_t)mode),
                         mode, mini);
    };
    double best = kClearLimit;
    for (const Obj* o : near) {
        if (o->type != 2 && o->type != 47) continue;
        if (hit(o, py)) return 0.0f;     // already touching (a sub-step killed it)
        // ONE HAZARD AT A TIME, bracketed against its own centre.
        //
        // The first version bisected "does ANY hazard hit" between py and
        // py +- 64 and took the endpoint as the far bracket. That silently
        // assumes the hit set grows with distance, and it does not: a spike
        // 10 px above is missed at 64 px, so the probe called it clear. The
        // symptom was lv17 -- a WAVE level -- reporting p10 = p50 = p90 = 64 px
        // of hazard room for the wave, i.e. no hazard ever within 64 px.
        //
        // Bracketing on the object's own cy fixes it: the hit set for a single
        // hazard IS an interval in y, so a confirmed miss at py and a confirmed
        // hit at cy bound its near edge, and bisection between them is exact.
        // A hazard that does not hit even at its own centre cannot be reached
        // by vertical motion at this x at all, and is skipped.
        const double d = o->cy - py;
        if (std::fabs(d) > kClearLimit || !hit(o, o->cy)) continue;
        double lo = 0.0, hi = d;         // lo = miss (py), hi = hit (its centre)
        for (int i = 0; i < 12; ++i) {   // -> |d|/4096, well under 0.02 px
            const double mid = 0.5 * (lo + hi);
            (hit(o, py + mid) ? hi : lo) = mid;
        }
        best = std::min(best, std::fabs(lo));
    }
    return (float)best;
}

// Is this position tight? ONE PASS, no bisection, boolean answer.
//
// The probe above measures a distance and pays for it (a bisection per hazard);
// this runs on every child of every layer, and the score only ever asks "under
// eps or not". The hazard side uses the DUMPED rect rather than the real kill
// shape, so it over-reports -- a spike's kill box is smaller than its rect. That
// is acceptable and it is the reason this is a SCORE and not a physics test: it
// ranks routes consistently, it never decides whether anything lives or dies.
inline bool tightHere(const std::vector<const Obj*>& near, double px, double py,
                      double half, double eps) {
    const double pl = px - half, pr = px + half;
    const double pb = py - half, pt = py + half;
    for (const Obj* o : near) {
        if (o->type == 0) {
            if (o->slope || o->oneway) continue;
            if (o->cx + o->hw <= pl || o->cx - o->hw >= pr) continue;
            const double b = o->cy - o->hh, t = o->cy + o->hh;
            if (t <= pb) { if (pb - t < eps) return true; }
            else if (b >= pt) { if (b - pt < eps) return true; }
            else return true;                       // already inside one
        } else if (o->type == 2 || o->type == 47) {
            if (o->radius > 0.0) {
                const double dx = px - o->cx, dy = py - o->cy;
                const double r = o->radius + half + eps;
                if (dx * dx + dy * dy < r * r) return true;
            } else if (std::fabs(px - o->cx) < o->hw + half + eps
                       && std::fabs(py - o->cy) < o->hh + half + eps) {
                return true;
            }
        }
    }
    return false;
}

// Lower is roomier. Ties keep the incumbent, so with the score off (every
// `tight` still 0) this is exactly "the first accepted ordinal wins" -- the rule
// that is there today, which is how the flag-off path stays bit-identical.
inline bool roomierRoute(uint16_t cand, uint16_t held) { return cand < held; }

// One live child, as the probe sees it.
struct ClearSample {
    uint64_t key;
    float vy;
    float solid;
    float haz;
    uint8_t mode;
    uint8_t goal;    // 1 = this child reached goalX
};

// Per-mode tallies.  Kept as raw samples because the interesting statistics are
// medians and tails, and a running mean would hide exactly the shape that
// decides this (a few cells with a lot of spread reads the same as none).
struct ClearAcc {
    struct Mode {
        std::vector<float> spreadSolid, spreadHaz;   // M1
        std::vector<float> lostSolid, lostHaz;       // M2: room the merge threw away
        long long cells = 0, cellsMulti = 0, kids = 0;
        // The raw clearances, subsampled. INSTRUMENT CHECK, not a result: a
        // spread of 0 means nothing until you can see that the underlying
        // numbers vary at all. If these are all kClearLimit the probe is
        // measuring nothing and every M1/M2 figure below is noise.
        std::vector<float> rawSolid, rawHaz;
        long long rawSeen = 0;
    };
    Mode per[8];
    long long slopesSkipped = 0;
    long long framesSkipped = 0;      // rotated frames, not measured
    std::vector<int> goalCount;       // M3: goal-reachers per layer that had any
    std::vector<float> goalSpread;    // M3: their solid-clearance spread

    void reset() {
        for (Mode& m : per) m = Mode{};
        slopesSkipped = framesSkipped = 0;
        goalCount.clear();
        goalSpread.clear();
    }
    // (g_goalDiv is reset alongside this -- see resetInvocationState)

    // One layer's live children, already keyed.  Groups by dedupe cell and
    // replays the hi/lo rule to find which two the merge would keep.
    void addLayer(std::vector<ClearSample>& s) {
        if (s.empty()) return;
        std::stable_sort(s.begin(), s.end(),
                         [](const ClearSample& a, const ClearSample& b) {
                             return a.key < b.key;
                         });
        int nGoal = 0;
        float gLo = 1e18f, gHi = -1e18f;
        for (size_t i = 0; i < s.size();) {
            size_t j = i;
            while (j < s.size() && s[j].key == s[i].key) ++j;
            Mode& m = per[s[i].mode & 7];
            ++m.cells;
            m.kids += (long long)(j - i);
            for (size_t k = i; k < j; ++k)
                if ((m.rawSeen++ & 63) == 0) {      // every 64th, to bound memory
                    m.rawSolid.push_back(s[k].solid);
                    m.rawHaz.push_back(s[k].haz);
                }
            for (size_t k = i; k < j; ++k)
                if (s[k].goal) {
                    ++nGoal;
                    gLo = std::min(gLo, s[k].solid);
                    gHi = std::max(gHi, s[k].solid);
                }
            if (j - i >= 2) {
                ++m.cellsMulti;
                // emit()'s rule: the max-vy and min-vy children survive, ties
                // to the first seen.  Everything between them is discarded.
                size_t hi = i, lo = i;
                for (size_t k = i + 1; k < j; ++k) {
                    if (s[k].vy > s[hi].vy) hi = k;
                    if (s[k].vy < s[lo].vy) lo = k;
                }
                float bs = -1e18f, bh = -1e18f, ws = -1e18f, wh = -1e18f;
                for (size_t k = i; k < j; ++k) {
                    bs = std::max(bs, s[k].solid);
                    bh = std::max(bh, s[k].haz);
                }
                ws = std::max(s[hi].solid, s[lo].solid);
                wh = std::max(s[hi].haz, s[lo].haz);
                float ls = 1e18f, lh = 1e18f;
                for (size_t k = i; k < j; ++k) {
                    ls = std::min(ls, s[k].solid);
                    lh = std::min(lh, s[k].haz);
                }
                m.spreadSolid.push_back(bs - ls);
                m.spreadHaz.push_back(bh - lh);
                m.lostSolid.push_back(bs - ws);   // 0 when a survivor was roomiest
                m.lostHaz.push_back(bh - wh);
            }
            i = j;
        }
        if (nGoal > 0) {
            goalCount.push_back(nGoal);
            goalSpread.push_back(gHi - gLo);
        }
    }
};

inline ClearAcc g_clear;

// ---- M3, done properly ----------------------------------------------------
//
// The first cut of M3 scored the goal-reaching states by their OWN clearance
// and found a spread of exactly 0. That is not "they are all equally safe" --
// goalX is L.maxX + 60, i.e. past the last object, so every candidate is
// sitting in open sky and the number says nothing. Scoring the ENDPOINT of a
// route can never rank routes.
//
// The question underneath is whether the candidates are different routes at
// all. They are all descended from one heavily deduped frontier, so if they
// share an ancestor 50 ticks back, picking a different one changes the last 50
// ticks and cannot avoid a death 5,000 ticks earlier -- and no scoring
// function, clearance or otherwise, would give the final pick any power.
//
// So: walk the lineages back in lockstep and find how deep they stay together.
// Read straight off the arena, no re-simulation and no per-node storage.
struct GoalDiversity {
    long long candidates = 0;
    long long sampled = 0;
    long long convergeDepth = -1;   // ticks back before ALL sampled agree
    long long walked = 0;           // how far back the walk went
    // What the pick actually bought. `first` is the candidate
    // the old ordinal rule would have taken, `best` the roomiest available:
    // first - best is the whole value of the change, in tight ticks.
    int tightFirst = -1, tightBest = -1, tightWorst = -1;
};

inline GoalDiversity g_goalDiv;

inline void measureGoalDiversity(const std::vector<State>& nxt,
                                 const std::vector<Node>& arena, double goalX) {
    if (!g_clearProbe || g_goalDiv.candidates) return;
    std::vector<uint32_t> chain;
    for (const State& s : nxt)
        if (s.frame == 0 && (double)s.xAbs >= goalX) {
            if (g_goalDiv.tightFirst < 0) g_goalDiv.tightFirst = (int)s.tight;
            if (g_goalDiv.tightBest < 0 || (int)s.tight < g_goalDiv.tightBest)
                g_goalDiv.tightBest = (int)s.tight;
            if ((int)s.tight > g_goalDiv.tightWorst)
                g_goalDiv.tightWorst = (int)s.tight;
            chain.push_back(s.parent);
        }
    g_goalDiv.candidates = (long long)chain.size();
    if (chain.size() < 2) return;
    // A sample is enough: convergence depth is a property of the set, and 256
    // lineages already pin it (the whole set can only converge SOONER).
    const size_t kMax = 256;
    if (chain.size() > kMax) {
        const size_t step = chain.size() / kMax;
        std::vector<uint32_t> s;
        for (size_t i = 0; i < chain.size() && s.size() < kMax; i += step)
            s.push_back(chain[i]);
        chain.swap(s);
    }
    g_goalDiv.sampled = (long long)chain.size();
    for (long long d = 0; d < 200000; ++d) {
        bool same = true;
        for (size_t i = 1; i < chain.size() && same; ++i)
            same = (chain[i] == chain[0]);
        if (same) { g_goalDiv.convergeDepth = d; g_goalDiv.walked = d; return; }
        bool moved = false;
        for (uint32_t& c : chain)
            if (c) { c = arena[c].parent(); moved = true; }
        g_goalDiv.walked = d + 1;
        if (!moved) return;          // all at the root and still not equal
    }
}

inline void clearReport() {
    if (g_goalDiv.candidates) {
        // The candidates share a long prefix (they only diverge `convergeDepth`
        // ticks back), so the whole-route total is the WRONG denominator: it
        // adds the same constant to everyone and makes any difference look
        // negligible. What is actually on offer is worst - best, the span the
        // pick gets to choose within; report the saving against that.
        const int span = g_goalDiv.tightWorst - g_goalDiv.tightBest;
        const int saved = g_goalDiv.tightFirst - g_goalDiv.tightBest;
        std::printf("clearscore: goal candidates=%lld tightpx=%.2f | tight "
                    "ordinal=%d best=%d worst=%d | saved %d of a %d span "
                    "(%.0f%% of what was on offer)\n",
                    g_goalDiv.candidates, kTightPx, g_goalDiv.tightFirst,
                    g_goalDiv.tightBest, g_goalDiv.tightWorst, saved, span,
                    span > 0 ? 100.0 * saved / span : 0.0);
    }
    if (!g_clearProbe) return;
    auto q = [](std::vector<float>& v, double p) {
        if (v.empty()) return -1.0f;
        std::sort(v.begin(), v.end());
        return v[std::min(v.size() - 1, (size_t)(v.size() * p))];
    };
    auto frac = [](const std::vector<float>& v, float c) {
        if (v.empty()) return -1.0;
        long long n = 0;
        for (float z : v) if (z > c) ++n;
        return 100.0 * (double)n / (double)v.size();
    };
    static const char* kName[8] = {"cube", "ship", "ball", "ufo",
                                   "wave", "robot", "spider", "swing"};
    std::printf("clearprobe: slopes skipped %lld, rotated frames skipped %lld\n",
                g_clear.slopesSkipped, g_clear.framesSkipped);
    // Instrument check first. Read this row before any M1/M2 number below: if
    // the raw clearances do not vary, nothing downstream means anything.
    std::printf("clearprobe: %-6s %10s | raw solid p10/med/p90 | raw haz p10/med/p90\n",
                "mode", "kids");
    for (int i = 0; i < 8; ++i) {
        ClearAcc::Mode& m = g_clear.per[i];
        if (!m.cells) continue;
        std::printf("clearprobe: %-6s %10lld |  %6.2f %6.2f %6.2f |  %6.2f %6.2f %6.2f\n",
                    kName[i], m.kids,
                    q(m.rawSolid, 0.1), q(m.rawSolid, 0.5), q(m.rawSolid, 0.9),
                    q(m.rawHaz, 0.1), q(m.rawHaz, 0.5), q(m.rawHaz, 0.9));
    }
    // M1 asks whether a cell holds anything worth choosing between; M2 asks
    // whether the rule that is there already picks it. Both are reported as the
    // fraction over 1 px, because 1 px is the size of the model's own error at
    // the ticks it gets wrong (fixups_log p50 0.81 / p99 3.7) -- a difference
    // smaller than that cannot decide a life-or-death verdict either way.
    std::printf("clearprobe: %-6s %9s %6s | %-22s | %s\n", "mode",
                "multi", "%multi", "M1 spread p90 >1px% (s|h)",
                "M2 lost p90 >1px% (s|h)");
    for (int i = 0; i < 8; ++i) {
        ClearAcc::Mode& m = g_clear.per[i];
        if (!m.cells) continue;
        std::printf(
            "clearprobe: %-6s %9lld %5.1f%% | %5.2f %5.1f%% %5.2f %5.1f%% | "
            "%5.2f %5.1f%% %5.2f %5.1f%%\n",
            kName[i], m.cellsMulti,
            m.cells ? 100.0 * (double)m.cellsMulti / (double)m.cells : 0.0,
            q(m.spreadSolid, 0.9), frac(m.spreadSolid, 1.0f),
            q(m.spreadHaz, 0.9), frac(m.spreadHaz, 1.0f),
            q(m.lostSolid, 0.9), frac(m.lostSolid, 1.0f),
            q(m.lostHaz, 0.9), frac(m.lostHaz, 1.0f));
    }
    if (g_goalDiv.candidates) {
        std::printf("clearprobe: M3 goal candidates=%lld sampled=%lld "
                    "converge at %lld ticks back (walked %lld)\n",
                    g_goalDiv.candidates, g_goalDiv.sampled,
                    g_goalDiv.convergeDepth, g_goalDiv.walked);
    }
    if (g_clear.goalCount.empty()) {
        std::printf("clearprobe: M3 no layer reached goalX\n");
    } else {
        long long tot = 0, mx = 0;
        for (int c : g_clear.goalCount) { tot += c; mx = std::max(mx, (long long)c); }
        std::printf("clearprobe: M3 goal-reaching layers=%zu, states total=%lld, "
                    "max in one layer=%lld, solid-clearance spread med=%.2f\n",
                    g_clear.goalCount.size(), tot, mx,
                    q(g_clear.goalSpread, 0.5));
    }
}

}  // namespace dp
