#pragma once
// The search: the layer DP's loop (searchLayers), its dedupe tables, its stepped children, and the
// coin rules it enforces. cliMainOnce (cli.hpp) sets the search up and hands it a SearchEnv; the
// model it steps through is model.hpp.
#include "dp/reset.hpp"
#include "dp/clearance.hpp"
#include "dp/refwatch.hpp"
#include "dp/search_census.hpp"
#include "dp/model.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dp {

// --coins: a counter a coin needs (see where coinReq is filled in cliMainOnce).
struct CoinCountReq {
    int item;      // which counter
    int need;      // ...has to reach this
    int coinIdx;   // the coin it decides (skip once that one is collected)
};

// --coins: the touch bits that, once fired, put a coin out of reach (see coinGate in cliMainOnce).
struct CoinOnGate {
    int coinIdx;
    // THE MASK'S OWN TYPE. This was uint32_t while s.trig, which it is
    // tested against, is a TouchMask: at a width of 64 the `1u << b` that
    // filled it is undefined for b >= 32 (and wraps mod 32 on x86), the
    // store truncates, and the test can only ever match the low half. The
    // gate sets dead = true, so a wrong bit KILLS BRANCHES -- it is the
    // expensive direction of that mistake, not the harmless one.
    std::vector<std::pair<TouchMask, double>> ons;  // bit, x past which it is gone
};

// cell -> hi/lo representatives (indices into nxt). Keep BOTH vy extremes
// per cell: first-wins kept the slowest lineage and strangled climbs
// (measured: ship climb rate collapsed to ~0.02 vy/tick); a single
// extreme-|vy| slot instead discarded dive-recovery lineages.
struct Slots { int hi = -1; int lo = -1; };
// Open addressing, linear probing, cleared in O(used) via `touched`.
// std::unordered_map was the whole serial phase once stepping went parallel
// (a node allocation and a pointer chase per child, ~80k children a layer).
// Semantics are identical -- key -> Slots, lookup and insert only, never
// iterated -- so `nxt` still fills in the same order and the plan is
// bit-identical. Key 0 doubles as "empty": keyOf is a scrambled XOR, so a
// real key of exactly 0 is a 2^-64 event, and if it ever happened it would
// merge two cells, which is what a hash collision already does.
struct CellMap {
    std::vector<uint64_t> keys;
    std::vector<Slots> vals;
    std::vector<uint32_t> touched;
    size_t mask = 0;
    void ensure(size_t want) {
        size_t n = 1024;
        while (n < want * 4) n <<= 1;   // keep the load factor under 1/4
        if (n <= keys.size()) return;
        keys.assign(n, 0);
        vals.assign(n, Slots{});
        mask = n - 1;
    }
    void clear() {
        for (uint32_t i : touched) keys[i] = 0;
        touched.clear();
    }
    // same names the memory report used when this was an unordered_map
    size_t size() const { return touched.size(); }
    size_t bucket_count() const { return keys.size(); }
    Slots& at(uint64_t k) {
        size_t i = (size_t)(k * 0x9E3779B97F4A7C15ull) & mask;
        for (;;) {
            if (keys[i] == k) return vals[i];
            if (keys[i] == 0) {
                keys[i] = k;
                vals[i] = Slots{};
                touched.push_back((uint32_t)i);
                return vals[i];
            }
            i = (i + 1) & mask;
        }
    }
};

// ---- parallel dedupe (phase 2p) -------------------------------------
// The serial dedupe was the measured ceiling of the whole DP (1 thread
// 22.5 s -> 8 threads 8.0 s -> 16 threads 8.9 s on lv16's cap-40,000
// layers: the stepping scales, the dedupe does not). It parallelises by
// KEY: children of one key all land in one shard (keyed by a hash of the
// dedupe key), each shard replays emit()'s exact hi/lo hysteresis over
// its keys in ordinal order, and a serial merge then materialises `nxt`
// in the SAME slot order the serial loop would have produced -- the
// emitted plan stays bit-identical (checked against --old-slope-era runs
// and asserted by the regression). Only the arena's node COUNT differs:
// the serial loop pushed a node per accepted improvement, this pushes one
// per final representative, so the arena is strictly smaller and parent
// CHAINS (the only thing read back) are unchanged.
struct KeyRec {
    uint64_t key;
    uint32_t aOrd;          // ordinal that allocated the key's first slot
    uint32_t bOrd;          // ordinal of the first hi/lo split (kNone: none)
    uint32_t hiIdx, loIdx;  // child index of the current hi / lo rep
    float hiVy, loVy;
    uint8_t bWasHi;
};
struct ShardMap {           // CellMap's open addressing, key -> rec index
    std::vector<uint64_t> keys;
    std::vector<uint32_t> vals;
    std::vector<uint32_t> touched;
    std::vector<KeyRec> recs;
    uint32_t goalOrd = kNone;
    uint16_t goalTight = 0xffff;   // route score; see the goal pick below
    size_t mask = 0;
    void ensure(size_t want) {
        size_t n = 1024;
        while (n < want * 4) n <<= 1;
        if (n <= keys.size()) return;
        keys.assign(n, 0);
        vals.assign(n, kNone);
        mask = n - 1;
    }
    void clear() {
        for (uint32_t i : touched) keys[i] = 0;
        touched.clear();
        recs.clear();
        goalOrd = kNone;
        goalTight = 0xffff;
    }
    // ensure() sizes for the EXPECTED keys per shard; a lopsided hash
    // could still overfill one shard's table, and a full open-addressing
    // table is an infinite probe loop, not a slowdown. Grow keeps the
    // guarantee unconditional.
    void grow() {
        const size_t n = keys.size() ? keys.size() * 2 : 1024;
        std::vector<uint64_t> ok;
        ok.swap(keys);
        std::vector<uint32_t> ov;
        ov.swap(vals);
        keys.assign(n, 0);
        vals.assign(n, (uint32_t)kNone);
        mask = n - 1;
        touched.clear();
        for (size_t i = 0; i < ok.size(); ++i) {
            if (!ok[i]) continue;
            size_t j = (size_t)(ok[i] * 0x9E3779B97F4A7C15ull) & mask;
            while (keys[j]) j = (j + 1) & mask;
            keys[j] = ok[i];
            vals[j] = ov[i];
            touched.push_back((uint32_t)j);
        }
    }
    uint32_t& at(uint64_t k) {
        if ((touched.size() + 1) * 4 > keys.size()) grow();
        size_t i = (size_t)(k * 0x9E3779B97F4A7C15ull) & mask;
        for (;;) {
            if (keys[i] == k) return vals[i];
            if (keys[i] == 0) {
                keys[i] = k;
                vals[i] = kNone;
                touched.push_back((uint32_t)i);
                return vals[i];
            }
            i = (i + 1) & mask;
        }
    }
};

// One stepped child. Slot 2i is state i's no-input child, 2i+1 its pressed
// one (empty when that state does not expand both -- see stepKid).
struct Child {
    State s{};
    uint64_t key = 0;         // modelKeyOf(s) ^ rules.keyBits(s), computed on the worker thread
    uint8_t dead = 0;
    uint8_t valid = 0;
    const char* why = "";     // --dbg only
    const Obj* obj = nullptr; // --dbg only
    int rotUid = -1;          // --rotwatch only: the 2900 stepKid applied
    int8_t rotF0 = -1, rotNf = -1;
};


// ---- per-tick input levels -> the plan's `input=press,level` edges --------------------------
//
// ONE copy of this conversion, because two callers now depend on it agreeing with itself: the
// plan writer at the end of cliMain, and the common-prefix publisher inside the layer loop
// (dp/progress.hpp). The prefix is only usable if the inputs the caller flies are the ones the
// emitted plan WOULD have carried for those ticks -- an edge placed one tick out is 2 ticks of
// dy at the seam (the measured lv20 case in the writer's own note below), and the caller would
// be testing a plan the search never proposed.
//
// It is a left-to-right scan carrying `prev`, so running it over a PREFIX of `lvl` gives
// exactly the leading edges it gives over the whole of `lvl`: every index it reads
// (`i - 1`, `i - lat0`) is smaller than `i`, so nothing behind the cut is consulted.
struct PlanEdge {
    long long press;   // the tick the button changes state on
    int level;         // ...and to what
};

// --start-state (statefile.hpp): the button state the plan starts from, by PRESS tick, as --start's
// hold field names it (the caller's heldBefore over the prefix it splices onto). The state's own
// `held` is the physics' (a cube's is never written), so the edges are counted from this instead,
// which is what a call seeded from the game's anchor counts them from. -1 = not a --start-state call.
inline int g_planPrevHeld = -1;

// --needsize <tick>:<0|1>: player 1 must be normal (0) or mini (1) on that tick; the search drops
// every state that is not (a rule, not the game's physics). The loop asks it of a search that begins
// before an optional size change when the game has shown that only the other size gets over the
// wall ahead (src/mod/repair.hpp, cfg dpsecstate): "be that size at the wall" lets the route take an
// unavoidable portal and the one that undoes it, which "never change the size" cannot. -1 = off.
inline long long g_needSizeT = -1;
inline int g_needSizeMini = 0;

// --needspeed <x>:<multiplier> / <x>:!<multiplier>: the state that crosses x must be at that speed (0.7 / 0.9 / 1.1 /
// 1.3 / 1.6) as it does, or must not be (the "!" form); the search drops every one that breaks it (a rule, not the
// game's physics). The same question as --needsize, for the speed: a route that took an optional speed portal can reach a
// wall only another speed gets over. It is asked by x and not by tick because the speeds do not share a clock: at
// the same tick a faster state is already further on, and "be at another speed when you reach the wall" is
// the same for every one of them. A state that starts at or past x is not asked. Negative x = off.
inline double g_needSpeedX = -1.0;
inline int g_needSpeedCode = 0;
inline bool g_needSpeedNot = false;
inline int planPrevHeld(const State& init) {
    return g_planPrevHeld >= 0 ? g_planPrevHeld : (int)init.held;
}

// `modeAt[i]` is the mode at the END of tick t0+i+1, `prevHeld` the button state the plan
// starts from and `initMode` the anchor's mode (used when the lookback reaches before tick 0).
inline std::vector<PlanEdge> planEdges(const std::vector<uint8_t>& lvl,
                                       const std::vector<uint8_t>& modeAt, long long t0,
                                       int prevHeld, uint8_t initMode) {
    std::vector<PlanEdge> out;
    int prev = prevHeld;
    for (size_t i = 0; i < lvl.size(); ++i) {
        if (lvl[i] == prev) continue;
        const long long effect = t0 + (long long)i + 1;
        auto latOf = [](uint8_t m) { return (m == 1 || m == 3) ? 2 : 1; };
        auto modeAtTick = [&](long long j) -> uint8_t {
            if (j < 0) return initMode;
            if ((size_t)j >= modeAt.size()) return modeAt.empty() ? initMode : modeAt.back();
            return modeAt[(size_t)j];
        };
        // The latency is read at the tick the button last changed under: lat0 from the tick before this one,
        // then the mode at (i - lat0). (A mode-at-i form, --old-latency, existed only to bisect lv18.)
        const int lat0 = latOf(modeAtTick((long long)i - 1));
        const int lat = latOf(modeAtTick((long long)i - lat0));
        out.push_back(PlanEdge{effect - lat, (int)lvl[i]});
        prev = lvl[i];
    }
    return out;
}

// The gamble (PlainElsewhere::ready): the deepest tick an attempt of the ladder in progress has
// died on, for the plain search beside it to compare itself with, and the flag that stops the
// attempt in progress once that search has been taken. Both belong to one ladder: cliMain sets
// them at its start and clears them on the way out. The CLI never sets the flag.
inline std::atomic<long long> g_ladderBestT{-1};
inline std::atomic<bool> g_ladderStop{false};

// ---- where the search is going ----------------------------------------------------------------
// The goal and the route the layer loop is asked to reach, kept apart from how it searches
// (search-plugin cut 5). searchLayers takes any type with these members; CurrentRules is today's:
// the level's end in frame 0, with every coin when --coins is on. `C` is cliMainOnce's CoinTables:
// references to the coin tables built at load and to the two lambdas the coin rules call.
template <class C>
struct CurrentRules {
    double goalX;      // the level's end (cliMainOnce: L.maxX + 60)
    bool coinOn;       // --coins, on a level whose coins fit the mask
    uint32_t coinAll;  // ...and the mask of all of them
    const C& c;

    // Has `s` reached the goal? Only in frame 0: goalX is a world x, and in a turned frame xAbs is a
    // different axis. Under --coins, only with the full set. Both dedupes ask this.
    bool reached(const State& s) const {
        return s.frame == 0 && (double)s.xAbs >= goalX
               && (!coinOn || s.coins == (uint8_t)coinAll);
    }
    // Of two states that reached it, is the one with route score `cand` to replace `held`?
    bool prefer(uint16_t cand, uint16_t held) const { return roomierRoute(cand, held); }
    // How good `s` is, for a search that ranks states (the beam): more coins first under --coins,
    // then how far it has got.
    double score(const State& s) const {
        return (coinOn ? 1e7 * popCount32((uint32_t)s.coins) : 0.0) + progressOf(s).x;
    }
    // --coinpick: the state a cut or a PARTIAL emits -- the first one holding the most coins.
    // Without --coins it is front().
    const State& pick(const std::vector<State>& v) const {
        size_t best = 0;
        if (coinOn) {
            int most = -1;
            for (size_t i = 0; i < v.size(); ++i) {
                const int c = popCount32((uint32_t)v[i].coins);
                if (c > most) { most = c; best = i; }
            }
        }
        return v[best];
    }

    // --coins only. Measured 2026-08-31: with the band class on for
    // every run, 20 of the 21 levels the cold suite reached kept their
    // baseline iteration count exactly -- and lv20 went 37 -> 50. That
    // is the level where the band really does branch, and +13 cold
    // iterations is not a price the PLAIN solver has any reason to pay:
    // it needs the deepest route, which is the majority the water-fill
    // already keeps. Preserving a minority branch is a coin-routing
    // problem, so the class is a coin-routing class.
    bool bandClass() const { return coinOn; }
    // --coinwin (thread_pool.hpp): the (y, vy) cell of a state inside the approach window
    // of a coin it still lacks, and 0 everywhere else -- so with the flag off, or outside
    // every window, the cap's partition is the one it was.
    uint64_t winCell(const State& s) const {
        auto& L = c.L;
        auto& coinPruneOk = c.coinPruneOk;
        if (!coinOn || g_coinWinY <= 0.0 || s.frame != 0) return 0;
        for (size_t ci = 0; ci < L.coins.size() && ci < 8; ++ci) {
            if (!coinPruneOk[ci] || ((s.coins >> ci) & 1u)) continue;
            const double cx = L.coins[ci].cx;
            if ((double)s.xAbs < cx - g_coinWinLen || (double)s.xAbs > cx + L.coins[ci].hw)
                continue;
            const int64_t yb = (int64_t)std::floor((double)s.y / g_coinWinY);
            const int64_t vb = (int64_t)std::floor((double)s.vy / g_coinWinVy);
            return (((uint64_t)(ci + 1) << 40) ^ ((uint64_t)(yb & 0xfffff) << 16)
                    ^ (uint64_t)(vb & 0xffff)) * 0xBF58476D1CE4E5B9ull;
        }
        return 0;
    }
    // May the search join an old plan at `rjS` (null: no join found)? With --coins, not from
    // INSIDE an open counting-tap window whose gate has not fired. The old plan's tail is flown as
    // it is, and a join is a SOLVED: joined there, the presses still owed are never planned.
    // Anywhere else a join stays what it was -- a tail that then passes the window short is ended
    // by the mod at the shut point (Outcome::coinGates), and the loop plans the window again from
    // an anchor inside it. (Refusing every join short of all coins was tried first: 0 joins
    // against 34, and the run stalled at x=3.6-4.9k, 1,000 px before any coin.)
    // ...nor after the gate has fired while a coin it opens is still to be taken: the old tail was
    // flown with the coin off, and on lv22 it glides under it.
    bool rejoinOk(const State* rjS) const {
        auto& tapGive = c.tapGive;
        auto& tapGateBits = c.tapGateBits;
        auto& gatedMask = c.gatedMask;
        const bool rjWindow = rjS && (rjS->taps & 0x80) && !(rjS->taps & 0x40)
                              && !(rjS->trig & tapGateBits);
        const bool rjOwed = rjS && gatedMask && (rjS->trig & tapGateBits)
                            && (rjS->coins & gatedMask) != gatedMask;
        return !coinOn || !tapGive.bit || !(rjWindow || rjOwed);
    }

    // The route's own part of a state's dedupe key: the coins collected. A search keys a state by
    // the model's key (search_key.hpp modelKeyOf) xor this. 0 with no coin taken, so a run without
    // --coins keys by the model alone.
    uint64_t keyBits(const State& s) const { return coinKeyBits(s); }
    // ...and of its class under the cap's water-fill (searchLayers classOf): the collected set is a
    // class of its own, so the fill -- which serves the SMALLEST class first -- protects the few
    // lineages that took a coin instead of thinning them at the stride of the many that are merely
    // inside its window. 0 without coins, so every partition without --coins is unchanged.
    uint64_t classBits(const State& s) const { return (uint64_t)s.coins << 40; }
    // The route's part of the root state. --coinmask: the coins GD had already credited at the
    // anchor (the repair loop's re-anchor, repair.hpp). An anchored tail otherwise starts with an
    // empty set, so every coin behind x0 reads as missed and the miss prune kills the seed on its
    // first step. Bits for coins the level does not have drop. --coinskip: every coin test skips a
    // collected coin, so seeding the bit takes that coin out of the goal and out of both miss prunes.
    void init(State& s) const {
        if (!coinOn) return;
        s.coins = (uint8_t)((uint32_t)g_coinMaskSeed & coinAll);
        if (s.coins)
            std::printf("coins: starting with 0x%x already collected\n", (unsigned)s.coins);
        const uint8_t skip = (uint8_t)((uint32_t)g_coinSkip & coinAll);
        if (skip) {
            s.coins |= skip;
            std::printf("coins: 0x%x not a goal this run (--coinskip)\n", (unsigned)skip);
        }
    }
    // Does the route need afterStep on every step of every state? A search that steps without it
    // (montecarlo's rollouts) cannot follow such a route, and declines the call.
    bool needsAfterStep() const { return coinOn; }

    // ---- --coins: collect, then kill whoever left one behind --------
    // A child `kid` of `s` (pressed or not: `input`) has just stepped through the group's tick (`K`,
    // frame `gframe`; the coins where they are now are K.coins, the group's list when it was asked
    // for -- GroupOpts::coins). Credit what it touched, and set `dead` if it can no longer collect
    // what --coins asks for. Runs on the worker threads.
    void afterStep(const State& s, Child& kid, int input, const StepCtx& K, int gframe, long long t,
                   bool& dead) const {
        static const std::vector<CoinLive> kNoCoins;
        const std::vector<CoinLive>& coinLive = K.coins ? *K.coins : kNoCoins;
        auto& L = c.L;
        auto& passedOn = c.passedOn;
        auto& tapGive = c.tapGive;
        auto& tapGateBits = c.tapGateBits;
        auto& gatedMask = c.gatedMask;
        auto& tapMissX = c.tapMissX;
        auto& tapMissY = c.tapMissY;
        auto& coinGate = c.coinGate;
        auto& tapSole = c.tapSole;
        auto& coinEnable = c.coinEnable;
        auto& coinReq = c.coinReq;
        auto& coinPruneOk = c.coinPruneOk;
        // After the step and after any rotation, so the position tested is
        // the one the tick ended at, in the frame the coin's world
        // coordinates mean something. In the frame coinLive was built in
        // (the group's): frame 0 for every coin, a turned frame only for
        // the coins that never move (coinAnyFrame), whose rows are turned
        // into it. A tick that turns the player skips the test, as before.
        if (coinOn && !dead && kid.s.frame == gframe) {
            const double ph =
                hazardHalfFor(kid.s.mode, kid.s.mini != 0);
            const double px = (double)kid.s.xAbs, py = (double)kid.s.y;
            // COLLECTING reads the live rows (coinLive): a coin its group
            // has moved is tested where it is now, and one the group has
            // switched off is not in the list at all -- GD credits neither
            // a moved coin at its old place nor a disabled one.
            for (const CoinLive& lc : coinLive) {
                const uint8_t bit = (uint8_t)(1u << lc.bit);
                if (kid.s.coins & bit) continue;
                // --coindbg: how close anything got. Recorded BEFORE the
                // collect test and only inside the x bound, so a coin that
                // is never collected still says which of the three reasons
                // it was (see g_coinNearDy).
                // (frame 0 only: the probe's x is a world x)
                if (g_coinDbgT >= 0 && gframe == 0 && px > g_coinProbeMaxX)
                    g_coinProbeMaxX = px;
                if (g_coinDbgT >= 0 && gframe == 0
                    && std::fabs(px - lc.cx) <= lc.hw + ph) {
                    ++g_coinSeen[lc.bit];
                    const double dy = std::fabs(py - lc.cy);
                    double& best = g_coinNearDy[lc.bit];
                    if (best < 0.0 || dy < best) {
                        best = dy;
                        g_coinNearX[lc.bit] = px;
                        g_coinNearY[lc.bit] = py;
                        g_coinNearT[lc.bit] = (long long)t;
                    }
                }
                // Touching counts (measured: exactly on the boundary is
                // credited), hence <= and not <.
                // The margin is an allowance for THIS model's own error, so
                // it must never eat the window it is protecting: a wave's
                // coin bound is 25 px against a cube's 35, and the learnt 8
                // takes a third of it -- measured on lv21, where the search
                // then had no route left it could call a collection. A
                // quarter of the bound is the cap.
                const double mx = std::min(kCoinMargin, (lc.hw + ph) * 0.25);
                const double my = std::min(kCoinMargin, (lc.hh + ph) * 0.25);
                if (std::fabs(px - lc.cx) <= lc.hw + ph - mx
                    && std::fabs(py - lc.cy) <= lc.hh + ph - my) {
                    // --coindbg: WHERE the model decided it had one, once
                    // per coin. "The game refused a coin the plan claimed"
                    // names the coin and nothing else, so without this the
                    // claim's own position is only inferable -- and the two
                    // sides were compared at different ticks twice before.
                    if (g_coinDbgT >= 0 && !g_coinSaid[lc.bit]) {
                        g_coinSaid[lc.bit] = 1;
                        // ...with vy and the mode, because the next question
                        // this line gets asked is always "does the game
                        // survive from there", and that is an injection
                        // which needs the whole state. Without vy the
                        // probe has to guess it, and a cube dropped at
                        // this coin with vy=0 is inside the spikes three
                        // ticks later -- which says nothing about the plan.
                        std::printf("coincollect: t=%lld coin=%d player=(%.3f,%.3f)"
                                    " vy=%.4f mode=%d mini=%d frame=%d"
                                    " live=(%.3f,%.3f) |dx|=%.3f |dy|=%.3f"
                                    " bound=(%.3f,%.3f)\n",
                                    (long long)t, lc.bit, px, py,
                                    (double)kid.s.vy, (int)kid.s.mode,
                                    (int)kid.s.mini, gframe, lc.cx, lc.cy,
                                    std::fabs(px - lc.cx), std::fabs(py - lc.cy),
                                    lc.hw + ph - mx, lc.hh + ph - my);
                    }
                    kid.s.coins |= bit;
                }
            }
            // MISSING is judged on the load-time x instead, for the two
            // reasons the live row cannot serve: a switched-off coin has no
            // live row at all (and must still be counted as passed), and
            // the bound has to be the same one the game side uses to end an
            // attempt (hooks_gamelayer.cpp). Every coin this corpus moves is
            // moved in y only, so the two agree; a coin carried in x would
            // need its own bound and does not exist here.
            //
            // ...AND ONLY WHERE THE PLAYER CANNOT COME BACK. The whole
            // verdict rests on x growing for ever, which a level with
            // rotated gameplay breaks outright: lv22 travels -x through its
            // maze and its third coin sits at x=16,097, inside it. There the
            // prune would kill a lineage that has merely not turned round
            // yet. Without it the search only loses the early exit -- the
            // goal still is not reached without every coin.
            // `dead` and no early return: the bookkeeping below (flags,
            // key, the dbg reason) is the same for this death as for a
            // physical one, and duplicating it is how the two drift.
            // ---- the PICKUP ITEMS, and the Count triggers they fire -----
            // Same test as a coin's (the player's own box against the
            // object), and state for the same reason: lv21's third coin
            // only comes into reach once ten of item 1 have been taken.
            // The trigger they fire takes a bit in the SAME mask a touch
            // box does, so the chain, the fire tick and the dedupe key are
            // machinery that already exists.
            //
            // AND THE COUNT LOOP BELOW LIVES INSIDE THIS BLOCK, which was
            // opened only when there are pickups. It was written for lv21,
            // whose gate is fed by collectibles; on a level whose counter is
            // fed by CHAINS instead there are no pickups at all, the block
            // is skipped, and no Count trigger is ever asked anything. That
            // is lv22's first coin exactly: its three item-2 rows all have
            // pickup=0, g_collect is empty, and `item 2 == 5` never fired
            // even with every feeder forced -- a --coindbg print inside the
            // loop produced zero lines for any Count on that level. Opened
            // for either source now; the pickup loop just below is a no-op
            // when g_collect is empty, so a level with neither is unchanged
            // and a level with pickups takes the same path as before.
            // Frame 0 only, as everything in this block was before the coin
            // collect above learnt the turned frames: the pickups are world
            // rows, and the counters are asked where they always were.
            // The exception is an Item Compare gate (cmode 3): it reads a
            // count and nothing else, and lv22's third coin is collected in
            // frame 3, where an anchored call has to open it from GD's own
            // counter (--itembase) or never see the coin at all.
            // (The gates themselves are the model's now -- Model::fireGates, which the search
            // calls on the child before this; neither the coin test above nor the prunes below
            // write what the other reads.)
            // A gated coin passed for good (gatedMask, tapMissX/Y):
            // once its window has resolved, past the coin's far edge on the
            // shut channel without it. Same reading of "passed" as the
            // window's own ends.
            if (!dead && gatedMask
                && ((kid.s.trig & tapGateBits) || (kid.s.taps & 0x40))) {
                double wx = 0.0, wy = 0.0;
                fromFrame(gframe, (double)kid.s.xAbs, (double)kid.s.y, wx, wy);
                for (size_t ci = 0; ci < L.coins.size(); ++ci) {
                    if (!((gatedMask >> ci) & 1) || (kid.s.coins & (uint8_t)(1u << ci)))
                        continue;
                    if (passedOn(kid.s, gframe, tapGive.closeChan, wx, wy,
                                 tapMissX[ci], tapMissY[ci])) {
                        dead = true;
                        break;
                    }
                }
            }
            // ...a coin whose group is OFF and whose every switch-on box is
            // behind the player (coinGate, built at load). "Off right now"
            // is read from the live rows: a switched-off coin is not in
            // them. Nothing to learn from flying the rest of the level.
            // Frame 0 only, and for the same reason as the miss test: `px`
            // is a world Y in a turned frame, and coinLive is empty there,
            // so every coin would read as "off, and the switch-ons are all
            // behind" and be killed on a comparison between two axes.
            for (size_t gi = 0; gframe == 0 && gi < coinGate.size(); ++gi) {
                const CoinOnGate& G = coinGate[gi];
                if (kid.s.coins & (uint8_t)(1u << G.coinIdx)) continue;
                bool liveNow = false;
                for (const CoinLive& lc : coinLive)
                    if (lc.bit == G.coinIdx) { liveNow = true; break; }
                if (liveNow) continue;
                bool canStill = false;
                for (const auto& on : G.ons)
                    if ((kid.s.trig & on.first) || px <= on.second) {
                        canStill = true;
                        break;
                    }
                if (!canStill) { dead = true; break; }
            }
            // ...and a coin that only an Item Compare on the counting tap's
            // item switches on, once that tap's window has shut with the
            // gate unfired: the Stop at its end halts the tap itself (lv22's
            // uid17958 sits in the Stop's group 535), nothing else feeds the
            // item (tapSole), and the gate reads only the count -- so the
            // coin stays off for good. Without this the branches that
            // pressed too few times fly on to the coin and only the goal
            // test tells them apart, a whole frontier later.
            if (!dead && tapSole && (kid.s.taps & 0x40)
                && !(kid.s.trig & tapGateBits))
                for (size_t ci = 0; ci < L.coins.size(); ++ci) {
                    if (!coinEnable[ci] || (coinEnable[ci] & ~tapGateBits)) continue;
                    if (kid.s.coins & (uint8_t)(1u << ci)) continue;
                    dead = true;
                    break;
                }
            // ...and the COUNTER a coin needs (coinReq, built at load).
            // A counter never decreases, so once `have + ahead < need` the
            // coin can never come into reach again and the branch is done.
            // This is the prune the miss test could not be: it fires at the
            // last feeder the branch could still have taken, not at the
            // coin, which on lv21 is 2,759 px and a whole cap's worth of
            // frontier earlier.
            for (size_t ri = 0; gframe == 0 && ri < coinReq.size(); ++ri) {
                const CoinCountReq& R = coinReq[ri];
                if (kid.s.coins & (uint8_t)(1u << R.coinIdx)) continue;
                int have = 0, ahead = 0;
                for (const auto& ib : g_itemBase)
                    if (ib.first == R.item) have += ib.second;
                for (size_t ii = 0; ii < g_collect.size() && ii < 16; ++ii) {
                    if (g_collect[ii].item != R.item) continue;
                    if (kid.s.items & (uint16_t)(1u << ii)) ++have;
                    else if (g_collect[ii].cx > px) ++ahead;
                }
                if (have + ahead < R.need) { dead = true; break; }
            }
            // ONLY IN FRAME 0, and that is not a detail: px is the frame's
            // travel coordinate, so in a turned frame it is a world Y and
            // comparing it with the coin's world x kills branches for a
            // reason that does not exist. The old form could skip the guard
            // because it only ran on levels that never turn.
            for (size_t ci = 0; !dead && gframe == 0 && ci < L.coins.size(); ++ci) {
                if (!coinPruneOk[ci]) continue;
                if (kid.s.coins & (uint8_t)(1u << ci)) continue;
                const Obj& oc = L.coins[ci];
                // x only ever grows in frame 0, and coinPruneOk says every
                // later return to frame 0 is past this coin, so it is final.
                if (px > oc.cx + oc.hw + ph) { dead = true; break; }
            }
        }
    }
};

// ---- what a search answers ------------------------------------------------------------------------
// One shape for every search. Solved: a plan to the goal (or to where the caller asked it to stop --
// a horizon, the memory limit, a join onto an old plan), rebuilt from `goal`. Partial: no goal, but
// the caller asked for the deepest branch (a horizon search), rebuilt from `goal`. NoPlan: nothing
// to emit. `t` / `x` are how far the frontier got -- for NoPlan, where it got stuck, which is what
// the caller's fallback (the section search, repair.hpp) reads.
// Declined: this search does not take this call (a level it cannot walk) and has touched nothing;
// the caller's default search answers instead. `deterministic` is false when the search stopped on
// something other than its own budget (a wall-clock safety valve), so a comparison can leave it out.
struct SearchResult {
    enum Kind : uint8_t { Solved, Partial, NoPlan, Declined };
    Kind kind = NoPlan;
    long long t = -1;
    double x = -1.0;
    State goal{};
    bool deterministic = true;
    // The plan, Solved or Partial: the input level in effect at each tick from t0 + 1 up to the
    // goal (inputs[i] is tick t0 + i + 1). cliMainOnce reads the plan from here and nothing else of
    // the method's, so how a method keeps its states (the arena, planFromArena below) is its own.
    std::vector<uint8_t> inputs;
    // NoPlan: the first states (up to 12) of the last layer the frontier had, for the FAILED lines.
    std::vector<State> lastLayer;
};

// The layer searches' own frontier and dedupe scratch (searchLayers / searchBeam make one each).
// cliMainOnce used to declare these and lend them to every method; a method that is not a layer
// search never touched them.
struct LayerWork {
    std::vector<State> cur, nxt;
    CellMap seen;
    std::vector<ShardMap> shards;   // parallel dedupe (KeyRec, ShardMap above)
    std::vector<uint64_t> kidKeys;  // compact copy of kids[i].key (8 B strides
                                    // instead of pulling the whole Child in)
    std::vector<uint8_t> kidFlag;   // 0 = not expanded, 1 = dead, 2 = alive
    // per-ordinal slot events, written race-free (one shard owns each key):
    // evKind[i] = 0 none / 1+shard; evRec[i] = record index within that shard.
    // An ordinal is an 'a' event iff recs[evRec[i]].aOrd == i.
    // Cleared by the merge scan itself (every set entry is visited).
    std::vector<uint8_t> evKind;
    std::vector<uint32_t> evRec;
    std::vector<Child> kids;      // reused every layer
    std::vector<ClearSample> clearKids;   // --clearprobe only, reused too
    std::vector<size_t> gidx;     // this speed group's indices into `cur`
    std::vector<uint8_t> kidMoot;   // --twinaudit: per parent of the group, predicted twin
    // next arena size that triggers a GC (doubles after each compaction)
    size_t gcNext = g_gcNodes > 0 ? g_gcNodes : (size_t)-1;
    long long nBornPrev = 0, nDiedPrev = 0;   // last layer's expansion counters
    size_t prevAlive = 0;
    // The frontier starts at the root state -- the call's init, after every amendment.
    explicit LayerWork(const State& root) { cur.push_back(root); }
};

// Where a search method keeps its states (searchLayers / searchBeam / searchMontecarlo make one
// each): every node records its parent and the input that made it, and the plan is read back from
// the goal's parent (planFromArena above). Node 0 is the root.
// arenaMode is the mode of the state each node was created for, parallel to `arena` and ONLY filled
// when the checkpoint channel has a subscriber (dp/progress.hpp). A checkpoint's `input=` edges
// have to carry the plan writer's per-mode latency, and the writer reads that off `modeAt`, which
// does not exist until the witness resim runs at the end of the call. The lineage's own modes are
// the same sequence and they are available now. Costs one byte per node while a subscriber is
// listening and nothing at all otherwise -- which is why it is a side vector and not three spare
// bits of Node: Node's parent field is 31 bits wide and the arena has been measured at 147.9M
// nodes. No arena index leaves the method: a checkpoint carries its lineage as edges.
struct SearchArena {
    std::vector<Node> arena;
    std::vector<uint8_t> arenaMode;
    explicit SearchArena(bool checkWanted) {
        arena.push_back(Node{0});
        if (checkWanted) arenaMode.push_back(0);   // the root, which no state owns
    }
};

// A method that keeps its states in the arena (searchLayers / searchBeam / searchMontecarlo) reads
// its plan off it: from the goal's parent back to the root, then reversed.
inline void planFromArena(const std::vector<Node>& arena, const State& goal,
                          std::vector<uint8_t>& inputs) {
    inputs.clear();
    for (uint32_t i = goal.parent; i != 0; i = arena[i].parent()) inputs.push_back(arena[i].action());
    std::reverse(inputs.begin(), inputs.end());
}

// --ladderprefix: a cap ladder's attempt k+1 starts from attempt k's layer loop where the two
// attempts can no longer differ. Inside a ladder the attempts' arguments differ only in --capmap,
// and a layer reads the map in exactly two places, capAtX(bestX) and gridAtX(bestX), both with
// the leader x the layer starts with. So attempt k logs that x per layer (layerX); once attempt
// k+1's map is known, the first layer whose cap under it differs is t* (cliMain), every layer
// before t* is the same in both attempts, and attempt k+1 starts from the last state saved at a
// boundary T < t*. A save holds everything the layer loop carries from one layer to the next:
// the frontier, the arena's size (the arena itself is kept, append-only; a GC after a save
// renumbers it and drops the saves), the loop's counters and the outcome it accumulates, the
// globals it writes (autonomous and rotation triggers, statistics, the rejoin node), and the
// moving geometry's carried placement per bound frame. --ladderprefixcheck (cliMain) runs such an
// attempt both ways and compares plan, trace and outcome.
// What a search writes into an AutoTrig: model.hpp fireAutoTriggers, the lock window in dynamics.hpp, the
// witness and rollout walks. The rest of it (the controls, toggles and stops: ~1 GB on a level with a
// hundred thousand objects) is built once per call by loadAutoTriggers and never changes, so a save keeps
// these four and the resumed attempt, which loaded the same triggers, gets them back.
struct AutoFireState {
    int fireT = -1;
    double fireX = 0.0;
    int fireSpreadT = 0;
    double lockLastX = 0.0;
};
struct LadderPrefixSnap {
    long long T = -1;
    std::vector<State> cur;
    std::vector<uint64_t> objRefs;   // the frontier's Obj pointers, 12 per state (Model::encodeObj)
    size_t arenaSize = 0, arenaModeSize = 0;
    size_t gcNext = 0;
    long long nBornPrev = 0, nDiedPrev = 0;
    size_t prevAlive = 0;
    long long bestT = 0;
    double bestX = 0;
    State goalState{};
    bool solved = false;
    size_t maxAlive = 0;
    long long capDropped = 0, capHits = 0, cellCuts = 0, cellSpared = 0, cellTwinsKept = 0;
    SearchCensus census;
    TwinStats twin;
    long long workStates = 0;
    std::vector<AutoFireState> autoFire;   // per g_autoTrig entry
    std::vector<RotTrig> rotTrig;
    std::vector<std::vector<double>> carry;   // per frame 0..3, empty when not bound
    std::vector<uint8_t> carryOn;
    long long gfireGroups = 0, gfireSpread = 0, gfireSum = 0;
    int gfireMax = 0;
    int groupXSplitMax = 0;
    long long rjJoinT = -1;
    uint32_t rjOldNode = 0xffffffffu;
    size_t layerXSize = 0;
    // What this save holds allocated, for the cap on all of them (g_ladderPrefixMaxMiB).
    size_t bytes() const {
        size_t n = sizeof(*this) + cur.capacity() * sizeof(State) + objRefs.capacity() * sizeof(uint64_t)
                   + autoFire.capacity() * sizeof(AutoFireState) + rotTrig.capacity() * sizeof(RotTrig)
                   + carry.capacity() * sizeof(std::vector<double>) + carryOn.capacity();
        for (const std::vector<double>& c : carry) n += c.capacity() * sizeof(double);
        return n;
    }
};
struct LadderPrefixCache {
    bool active = false;        // this call is a ladder attempt under --ladderprefix (cliMain)
    long long t0 = -1;          // the anchor the saves were made from
    std::vector<double> layerX; // the leader x each layer t0+1.. started with (the map's argument)
    std::vector<LadderPrefixSnap> snaps;
    std::vector<Node> arena;    // the last attempt's arena (append-only between its saves)
    std::vector<uint8_t> arenaMode;
    bool usable = false;        // the last attempt ended normally, so its saves can be resumed
    int resume = -1;            // index into snaps to start this attempt from (cliMain), -1 = t0
    long long resumedFrom = -1; // what this call started from, for the account
    // ...and at most this much in all (--ladderprefixmaxmib, read by cliMain), oldest dropped first; the newest
    // save is always kept. A save is one frontier plus the loop's counters, a few MiB at a frontier of
    // thousands of states, so 64 of them sit well under the default; the cap is there so that whatever a
    // save grows to hold can never again run the process out of memory unseen (the search's own budget,
    // g_memLimitMiB, does not count them).
    size_t maxMiB = 1024;
    bool capSaid = false;       // the over-the-cap line has been printed for this ladder
};
inline LadderPrefixCache g_lpc;            // --ladderprefix / --ladderprefixcheck are read by cliMain
inline int g_ladderPrefixEvery = 64;       // save every N layers
inline size_t g_ladderPrefixMaxSnaps = 64; // and keep at most this many (oldest dropped)

// The search's layer loop: from t0 + 1 until the plan is solved or tEnd is reached, step every
// state of the frontier through the model (model.hpp), dedupe the children, cap the frontier. `e`
// is cliMainOnce's SearchEnv -- references to what the loop shares with that call, in two parts:
// e.in (inputs) and e.outcome (what the call reads back); the plan goes back in out.inputs. The
// arena and the frontier and dedupe scratch are the loop's own (SearchArena, LayerWork above);
// `rules` is where it is going (CurrentRules above); `out` is the answer (SearchResult above).
// Returns -1 when the loop has ended, or the code cliMainOnce exits with (3: cancelled / stopped).
template <class E, class Rules>
inline int searchLayers(E& e, const Rules& rules, SearchResult& out) {
    auto& t0 = e.in.t0;
    auto& dbgLayers = e.in.dbgLayers;
    auto& init = e.in.init;
    auto& horizon = e.in.horizon;
    auto& planCut = e.in.planCut;
    auto& goalX = e.in.goalX;
    auto& tEnd = e.in.tEnd;
    LayerWork work(init);   // the frontier starts at the root
    auto& cur = work.cur;
    auto& nxt = work.nxt;
    SearchArena store(e.in.checkWanted);   // this method's states
    auto& arena = store.arena;
    auto& checkWanted = e.in.checkWanted;
    auto& arenaMode = store.arenaMode;
    auto& seen = work.seen;
    auto& shards = work.shards;
    auto& kidKeys = work.kidKeys;
    auto& kidFlag = work.kidFlag;
    auto& evKind = work.evKind;
    auto& evRec = work.evRec;
    auto& model = e.in.model;
    auto& kids = work.kids;
    auto& clearKids = work.clearKids;
    auto& gidx = work.gidx;
    auto& kParallelMin = e.in.kParallelMin;
    auto& pool = e.in.pool;
    auto& bestT = e.outcome.bestT;
    auto& bestX = e.outcome.bestX;
    auto& goalState = e.outcome.goalState;
    auto& solved = e.outcome.solved;
    auto& maxAlive = e.outcome.maxAlive;
    auto& capDropped = e.outcome.capDropped;
    auto& capHits = e.outcome.capHits;
    auto& cellCuts = e.outcome.cellCuts;
    auto& cellSpared = e.outcome.cellSpared;
    auto& cellTwinsKept = e.outcome.cellTwinsKept;
    auto& census = e.outcome.census;
    auto& twin = e.outcome.twin;
    auto& kidMoot = work.kidMoot;
    auto& gcNext = work.gcNext;
    auto& nBornPrev = work.nBornPrev;
    auto& nDiedPrev = work.nDiedPrev;
    auto& prevAlive = work.prevAlive;
    auto& curDxF = e.in.curDxF;
    auto& ppMark = e.in.ppMark;
    if (g_stateCross) stateCrossPrepare();   // --statecross (search_key.hpp)
    // --ladderprefix: only inside a ladder, and not where the loop talks to the outside or
    // prints per layer (the checkpoint channel, the reference watch, the census, the band file).
    const bool lpOn = g_lpc.active && !checkWanted && !g_refWatch && g_searchCensus == 0
                      && g_bandPath.empty();
    long long tStart = t0 + 1;
    g_lpc.resumedFrom = -1;
    const long long lpWork0 = g_outcome.workStates;   // what the call had before this loop
    if (lpOn && g_lpc.usable && g_lpc.t0 == t0 && g_lpc.resume >= 0
        && (size_t)g_lpc.resume < g_lpc.snaps.size()
        && g_lpc.snaps[(size_t)g_lpc.resume].autoFire.size() == g_autoTrig.size()) {
        LadderPrefixSnap S = g_lpc.snaps[(size_t)g_lpc.resume];
        cur = S.cur;
        for (size_t i = 0; i < cur.size(); ++i) {   // the pointers into THIS attempt's level
            const uint64_t* r = &S.objRefs[i * 12];
            State& s = cur[i];
            s.snapObj = model.decodeObj(r[0]);
            s.usedOrb = model.decodeObj(r[1]);
            s.snapObj2 = model.decodeObj(r[2]);
            s.usedOrb2 = model.decodeObj(r[3]);
            for (int k = 0; k < 4; ++k) s.usedPad[k] = model.decodeObj(r[4 + k]);
            for (int k = 0; k < 4; ++k) s.usedPad2[k] = model.decodeObj(r[8 + k]);
        }
        g_lpc.arena.resize(S.arenaSize);
        arena.swap(g_lpc.arena);
        g_lpc.arenaMode.resize(S.arenaModeSize);
        arenaMode.swap(g_lpc.arenaMode);
        work.gcNext = S.gcNext;
        nBornPrev = S.nBornPrev;
        nDiedPrev = S.nDiedPrev;
        prevAlive = S.prevAlive;
        bestT = S.bestT;
        bestX = S.bestX;
        goalState = S.goalState;
        solved = S.solved;
        maxAlive = S.maxAlive;
        capDropped = S.capDropped;
        capHits = S.capHits;
        cellCuts = S.cellCuts;
        cellSpared = S.cellSpared;
        cellTwinsKept = S.cellTwinsKept;
        census = S.census;
        twin = S.twin;
        g_outcome.workStates = lpWork0 + S.workStates;   // the loop's own work up to T
        for (size_t i = 0; i < g_autoTrig.size(); ++i) {   // the entries are this attempt's own; the fire state is the save's
            AutoTrig& A = g_autoTrig[i];
            const AutoFireState& F = S.autoFire[i];
            A.fireT = F.fireT;
            A.fireX = F.fireX;
            A.fireSpreadT = F.fireSpreadT;
            A.lockLastX = F.lockLastX;
        }
        g_rotTrig = S.rotTrig;
        for (int f = 0; f < 4; ++f)
            if (S.carryOn[(size_t)f]) model.loadWorld(f, S.carry[(size_t)f], S.T);
        g_gfireGroups = S.gfireGroups;
        g_gfireSpread = S.gfireSpread;
        g_gfireSum = S.gfireSum;
        g_gfireMax = S.gfireMax;
        g_groupXSplitMax = S.groupXSplitMax;
        g_rjJoinT = S.rjJoinT;
        g_rjOldNode = S.rjOldNode;
        g_lpc.layerX.resize(S.layerXSize);
        g_lpc.snaps.resize((size_t)g_lpc.resume + 1);   // later saves belong to the old attempt
        tStart = S.T + 1;
        g_lpc.resumedFrom = S.T;
        std::printf("ladderprefix: attempt resumes after t=%lld (%lld layers not searched again)\n",
                    S.T, S.T - t0);
    } else if (lpOn) {
        g_lpc.layerX.clear();
        g_lpc.snaps.clear();
    }
    g_lpc.usable = false;
    g_lpc.arena.clear();
    g_lpc.arenaMode.clear();
    g_lpc.t0 = t0;
    bool lpBound = capHits > 0;   // save only once this attempt's cap has bound (a call that never binds never climbs)
    for (long long t = tStart; t <= tEnd && !solved; ++t) {
        if (lpOn) g_lpc.layerX.push_back(bestX);   // the x this layer's capAtX / gridAtX read
        ppMark(6);
        nxt.clear();
        // ---- reference watch, part 1: where is it now (--refwatch) ---------
        // Locate the reference's PARENT in the frontier before the layer runs.
        // On the first layer it is looked up in `cur` directly; afterwards the
        // previous layer's check has already confirmed it is there.
        if (g_refWatch && g_refLostAt < 0) {
            if (t == t0 + 1) {
                // The trace's first row is t0+1: the state AT the anchor tick
                // is the anchor itself, which is what the frontier was seeded
                // with, so there is nothing to match. After this layer the
                // watch follows the state it is carrying, not the trace.
                g_refParent = cur.empty() ? -1 : 0;
            }
            if (g_refParent < 0 && t == t0 + 1) {
                // The anchor itself is not in the frontier. That is a fault in
                // the harness (wrong trace, wrong anchor tick), not a finding
                // about the model, and it must not read as one.
                std::printf("refwatch: the reference is not in the FIRST "
                            "frontier (t=%lld) -- wrong trace or wrong anchor, "
                            "not a search result\n", t - 1);
                g_refWatch = false;
            }
            if (t == t0 + 1)
                std::printf("refwatch: armed rows=%zu t0=%lld parent=%d "
                            "frontier=%zu\n",
                            g_refRows.size(), t0, g_refParent, cur.size());
            for (int a = 0; a < 2; ++a) {
                g_refKidFate[a] = -1;
                g_refKidWhy[a] = "";
                g_refKidKey[a] = 0;
            }
        }

        if (g_phaseProf) g_ppSubLast = std::chrono::steady_clock::now();   // ppSubMark's start for this layer's setup
        // Resolve autonomous fires from the frontier's leading edge. `cur`
        // holds states AT tick t-1, so the first layer whose lead x reaches
        // the trigger's x means the crossing happened at t-1 and the move
        // starts at t-1 + delay (measured). States behind the
        // lead see the move at most a tick or two early -- under 1 px at the
        // slide rates in play, where the recording-timeline error this
        // replaces was measured at 1,707 ticks (see AutoTrig).
        // The autonomous triggers the frontier's leader has crossed (model.hpp fireAutoTriggers).
        model.fireAutoTriggers(cur, t);
        if (g_stateCross) stateCrossSyncFire();   // the leader's dates, before any group overrides them
        ppSubMark(0);

        // ---- speed groups -------------------------------------------------
        // The frontier is no longer a single x. A state that took a speed
        // portal and one that flew past it are at the same TICK but at
        // different x, and the gap only grows (0.9 vs 1.1 is 0.316 px/tick;
        // on lv18 it reached 1,700 px). So the layer is processed once per
        // distinct speed: each group gets its own object windows, built from
        // that group's own x span, and its own dedupe map.
        // Within a group the x values differ only by stair snaps (sub-pixel to
        // a couple of px), so the windows stay as tight as the shared one was.
        // The previous scheme -- one shared accumulator, switched when ANY
        // state's y overlapped the portal -- is what put the search on
        // displaced geometry.
        // A separate `seen` per group also keeps two speeds from ever merging
        // into one cell, without putting dx in keyOf (which would repartition
        // every single-speed layer in the suite for nothing).
        // ...and by the touch-trigger mask for the same reason (see TouchTrig).
        // A state that flew through the box and one that flew under it disagree
        // about where a door IS, so they cannot share object windows and must
        // not merge in the dedupe. Levels without touch triggers keep exactly
        // one mask (0) and are partitioned exactly as before.
        // ...and by the gameplay FRAME (see RotTrig): two states that turned at
        // different ticks are in different worlds, so they cannot share object
        // windows any more than two speeds can.
        // ...and by REVERSE (id 2899) for the same reason again: a reversed
        // state's x runs the other way, so it cannot share a window with a
        // forward one. frame and rev are separate axes, so both go in the key.
        // NOT by the spent-gravity-portal mask, though it is carried per state
        // the same way `trig` is. What puts `trig` here is that it moves the
        // GEOMETRY: two masks need two sets of object windows. A spent portal
        // moves nothing -- both states see the same box, they only disagree
        // about whether it still fires -- so the mask has no business in a key
        // whose job is to choose windows, and an axis here would be paid for in
        // parent steps: gidx deliberately hands every parent to every group
        // (see the rebind note), so k masks in a layer means stepping each
        // parent k times for k identical children. It goes in keyOf instead,
        // where the separation is exactly as absolute and costs one xor.
        // The model's group key (model.hpp GroupKey), and the search's own split of it:
        // xc, --groupxsplit's part, 0 without the flag (search_key.hpp).
        struct GKey : GroupKey {
            uint16_t xc;
            bool operator==(const GKey& o) const {
                return GroupKey::operator==(o) && xc == o.xc;
            }
        };
        std::vector<uint64_t> curSig((g_groupFire || g_stateCross) ? cur.size() : 0);
        for (size_t i = 0; i < curSig.size(); ++i)
            curSig[i] = groupFireSig(cur[i], t) ^ (g_stateCross ? xcSig(cur[i], t) : 0);   // --statecross: what the members dated themselves
        auto sigOf = [&](size_t i) -> uint64_t { return curSig.empty() ? 0 : curSig[i]; };
        // --groupxsplit: the members of each group in x order, a new part wherever the gap to the
        // previous member is wider than the threshold. Part 0 is the lowest x.
        std::vector<uint16_t> curXc;
        // ...only in a layer where every state is in frame 0 and runs forward. gidx hands a parent
        // of another frame to every group of this one (the note above); splitting changed that
        // (such a parent went to part 0 alone), and on lv22 -- whose layers carry turned frames --
        // the flag took the loop off d008ee2's route at the second iteration, the search's
        // chosen plan dying in its own resim in frame 1 (`resimdie=511@1758/spider/no-target
        // ... f1`). Where a turned or reversed frame is live the layer is processed as before.
        bool xsplitHere = g_groupXSplit && cur.size() > 1;
        for (size_t i = 0; xsplitHere && i < cur.size(); ++i)
            if (model.progress(cur[i]).frame != 0 || model.progress(cur[i]).rev != 0) xsplitHere = false;
        if (xsplitHere) {
            curXc.assign(cur.size(), 0);
            std::vector<GKey> bases;
            std::vector<std::vector<size_t>> members;
            for (size_t i = 0; i < cur.size(); ++i) {
                const State& s = cur[i];
                const GKey b{model.groupKey(s, curDxF, sigOf(i)), 0};
                const size_t j = (size_t)(std::find(bases.begin(), bases.end(), b) - bases.begin());
                if (j == bases.size()) {
                    bases.push_back(b);
                    members.emplace_back();
                }
                members[j].push_back(i);
            }
            bool split = false;
            for (size_t j = 0; j < members.size(); ++j) {
                std::vector<size_t>& m = members[j];
                const double gap = 2.0 * kGroupWinMargin + std::fabs((double)bases[j].dx);
                std::sort(m.begin(), m.end(), [&](size_t a, size_t b) {
                    const double xa = model.progress(cur[a]).x, xb = model.progress(cur[b]).x;
                    return xa < xb || (xa == xb && a < b);
                });
                uint16_t c = 0;
                for (size_t k = 1; k < m.size(); ++k) {
                    if (model.progress(cur[m[k]]).x - model.progress(cur[m[k - 1]]).x > gap
                        && c < 0xFFFF)
                        ++c;
                    curXc[m[k]] = c;
                }
                if (c > 0) split = true;
                if ((int)c + 1 > g_groupXSplitMax) g_groupXSplitMax = (int)c + 1;
            }
            if (split) ++g_groupXSplitLayers;
        }
        auto xcOf = [&](size_t i) -> uint16_t { return curXc.empty() ? 0 : curXc[i]; };
        std::vector<GKey> groupDx;
        for (size_t i = 0; i < cur.size(); ++i) {
            const State& s = cur[i];
            const GKey k{model.groupKey(s, curDxF, sigOf(i)), xcOf(i)};
            if (std::find(groupDx.begin(), groupDx.end(), k) == groupDx.end())
                groupDx.push_back(k);
        }

        auto emit = [&](State s, const State& from, uint8_t action, uint64_t k) {
            auto finish = [&](State& stored) {
                arena.push_back(Node{from.parent | ((uint32_t)action << 31)});
                if (checkWanted) arenaMode.push_back(s.mode);
                s.parent = (uint32_t)(arena.size() - 1);
                s.action = action;
                stored = s;
                // the goal is reached by a STATE, at its own x -- not by the
                // layer's nominal timeline. Only in frame 0: `goalX` is a world
                // x, and in a turned frame xAbs is a different axis entirely
                // (lv22 ends un-rotated, so nothing is lost).
                // The goal is reached by a STATE, and by MORE THAN ONE of them:
                // the layer runs to the end (the loop tests `!solved` on the
                // NEXT iteration), so every other state that got past goalX is
                // emitted too and only loses to this latch. Measured on lv16:
                // 2,000 of them, whose lineages stay distinct for 3,345 ticks.
                // Keep the roomiest instead of the first; with the score off
                // they all read 0 and the incumbent always wins, which is the
                // old latch exactly.
                // --coins: the end of the level is not the goal on its own.
                // A lineage that reached it without the full set is not a
                // solution, and saying so HERE rather than filtering afterwards
                // is what makes the search look for the other one instead of
                // reporting the first thing that got to the end.
                if (rules.reached(s) && (!solved || rules.prefer(s.tight, goalState.tight))) {
                    solved = true;
                    goalState = s;
                }
            };
            Slots& sl = seen.at(k);
            if (sl.hi < 0) {
                nxt.push_back(State{});
                sl.hi = sl.lo = (int)nxt.size() - 1;
                finish(nxt.back());
            } else if (s.vy > nxt[(size_t)sl.hi].vy + 1e-9) {
                if (sl.hi == sl.lo) {
                    nxt.push_back(State{});
                    sl.hi = (int)nxt.size() - 1;
                }
                finish(nxt[(size_t)sl.hi]);
            } else if (s.vy < nxt[(size_t)sl.lo].vy - 1e-9) {
                if (sl.hi == sl.lo) {
                    nxt.push_back(State{});
                    sl.lo = (int)nxt.size() - 1;
                }
                finish(nxt[(size_t)sl.lo]);
            }
        };

        // "alive dropped" does not say whether the children DIED or were merged
        // away by the dedupe; those need opposite fixes, so count both.
        long long nBorn = 0, nDied = 0;
        // --qfoldwatch: which rotation-queue states this group's dedupe merged
        // away. keyOf has no rotSpent/rotChan/rotRev, so children that differ
        // only in the queue share a cell. For each key, the queue states among
        // the group's live children minus those among what the dedupe kept (the
        // survivors are copies of children, so they key the same). Print only.
        long long qfLost = 0, qfKeys = 0;
        auto qfoldAccount = [&](size_t before) {
            auto qs = [](const State& s) {
                return ((uint64_t)s.rotSpent << 32) | ((uint64_t)s.rotChan << 16)
                       | (uint64_t)s.rotRev;
            };
            auto add = [](std::vector<uint64_t>& v, uint64_t q) {
                if (std::find(v.begin(), v.end(), q) == v.end()) v.push_back(q);
            };
            std::unordered_map<uint64_t, std::vector<uint64_t>> alive, kept;
            for (size_t i = 0; i < kids.size(); ++i)
                if (kidFlag[i] == 2) add(alive[kidKeys[i]], qs(kids[i].s));
            for (size_t j = before; j < nxt.size(); ++j)
                add(kept[modelKeyOf(nxt[j], (long long)t) ^ rules.keyBits(nxt[j])], qs(nxt[j]));
            for (const auto& [k, a] : alive) {
                const auto it = kept.find(k);
                long long lost = 0;
                for (uint64_t q : a)
                    if (it == kept.end()
                        || std::find(it->second.begin(), it->second.end(), q)
                               == it->second.end())
                        ++lost;
                if (lost) { qfLost += lost; ++qfKeys; }
            }
        };
        const bool qfHere = (g_qfoldLo >= 0 && t >= g_qfoldLo && t <= g_qfoldHi);
        ppSubMark(1);
        for (const auto& gkey : groupDx) {
        if (g_phaseProf) g_ppSubLast = std::chrono::steady_clock::now();   // this group's setup starts here (not at the end of the previous group's step)
        const float gdx = gkey.dx;
        const TouchMask gtrig = gkey.trig;
        const int gframe = (int)gkey.frame;
        const int grev = (int)gkey.rev;
        // this group's own x span, and the windows that cover it for the whole
        // tick (from where its slowest member starts to where it ends up)
        double gLo = 1e18, gHi = -1e18;
        int gFire = -1;   // latest tick any of them fired a touch trigger
        int gFireMin = 0x7fffffff;   // ...and the earliest, for the pass count only
        // ...and the same thing per box, for the formula path. The group key
        // holds `trig`, so every member has entered the SAME set of boxes --
        // what differs is when, and this keeps the latest for each one
        // separately instead of collapsing them onto a single scalar.
        uint16_t gFireB[kTouchBits] = {};
        // ...and how far the least-advanced member has carried a locked
        // platform. Same conservative direction as `gFire`, which takes the
        // LATEST firing tick so the group is shown a door that is still
        // opening: the smallest offset is the one that has ridden least.
        float gLockOff = 1e30f;
        size_t gRep = (size_t)-1;   // --statecross: a member, whose records are the group's
        for (size_t ci = 0; ci < cur.size(); ++ci) {
            const State& s = cur[ci];
            // Asked for every state of every group, every layer: the model's test compares the
            // cheapest fields first and stops at the first difference; the fire signature and the
            // part, which this search holds per state, only after it.
            if (!model.inGroup(gkey, s, curDxF) || sigOf(ci) != gkey.fsig || xcOf(ci) != gkey.xc)
                continue;
            if (gRep == (size_t)-1) gRep = ci;
            // x read straight off the State, not through model.progress, here and in the three other
            // per-state loops of a layer (gidx's frame/rev, the cap's x range, the layer's leader):
            // through it, lv22's search took 3% longer (measured on 8eb7b5dc against e5eec93a).
            gLo = std::min(gLo, (double)s.xAbs);
            gHi = std::max(gHi, (double)s.xAbs);
            gFire = std::max(gFire, (int)s.trigT);
            gFireMin = std::min(gFireMin, (int)s.trigT);
            // gFireB is uint16_t[kTouchBits] -- fold all of it, not the first 32.
            for (int b = 0; b < kTouchBits; ++b)
                if (s.fireB[b] > gFireB[b]) gFireB[b] = s.fireB[b];
            if (s.lockOff < gLockOff) gLockOff = s.lockOff;
        }
        if (gLockOff > 1e29f) gLockOff = 0.f;   // no member (group is empty)
        // --p2xoff: how far the members' second bodies run from their first (model.hpp p2Widen). A
        // pass of its own, under the flag only: the loop above is the layer's hottest (one helper
        // call in it cost lv22's search 3%, see its note), so nothing of the flag's goes in it.
        float gD2Lo = 0.f, gD2Hi = 0.f;
        for (size_t ci = 0; ci < cur.size(); ++ci) {
            const State& s = cur[ci];
            if (!model.inGroup(gkey, s, curDxF) || sigOf(ci) != gkey.fsig || xcOf(ci) != gkey.xc)
                continue;
            const float d2 = p2Dx(s);
            gD2Lo = std::min(gD2Lo, d2);
            gD2Hi = std::max(gD2Hi, d2);
        }
        // The gate's pass count: this group is APPROXIMATED only if its members
        // disagree about when the trigger fired. Counted here, before any of the
        // reasons below can `continue` past it, so "placed" means placed.
        if (gFire >= 0) {
            ++g_gfireGroups;
            if (gFireMin < gFire) {
                ++g_gfireSpread;
                const int d = gFire - gFireMin;
                g_gfireSum += d;
                if (d > g_gfireMax) g_gfireMax = d;
            }
        }
        // Drop a whole group once it is past a box it was required to enter.
        // Killing it here rather than at the end keeps the frontier spent on
        // states that can still satisfy the requirement.
        if (g_needTrig) {
            bool missed = false;
            for (size_t b = 0; b < g_touch.size() && !missed; ++b)
                if ((g_needTrig & ~gtrig) & touchBit(b))
                    // --needtrigrev (triggers.hpp): a reversed group passes a box going left
                    missed = (g_needTrigRev && grev) ? gHi < g_touch[b].cx - g_touch[b].hw - 40.0
                                                     : gLo > g_touch[b].cx + g_touch[b].hw + 40.0;
            if (missed) continue;
        }
        const double xPrev = gHi;                    // nominal, reports only
        // A reverse (id 2899) group's x decreases. The windows too are laid
        // out along the direction of travel.
        const double sdx = grev ? -(double)gdx : (double)gdx;
        const double x = (grev ? gLo : gHi) + sdx;   // ditto
        const float dxUsed = (float)sdx;
        const double wLo = grev ? (gLo + sdx) : gLo;
        const double wHi = grev ? gHi : (gHi + sdx);
        ppSubMark(1);
        // Gameplay rotation triggers this group's advance reaches (model.hpp fireRotTriggers).
        model.fireRotTriggers(gframe, grev, gLo, gHi, wLo, wHi, t);
        // The model builds this group's tick (model.hpp prepareGroup): the moving geometry placed
        // for tick t, the doors the group has opened, the windows, the reachable touch boxes and
        // the coins where they are now.
        // --autoown (dynamics.hpp): this group's own crossings for the autonomous triggers, for its
        // placement and its step; the leader's come back when the group is done.
        AutoOwnGuard autoOwn;
        g_autoOwnT0 = t0;   // --autoownpre (dynamics.hpp); the same value from every thread
        if (gframe == 0 && !grev) {
            if (g_stateCross && gRep != (size_t)-1) autoOwnApplyCross(autoOwn, gHi, (double)gdx, t, cur[gRep]);
            else autoOwnApply(autoOwn, gHi, (double)gdx, t);
        }
        ppSubMark(2);
        GroupEnv env;
        double pLo = wLo, pHi = wHi;
        p2Widen(pLo, pHi, gD2Lo, gD2Hi);   // --p2xoff
        model.prepare(env, gframe, gtrig, gFire, gFireB, gLockOff + (float)sdx, x, xPrev, dxUsed,
                      pLo, pHi, t);
        ppSubMark(3);
        const std::vector<const Obj*>& near = env.near;
        const std::vector<const Obj*>& ports = env.ports;
        const std::vector<const Obj*>& pads = env.pads;
        const std::vector<const Obj*>& orbs = env.orbs;
        const std::vector<const Obj*>& slps = env.slps;
        const std::vector<std::pair<const TouchTrig*, TouchMask>>& trigs = env.trigs;
        seen.ensure(cur.size() * 2 + 64);
        seen.clear();   // per GROUP, not per layer (see the note above)
        (void)x; (void)xPrev;
        const StepCtx K = env.ctx();
        ppMark(0);
        // ---- phase 1: STEP every state of this group (parallel) -------------
        // Stepping is pure -- it reads the shared windows and writes only its
        // own child -- so it parallelises exactly. The dedupe, the arena and
        // `nxt` stay on the main thread in phase 2, walked in the SAME order as
        // the serial loop was, which keeps the emitted plan bit-identical.
        gidx.clear();
        for (size_t i = 0; i < cur.size(); ++i)
            if (((cur[i].dx > 0.f) ? cur[i].dx : curDxF) == gdx
                && cur[i].trig == gtrig && sigOf(i) == gkey.fsig
                // --groupxsplit: a member of this frame and direction goes to its own part only.
                // A parent from another frame (gidx hands those to every group, see above) keeps
                // being stepped once per frame group, in its part 0.
                && (curXc.empty()
                    || ((((int)cur[i].frame == gframe && (int)cur[i].rev == grev))
                            ? curXc[i] == gkey.xc
                            : gkey.xc == 0)))
                gidx.push_back(i);
        if (g_phaseProf) { ++g_ppGroups; g_ppParents += (long long)gidx.size(); }
        // Airborne cube input is normally moot, so only one child is expanded --
        // but NOT next to an orb, where a press in mid-air is the whole point.
        // Leaving this unconditional silently threw away every orb activation
        // (lv3: 0 boosts in 35k expansions). Decided per state up front so that
        // slot 2i+1 either holds that state's second child or nothing.
        // ...and NOT while DASHING, where the input is the only thing holding the
        // player up. A dash ends the tick after the button comes off (the
        // `s.dashing && s.action` rule in the step), so pruning the pressed child
        // means the frontier can never keep a dash alive further than 50 px past
        // the last orb in the window -- the dash is forcibly released the moment
        // the ring leaves the window, whatever the level needs.
        // Measured on lv21 (2026-08-10): the dash ring at (2445,229) is the only
        // way across the pit at x=2550..2700 (no floor at all, spikes at y<=181).
        // At t=1966 the layer expanded 1,402 pressed children; at t=1967 it
        // expanded ZERO, so every dashing state was released at t=1968 (x=2,557)
        // and fell into the pit. GD with the same press held to t=2200 reaches
        // x=2,996. The frontier collapse the previous session blamed on
        // granularity was this: the winning branch was never enumerated.
        kids.assign(gidx.size() * 2, Child{});
        kidKeys.assign(gidx.size() * 2, 0);
        kidFlag.assign(gidx.size() * 2, 0);
        const bool dbgHere = (dbgLayers > 0 && t - t0 <= dbgLayers);
        const bool orbsEmpty = orbs.empty();
        // --twinskip / --twinaudit (search_census.hpp): is this parent's pressed child a twin
        // of its released one? Only the airborne ground modes, whose button does nothing in
        // the air except through the things excluded here: something the press fires (a
        // ring or portal close enough to matter this tick, a toggle block, a tap or
        // the counting tap), a hold the release would change (ringHold / pressSpent /
        // holdDead, the robot's hover, a dash), and GD's mid-air coyote jump (ogLinger).
        // Off wherever an instrument reads the pressed child itself.
        bool groupPressTrig = false;
        for (const auto& tb : trigs)
            if (tb.first->tap || tb.first->press) { groupPressTrig = true; break; }
        // --heldall writes the button into `held` in every mode, and keyOf keys `held` in every
        // mode, so under it the two children part in the key and the pressed one is no longer
        // deduped away behind its released twin: it is kept, and it has to be the child stepping
        // would have made. The fill below sets what the button wrote (held, the jump buffer) and
        // takes the key again; --twinaudit checks that against the stepped child.
        const bool twinOn = !dbgHere && !g_refWatch
                            && !g_clearProbe && !qfHere && g_coinDbgT < 0 && !g_airPress
                            && false   // --latgap was always on, and the twin skip never ran with it
                            && !(g_rotWatchLo >= 0 && t >= g_rotWatchLo && t <= g_rotWatchHi);
        kidMoot.assign(twinOn ? gidx.size() : 0, 0);
        // UNREACHED while twinOn carries `&& false`: no run asks this, so nothing has checked it
        // against the input latency the step now models (--latgap). It stays here, in the search,
        // rather than in the model's plugs (model.hpp) until --twinaudit can vouch for it again.
        auto pressMoot = [&](const State& s) -> bool {
            if (s.dual || s.grounded || s.onSlope) return false;
            if (s.mode != 0 && s.mode != 2 && s.mode != 5 && s.mode != 6) return false;
            if ((int)s.frame != gframe || (int)s.rev != grev) return false;
            if (s.ringHold || s.pressSpent || s.rHover || s.dashing || s.ogLinger || s.holdDead)
                return false;
            // Inside the flip grace a head-side contact seats the body (step.hpp, flipGraceSeat),
            // and whether it does reads the press: a ball's tap after the collision pass keeps the
            // window open, so the pair can part there. No corpus state has shown it on this path;
            // the gravity pad below is the same window opened on the tick itself, and it has.
            if ((int)s.flipT + 1 < kFlipGraceTicks) return false;
            if (nearPressBox(s)) return false;
            if (model.pressMatters(s)) return false;
            // A fixup record is matched per INPUT (fixup.hpp fixupMatches), so one landing on
            // either child splits the pair -- measured in a loop call of a custom level at
            // t=4,325, an upside-down ball whose released child took a +2.83 vy record.
            // Any record within a few px of the parent's x rules the pair out.
            for (const std::vector<Fixup>* fv : {&g_fixupKills, &g_fixupDeltas}) {
                const float lo = s.xAbs - 8.f;
                const auto it = std::lower_bound(
                    fv->begin(), fv->end(), lo,
                    [](const Fixup& f, float xx) { return f.x < xx; });
                if (it != fv->end() && it->x <= s.xAbs + 8.f) return false;
            }
            const double px = (double)s.xAbs, py = (double)s.y;
            const double rx = 60.0 + 2.0 * std::fabs((double)K.dxF);
            const double ry = 60.0 + std::fabs((double)s.vy);
            auto within = [&](const std::vector<const Obj*>& v) {
                for (const Obj* o : v)
                    if (std::fabs(o->cx - px) <= o->hw + rx && std::fabs(o->cy - py) <= o->hh + ry)
                        return true;
                return false;
            };
            // Rings: the contact test reads this tick's and the previous tick's position, and a
            // rotated ring reaches 18 + 15*sqrt(2) = 39.2 px along its axis; the margin covers
            // both. Portals: a mode portal can hand the press to the new mode on its tick
            // (--portalpress). Pads read no input in the air, so they are not a reason.
            // Ramps: a contact made on this tick goes through slopeNudge, which reads the
            // input (a custom level t=15,834, an airborne cube at vy 0: released -2.000, pressed
            // -0.216).
            // A gravity pad (type 10) flips the body in the collision pass, which opens the flip
            // grace above from any flipT (flipGraceSeat's age 0), so one in reach rules the pair out
            // too. Gravity portals are among `ports` already. The twinaudit witnesses are this
            // path: lv11 t=8,968 and lv12 t=18,838, a ball whose pressed child is seated at vy 0
            // while the released one leaves at -3.072.
            for (const Obj* o : pads)
                if (o->type == 10 && std::fabs(o->cx - px) <= o->hw + rx
                    && std::fabs(o->cy - py) <= o->hh + ry)
                    return false;
            return !within(orbs) && !within(ports) && !within(slps);
        };
        // --inputgrid / --gridmap: this layer's grid, from the leader x of the layer before it
        // (the x --capmap reads, so a ladder window lifts both over the same stretch).
        const int gridNow = gridAtX(bestX);
        // --airpressnear (with --airpress): the air press child is kept only where a landing is within
        // reach. A press held from the air into a landing is one future whatever tick in the arc it began
        // on (the buffer is set at the first and the states merge once it is), so the children of the
        // ticks long before any surface add expansions and no reachable state.
        auto landingNear = [&](const State& s) -> bool {
            constexpr double n = 8.0, g = 0.958;
            const double px = (double)s.xAbs, vy = (double)s.vy;
            const double foot = (double)s.y - 15.0;
            const double lb = foot - 0.5 * g * n * n - std::max(0.0, -vy) * n;
            const double ub = foot + std::max(0.0, vy) * n + 1.0;
            const double rx = 15.0 + n * std::fabs((double)K.dxF);
            auto hit = [&](const std::vector<const Obj*>& v, bool solidOnly) {
                for (const Obj* o : v) {
                    if (solidOnly && o->type != 0) continue;
                    const double top = o->cy + o->hh;
                    if (top >= lb && top <= ub && std::fabs(o->cx - px) <= o->hw + rx) return true;
                }
                return false;
            };
            return hit(near, true) || hit(slps, false);
        };
        auto stepKidAt = [&](size_t i, bool mayTwin) {
            const State& s = cur[gidx[i >> 1]];
            const int input = (int)(i & 1);
            // May the button change on this tick (model.hpp edgeLocked)?
            const bool edgeLocked = model.edgeLocked(s, gridNow, K.t);
            const int forcedIn = mustPressAt((long long)K.t);   // --mustpress: the tick's button is given
            if (forcedIn >= 0 && input != forcedIn) return;
            if (forcedIn < 0 && edgeLocked && input != (int)s.action) return;
            // ...and NOT next to a toggle block it has not fired (nearPressBox), which the
            // button fires: SubZero 4003's uid 4001 is passed by an airborne cube, so with
            // the pressed child pruned no state ever entered it (2026-09-24).
            // ...and NOT under --airpress (g_airPress): a press held from the air into a
            // landing is a different jump from one pressed on the landing tick.
            // ...and not when the input rule has just forbidden the release: then the held child
            // is the only one left.
            if (input == 1 && forcedIn < 0 && s.mode == 0 && !s.grounded && orbsEmpty
                && !s.dashing && !(s.dual && s.dashing2)   // --dualdash
                && !nearPressBox(s) && !nearCtrlHoldBox((double)s.xAbs)
                && !(g_airPress && (!g_airPressNear || landingNear(s)))
                && !nearHoldTap((double)s.xAbs)   // --holdtap (object.hpp)
                && !(edgeLocked && s.action == 1))
                return;
            // --latgap: no edge on a tick GD cannot put one (model.hpp inputAllowed)
            if (!model.inputAllowed(s, input, gridNow, K.t))
                return;
            // --twinskip: decided after the step pass, from the released sibling (slot i-1):
            // copied in when that one stayed in the air, stepped for real otherwise.
            if (input == 1 && forcedIn < 0 && mayTwin && twinOn && pressMoot(s)) {
                if (!g_twinAudit) {
                    kidFlag[i] = 3;
                    return;
                }
                kidMoot[i >> 1] = 1;
            }
            Child& kid = kids[i];
            bool dead = false;
            // The model's tick for this state and input (model.hpp advanceState): the physics,
            // then the gameplay rotation if the tick crossed one -- BEFORE keyOf below, which must
            // key the child in the frame it ended in.
            RotNote rot;
            kid.s = model.advance(s, input, K, dead, &rot);
            if (rot.set) {   // --rotwatch: print only, read in the serial pass below
                kid.rotUid = rot.uid;
                kid.rotF0 = rot.f0;
                kid.rotNf = rot.nf;
            }
            // The child's own action, set HERE and not only in emit()'s
            // finish(): keyOf reads it (for the robot's hover) and the key is
            // computed on this thread, before phase 2 runs. Without this the
            // key saw the PARENT's action and the held and released children
            // hashed to the same cell -- which is the whole bug the rHover
            // note in keyOf describes.
            // (model.hpp noteInput: the action, --minpulse's edge age, --latgap's lock.)
            model.noteInput(s, kid.s, input);
            // --statecross: the late crossings of autonomous triggers this step made (search_key.hpp)
            if (g_stateCross && !dead) stateCrossNote(kid.s, s, t);
            // ---- --coins: collect, then kill whoever left one behind (rules.afterStep) ----
            model.fireGates(kid.s, s.action, input, K, gframe, t, dead);
            rules.afterStep(s, kid, input, K, gframe, t, dead);
            // --needsize: a state that is not the size the wall needs is dropped on the tick named
            if (!dead && t == g_needSizeT && (int)kid.s.mini != g_needSizeMini)
                DIE_AS(kDeathRule, "size/required", nullptr);
            // --needspeed: a state that crosses the wall's x at a speed the rule does not allow is dropped on the tick it does
            if (!dead && g_needSpeedX >= 0.0 && (double)s.xAbs < g_needSpeedX && (double)kid.s.xAbs >= g_needSpeedX
                && ((int)speedCodeForDx((double)kid.s.dx) == g_needSpeedCode) == g_needSpeedNot)
                DIE_AS(kDeathRule, "speed/required", nullptr);
            // Route tightness rides along with the child (State c = s copies it
            // already, so this only adds this tick's own contribution). Read by
            // the goal pick below; nothing else looks at it.
            if (!dead && kid.s.tight < 0xffff
                && tightHere(near, (double)kid.s.xAbs, (double)kid.s.y,
                             playerHalf(kid.s.mode, kid.s.mini != 0), kTightPx))
                ++kid.s.tight;
            // --wavetidy / --wavetoggle: a wave's tick is charged for how far it sits from the middle of its corridor,
            // and a change of the button for itself (thread_pool.hpp); saturating, never wrapping.
            if (!dead && (g_waveTidy > 0.0 || g_waveToggle > 0) && kid.s.mode == 4 && kid.s.frame == 0) {
                int add = 0;
                if (g_waveTidy > 0.0) {
                    const double half = playerHalf(kid.s.mode, kid.s.mini != 0);
                    const double off = waveOffCentre(near, (double)kid.s.xAbs, (double)kid.s.y, half,
                                                     (double)kid.s.bandFloor, (double)kid.s.bandCeil,
                                                     kid.s.mini ? kWaveClampMini : kWaveClamp);
                    add += (int)(g_waveTidy * off * off);
                }
                if (g_waveToggle > 0 && (int)s.action != input) add += g_waveToggle;
                kid.s.tight = (uint16_t)std::min(0xffff, (int)kid.s.tight + add);
            }
            kid.dead = dead ? 1 : 0;
            kid.valid = 1;
            // the dedupe key is a pure function of the child, so it belongs on
            // this thread rather than in the serial phase
            if (!dead) kid.key = modelKeyOf(kid.s, (long long)t) ^ rules.keyBits(kid.s);
            // fireB's invariant, checked where every surviving child passes.
            // (i) is invisible to every replay harness -- dedupe is not on the
            // replay path -- so "quick_regress is byte-identical" says the
            // change was not SEEN, not that it is right. This is the direct
            // witness that the array is written where it should be: a tick
            // present for a box the state never entered, or a box entered with
            // no tick, is an implementation defect and nothing else would
            // catch it.
            if (!dead && g_fireBCheck) {
                for (int b = 0; b < kTouchBits; ++b) {
                    const bool bit = kid.s.trig.test(b);
                    const bool has = kid.s.fireB[b] != 0;
                    if (bit && !has) ++g_fireBNoTick;
                    if (!bit && has) ++g_fireBNoBit;
                    // ...and against an INDEPENDENT tick. g_touchFireT[b] is
                    // written in a different scope (level-wide, first entry
                    // only), so "no state entered box b before the first time
                    // any state entered box b" is a real check on the index and
                    // the value rather than a restatement of the line that
                    // wrote them. An anchored run seeds fireB from --start, so
                    // those may legitimately predate it and are skipped.
                    if (bit && has && g_touchFireT[b] >= 0
                        && (int)kid.s.fireB[b] < g_touchFireT[b]
                        && !init.trig.test(b))
                        ++g_fireBTooEarly;
                }
            }
            // compact side arrays for phase 2p: the shard scans read 9 B per
            // child instead of dragging the whole Child through the cache
            kidKeys[i] = kid.key;
            kidFlag[i] = dead ? 1 : 2;
            if (dbgHere || g_refWatch) { kid.why = g_deadWhy; kid.obj = g_deadObj; }
        };
        auto stepKid = [&](size_t i) {
            if (!g_phaseProf) { stepKidAt(i, true); return; }
            const auto k0 = std::chrono::steady_clock::now();   // --phaseprof: the work inside the step, per child
            stepKidAt(i, true);
            g_ppKidNs.fetch_add((long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - k0).count(),
                                std::memory_order_relaxed);
        };
        // --dbg stays serial: it prints per child, in order, and the reason
        // globals are per thread.
        const auto pf0 = g_phaseProf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
        if (pool && !dbgHere && kids.size() >= kParallelMin)
            pool->parallelFor(kids.size(), stepKid);
        else {
            for (size_t i = 0; i < kids.size(); ++i) stepKid(i);
            if (g_phaseProf) ++g_ppSerialGroups;
        }
        if (g_phaseProf)
            g_ppStepNs += (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - pf0).count();
        // --twinskip: fill each skipped pressed slot with its released sibling, which is what
        // stepping it would have produced in everything the dedupe and the counts read.
        // --twinaudit: both were stepped; check that claim instead.
        // The claim needs the released sibling to have STAYED in the air, alive: a body that
        // lands on this tick jumps on this tick if the button is down (measured on lv22 t=372:
        // released vy 0.000, pressed 11.180, same y), and a death is not worth reasoning about.
        if (twinOn) {
            auto airborne = [&](size_t a) {
                const Child& k = kids[a];
                return kidFlag[a] == 2 && !k.s.grounded && !k.s.onSlope;
            };
            std::vector<size_t> redo;   // pending pressed slots whose sibling did not qualify
            for (size_t j = 0; j < gidx.size(); ++j) {
                const size_t a = 2 * j, b = a + 1;
                const State& p = cur[gidx[j]];
                const int m = p.mode < 8 ? p.mode : 0;
                // The pressed child as the fill makes it from the released one. Without --heldall
                // the two share a key and the pressed one never survives the dedupe, so only the
                // key is carried; under it the button's own writes are set too and the key taken
                // again (the note above twinOn).
                auto pressedFrom = [&](const Child& rel) {
                    Child k = rel;
                    k.s.action = 1;
                    if (g_minPulse > 1 || g_wavePulse > 1)   // --wavepulse reads edgeAge too
                        k.s.edgeAge = (p.action != 1)
                                          ? 0 : (uint8_t)std::min(254, (int)p.edgeAge + 1);
                    if (g_heldAll) {
                        k.s.held = 1;
                        k.s.jumpBuf = 1;
                        if (g_dualJumpBuf && k.s.dual) k.s.jumpBuf2 = 1;   // --dualjumpbuf
                        k.key = modelKeyOf(k.s, (long long)t) ^ rules.keyBits(k.s);
                    }
                    return k;
                };
                if (g_twinAudit) {
                    if (!kidMoot[j] || !airborne(a)) continue;
                    ++twin.predicted;
                    ++twin.byMode[m];
                    const int fa = kidFlag[a], fb = kidFlag[b];
                    bool ok = fa == fb;
                    if (ok && fa == 2) {
                        if (g_heldAll) {
                            const Child pk = pressedFrom(kids[a]);
                            ok = pk.key == kidKeys[b] && pk.s.vy == kids[b].s.vy
                                 && pk.s.y == kids[b].s.y && pk.s.trig == kids[b].s.trig
                                 && pk.s.held == kids[b].s.held
                                 && pk.s.jumpBuf == kids[b].s.jumpBuf;
                        } else {
                            ok = kidKeys[a] == kidKeys[b] && kids[a].s.vy == kids[b].s.vy
                                 && kids[a].s.trig == kids[b].s.trig;
                        }
                    }
                    if (!ok)
                        twin.violation(t, p, fa, fb, kids[a].s, kids[b].s, kidKeys[a],
                                       kidKeys[b]);
                } else if (kidFlag[b] == 3) {
                    if (!airborne(a)) {
                        kidFlag[b] = 0;
                        redo.push_back(b);
                        continue;
                    }
                    kids[b] = pressedFrom(kids[a]);
                    kidKeys[b] = g_heldAll ? kids[b].key : kidKeys[a];
                    kidFlag[b] = kidFlag[a];
                    ++twin.skipped;
                    ++twin.byMode[m];
                }
            }
            if (!redo.empty()) {
                auto stepRedo = [&](size_t k) { stepKidAt(redo[k], false); };
                if (pool && redo.size() >= kParallelMin)
                    pool->parallelFor(redo.size(), stepRedo);
                else
                    for (size_t k = 0; k < redo.size(); ++k) stepRedo(k);
            }
        }
        ppMark(1);
        // --searchcensus: did the two inputs of each parent lead anywhere different? Print only.
        if (g_searchCensus > 0)
            for (size_t j = 0; j < gidx.size(); ++j)
                census.siblings(cur[gidx[j]], kidFlag[2 * j], kidFlag[2 * j + 1], kids[2 * j].s,
                                kids[2 * j + 1].s, kidKeys[2 * j], kidKeys[2 * j + 1],
                                (long long)t);
        // --rotwatch: the rotations this group's children actually took, counted
        // serially after the parallel step. Print only.
        if (g_rotWatchLo >= 0 && t >= g_rotWatchLo && t <= g_rotWatchHi) {
            struct RW { int uid, f0, nf; size_t n; };
            std::vector<RW> rws;
            size_t nk = 0;
            for (const Child& k : kids) {
                if (!k.valid) continue;
                ++nk;
                if (k.rotNf < 0) continue;
                bool found = false;
                for (RW& r : rws)
                    if (r.uid == k.rotUid && r.f0 == k.rotF0 && r.nf == k.rotNf) { ++r.n; found = true; break; }
                if (!found) rws.push_back(RW{k.rotUid, (int)k.rotF0, (int)k.rotNf, 1});
            }
            for (const RW& r : rws)
                std::printf("rotwatch: t=%lld gdx=%.4f uid=%d f0=%d nf=%d kids=%zu/%zu\n",
                            (long long)t, (double)gdx, r.uid, r.f0, r.nf, r.n, nk);
        }

        // ---- reference watch, part 2: what happened to its child ------------
        // Every child has been stepped and keyed, and nothing has been merged
        // or capped yet, so this is where the reference's own continuation can
        // still be told apart from the ones that replaced it. Read-only, in the
        // same position and for the same reason as the clearance probe below.
        //
        // The parent may be stepped in more than one group when a level has
        // several live frames (gidx filters on dx and trig only), so an ALIVE
        // fate wins over a dead one rather than the last one written.
        if (g_refWatch && g_refParent >= 0) {
            for (size_t i = 0; i < kids.size(); ++i) {
                if ((int)gidx[i >> 1] != g_refParent) continue;
                const int a = (int)(i & 1);
                if (g_refKidFate[a] == 2) continue;   // already alive somewhere
                g_refKidFate[a] = (int)kidFlag[i];
                g_refKidWhy[a] = kids[i].why ? kids[i].why : "";
                g_refKidKey[a] = kidKeys[i];
                g_refKidState[a] = kids[i].s;
            }
        }

        // ---- clearance probe (instrumentation, --clearprobe) ----------------
        // Every child has been stepped and keyed by now, and this only READS
        // them -- no slot, no ordinal and no key is touched -- so the emitted
        // plan is the same with the flag on or off. That equality is the
        // acceptance test (py/quick_regress.py, bit-identical).
        // Serial on purpose: the stepping above is what needed the threads, and
        // a probe that took a lock per child would be measuring the lock.
        if (g_clearProbe) {
            clearKids.clear();
            for (size_t i = 0; i < kids.size(); ++i) {
                if (kidFlag[i] != 2) continue;   // 1 = died, 0 = never expanded
                const State& c = kids[i].s;
                // A turned frame has "above" and "below" on a different axis;
                // measuring it here would mix two coordinate systems in one
                // histogram. Counted instead, so the omission is visible.
                if (c.frame != 0) { ++g_clear.framesSkipped; continue; }
                const double px = (double)c.xAbs, py = (double)c.y;
                ClearSample cs;
                cs.key = kidKeys[i];
                cs.vy = c.vy;
                cs.solid = solidClearance(near, px, py,
                                          playerHalf(c.mode, c.mini != 0),
                                          &g_clear.slopesSkipped);
                cs.haz = hazardClearance(near, px, py,
                                         hazardHalfFor(c.mode, c.mini),
                                         c.mode, c.mini);
                cs.mode = c.mode;
                cs.goal = (uint8_t)(c.frame == 0 && px >= goalX ? 1 : 0);
                clearKids.push_back(cs);
            }
            g_clear.addLayer(clearKids);
        }

        // ---- phase 2p: dedupe in parallel shards (big layers) ---------------
        // Same result as the serial loop below, built in three steps:
        //   1. each shard walks the ordinals ascending and, for ITS keys only,
        //      replays emit()'s hi/lo hysteresis (a per-key sequential scan --
        //      exactness needs order only WITHIN a key, and a key lives in one
        //      shard). Slot-allocation events (first sight of a key, first
        //      hi/lo split) are marked per ordinal -- race-free, one owner.
        //   2. a serial scan over the ordinals materialises the slots in the
        //      same order the serial emit() would have pushed them.
        //   3. the goal is the roomiest ACCEPTED state that reached it
        //      (rules.reached), the smallest ordinal breaking ties -- the state
        //      the serial loop would have ended up holding.
        const size_t qfBefore = nxt.size();   // --qfoldwatch: this group's survivors start here
        const bool parDedupe = pool && !dbgHere && g_threads > 1
                               && kids.size() >= 4096;
        if (parDedupe) {
            const size_t nsh = (size_t)g_threads;
            if (shards.size() < nsh) shards.resize(nsh);
            if (evKind.size() < kids.size()) {
                evKind.resize(kids.size(), 0);
                evRec.resize(kids.size(), 0);
            }
            pool->parallelTasks(nsh, [&](size_t si) {
                ShardMap& sm = shards[si];
                sm.clear();
                sm.ensure(kids.size() / nsh + 64);
                for (uint32_t i = 0; i < (uint32_t)kids.size(); ++i) {
                    if (kidFlag[i] != 2) continue;
                    const uint64_t k = kidKeys[i];
                    if ((size_t)((k * 0x9E3779B97F4A7C15ull) >> 32) % nsh != si)
                        continue;
                    const float vy = kids[i].s.vy;
                    uint32_t& ri = sm.at(k);
                    bool accepted = false;
                    if (ri == kNone) {
                        ri = (uint32_t)sm.recs.size();
                        sm.recs.push_back(KeyRec{k, i, kNone, i, i, vy, vy, 0});
                        evKind[i] = (uint8_t)(1 + si);
                        evRec[i] = ri;
                        accepted = true;
                    } else {
                        KeyRec& r = sm.recs[ri];
                        // the comparisons mirror emit() exactly, including the
                        // float->double promotion of `vy + 1e-9`
                        if ((double)vy > (double)r.hiVy + 1e-9) {
                            if (r.bOrd == kNone) {
                                r.bOrd = i;
                                r.bWasHi = 1;
                                evKind[i] = (uint8_t)(1 + si);
                                evRec[i] = ri;
                            }
                            r.hiIdx = i;
                            r.hiVy = vy;
                            accepted = true;
                        } else if ((double)vy < (double)r.loVy - 1e-9) {
                            if (r.bOrd == kNone) {
                                r.bOrd = i;
                                r.bWasHi = 0;
                                evKind[i] = (uint8_t)(1 + si);
                                evRec[i] = ri;
                            }
                            r.loIdx = i;
                            r.loVy = vy;
                            accepted = true;
                        }
                    }
                    // Same test as the serial path (rules.reached): keep the
                    // roomiest goal state this shard saw rather than its first.
                    // `goalTight` starts at 0xffff, so with the score off the
                    // first one wins and nothing after it can tie-break past it.
                    // Asked when the layer is already solved too: the serial path
                    // replaces a goal with a roomier one, and the merge below
                    // decides against the one held. This test used to read x
                    // alone, and only while unsolved -- no frame, no coins -- so a
                    // big layer could take a goal the serial path would refuse.
                    if (accepted && rules.reached(kids[i].s)
                        && (sm.goalOrd == kNone
                            || rules.prefer(kids[i].s.tight, sm.goalTight))) {
                        sm.goalOrd = i;
                        sm.goalTight = kids[i].s.tight;
                    }
                }
            });
            // counters (compact array, trivial serial pass)
            for (size_t i = 0; i < kids.size(); ++i) {
                if (!kidFlag[i]) continue;
                ++nBorn;
                if (kidFlag[i] == 1) ++nDied;
            }
            // materialise nxt in the serial loop's slot order
            for (uint32_t i = 0; i < (uint32_t)kids.size(); ++i) {
                if (!evKind[i]) continue;
                ShardMap& sm = shards[(size_t)evKind[i] - 1];
                const KeyRec& r = sm.recs[evRec[i]];
                evKind[i] = 0;   // leave the array clean for the next group
                uint32_t src;
                if (r.bOrd == kNone) src = r.hiIdx;   // never split: hi == lo
                else if (i == r.aOrd) src = r.bWasHi ? r.loIdx : r.hiIdx;
                else src = r.bWasHi ? r.hiIdx : r.loIdx;
                const State& from = cur[gidx[src >> 1]];
                const uint8_t action = (uint8_t)(src & 1);
                arena.push_back(Node{from.parent | ((uint32_t)action << 31)});
                if (checkWanted) arenaMode.push_back(kids[src].s.mode);
                nxt.push_back(kids[src].s);
                nxt.back().parent = (uint32_t)(arena.size() - 1);
                nxt.back().action = action;
            }
            {
                // Across shards, the same rule as within one: roomiest first,
                // the smaller ordinal breaking ties. With the score off every
                // tight is 0, no candidate is ever "roomier", and this reduces
                // to min(goalOrd). Against a goal already held, the serial
                // path's rule: only a roomier one replaces it.
                uint32_t g = kNone;
                uint16_t gt = 0xffff;
                for (size_t si = 0; si < nsh; ++si) {
                    const uint32_t o = shards[si].goalOrd;
                    if (o == kNone) continue;
                    const uint16_t ot = shards[si].goalTight;
                    if (g == kNone || ot < gt || (ot == gt && o < g)) {
                        g = o;
                        gt = ot;
                    }
                }
                if (g != kNone && (!solved || rules.prefer(gt, goalState.tight))) {
                    const State& from = cur[gidx[g >> 1]];
                    const uint8_t action = (uint8_t)(g & 1);
                    arena.push_back(
                        Node{from.parent | ((uint32_t)action << 31)});
                    if (checkWanted) arenaMode.push_back(kids[g].s.mode);
                    goalState = kids[g].s;
                    goalState.parent = (uint32_t)(arena.size() - 1);
                    goalState.action = action;
                    solved = true;
                }
            }
            if (qfHere) qfoldAccount(qfBefore);
            continue;   // next speed group -- skips the serial phase 2
        }
        // ---- phase 2: dedupe and record (serial, in order) ------------------
        for (size_t i = 0; i < kids.size(); ++i) {
            const Child& kid = kids[i];
            if (!kid.valid) continue;
            const State& s = cur[gidx[i >> 1]];
            const int input = (int)(i & 1);
            const State& c = kid.s;
            const bool dead = kid.dead != 0;
            ++nBorn;
            if (dead) ++nDied;
            {
                if (dbgHere) {
                    // name the killer. "dead=1" alone never says whether the
                    // model is over-killing or under-killing (see g_deadWhy).
                    char why[128] = "";
                    if (dead) {
                        if (kid.obj)
                            std::snprintf(why, sizeof why,
                                          " %s id=%d type=%d (%.0f,%.0f) %gx%g",
                                          kid.why, kid.obj->id,
                                          (int)kid.obj->type, kid.obj->cx,
                                          kid.obj->cy, kid.obj->hw * 2,
                                          kid.obj->hh * 2);
                        else
                            std::snprintf(why, sizeof why, " %s", kid.why);
                    }
                    // The second body, when there is one. Without it a dual
                    // layer prints two parents with the SAME (y, vy) and no way
                    // to tell what separated them -- which is how lv20's
                    // t=17,115 layer reads: four lines, two distinct children,
                    // nothing on the line that explains the split.
                    // `held` is on every line for the same reason: GD runs
                    // buttons AFTER the update, so a wave's direction on this
                    // tick follows the PREVIOUS tick's button, and two parents
                    // differing only in `held` step apart under the same input.
                    char two[160] = "";
                    if (s.dual)
                        std::snprintf(two, sizeof two,
                                      " | p2 (%.3f,%.3f)->(%.3f,%.3f) flip2=%d "
                                      "g2=%d free=%d",
                                      (double)s.y2, (double)s.vy2,
                                      (double)c.y2, (double)c.vy2,
                                      (int)c.flip2, (int)c.grounded2,
                                      (int)c.freeHalf);
                    std::printf("dbg t=%lld in=%d: (%.3f,%.3f)->(%.3f,%.3f) "
                                "x=%.2f mode=%d flip=%d g=%d held=%d dead=%d%s%s\n",
                                t, input, (double)s.y, (double)s.vy, (double)c.y,
                                (double)c.vy, (double)c.xAbs, (int)c.mode,
                                (int)c.flip, (int)c.grounded, (int)s.held,
                                (int)dead, why, two);
                }
                if (!dead) emit(c, s, (uint8_t)input, kid.key);
            }
        }
        if (qfHere) qfoldAccount(qfBefore);
        }   // speed group
        // ---- SAFE BANDS ------------------------------------------------------
        // How wide the frontier is at this tick, written out for the driver.
        //
        // The driver's anchor policy is "just before the death, then back off
        // geometrically", and that is what breaks on a deep wall: it can only
        // crawl backwards from the wall, so when a stretch of the route becomes
        // invalid it never gets far enough back to redo it. Measured on lv16
        // (60 iterations, oriented rule): the loop reached x=19,174 once and
        // then fell to 13,310 and stayed there; on lv19 it bounced 29,024 <->
        // 27,319 three times. Both are the same shape.
        // A tick where the frontier is WIDE is a tick where many different
        // states survive -- i.e. a place where the level does not care much how
        // you arrived. Those are the places to cut a level into segments, and
        // the DP already knows them; it just never told anyone.
        // Cheap: two words per layer, flushed once at the end.
        if (!g_bandPath.empty()) {
            float ylo = 1e9f, yhi = -1e9f;
            double xr = 0;
            // semantic classes: mode/mini/dual/speed. Small in practice (a
            // handful per layer), so a flat vector beats a map.
            struct Cls { uint32_t k; int n; float lo, hi; };
            std::vector<Cls> cs;
            for (const State& s : nxt) {
                ylo = std::min(ylo, s.y);
                yhi = std::max(yhi, s.y);
                xr = std::max(xr, (double)s.xAbs);
                const uint32_t k = ((uint32_t)s.mode << 24)
                                   ^ ((uint32_t)s.mini << 20)
                                   ^ ((uint32_t)s.dual << 16)
                                   ^ (uint32_t)std::lround(s.dx * 1000.0);
                size_t j = 0;
                for (; j < cs.size(); ++j) if (cs[j].k == k) break;
                if (j == cs.size()) cs.push_back({k, 0, 1e9f, -1e9f});
                cs[j].n++;
                cs[j].lo = std::min(cs[j].lo, s.y);
                cs[j].hi = std::max(cs[j].hi, s.y);
            }
            if (!nxt.empty()) {
                BandRow b;
                b.t = t; b.alive = (int)nxt.size();
                b.ylo = ylo; b.yhi = yhi; b.x = (float)xr;
                // merged this layer = children born - children that died -
                // survivors (what the dedupe folded away)
                b.merged = (int)std::max<long long>(
                    0, nBorn - nDied - (long long)nxt.size());
                char buf[64];
                for (const Cls& c : cs) {
                    std::snprintf(buf, sizeof buf, "%u.%u.%u.%u:%d:%.0f:%.0f",
                                  c.k >> 24, (c.k >> 20) & 0xf,
                                  (c.k >> 16) & 0xf, c.k & 0xffff, c.n,
                                  (double)c.lo, (double)c.hi);
                    if (!b.cls.empty()) b.cls += ';';
                    b.cls += buf;
                }
                g_bands.push_back(std::move(b));
            }
        }
        // Say it the FIRST time a touch trigger fires anywhere in the frontier.
        // Without this the only symptom of "the search never found the box" is
        // that the wall does not move, which is exactly the symptom of a dozen
        // other things (see the three sessions spent on x=28,075).
        if (!g_touch.empty()) {
            TouchMask m{};
            for (const State& s : nxt) m |= s.trig;
            if (m & ~g_trigReported) {
                for (size_t b = 0; b < g_touch.size(); ++b)
                    if ((m & ~g_trigReported) & touchBit(b))
                        std::printf("triggers: box (%.0f,%.0f) entered at "
                                    "t=%lld\n", g_touch[b].cx, g_touch[b].cy, t);
                g_trigReported |= m;
            }
        }
        ppMark(2);
        // ---- stride cap, PER CLASS ------------------------------------------
        // A "class" is what makes two states play a different game: mode, size,
        // gravity, dual, speed. The cap used to be one stride over the whole
        // layer, which quietly starves any class that expands less per tick --
        // and one does: an AIRBORNE CUBE gets one child (its input is moot)
        // while a ship gets two. So a cube branch's share halves every layer
        // once the cap binds, no matter how healthy it is.
        // Measured on lv18 from t=14,200: the branch that takes the RegularSize
        // portal at x=20,386 (the one route that survives -- it reaches
        // x=27,356 while the mini one dies at 24,716) is 416 states at
        // t=15,000 and ZERO at t=15,500 with cap 2,000, while the same run at
        // cap 40,000 still has 2,945 of them. It was not being out-competed on
        // merit; it was being out-multiplied.
        // Max-min fair instead: a small class keeps everything it has, and the
        // big ones split what is left. Kept states stay in their original order
        // so the layer is otherwise unchanged.
        // ---- reference watch, part 3: is it still there, and if not, why ----
        // Checked twice, on purpose: once here, with the layer complete and the
        // cap not yet applied, and once after the cap. Only the pair can tell
        // "the cap evicted it" from "it never got into the layer", and those
        // two have nothing to do with each other.
        bool refPreCap = false;
        State refCarried{};     // the tracked state, captured BEFORE the cap
        if (g_refWatch && g_refLostAt < 0) {
            const char* gate = nullptr;
            char detail[256];
            detail[0] = '\0';
            const auto it = g_refRows.find(t);
            // Which child IS the reference's next state: the one that matches
            // the trace. Both are examined, so a dead one can still be named.
            int want = -1;
            if (it != g_refRows.end() && it->second.act >= 0
                && it->second.act < 2 && g_refKidFate[it->second.act] >= 1) {
                // The reference recorded which input stepped this tick. Read it
                // rather than infer it: on a swing's press tick the two children
                // are identical in everything the trace shows.
                want = it->second.act;
            } else {
                for (int a = 0; a < 2 && it != g_refRows.end(); ++a)
                    if (g_refKidFate[a] >= 1 && refMatches(g_refKidState[a],
                                                           it->second))
                        want = a;
            }
            if (g_refParent < 0) {
                gate = "parent-gone";
            } else if (it == g_refRows.end()) {
                gate = "trace-ended";
            } else if (want < 0) {
                // Neither child is the reference's next state. The search's
                // carrier and the replay have parted company -- a difference in
                // stepping or in context, not a pruning decision, and it must
                // not be reported as one.
                gate = "cannot-reproduce";
                snprintf(detail, sizeof(detail),
                         " ref y=%.4f vy=%.4f | kid0 y=%.4f vy=%.4f f=%d"
                         " | kid1 y=%.4f vy=%.4f f=%d",
                         it->second.y, it->second.vy,
                         (double)g_refKidState[0].y, (double)g_refKidState[0].vy,
                         g_refKidFate[0],
                         (double)g_refKidState[1].y, (double)g_refKidState[1].vy,
                         g_refKidFate[1]);
            } else if (g_refKidFate[want] == 2
                       && !refMatches(g_refKidState[want], it->second)) {
                // The reference says this input stepped the tick, and the search
                // stepping the same state with the same input produced something
                // else. That is a transition difference, not a pruning decision.
                gate = "cannot-reproduce";
                snprintf(detail, sizeof(detail),
                         " act=%d ref y=%.4f vy=%.4f flip=%d fr=%d | kid y=%.4f "
                         "vy=%.4f flip=%d fr=%d", want, it->second.y,
                         it->second.vy, it->second.flip, it->second.frame,
                         (double)g_refKidState[want].y,
                         (double)g_refKidState[want].vy,
                         (int)g_refKidState[want].flip,
                         (int)g_refKidState[want].frame);
            } else if (g_refKidFate[want] == 1) {
                gate = "kill";
                snprintf(detail, sizeof(detail), " rule=%s action=%d",
                         *g_refKidWhy[want] ? g_refKidWhy[want] : "(unnamed)",
                         want);
            } else {
                const int idx = refFindExact(nxt, g_refKidState[want]);
                if (idx >= 0) {
                    g_refParent = idx;      // carried, untouched
                    refPreCap = true;
                } else {
                    // Merged away. The watch stops here rather than adopting the
                    // survivor and carrying on: past a merge the carrier is no
                    // longer on the reference's trajectory, so the next layer's
                    // children cannot be matched against the trace and every
                    // line after it would be about a different path.
                    //
                    // What IS recorded is how far the kept state is, because
                    // that is the whole question about a merge: 0.19 px/tick of
                    // vy is a state the cell may fairly stand in for, and a
                    // larger gap is a passage being represented by something
                    // that cannot make it.
                    gate = "dedupe";
                    const int alt = refNearestInCell(nxt, g_refKidKey[want],
                                                     g_refKidState[want],
                                                     [t, &rules](const State& st) {
                                                         return modelKeyOf(st, (long long)t) ^ rules.keyBits(st);
                                                     });
                    int n = 0;
                    for (const State& s : nxt)
                        if ((modelKeyOf(s, (long long)t) ^ rules.keyBits(s)) == g_refKidKey[want]) ++n;
                    // A merge whose survivor differs from the reference only in WHEN
                    // boxes fired (fireB / trigT) -- a box whose chain moves nothing,
                    // entered a tick apart -- is followed on the survivor instead of
                    // ending the watch. Printed, so the reader knows the carrier is no
                    // longer the reference to the bit (SubZero 4003's toggle block:
                    // the held and the released entry merge, and the watch stopped
                    // 400 ticks before the question it was asked).
                    bool adopted = false;
                    // Every survivor in the cell, not just the nearest: the nearest in (y, vy)
                    // can be one from another group (a different trigger mask), which is never
                    // the reference's merge partner.
                    for (size_t si = 0; si < nxt.size() && !adopted; ++si) {
                        if ((modelKeyOf(nxt[si], (long long)t) ^ rules.keyBits(nxt[si])) != g_refKidKey[want]) continue;
                        State a = nxt[si], b = g_refKidState[want];
                        // Which of the overlooked fields actually differ, by name.
                        std::string diff;
                        if (a.action != b.action) diff += " action";
                        if (std::memcmp(a.fireB, b.fireB, sizeof(a.fireB)) != 0) diff += " fireB";
                        if (a.trigT != b.trigT) diff += " trigT";
                        if (a.tight != b.tight) diff += " tight";
                        if (a.rot != b.rot || a.rotNeg != b.rotNeg || a.rotStep != b.rotStep
                            || a.rotBoost != b.rotBoost)
                            diff += " rot";
                        if (a.groundUid != b.groundUid) diff += " groundUid";
                        a.parent = b.parent = 0;
                        a.action = b.action = 0;
                        std::memcpy(a.fireB, b.fireB, sizeof(a.fireB));
                        a.trigT = b.trigT;
                        a.tight = b.tight;
                        // ...and the sprite angle (rot / rotNeg / rotStep), which the key
                        // leaves out too; named when it is the difference.
                        const bool rotDiff = a.rot != b.rot || a.rotNeg != b.rotNeg
                                             || a.rotStep != b.rotStep || a.rotBoost != b.rotBoost;
                        a.rot = b.rot; a.rotNeg = b.rotNeg; a.rotStep = b.rotStep;
                        a.rotBoost = b.rotBoost;
                        // ...and --stickseam's tie (groundUid), which the key leaves out.
                        const bool tieDiff = a.groundUid != b.groundUid;
                        a.groundUid = b.groundUid;
                        (void)rotDiff; (void)tieDiff;
                        // --refadoptmem: ...and the press buffer and the ring memory, which the key
                        // leaves out as well (triggers.hpp, g_refAdoptMem).
                        if (g_refAdoptMem) {
                            if (a.jumpBuf != b.jumpBuf) diff += " jumpBuf";
                            if (a.usedOrb != b.usedOrb || a.usedOrb2 != b.usedOrb2
                                || a.usedOrbOld != b.usedOrbOld || a.usedOrbOld2 != b.usedOrbOld2
                                || std::memcmp(a.usedOrbHist, b.usedOrbHist, sizeof(a.usedOrbHist)) != 0
                                || std::memcmp(a.usedOrbHist2, b.usedOrbHist2, sizeof(a.usedOrbHist2)) != 0)
                                diff += " usedOrb";
                            a.jumpBuf = b.jumpBuf;
                            a.jumpBuf2 = b.jumpBuf2;   // --dualjumpbuf
                            a.usedOrb = b.usedOrb; a.usedOrb2 = b.usedOrb2;
                            a.usedOrbOld = b.usedOrbOld; a.usedOrbOld2 = b.usedOrbOld2;
                            std::memcpy(a.usedOrbHist, b.usedOrbHist, sizeof(a.usedOrbHist));
                            std::memcpy(a.usedOrbHist2, b.usedOrbHist2, sizeof(a.usedOrbHist2));
                        }
                        if (std::memcmp(&a, &b, sizeof(State)) == 0 && !g_refAdopt) {
                            std::printf("refwatch: merge t=%lld the survivor differs only in:%s "
                                        "(--refadopt would carry on with it)\n", t, diff.c_str());
                        } else if (std::memcmp(&a, &b, sizeof(State)) == 0) {
                            std::printf("refwatch: ADOPT t=%lld a SUBSTITUTE from here, differing in:%s\n",
                                        t, diff.c_str());
                            if (g_refAdoptT < 0) g_refAdoptT = t;
                            for (const char* fld : {"action", "fireB", "trigT", "tight", "rot", "groundUid",
                                                    "jumpBuf", "usedOrb"})
                                if (diff.find(std::string(" ") + fld) != std::string::npos
                                    && g_refAdoptFields.find(fld) == std::string::npos)
                                    g_refAdoptFields += std::string(" ") + fld;
                            g_refParent = (int)si;
                            refPreCap = true;
                            gate = nullptr;
                            adopted = true;
                        } else {
                            const unsigned char* pa = reinterpret_cast<const unsigned char*>(&a);
                            const unsigned char* pb = reinterpret_cast<const unsigned char*>(&b);
                            std::printf("refwatch: cell-mate %zu trig 0x%llx/0x%llx differs at:", si,
                                        (unsigned long long)nxt[si].trig.word(0),
                                        (unsigned long long)g_refKidState[want].trig.word(0));
                            int nd = 0;
                            for (size_t o = 0; o < sizeof(State) && nd < 16; ++o)
                                if (pa[o] != pb[o]) { std::printf(" %zu", o); ++nd; }
                            std::printf("\n");
                        }
                    }
                    if (adopted) {
                    } else if (alt < 0) {
                        snprintf(detail, sizeof(detail), " cell-gone");
                    } else {
                        g_refDriftY = std::fabs((double)nxt[(size_t)alt].y
                                                - (double)g_refKidState[want].y);
                        g_refDriftVy = std::fabs((double)nxt[(size_t)alt].vy
                                                 - (double)g_refKidState[want].vy);
                        snprintf(detail, sizeof(detail),
                                 " cell=%d action=%d ref y=%.4f vy=%.4f "
                                 "kept y=%.4f vy=%.4f", n, want,
                                 (double)g_refKidState[want].y,
                                 (double)g_refKidState[want].vy,
                                 (double)nxt[(size_t)alt].y,
                                 (double)nxt[(size_t)alt].vy);
                    }
                }
            }
            if (refPreCap) refCarried = nxt[(size_t)g_refParent];
            if (gate) {
                std::printf("refwatch: LOST t=%lld gate=%s%s "
                            "dy=%.3f dvy=%.3f alive=%zu\n",
                            t, gate, detail, g_refDriftY,
                            g_refDriftVy, nxt.size());
                g_refLostAt = t;
            }
        }
        maxAlive = std::max(maxAlive, nxt.size());
        const bool censusHere = g_searchCensus > 0 && ((t - t0) % g_searchCensus) == 0;
        if (g_searchCensus > 0) census.layer(nxt, (long long)t, curDxF, nBorn, nDied, censusHere);
        // --capmap: this layer's cap, from the leader x of the layer before it.
        const size_t capNow = capAtX(bestX);
        g_outcome.workStates += (long long)std::min(nxt.size(), capNow);
        const size_t qfPre = nxt.size();   // --qfoldwatch: the frontier before the alive cap
        // --heldcellcap: the layer's cells, keyed without the button (held, jumpBuf), each named by
        // its first state in layer order -- the population the water-fill samples -- and the cell
        // of every state. Built only for a layer over the cap; a layer whose CELLS fit is kept whole.
        std::vector<uint32_t> cellPool;
        std::vector<uint32_t> cellOf;
        bool overCap = nxt.size() > capNow;
        if (overCap && g_heldCellCap && g_heldAll) {
            // The keys on the pool's threads (the layer is thousands of states on every capped
            // layer, and keyOf is the costly part), then (key, index) sorted: a cell's first
            // state is its lowest index.
            const size_t n = nxt.size();
            std::vector<std::pair<uint64_t, uint32_t>> ck(n);
            auto keyAt = [&](size_t i) {
                State s = nxt[i];
                s.held = 0;
                s.jumpBuf = 0;
                ck[i] = {modelKeyOf(s, (long long)t) ^ rules.keyBits(s), (uint32_t)i};
            };
            if (pool && n >= 512) pool->parallelFor(n, keyAt);
            else for (size_t i = 0; i < n; ++i) keyAt(i);
            std::sort(ck.begin(), ck.end());
            cellOf.resize(n);
            for (size_t j = 0; j < n; ++j) {
                const uint32_t rep = (j > 0 && ck[j].first == ck[j - 1].first)
                                         ? cellOf[ck[j - 1].second] : ck[j].second;
                cellOf[ck[j].second] = rep;
                if (rep == ck[j].second) cellPool.push_back(rep);
            }
            std::sort(cellPool.begin(), cellPool.end());
            overCap = cellPool.size() > capNow;
            if (overCap) ++cellCuts;
            else ++cellSpared;
        }
        const bool cellCap = !cellOf.empty();
        if (overCap) {
            ++capHits;
            if (!cellCap) capDropped += (long long)(nxt.size() - capNow);
            if (!g_bandPath.empty() && !g_bands.empty() && g_bands.back().t == t)
                g_bands.back().capdrop = (int)((cellCap ? cellPool.size() : nxt.size()) - capNow);
            // The flight band as a class of its own, and --coinwin's cell (rules.bandClass,
            // rules.winCell).
            const bool bandClass = rules.bandClass();
            auto coinWinCell = [&rules](const State& s) -> uint64_t { return rules.winCell(s); };
            auto classOf = [bandClass, &coinWinCell, &rules](const State& s) {
                return coinWinCell(s) ^ ((uint64_t)s.mode << 32) ^ ((uint64_t)s.mini << 24)
                       ^ ((uint64_t)s.flip << 16) ^ ((uint64_t)s.dual << 8)
                       // the route's own class (CurrentRules::classBits: the coins taken)
                       ^ rules.classBits(s)
                       // ...and the FLIGHT BAND. Every ship state is otherwise
                       // one class, so the water-fill samples the whole layer at
                       // one stride -- and a route that has to thread many
                       // saturated layers as a minority survives that with
                       // probability (kept/born)^n. lv1's coin corridor is
                       // exactly such a route, and it is what this is for: at
                       // cap 16,000 the frontier at t=19,000 is y=[113,367]
                       // while the reference route is at 457, so the search
                       // reports the coin unreachable even though the model
                       // replays the human's own route through it to 0.00 px.
                       // At cap 200,000 the frontier is y=[435,495] and it
                       // solves -- so it was never physics, only thinning.
                       // The band is the right axis rather than a y grid (tried
                       // first, floor(y/60)): it is what a BRANCH actually is
                       // here. lv1's corridor is entered through a portal at
                       // cy=405 that a low route never touches, and firing it is
                       // what rewrites bandCeil; keyOf already treats two bands
                       // as two worlds. The y grid recovered lv1 too but cost
                       // capHits on levels that have no branch at all (lv19
                       // +30%, lv14 +13%, lv18 +10%), while the band left
                       // lv14/17/18/19 identical to the digit.
                       // (Only where the band clamps -- search_key.hpp bandClamps.)
                       ^ ((bandClass && bandClamps(s)) ? ((uint64_t)(uint32_t)(int32_t)
                              std::lround((double)s.bandCeil) << 48) : 0)
                       // ...and which ACTIVATORS have fired (g_actMask, empty
                       // without --activators). SubZero 4003: the one lineage that
                       // pressed the toggle block, and so has the ring that leads
                       // to the second coin, was evicted by this cap at t=8,288
                       // (refwatch on a human route the model replays to 0 px).
                       ^ (bandClass ? (s.trig & g_actMask).word(0) * 0x94D049BB133111EBull
                                    : 0)
                       ^ (uint64_t)(uint32_t)std::lround(s.dx * 1000.0);
            };
            std::vector<uint64_t> cls;
            std::vector<std::vector<uint32_t>> byCls;
            const size_t popN = cellCap ? cellPool.size() : nxt.size();
            for (size_t p = 0; p < popN; ++p) {
                const uint32_t i = cellCap ? cellPool[p] : (uint32_t)p;
                const uint64_t k = classOf(nxt[i]);
                size_t j = 0;
                for (; j < cls.size(); ++j) if (cls[j] == k) break;
                if (j == cls.size()) { cls.push_back(k); byCls.push_back({}); }
                byCls[j].push_back(i);
            }
            // water-fill: smallest class first, each takes at most an equal
            // share of what is still free
            std::vector<size_t> ord(byCls.size());
            for (size_t i = 0; i < ord.size(); ++i) ord[i] = i;
            std::stable_sort(ord.begin(), ord.end(), [&](size_t a, size_t b) {
                return byCls[a].size() < byCls[b].size();
            });
            std::vector<uint32_t> keepIdx;
            keepIdx.reserve(capNow);
            size_t left = capNow;
            for (size_t n = 0; n < ord.size(); ++n) {
                const std::vector<uint32_t>& v = byCls[ord[n]];
                const size_t fair = left / (ord.size() - n);
                const size_t take = std::min(v.size(), fair);
                if (take == v.size()) {
                    keepIdx.insert(keepIdx.end(), v.begin(), v.end());
                } else if (g_capEnvelope && take >= 3) {
                    // --capenvelope: the class's two extremes, then the stride over the rest.
                    auto lower = [&](uint32_t a, uint32_t b) {
                        if (nxt[a].y != nxt[b].y) return nxt[a].y < nxt[b].y;
                        if (nxt[a].vy != nxt[b].vy) return nxt[a].vy < nxt[b].vy;
                        return a < b;
                    };
                    size_t lo = 0, hi = 0;
                    for (size_t k = 1; k < v.size(); ++k) {
                        if (lower(v[k], v[lo])) lo = k;
                        if (lower(v[hi], v[k])) hi = k;
                    }
                    keepIdx.push_back(v[lo]);
                    keepIdx.push_back(v[hi]);
                    std::vector<uint32_t> rest;
                    rest.reserve(v.size() - 2);
                    for (size_t k = 0; k < v.size(); ++k)
                        if (k != lo && k != hi) rest.push_back(v[k]);
                    const double stride = (double)rest.size() / (double)(take - 2);
                    for (double i = 0; (size_t)i < rest.size(); i += stride)
                        keepIdx.push_back(rest[(size_t)i]);
                } else if (take > 0) {
                    const double stride = (double)v.size() / (double)take;
                    for (double i = 0; (size_t)i < v.size() && take > 0; i += stride)
                        keepIdx.push_back(v[(size_t)i]);
                }
                left -= std::min(left, take);
            }
            // --heldcellcap: every state of a sampled cell comes back with it, in layer order.
            auto withTwins = [&](std::vector<uint32_t>& idx) {
                if (!cellCap) return;
                std::vector<uint8_t> pick(nxt.size(), 0);
                for (uint32_t i : idx) pick[i] = 1;
                idx.clear();
                if (g_heldCellTwins <= 0) {
                    for (uint32_t i = 0; i < (uint32_t)nxt.size(); ++i)
                        if (pick[cellOf[i]]) idx.push_back(i);
                    return;
                }
                // --heldcelltwins N: the cell's first state, then the states whose held differs
                // from it, then the rest, until the cell has N.
                // TRIED AND REMOVED (2026-09-26): a bound on the layer's total as well
                // (--heldcelltotal P, at most (100 + P)% of the cap, the twins taken at an even
                // stride). With N = 3 it did not bring lv16's seconds under 2x (P=50 2.63x, P=25
                // 2.48x, against 2.6x without it) and it broke the ladder on lv14 (kids 4.2x at
                // P=50, 39x at P=25) and lv11 (13x, not solved, against 0.55x and SOLVED at N = 3
                // alone): official 22 from t=0, the loop's base argv.
                std::vector<uint8_t> kept(nxt.size(), 0);
                std::vector<int> nOf(nxt.size(), 0);
                for (int pass = 0; pass < 3; ++pass)
                    for (uint32_t i = 0; i < (uint32_t)nxt.size(); ++i) {
                        const uint32_t r = cellOf[i];
                        if (!pick[r] || kept[i] || nOf[r] >= g_heldCellTwins) continue;
                        if (pass == 0 && i != r) continue;
                        if (pass == 1 && nxt[i].held == nxt[r].held) continue;
                        kept[i] = 1;
                        ++nOf[r];
                        idx.push_back(i);
                    }
                std::sort(idx.begin(), idx.end());
            };
            const size_t cellsKept = keepIdx.size();
            withTwins(keepIdx);
            if (cellCap) {
                capDropped += (long long)(nxt.size() - keepIdx.size());
                cellTwinsKept += (long long)(keepIdx.size() - cellsKept);
            }
            std::sort(keepIdx.begin(), keepIdx.end());
            // --searchcensus: the same water-fill, with each class's stride taken over the
            // class sorted by (y, vy) instead of in layer order. Counted, never kept.
            if (g_searchCensus > 0) census.cap(nxt.size(), capNow, nDied);
            if (censusHere) {
                std::vector<uint32_t> altIdx;
                size_t leftA = capNow;
                for (size_t n = 0; n < ord.size(); ++n) {
                    std::vector<uint32_t> v = byCls[ord[n]];
                    std::sort(v.begin(), v.end(), [&](uint32_t a, uint32_t b) {
                        if (nxt[a].y != nxt[b].y) return nxt[a].y < nxt[b].y;
                        if (nxt[a].vy != nxt[b].vy) return nxt[a].vy < nxt[b].vy;
                        return a < b;
                    });
                    const size_t fair = leftA / (ord.size() - n);
                    const size_t take = std::min(v.size(), fair);
                    if (take == v.size()) {
                        altIdx.insert(altIdx.end(), v.begin(), v.end());
                    } else if (take > 0) {
                        const double stride = (double)v.size() / (double)take;
                        for (double i = 0; (size_t)i < v.size() && take > 0; i += stride)
                            altIdx.push_back(v[(size_t)i]);
                    }
                    leftA -= std::min(leftA, take);
                }
                withTwins(altIdx);
                census.coverage(nxt, keepIdx, altIdx, curDxF);
            }
            std::vector<State> kept;
            kept.reserve(keepIdx.size());
            for (uint32_t i : keepIdx) kept.push_back(nxt[i]);
            nxt.swap(kept);
            // ...and the second half of the pair: it was in the layer before
            // the cap and is not in it now.
            if (g_refWatch && g_refLostAt < 0 && refPreCap) {
                const int idx = refFindExact(nxt, refCarried);
                if (idx < 0) {
                    std::printf("refwatch: LOST t=%lld gate=cap alive=%zu "
                                "cap=%zu\n",
                                t, nxt.size(), capNow);
                    g_refLostAt = t;
                } else {
                    g_refParent = idx;    // the cap reordered the layer
                }
            }
        }
        // --heldcellcap keeps more states than the cap counts: charge what the layer really keeps.
        if (cellCap) g_outcome.workStates += (long long)nxt.size() - (long long)std::min(qfPre, capNow);
        if (g_searchCensus > 0) census.kept(nxt);
        // How many complete routes the final pick is choosing between, and how
        // far back they are actually different (--clearprobe only; reads the
        // arena, changes nothing).
        if (g_clearProbe && solved) measureGoalDiversity(nxt, arena, goalX);
        if (g_rjOn) {
            if ((long long)t == g_rjAfter) {
                const auto rr = g_rjRows.find((long long)t);
                const int oi = rr == g_rjRows.end() ? -1 : refFind(nxt, rr->second);
                g_rjOldNode = oi >= 0 ? nxt[(size_t)oi].parent : 0xffffffffu;
                std::printf("rejoin: the old path is %s the frontier at its death tick %lld\n",
                            oi >= 0 ? "in" : "NOT in", (long long)t);
            }
            const long long tt = (long long)t;
            const int ji = rejoinLayer(nxt, tt, [&](const State& s) {
                if (g_rjOldNode == 0xffffffffu) return false;
                uint32_t n = s.parent;
                for (long long k = tt; k > g_rjAfter; --k) n = arena[n].parent();
                return n == g_rjOldNode;
            }, [&](const State& s, const RefRow& r) {
                return !g_rjFull || (r.haveKey && (modelKeyOf(s, tt) ^ rules.keyBits(s)) == r.key);
            });
            // ...and only onto an old plan that goes on: a trace that ends a tick past the join
            // (the old plan was a PARTIAL that died there) would turn a dead end into a SOLVED.
            const bool rjGoesOn = !g_rjRows.empty() && g_rjRows.rbegin()->first - tt >= kRjMinTail;
            // ...and, with --coins, not where the route still owes a press or a coin
            // (rules.rejoinOk).
            const State* rjS = ji >= 0 ? &nxt[(size_t)ji] : nullptr;
            const bool rjCoins = rules.rejoinOk(rjS);
            if (g_rjUse && ji >= 0 && !solved && rjGoesOn && rjCoins) {
                solved = true;
                goalState = nxt[(size_t)ji];
                planCut = 0;
                g_rjJoinT = tt;
                g_outcome.rejoinT = tt;
                bestT = tt;
                std::printf("rejoin: JOINED the old plan at t=%lld (%lld ticks past its death), "
                            "stopping the search here\n", tt, tt - g_rjAfter);
                break;
            }
        }
        if (qfHere)
            std::printf("qfold: t=%lld frontier=%zu kept=%zu lostq=%lld keys=%lld\n",
                        (long long)t, qfPre, nxt.size(), qfLost, qfKeys);
        // --wavetidy / --wavetoggle: the next layer is stepped tidiest route first, so the first-accepted-wins dedupe
        // of its children keeps the tidier route into a cell. Stable: equal scores keep the order they had.
        // Only in a layer that has a wave in it: elsewhere the order is the one it was, and so is the cap's stride.
        if ((g_waveTidy > 0.0 || g_waveToggle > 0)
            && std::any_of(nxt.begin(), nxt.end(), [](const State& s) { return s.mode == 4; }))
            std::stable_sort(nxt.begin(), nxt.end(),
                             [](const State& a, const State& b) { return a.tight < b.tight; });
        prevAlive = cur.size();
        cur.swap(nxt);
        nBornPrev = nBorn;
        nDiedPrev = nDied;
        // The layer no longer HAS one x (see the speed groups above), so the
        // number everything downstream reports is the frontier's own leader.
        double x = 0.0;
        for (const State& s : cur) x = std::max(x, (double)s.xAbs);
        if (!cur.empty()) { bestT = t; bestX = x; }
        if (cur.empty()) break;
        // horizon reached: take the surviving state with the most clearance
        // (highest |vy| is a bad proxy; just take the first survivor -- they
        // are all alive, and the driver re-anchors from GD anyway)
        // The horizon bounds the PLAN, not the SEARCH. It used to stop the
        // search dead at t0+horizon and emit `cur.front()`, an arbitrary
        // survivor -- and a survivor at the horizon is not the same thing as a
        // survivor that goes anywhere. On lv11 that prefix was a dead end: GD
        // replayed it, died 37 ticks after the plan ran out, and every anchor
        // the driver backed off to (six of them, down to t=1537) was doomed,
        // because they all sat on that one doomed branch. The full search
        // reached x=6,067 on a different branch the whole time.
        // So: keep searching another `horizon` ticks past the cut, and only
        // then take a survivor. The emitted plan is truncated back to the cut
        // below, so what the driver replays is a prefix that is KNOWN to still
        // be alive `horizon` ticks later. Cost is at most 2x the search.
        if (horizon > 0 && !solved && t - t0 >= horizon * 2) {
            solved = true;
            g_outcome.horizonCut = true;   // survived, did not reach the end (SearchOutcome)
            goalState = rules.pick(cur);
            planCut = t0 + horizon;
            break;
        }
        ppMark(3);
        // ---- mark-compact GC ----
        // The arena is append-only and keeps every dead lineage's whole
        // ancestor chain; on lv16 cap=40000 it reached 147.9M nodes (564 MiB)
        // and the process died growing it. The frontier's chains coalesce a
        // few hundred ticks back, so the REACHABLE set is a tiny fraction
        // (measure it with --memstat). Compact whenever the arena doubles
        // past the threshold: amortized O(1) per node. Indices are internal,
        // so the emitted plan is bit-identical with GC on or off.
        if (g_gcNodes > 0 && !solved && arena.size() >= gcNext) {
            std::vector<bool> mark(arena.size(), false);
            mark[0] = true;
            auto markFrom = [&](const std::vector<State>& v) {
                for (const State& s : v)
                    for (uint32_t i = s.parent; i != 0 && !mark[i];
                         i = arena[i].parent())
                        mark[i] = true;
            };
            markFrom(cur);
            markFrom(nxt);   // the PARTIAL path reads nxt after the loop
            size_t keep = 0;
            for (size_t i = 0; i < arena.size(); ++i) keep += mark[i];
            std::vector<uint32_t> remap(arena.size(), 0);
            std::vector<Node> na;
            na.reserve(keep);
            // The mode side vector is indexed by the same numbers, so it is compacted in the
            // same pass or it would describe the nodes the arena used to hold. Nothing that has
            // ALREADY been published can be reached from here -- a published prefix is ticks
            // and levels, never indices (dp/progress.hpp) -- so this only has to keep the live
            // array consistent for the next LCA walk.
            std::vector<uint8_t> nm;
            if (checkWanted) nm.reserve(keep);
            for (size_t i = 0; i < arena.size(); ++i) {
                if (!mark[i]) continue;
                remap[i] = (uint32_t)na.size();
                // a parent always precedes its children in the arena, so
                // remap[parent()] is already final here
                na.push_back(Node{remap[arena[i].parent()]
                                  | ((uint32_t)arena[i].action() << 31)});
                if (checkWanted) nm.push_back(arenaMode[i]);
            }
            const size_t before = arena.size();
            arena.swap(na);
            na = std::vector<Node>();   // release the old allocation NOW
            if (checkWanted) {
                arenaMode.swap(nm);
                nm = std::vector<uint8_t>();
            }
            for (State& s : cur) s.parent = remap[s.parent];
            for (State& s : nxt) s.parent = remap[s.parent];
            if (g_rjOldNode != 0xffffffffu)
                g_rjOldNode = mark[g_rjOldNode] ? remap[g_rjOldNode] : 0xffffffffu;
            gcNext = std::max(g_gcNodes, arena.size() * 2);
            if (lpOn) g_lpc.snaps.clear();   // --ladderprefix: their arena sizes refer to the old numbering
            std::printf("gc t=%lld arena %zu -> %zu nodes (%.1f%% live)\n", t,
                        before, arena.size(),
                        100.0 * (double)arena.size() / (double)before);
            std::fflush(stdout);
        }
        ppMark(4);
        // ---- --ladderprefix: save the loop's state after this layer ----
        if (lpOn && !solved) {
            if (!lpBound && capHits > 0) lpBound = true;
            if (lpBound && g_ladderPrefixEvery > 0 && ((t - t0) % g_ladderPrefixEvery) == 0) {
                LadderPrefixSnap S;
                S.T = t;
                S.cur = cur;
                bool refsOk = true;
                S.objRefs.resize(cur.size() * 12);
                for (size_t i = 0; i < cur.size(); ++i) {
                    const State& s = cur[i];
                    uint64_t* r = &S.objRefs[i * 12];
                    r[0] = model.encodeObj(s.snapObj, refsOk);
                    r[1] = model.encodeObj(s.usedOrb, refsOk);
                    r[2] = model.encodeObj(s.snapObj2, refsOk);
                    r[3] = model.encodeObj(s.usedOrb2, refsOk);
                    for (int k = 0; k < 4; ++k) r[4 + k] = model.encodeObj(s.usedPad[k], refsOk);
                    for (int k = 0; k < 4; ++k) r[8 + k] = model.encodeObj(s.usedPad2[k], refsOk);
                }
                S.arenaSize = arena.size();
                S.arenaModeSize = arenaMode.size();
                S.gcNext = work.gcNext;
                S.nBornPrev = nBornPrev;
                S.nDiedPrev = nDiedPrev;
                S.prevAlive = prevAlive;
                S.bestT = bestT;
                S.bestX = bestX;
                S.goalState = goalState;
                S.solved = solved;
                S.maxAlive = maxAlive;
                S.capDropped = capDropped;
                S.capHits = capHits;
                S.cellCuts = cellCuts;
                S.cellSpared = cellSpared;
                S.cellTwinsKept = cellTwinsKept;
                S.census = census;
                S.twin = twin;
                S.workStates = g_outcome.workStates - lpWork0;
                S.autoFire.resize(g_autoTrig.size());
                for (size_t i = 0; i < g_autoTrig.size(); ++i) {
                    const AutoTrig& A = g_autoTrig[i];
                    S.autoFire[i] = {A.fireT, A.fireX, A.fireSpreadT, A.lockLastX};
                }
                S.rotTrig = g_rotTrig;
                S.carry.assign(4, {});
                S.carryOn.assign(4, 0);
                for (int f = 0; f < 4; ++f)
                    if (model.frameBound(f)) {
                        model.saveWorld(f, S.carry[(size_t)f]);
                        S.carryOn[(size_t)f] = 1;
                    }
                S.gfireGroups = g_gfireGroups;
                S.gfireSpread = g_gfireSpread;
                S.gfireSum = g_gfireSum;
                S.gfireMax = g_gfireMax;
                S.groupXSplitMax = g_groupXSplitMax;
                S.rjJoinT = g_rjJoinT;
                S.rjOldNode = g_rjOldNode;
                S.layerXSize = g_lpc.layerX.size();
                if (refsOk) {   // a pointer outside the level's lists: this layer is not saved
                    if (g_lpc.snaps.size() >= g_ladderPrefixMaxSnaps) g_lpc.snaps.erase(g_lpc.snaps.begin());
                    g_lpc.snaps.push_back(std::move(S));
                    // The byte cap: the oldest saves go first, the newest one stays. Only a save that has grown past
                    // anything the levels measured reaches this; it says so once per ladder.
                    const size_t capBytes = g_lpc.maxMiB * (size_t)1048576;
                    size_t held = 0;
                    for (const LadderPrefixSnap& q : g_lpc.snaps) held += q.bytes();
                    if (held > capBytes && g_lpc.snaps.size() > 1) {
                        size_t dropped = 0;
                        while (held > capBytes && g_lpc.snaps.size() > 1) {
                            held -= g_lpc.snaps.front().bytes();
                            g_lpc.snaps.erase(g_lpc.snaps.begin());
                            ++dropped;
                        }
                        if (!g_lpc.capSaid) {
                            g_lpc.capSaid = true;
                            std::printf("ladderprefix: the saves passed %zu MiB at t=%lld - the %zu oldest dropped "
                                        "(%zu kept, %zu MiB)\n",
                                        g_lpc.maxMiB, t, dropped, g_lpc.snaps.size(), held >> 20);
                        }
                    }
                }
            }
        }
        // ---- memory budget ----
        // Estimate the search's own structures and stop GROWING before the OS
        // stops us (the lv16 run died as exit 255 with zero diagnostics). The
        // emitted plan keeps the horizon guarantee: a prefix that is known to
        // still be alive (t-t0)/2 ticks past its end, i.e. the same contract
        // as the horizon cut, just with a smaller effective horizon.
        if (g_memLimitMiB > 0 && !solved && (t & 127) == 0 && t - t0 > 2) {
            // arenaMode is empty with no subscriber, so this term is 0 and the estimate --
            // and therefore the tick this limit fires on -- is exactly what it was.
            const size_t est = arena.capacity() * sizeof(Node) + arenaMode.capacity()
                               + (cur.capacity() + nxt.capacity()) * sizeof(State)
                               + seen.bucket_count() * 8 + seen.size() * 48;
            if (est > g_memLimitMiB * (size_t)1048576) {
                solved = true;
                goalState = rules.pick(cur);
                planCut = t0 + (t - t0) / 2;
                std::printf("MEMORY_LIMIT: est=%zuMiB arena=%zu alive=%zu "
                            "t=%lld x=%.0f -> plan cut at t=%lld "
                            "(half the searched depth)\n",
                            est >> 20, arena.size(), cur.size(), t, x,
                            planCut);
                break;
            }
        }
        // ---- checkpoints (dp/progress.hpp) ----
        //
        // At a checkpoint layer, publish the lineage of the frontier's FIRST state, cut at this
        // layer, for the caller to fly. The first state because the frontier's order is a
        // function of the search alone, so the same call publishes the same lineage every time
        // -- which state is flown matters less than that the choice never depends on the clock.
        // Every state in `cur` is alive in the model through tick t, so the game killing this
        // lineage at or before t is a disagreement with the model by construction.
        if (checkWanted && !solved && !cur.empty() && isCheckpointLayer(t - t0)) {
            std::vector<uint8_t> plvl, pmode;
            plvl.reserve((size_t)(t - t0));
            pmode.reserve((size_t)(t - t0));
            for (uint32_t i = cur.front().parent; i != 0; i = arena[i].parent()) {
                plvl.push_back(arena[i].action());
                pmode.push_back(arenaMode[i]);
            }
            std::reverse(plvl.begin(), plvl.end());
            std::reverse(pmode.begin(), pmode.end());
            SearchCheckpoints::Point p;
            p.t0 = t0;
            p.tick = t0 + (long long)plvl.size();
            for (const PlanEdge& e :
                 planEdges(plvl, pmode, t0, planPrevHeld(init), init.mode))
                p.edges.emplace_back(e.press, e.level);
            g_check.publish(std::move(p));
        }
        // ---- the caller has stopped caring ----
        // Read every layer: once the game has refuted a checkpoint, every layer after it is paid
        // for and thrown away. One relaxed load -- and read with no subscriber too, because the
        // mod also cancels when its session ends (the player left the level). Read only under
        // `dpcheck`, which is off by default, that cancel did nothing: the search ran on to its
        // end on its detached thread, and the next level's Solve sat queued behind it with the
        // old search's numbers on its HUD (lv16 left mid-search went on for 22 s at 2,000 states
        // while lv1 waited). The CLI never sets the flag, so its output is unchanged.
        if (g_check.cancel.load(std::memory_order_relaxed)) {
            std::printf("CANCELLED: at t=%lld (search abandoned by the caller)\n", t);
            std::fflush(stdout);
            g_outcome.verdict = VerdictCancelled;
            g_outcome.cancelT = t;
            g_outcome.deepT = bestT;
            g_outcome.deepX = bestX;
            return 3;    // distinct from FAILED (1) and from --replay's unreadable plan (2)
        }
        // ...and the ladder that has taken the plain search beside it (the gamble, cliMain):
        // this attempt's answer can no longer be used. Never set outside a ladder.
        if (g_ladderStop.load(std::memory_order_relaxed)) {
            std::printf("STOPPED: at t=%lld (the ladder took the plain search beside it)\n", t);
            std::fflush(stdout);
            g_outcome.verdict = VerdictCancelled;
            g_outcome.cancelT = t;
            g_outcome.deepT = bestT;
            g_outcome.deepX = bestX;
            return 3;
        }
        ppMark(5);
        // Publish where the loop has got to. Every layer, not every 500th: this is what a UI
        // watching from another thread samples, and the printed line below is far too coarse
        // to look alive on screen (see dp/progress.hpp)
        g_progress.layer(t, x, cur.size());
        // "alive=1" tells you the frontier collapsed but not WHERE the survivors
        // are; the y span and mode mix is what says whether the search is stuck
        // on one ledge or spread out. Report more often once it gets thin.
        // also report the tick the frontier actually collapses on -- a periodic
        // sample tells you it happened somewhere in the last 500 ticks, which is
        // not enough to find the cause
        const bool crashed = prevAlive >= 8 && cur.size() * 4 <= prevAlive;
        if ((t % 500) == 0 || crashed
            || (cur.size() <= 64 && (t % 10) == 0)) {
            double ylo = 1e9, yhi = -1e9;
            // cube / ship / ball / ufo / wave / robot / spider / swing.
            // ALL of them: the print used to stop at 4, so a mode portal that
            // never fired was invisible here -- which is exactly how lv19's
            // robot section was planned as a cube for a whole session.
            int nm[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            // ...and how many are MINI. A size portal that the frontier is
            // missing shows up here and nowhere else: on lv18 every state
            // stayed mini past x=20,386 and the level is unplayable that way.
            int nMini = 0;
            for (const State& s : cur) {
                ylo = std::min(ylo, (double)s.y);
                yhi = std::max(yhi, (double)s.y);
                if (s.mode < 8) ++nm[s.mode];
                if (s.mini) ++nMini;
            }
            // ...and the SPEED spread. Since speed is per state the frontier can
            // hold branches that took a speed portal and branches that flew past
            // it, and those are at different x at the same tick -- which is
            // exactly the freedom the old shared timeline could not represent.
            // "x" above is the leader; xlo says how far back the slowest is.
            double xlo = 1e18;
            std::vector<float> spds;
            for (const State& s : cur) {
                xlo = std::min(xlo, model.progress(s).x);
                const float d = (s.dx > 0.f) ? s.dx : curDxF;
                if (std::find(spds.begin(), spds.end(), d) == spds.end())
                    spds.push_back(d);
            }
            std::sort(spds.begin(), spds.end());
            char sp[64] = "";
            for (size_t i = 0, o = 0; i < spds.size() && o + 8 < sizeof sp; ++i)
                o += (size_t)std::snprintf(sp + o, sizeof sp - o, "%s%.2f",
                                           i ? "/" : "", (double)spds[i]);
            std::printf("t=%lld x=%.0f alive=%zu arena=%zu y=[%.0f,%.0f] "
                        "cbswrpg=%d/%d/%d/%d/%d/%d/%d/%d mini=%d dx=%s xlo=%.0f "
                        "born=%lld died=%lld merged=%lld\n",
                        t, x, cur.size(), arena.size(),
                        cur.empty() ? 0.0 : ylo, cur.empty() ? 0.0 : yhi,
                        nm[0], nm[1], nm[2], nm[3], nm[4], nm[5], nm[6], nm[7],
                        nMini, sp,
                        cur.empty() ? 0.0 : xlo, nBornPrev, nDiedPrev,
                        nBornPrev - nDiedPrev - (long long)cur.size());
            if (g_memStat) {
                // 1) nodes still reachable from the live frontier. This is the
                // exact size a mark-compact GC would shrink the arena to, so
                // this one number decides whether a GC is worth building.
                std::vector<bool> mark(arena.size(), false);
                size_t reach = 0;
                for (const State& s : cur)
                    for (uint32_t i = s.parent; i != 0 && !mark[i];
                         i = arena[i].parent()) {
                        mark[i] = true;
                        ++reach;
                    }
                // 2) dual census. The mirror-compression proposal assumes the
                // frontier in a mirror region is a 2-body PRODUCT of the two
                // state sets; but while the pair IS a mirror, p2 is a function
                // of (p1, split axis) and the dedupe key already collapses it.
                // If dual ~= p1cells * axes there is no product to compress;
                // if dual >> p1cells * axes, the product is real.
                size_t nDual = 0, nMirror = 0;
                std::unordered_set<uint64_t> p1All, p1Mir, axes;
                for (const State& s : cur) {
                    if (!s.dual) continue;
                    ++nDual;
                    const bool flying = (s.mode == 1 || s.mode == 3);
                    const double ys = flying ? g_shipYq : g_cubeYq;
                    const double vs = flying ? g_shipVq : g_cubeVq;
                    const uint64_t c1 =
                        ((uint64_t)(uint32_t)(int32_t)std::lround(s.y * ys) << 32)
                        ^ (uint32_t)(int32_t)std::lround(s.vy * vs);
                    p1All.insert(c1);
                    // exact mirror: opposite gravity, opposite vy, both airborne
                    if (s.flip2 != s.flip && !s.grounded && !s.grounded2
                        && std::fabs((double)s.vy + (double)s.vy2) < 0.05) {
                        ++nMirror;
                        p1Mir.insert(c1);
                        // the split axis, 0.5 px bins: y2 = 2*axis - y
                        axes.insert((uint64_t)(int64_t)std::llround(
                            ((double)s.y + (double)s.y2)));
                    }
                }
                const double MiB = 1024.0 * 1024.0;
                std::printf(
                    "memstat t=%lld arena=%zu (%.0f MiB) reach=%zu (%.1f%%) "
                    "state=%zuB cur=%zu/%zu nxtcap=%zu seen=%zu/%zu\n",
                    t, arena.size(),
                    (double)(arena.size() * sizeof(Node)) / MiB, reach,
                    arena.empty() ? 0.0 : 100.0 * (double)reach / (double)arena.size(),
                    sizeof(State), cur.size(), cur.capacity(), nxt.capacity(),
                    seen.size(), seen.bucket_count());
                if (nDual)
                    std::printf(
                        "memstat-dual t=%lld dual=%zu p1cells=%zu product=%.1f "
                        "mirror=%zu (%.0f%%) p1mir=%zu axes=%zu\n",
                        t, nDual, p1All.size(),
                        (double)nDual / (double)p1All.size(), nMirror,
                        100.0 * (double)nMirror / (double)nDual, p1Mir.size(),
                        axes.size());
            }
            std::fflush(stdout);
        }
    }
    // The answer. A goal (or a stop the caller asked for) is Solved; without one, a horizon search
    // still emits its deepest branch (Partial: the driver wants to reach the wall); otherwise there
    // is no plan, and the frontier's last tick and x say where it stuck.
    if (solved) {
        out.kind = SearchResult::Solved;
        out.goal = goalState;
    } else if (horizon > 0 && !nxt.empty()) {
        out.kind = SearchResult::Partial;
        out.goal = rules.pick(nxt);
    } else {
        out.kind = SearchResult::NoPlan;
        // after the final swap the previous layer's states sit in nxt
        out.lastLayer.assign(nxt.begin(), nxt.begin() + (std::ptrdiff_t)std::min<size_t>(nxt.size(), 12));
    }
    out.t = bestT;
    out.x = bestX;
    if (out.kind != SearchResult::NoPlan) planFromArena(arena, out.goal, out.inputs);
    if (lpOn) {   // --ladderprefix: the next attempt may start from this one's saves
        g_lpc.arena.swap(arena);
        g_lpc.arenaMode.swap(arenaMode);
        g_lpc.usable = true;
    }
    return -1;
}

// One solve: everything a command line asks for. cliMain below is this, unless --capladder
// asks for a sequence of them.

// ---- a second search: a plain beam (prototype, --search beam) ------------------------------------
// The search contract's second implementation, for the yardstick to compare the DP against -- not a
// replacement. Each tick: the model fires the autonomous triggers; every state of the frontier is
// stepped on its own (a group of one, as the model's walk does) with each input the model allows;
// the rules credit and kill (afterStep); children the model keys the same are merged, the first one
// kept; and the B with the best rules.score survive (--beam B). No group sharing, no hi/lo pair per
// cell, no cap classes, no GC, no threads. Deterministic: states are stepped in (frame, x, index)
// order and ties keep that order. It fills the same frontier / arena / goal as searchLayers, so
// cliMainOnce rebuilds, walks and writes its plan the same way, and answers in the same shape.
template <class E, class Rules>
inline int searchBeam(E& e, const Rules& rules, SearchResult& out) {
    auto& t0 = e.in.t0;
    auto& horizon = e.in.horizon;
    auto& planCut = e.in.planCut;
    auto& tEnd = e.in.tEnd;
    LayerWork work(e.in.init);   // the frontier starts at the root
    auto& cur = work.cur;
    auto& nxt = work.nxt;
    SearchArena store(e.in.checkWanted);   // this method's states
    auto& arena = store.arena;
    auto& checkWanted = e.in.checkWanted;
    auto& arenaMode = store.arenaMode;
    auto& model = e.in.model;
    auto& bestT = e.outcome.bestT;
    auto& bestX = e.outcome.bestX;
    auto& goalState = e.outcome.goalState;
    auto& solved = e.outcome.solved;
    auto& maxAlive = e.outcome.maxAlive;
    auto& capHits = e.outcome.capHits;
    auto& capDropped = e.outcome.capDropped;
    auto& prevAlive = work.prevAlive;
    auto& curDxF = e.in.curDxF;
    const size_t B = (size_t)std::max(1, g_beamWidth);
    std::printf("search: beam, width %zu\n", B);
    struct Cand {
        State s;          // the child, its parent the PARENT's arena node until it is kept
        uint8_t action;
        double score;
    };
    std::vector<Cand> cand;
    std::vector<size_t> order, rank;
    std::unordered_set<uint64_t> seenKeys;
    // keep one candidate: its arena node, and the state that now owns it
    auto keep = [&](const Cand& c) -> State {
        arena.push_back(Node{c.s.parent | ((uint32_t)c.action << 31)});
        if (checkWanted) arenaMode.push_back(c.s.mode);
        State k = c.s;
        k.parent = (uint32_t)(arena.size() - 1);
        k.action = c.action;
        return k;
    };
    for (long long t = t0 + 1; t <= tEnd && !solved; ++t) {
        nxt.clear();
        cand.clear();
        seenKeys.clear();
        model.fireAutoTriggers(cur, t);
        order.resize(cur.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            const Progress pa = model.progress(cur[a]), pb = model.progress(cur[b]);
            return pa.frame != pb.frame ? pa.frame < pb.frame : pa.x < pb.x;
        });
        long long goalAt = -1;   // index into cand of the goal found this tick
        for (size_t oi : order) {
            const State& s = cur[oi];
            const int frame = (int)s.frame;
            const bool rev = s.rev != 0;
            // a group of one: its window is its own advance, its doors and lock its own
            const double sdx = rev ? -(double)((s.dx > 0.f) ? s.dx : curDxF)
                                   : (double)((s.dx > 0.f) ? s.dx : curDxF);
            const float dxUsed = (float)sdx;
            const double xPrev = (double)s.xAbs;
            const double x = (double)advanceX(s.xAbs, dxUsed);
            const double wLo = std::min(xPrev, x), wHi = std::max(xPrev, x);
            model.fireRotTriggers(frame, rev, xPrev, xPrev, wLo, wHi, t);
            GroupEnv env;
            double pLo = wLo, pHi = wHi;
            p2Widen(pLo, pHi, p2Dx(s), p2Dx(s));   // --p2xoff
            model.prepare(env, frame, s.trig, (int)s.trigT, s.fireB, s.lockOff + (float)sdx, x, xPrev,
                          dxUsed, pLo, pHi, t, GroupOpts{true, true, false});
            const StepCtx K = env.ctx();
            for (int input = 0; input < 2; ++input) {
                if (!model.inputAllowed(s, input, 1, t)) continue;
                Child kid;
                bool dead = false;
                kid.s = model.advance(s, input, K, dead, nullptr);
                model.noteInput(s, kid.s, input);
                model.fireGates(kid.s, s.action, input, K, frame, t, dead);
                rules.afterStep(s, kid, input, K, frame, t, dead);
                if (dead) continue;
                if (!seenKeys.insert(modelKeyOf(kid.s, t) ^ rules.keyBits(kid.s)).second) continue;
                kid.s.parent = s.parent;
                cand.push_back(Cand{kid.s, (uint8_t)input, rules.score(kid.s)});
                if (rules.reached(kid.s)
                    && (goalAt < 0 ? !solved || rules.prefer(kid.s.tight, goalState.tight)
                                   : rules.prefer(kid.s.tight, cand[(size_t)goalAt].s.tight)))
                    goalAt = (long long)cand.size() - 1;
            }
        }
        if (goalAt >= 0) {
            goalState = keep(cand[(size_t)goalAt]);
            solved = true;
        }
        // the B best survive, ties in stepping order
        rank.resize(cand.size());
        for (size_t i = 0; i < rank.size(); ++i) rank[i] = i;
        std::stable_sort(rank.begin(), rank.end(),
                         [&](size_t a, size_t b) { return cand[a].score > cand[b].score; });
        if (cand.size() > maxAlive) maxAlive = cand.size();
        if (cand.size() > B) {
            ++capHits;
            capDropped += (long long)(cand.size() - B);
            rank.resize(B);
        }
        std::sort(rank.begin(), rank.end());   // the frontier in stepping order
        for (size_t i : rank) nxt.push_back(keep(cand[i]));
        prevAlive = cur.size();
        cur.swap(nxt);
        double xl = 0.0;
        for (const State& s : cur) xl = std::max(xl, model.progress(s).x);
        if (!cur.empty()) { bestT = t; bestX = xl; }
        if (cur.empty()) break;
        // the caller's horizon, as searchLayers keeps it: search on to twice the horizon, then cut
        if (horizon > 0 && !solved && t - t0 >= horizon * 2) {
            solved = true;
            g_outcome.horizonCut = true;
            goalState = rules.pick(cur);
            planCut = t0 + horizon;
            break;
        }
        if (g_check.cancel.load(std::memory_order_relaxed)
            || g_ladderStop.load(std::memory_order_relaxed)) {
            std::printf("CANCELLED: at t=%lld (beam search abandoned by the caller)\n", t);
            std::fflush(stdout);
            g_outcome.verdict = VerdictCancelled;
            g_outcome.cancelT = t;
            g_outcome.deepT = bestT;
            g_outcome.deepX = bestX;
            return 3;
        }
    }
    if (solved) {
        out.kind = SearchResult::Solved;
        out.goal = goalState;
    } else if (horizon > 0 && !nxt.empty()) {
        out.kind = SearchResult::Partial;
        out.goal = rules.pick(nxt);
    } else {
        out.kind = SearchResult::NoPlan;
        // after the final swap the previous layer's states sit in nxt
        out.lastLayer.assign(nxt.begin(), nxt.begin() + (std::ptrdiff_t)std::min<size_t>(nxt.size(), 12));
    }
    out.t = bestT;
    out.x = bestX;
    if (out.kind != SearchResult::NoPlan) planFromArena(arena, out.goal, out.inputs);
    return -1;
}


// ---- the run: what a search may keep across the calls of one level's solve ------------------------
// A level's solve is many calls (the first search, the ladder's anchors, the fine passes, the
// resims), and the host -- the mod's repair loop -- tells the core where the run begins and ends and
// what each flight of a plan did (dp_bridge runBegin / runVerified / runEnd). Kept OUT of
// resetInvocationState on purpose: a call does not end a run. A run is one level, so begin refuses
// while another is still open -- the carry-over a one-session cold run exists to catch, caught at
// the start of the level it would leak into. The searches here keep no run state yet; the run
// records what it was told, and the plan this core emitted last, so a verification can say whether
// the plan flown was the search's own.
struct SearchRun {
    bool open = false;
    int level = -1;
    uint64_t seed = 0;
    long long verified = 0;   // flights reported
    long long own = 0;        // ...of which flew the plan this core emitted last
    long long calls = 0;      // search calls so far (the call index a seeded search mixes in)
    long long nondet = 0;     // ...of which stopped other than on their own budget (not reproducible)
    long long lastT0 = -1;    // the plan this core emitted last (cliMainOnce): its anchor...
    std::vector<std::pair<long long, int>> lastEdges;   // ...and its input edges (press, level)
    // The plan the host's section search spliced in last (runNoteSplice): from its seam on it is
    // the section search's, not this core's. A flight of it is counted apart (`via=secsolve`).
    long long spliceT0 = -1;
    std::vector<std::pair<long long, int>> spliceEdges;
    long long viaSplice = 0;  // flights reported that flew it
};
inline SearchRun g_run;

inline bool runBegin(int level, uint64_t seed, std::string& why) {
    if (g_run.open) {
        why = "a run is still open (level " + std::to_string(g_run.level) + ", "
              + std::to_string(g_run.verified) + " flights reported) at the start of level "
              + std::to_string(level);
        return false;
    }
    g_run = SearchRun{};
    g_run.open = true;
    g_run.level = level;
    g_run.seed = seed;
    return true;
}
// The plan a call finally answered with, read back from its file (`input=press,level` lines), in
// place of the one its last search wrote: the cap ladder can put an earlier attempt's plan back
// (cliMain step 5) and the gamble takes the second core's (PlainElsewhere::take). The host's bridge
// calls this after cliMain returns, when the call wrote a plan. The anchor (lastT0) is the call's:
// every attempt of one call searches from the same --start. False when the file cannot be read.
inline bool runNotePlanFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::vector<std::pair<long long, int>> e;
    std::string ln;
    while (std::getline(in, ln)) {
        long long press = 0;
        int level = 0;
        if (std::sscanf(ln.c_str(), "input=%lld,%d", &press, &level) == 2) e.emplace_back(press, level);
    }
    g_run.lastEdges = std::move(e);
    return true;
}
// Did the flight fly the plan this core emitted last? What the button does from that plan's seam on
// is what the emitted plan says -- compared as levels, not as edge lists: the plan writer can put
// the seam's edge a tick before the anchor (its latency) or emit one that changes nothing, and the
// host's splice onto the verified prefix keeps or merges such an edge as it sees fit. The prefix
// before the seam is not compared. No plan emitted yet -> no. "Emitted" is what the call answered
// with (the host reads it back with runNotePlanFile), not the last of a ladder's attempts.
inline bool runFlewPlan(const std::vector<std::pair<long long, int>>& flown, long long t0,
                        const std::vector<std::pair<long long, int>>& edges) {
    if (t0 < 0) return false;
    using Edge = std::pair<long long, int>;
    auto byTick = [](const Edge& a, const Edge& b) { return a.first < b.first; };
    std::vector<Edge> fl(flown.begin(), flown.end()), em(edges.begin(), edges.end());
    std::stable_sort(fl.begin(), fl.end(), byTick);
    std::stable_sort(em.begin(), em.end(), byTick);
    long long cut = t0;
    if (!em.empty()) cut = std::min(cut, em.front().first);
    int start = 0;   // the button as the flown plan left it just before the seam
    for (const Edge& e : fl)
        if (e.first < cut) start = e.second;
    auto changes = [&](const std::vector<Edge>& v) {   // only the edges that change the level
        std::vector<Edge> out;
        int lvl = start;
        for (const Edge& e : v) {
            if (e.first < cut || e.second == lvl) continue;
            lvl = e.second;
            out.push_back(e);
        }
        return out;
    };
    return changes(fl) == changes(em);
}
inline bool runOwnPlan(const std::vector<std::pair<long long, int>>& flown) {
    return runFlewPlan(flown, g_run.lastT0, g_run.lastEdges);
}
// The host's section search spliced `edges` (the whole plan as installed, press / level) from
// tick `t0` on. Compared like runOwnPlan, from that seam.
inline void runNoteSplice(const std::vector<std::pair<long long, int>>& edges, long long t0) {
    if (!g_run.open) return;
    g_run.spliceT0 = t0;
    g_run.spliceEdges = edges;
}
// A flight of `flown` ended: died at `deathT` / `x`, or cleared. `divT`: the first tick at which
// the host's recorder saw the model and GD part (-1: none recorded). A GD death is always a
// physical one, so no kind is passed. Returns the line the host logs.
inline std::string runVerified(const std::vector<std::pair<long long, int>>& flown, long long deathT,
                               double x, bool cleared, long long divT) {
    if (!g_run.open) return "dprun: verified with no run open";
    const bool own = runOwnPlan(flown);
    const bool splice = !own && runFlewPlan(flown, g_run.spliceT0, g_run.spliceEdges);
    ++g_run.verified;
    if (own) ++g_run.own;
    if (splice) ++g_run.viaSplice;
    char b[240];
    std::snprintf(b, sizeof b,
                  "dprun: verified %s t=%lld x=%.1f diverged=%lld own=%d (%lld of %lld) nondet=%lld%s",
                  cleared ? "cleared" : "death", deathT, x, divT, own ? 1 : 0, g_run.own,
                  g_run.verified, g_run.nondet, splice ? " via=secsolve" : "");
    return b;
}
inline std::string runEnd() {
    if (!g_run.open) return "dprun: end (no run open)";
    const std::string s = "dprun: end level=" + std::to_string(g_run.level) + " flights="
                          + std::to_string(g_run.verified) + " own=" + std::to_string(g_run.own)
                          + " calls=" + std::to_string(g_run.calls)
                          + " nondet=" + std::to_string(g_run.nondet)
                          + (g_run.viaSplice ? " via-secsolve=" + std::to_string(g_run.viaSplice)
                                             : std::string());
    g_run = SearchRun{};
    return s;
}


// ---- a third search: Monte Carlo rollouts (reference, --search montecarlo) -------------------------
// The search contract's implementation that draws random numbers. A round samples up to
// --mcrollouts input plans, each flipping the button at --mctoggles random ticks inside the next
// --mcwindow ticks, walks each one through the model until it dies, keeps the one that lived longest
// and commits the first third of that gain. A round that gains fewer than 3 ticks backs off, further
// each time, and the back-off resets whenever the committed path goes deeper than it ever has.
// Nothing prefers margin: the plan is as tight as luck made it.
//   Reproducible: the random numbers come from (seed, anchor, the run's call index), and the budget
// is a step count (--mcsteps), not seconds. The wall clock (--mcsafety) is only a safety valve; a
// call it stops is marked not deterministic (SearchResult::deterministic, and the run's count).
// Rollouts run one after another, so the draws do not depend on the thread count.
//   Declined (the caller's default search answers instead) where a rollback cannot rewind: gameplay
// rotation and the control windows keep one-shot state outside State, a turned start frame, and
// coins, whose route this walk does not model.
// The button's press latency is the search's own: a press at t takes effect at t+1 or t+2 (mode),
// and what the arena records per tick is the input in effect -- the same thing the other searches
// record, so cliMainOnce's witness walks the plan and writes its edges with the usual latency.
inline uint64_t mcMix(uint64_t z) {   // splitmix64's finaliser
    z += 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
template <class E, class Rules>
inline int searchMontecarlo(E& e, const Rules& rules, SearchResult& out) {
    auto& t0 = e.in.t0;
    auto& init = e.in.init;
    auto& tEnd = e.in.tEnd;
    SearchArena store(e.in.checkWanted);   // this method's states
    auto& arena = store.arena;
    auto& checkWanted = e.in.checkWanted;
    auto& arenaMode = store.arenaMode;
    auto& model = e.in.model;
    auto& bestT = e.outcome.bestT;
    auto& bestX = e.outcome.bestX;
    auto& goalState = e.outcome.goalState;
    auto& solved = e.outcome.solved;
    auto& curDxF = e.in.curDxF;
    auto& horizon = e.in.horizon;
    auto& planCut = e.in.planCut;
    // The caller's horizon, kept as searchLayers keeps it: alive at twice the horizon is a goal, and
    // the plan is then cut back to t0 + horizon -- a prefix KNOWN to stay alive that much longer.
    const long long hzEnd = horizon > 0 ? t0 + horizon * 2 : -1;
    // (...nor a world with item gates, which its walks never fire: Model::fireGates.)
    if (!g_rotTrig.empty() || !g_ctrlWin.empty() || (!g_ctrlWin2.empty())
        || init.frame != 0 || rules.needsAfterStep() || model.hasGates()) {
        std::printf("search: montecarlo declines (rotation=%zu control-windows=%zu frame=%d "
                    "coins=%d%s)\n", g_rotTrig.size(),
                    g_ctrlWin.size() + (g_ctrlWin2.size()), (int)init.frame,
                    rules.needsAfterStep() ? 1 : 0,
                    model.hasGates() && !rules.needsAfterStep() ? " gates=1" : "");
        out.kind = SearchResult::Declined;
        return -1;
    }
    const uint64_t seed = g_run.open ? g_run.seed : (uint64_t)g_searchSeed;
    const long long call = g_mcCall >= 0 ? g_mcCall : g_run.calls;
    std::mt19937_64 rng(mcMix(seed ^ mcMix((uint64_t)t0 ^ mcMix((uint64_t)call))));
    std::printf("search: montecarlo seed=%llu t0=%lld call=%lld rollouts=%d window=%d toggles=%d "
                "steps=%lld stall=%lld\n", (unsigned long long)seed, t0, call, g_mcRollouts,
                g_mcWindow, g_mcToggles, g_mcSteps, g_mcStall);
    using Clock = std::chrono::steady_clock;
    const auto mcStart = Clock::now();
    auto elapsedS = [&] { return std::chrono::duration<double>(Clock::now() - mcStart).count(); };
    auto latOf = [](uint8_t m) { return (m == 1 || m == 3) ? 2 : 1; };
    // One committed or rolled-out tick: the state AFTER the tick, the input in effect, the button as
    // pressed, and the effects already scheduled (a press lands 1 or 2 ticks later).
    struct McNode {
        State s;
        int curIn = 0;
        int held = 0;
        int nfx = 0;
        long long fxT[4] = {0, 0, 0, 0};
        int fxV[4] = {0, 0, 0, 0};
        bool pressed = false;   // the button changed on this tick
    };
    long long steps = 0;
    auto press = [&](McNode& n, long long t) {
        n.held ^= 1;
        n.pressed = true;
        if (n.nfx < 4) {
            n.fxT[n.nfx] = t + latOf(n.s.mode);
            n.fxV[n.nfx] = n.held;
            ++n.nfx;
        }
    };
    // Step tick t on n in place, through the model (a group of one, as the model's walk builds it).
    // Returns true if the player died on it.
    auto stepTick = [&](McNode& n, long long t) -> bool {
        ++steps;
        n.pressed = false;
        int k2 = 0;   // effects due now, in scheduling order (a later press wins a tie)
        for (int k = 0; k < n.nfx; ++k) {
            if (n.fxT[k] <= t) n.curIn = n.fxV[k];
            else { n.fxT[k2] = n.fxT[k]; n.fxV[k2] = n.fxV[k]; ++k2; }
        }
        n.nfx = k2;
        State& s = n.s;
        const float dxU = ((s.dx > 0.f) ? s.dx : curDxF) * (s.rev ? -1.f : 1.f);
        const double xPrev = (double)s.xAbs;
        const double x = (double)advanceX(s.xAbs, dxU);
        for (AutoTrig& A : g_autoTrig) {   // autonomous fires on this walk's own crossing
            if (A.fireT >= 0) continue;
            if (x < A.cx) break;   // sorted by cx
            A.fireT = (int)t + A.delay;
            A.fireX = A.lockLastX = x;
        }
        GroupEnv env;
        double pLo = x, pHi = x;
        p2Widen(pLo, pHi, p2Dx(s), p2Dx(s));   // --p2xoff
        model.prepare(env, (int)s.frame, s.trig, (int)s.trigT, s.fireB,
                      s.lockOff + (float)(x - xPrev), x, xPrev, dxU, pLo, pHi, t,
                      GroupOpts{false, false, false});
        const StepCtx K = env.ctx();
        g_nearOrb = 0;
        g_dashVySet = 0;
        bool dead = false;
        State c = model.advance(s, n.curIn, K, dead, nullptr);
        c.action = (uint8_t)n.curIn;
        s = c;
        return dead;
    };
    // Walk from `from` (the state after tick T) with the button flipped at the press ticks in `flips`
    // (sorted, all >= T). Returns the last tick the player was alive on, capped at `until`; `goal` is
    // set if it reached the goal. With `keep`, every tick is appended to it.
    // Every walk starts its world afresh (a jump to T + 1), so what it meets does not depend on where
    // the walk before it ended -- which the rounds' lanes below rely on.
    auto walk = [&](const McNode& from, long long T, const std::vector<long long>& flips,
                    long long until, bool& goal, std::vector<McNode>* keep) -> long long {
        McNode n = from;
        size_t fi = 0;
        goal = false;
        model.forgetWorld((int)from.s.frame);
        while (fi < flips.size() && flips[fi] == T) { press(n, T); ++fi; }
        for (long long t = T + 1; t <= until; ++t) {
            if (stepTick(n, t)) return t - 1;
            while (fi < flips.size() && flips[fi] == t) { press(n, t); ++fi; }
            if (keep) keep->push_back(n);
            if (rules.reached(n.s) || (hzEnd > 0 && t >= hzEnd)) { goal = true; return t; }
        }
        return until;
    };
    std::vector<McNode> path;   // path[k] = the state after tick t0 + k
    {
        McNode n0;
        n0.s = init;
        n0.curIn = (int)init.held;
        n0.held = (int)init.held;
        path.push_back(n0);
    }
    // Commit a rollout: a flip on T itself belongs to the node already on the path, the rest is
    // appended by walking it.
    auto commit = [&](long long T, const std::vector<long long>& fl, long long upto) {
        McNode& head = path.back();
        std::vector<long long> rest;
        for (long long p : fl) {
            if (p == T) press(head, T);
            else rest.push_back(p);
        }
        bool g = false;
        const McNode start = head;
        walk(start, T, rest, upto, g, &path);
    };
    // The autonomous triggers' one-shot state lives in g_autoTrig, which every walk writes. Before a
    // walk from tick T it is rebuilt from the COMMITTED path: fired at the first committed tick whose
    // x reached cx, else unfired; what the start had already resolved is kept.
    struct AutoInit { int fireT; double fireX, lockLastX; };
    std::vector<AutoInit> autoInit;
    for (const AutoTrig& A : g_autoTrig) autoInit.push_back({A.fireT, A.fireX, A.lockLastX});
    auto resetAuto = [&](long long T) {
        const size_t last = (size_t)(T - t0);   // path[0..last] is committed
        for (size_t i = 0; i < g_autoTrig.size(); ++i) {
            AutoTrig& A = g_autoTrig[i];
            if (autoInit[i].fireT >= 0) {
                A.fireT = autoInit[i].fireT;
                A.fireX = autoInit[i].fireX;
                A.lockLastX = autoInit[i].lockLastX;
                continue;
            }
            size_t a = 1, b = last + 1;   // the first k in [1, last] with x >= cx
            while (a < b) {
                const size_t mid = (a + b) / 2;
                if ((double)path[mid].s.xAbs < A.cx) a = mid + 1; else b = mid;
            }
            if (a <= last) {
                A.fireT = (int)(t0 + (long long)a) + A.delay;
                A.fireX = A.lockLastX = (double)path[a].s.xAbs;
            } else {
                A.fireT = -1;
            }
        }
    };
    // A round's rollouts are stepped kMcLanes at a time, together, tick by tick (runLanes). Each
    // lane's result is the one walk() gives it alone, but the world -- the moving geometry
    // Model::prepare places, measured 2026-10-05 at 97% of a step on lv19/lv20 (35-55 us against
    // 1 us for the step itself) -- is placed once a tick for every lane whose inputs to it are the
    // same: a class. A class splits when its lanes' x, fired boxes, their ticks or the lock differ,
    // and while there are several, each class's world is carried from tick to tick
    // (Model::saveWorld). The lanes are taken in rollout order and the width is fixed, so the
    // result depends on the seed alone, not on the machine or the thread count.
    constexpr int kMcLanes = 32;
    struct Lane {
        McNode n;
        size_t fi = 0;   // the next flip
        long long lived = 0, steps = 0;
        bool goal = false, done = false;
        int cls = 0;
    };
    struct LaneClass {
        std::vector<AutoInit> autoSt;   // the autonomous triggers as this class's walk left them
        std::vector<double> carry;      // its world after its last tick (Model::saveWorld)
        bool hasCarry = false;
    };
    std::vector<Lane> lanes((size_t)kMcLanes);
    std::vector<std::vector<long long>> laneFlips((size_t)kMcLanes);
    auto saveAuto = [&](std::vector<AutoInit>& v) {
        v.resize(g_autoTrig.size());
        for (size_t i = 0; i < g_autoTrig.size(); ++i)
            v[i] = {g_autoTrig[i].fireT, g_autoTrig[i].fireX, g_autoTrig[i].lockLastX};
    };
    auto loadAuto = [&](const std::vector<AutoInit>& v) {
        for (size_t i = 0; i < g_autoTrig.size() && i < v.size(); ++i) {
            g_autoTrig[i].fireT = v[i].fireT;
            g_autoTrig[i].fireX = v[i].fireX;
            g_autoTrig[i].lockLastX = v[i].lockLastX;
        }
    };
    // What a lane hands Model::prepare on a tick, beyond its state's own fields.
    struct LanePre { float dxU = 0.f, lockAt = 0.f; double xPrev = 0.0, x = 0.0; };
    std::vector<LanePre> pre((size_t)kMcLanes);
    std::vector<LaneClass> cls, nxt;
    std::vector<int> act, rep, prevOf, kids;
    // walk() for lanes 0..nl-1 at once, from `from` (the state after tick T), each with its
    // laneFlips; leaves each lane's lived / goal / steps.
    auto runLanes = [&](const McNode& from, long long T, long long until, size_t nl) {
        const int f = (int)from.s.frame;
        cls.assign(1, LaneClass{});
        resetAuto(T);
        saveAuto(cls[0].autoSt);
        for (size_t l = 0; l < nl; ++l) {
            Lane& L = lanes[l];
            L = Lane{};
            L.n = from;
            const std::vector<long long>& fl = laneFlips[l];
            while (L.fi < fl.size() && fl[L.fi] == T) { press(L.n, T); ++L.fi; }
        }
        // The class whose world the model holds, as of tick heldT. The first tick seeks afresh, as
        // walk() does, so class 0 at T has no world to carry: putting it back is forgetting.
        model.forgetWorld(f);
        int held = -1;
        long long heldT = T;
        auto sameInputs = [&](size_t a, size_t b) {
            const State& sa = lanes[a].n.s;
            const State& sb = lanes[b].n.s;
            return pre[a].x == pre[b].x && pre[a].xPrev == pre[b].xPrev && pre[a].dxU == pre[b].dxU
                   && pre[a].lockAt == pre[b].lockAt && sa.frame == sb.frame && sa.trig == sb.trig
                   && sa.trigT == sb.trigT && !std::memcmp(sa.fireB, sb.fireB, sizeof sa.fireB);
        };
        for (long long t = T + 1; t <= until; ++t) {
            act.clear();
            for (size_t l = 0; l < nl; ++l)
                if (!lanes[l].done) act.push_back((int)l);
            if (act.empty()) break;
            // Each lane's own half of stepTick: the effects due now, and where it goes.
            for (int l : act) {
                Lane& L = lanes[(size_t)l];
                McNode& n = L.n;
                ++L.steps;
                n.pressed = false;
                int k2 = 0;
                for (int k = 0; k < n.nfx; ++k) {
                    if (n.fxT[k] <= t) n.curIn = n.fxV[k];
                    else { n.fxT[k2] = n.fxT[k]; n.fxV[k2] = n.fxV[k]; ++k2; }
                }
                n.nfx = k2;
                const State& s = n.s;
                LanePre& P = pre[(size_t)l];
                P.dxU = ((s.dx > 0.f) ? s.dx : curDxF) * (s.rev ? -1.f : 1.f);
                P.xPrev = (double)s.xAbs;
                P.x = (double)advanceX(s.xAbs, P.dxU);
                P.lockAt = s.lockOff + (float)(P.x - P.xPrev);
            }
            // This tick's classes: a lane joins the first class of its parent with the same inputs.
            nxt.clear();
            rep.clear();
            prevOf.clear();
            for (int l : act) {
                Lane& L = lanes[(size_t)l];
                int c = 0;
                while (c < (int)rep.size()
                       && !(prevOf[(size_t)c] == L.cls && sameInputs((size_t)rep[(size_t)c], (size_t)l)))
                    ++c;
                if (c == (int)rep.size()) {
                    rep.push_back(l);
                    prevOf.push_back(L.cls);
                    nxt.emplace_back();
                }
                L.cls = c;   // (this tick's numbering from here on)
            }
            // A parent with several children has its world saved now, while the model holds it.
            kids.assign(cls.size(), 0);
            for (int p : prevOf) ++kids[(size_t)p];
            if (held >= 0 && heldT == t - 1 && kids[(size_t)held] > 1 && !cls[(size_t)held].hasCarry) {
                model.saveWorld(f, cls[(size_t)held].carry);
                cls[(size_t)held].hasCarry = true;
            }
            for (size_t c = 0; c < rep.size(); ++c) {
                const int p = prevOf[c];
                const Lane& R = lanes[(size_t)rep[c]];
                const LanePre& P = pre[(size_t)rep[c]];
                if (!(held == p && heldT == t - 1)) {
                    if (cls[(size_t)p].hasCarry) model.loadWorld(f, cls[(size_t)p].carry, t - 1);
                    else model.forgetWorld(f);   // class 0 at T
                }
                loadAuto(cls[(size_t)p].autoSt);
                for (AutoTrig& A : g_autoTrig) {   // autonomous fires on this class's own crossing
                    if (A.fireT >= 0) continue;
                    if (P.x < A.cx) break;   // sorted by cx
                    A.fireT = (int)t + A.delay;
                    A.fireX = A.lockLastX = P.x;
                }
                GroupEnv env;
                double pLo = P.x, pHi = P.x;
                p2Widen(pLo, pHi, p2Dx(R.n.s), p2Dx(R.n.s));   // --p2xoff
                model.prepare(env, (int)R.n.s.frame, R.n.s.trig, (int)R.n.s.trigT, R.n.s.fireB,
                              P.lockAt, P.x, P.xPrev, P.dxU, pLo, pHi, t,
                              GroupOpts{false, false, false});
                const StepCtx K = env.ctx();
                saveAuto(nxt[c].autoSt);
                held = (int)c;
                heldT = t;
                if (rep.size() > 1) {
                    model.saveWorld(f, nxt[c].carry);
                    nxt[c].hasCarry = true;
                }
                for (int l : act) {
                    Lane& L = lanes[(size_t)l];
                    if (L.cls != (int)c) continue;
                    g_nearOrb = 0;
                    g_dashVySet = 0;
                    bool dead = false;
                    State s2 = model.advance(L.n.s, L.n.curIn, K, dead, nullptr);
                    s2.action = (uint8_t)L.n.curIn;
                    L.n.s = s2;
                    if (dead) { L.done = true; L.lived = t - 1; continue; }
                    const std::vector<long long>& fl = laneFlips[(size_t)l];
                    while (L.fi < fl.size() && fl[L.fi] == t) { press(L.n, t); ++L.fi; }
                    if (rules.reached(L.n.s) || (hzEnd > 0 && t >= hzEnd)) {
                        L.done = true;
                        L.goal = true;
                        L.lived = t;
                    } else if (t == until) {
                        L.done = true;
                        L.lived = until;
                    }
                }
            }
            cls.swap(nxt);
        }
    };
    long long deepest = t0, back = 5, rounds = 0, rollouts = 0, backoffs = 0;
    // The rollout count when deepest last grew, and the longest run of rollouts without it.
    long long gainAt = 0, maxGap = 0;
    bool reached = false, valve = false, stalled = false;
    std::vector<long long> flips, bestFlips, f2;
    // The caller has stopped caring (the mod's session ended, or the ladder took the plain search
    // beside it), read as searchLayers and searchBeam read it. Once a round and once a batch of
    // lanes, so a cancel waits for one batch at most, not for the safety valve. The CLI never sets
    // either flag, so the draws and the output are unchanged.
    auto stopAsked = [&] {
        return g_check.cancel.load(std::memory_order_relaxed)
               || g_ladderStop.load(std::memory_order_relaxed);
    };
    auto cancelled = [&] {
        const long long t = t0 + (long long)path.size() - 1;
        std::printf("CANCELLED: at t=%lld (montecarlo search abandoned by the caller)\n", t);
        std::fflush(stdout);
        g_outcome.verdict = VerdictCancelled;
        g_outcome.cancelT = t;
        g_outcome.deepT = t;
        g_outcome.deepX = (double)path.back().s.xAbs;
        return 3;
    };
    while (true) {
        if (stopAsked()) return cancelled();
        if (steps >= g_mcSteps) break;
        if (g_mcStall > 0 && rollouts - gainAt >= g_mcStall) { stalled = true; break; }
        if (elapsedS() > g_mcSafetyS) { valve = true; break; }
        ++rounds;
        const long long T = t0 + (long long)path.size() - 1;
        if (T >= tEnd) break;
        const McNode from = path.back();
        long long best = T;
        bool bestGoal = false;
        bestFlips.clear();
        const long long until = std::min(T + (long long)g_mcWindow, tEnd);
        std::uniform_int_distribution<long long> pick(T, until - 1);
        auto draw = [&](std::vector<long long>& out) {
            flips.clear();
            for (int k = 0; k < g_mcToggles; ++k) flips.push_back(pick(rng));
            std::sort(flips.begin(), flips.end());
            out.clear();   // two flips on one tick cancel: keep a tick only if odd
            for (size_t i = 0; i < flips.size();) {
                size_t j = i;
                while (j < flips.size() && flips[j] == flips[i]) ++j;
                if ((j - i) & 1) out.push_back(flips[i]);
                i = j;
            }
        };
        // The rollouts in rollout order, kMcLanes at a time (runLanes); a round ends at the first
        // rollout, in that order, that reaches the goal or gets far enough -- as if they ran one by
        // one. The lanes drawn past it are thrown away and the generator is put back to just after it.
        bool roundDone = false;
        for (int r0 = 0; r0 < g_mcRollouts && !roundDone; r0 += kMcLanes) {
            if (r0 > 0 && stopAsked()) return cancelled();
            const int nl = std::min(kMcLanes, g_mcRollouts - r0);
            const std::mt19937_64 rngAt = rng;
            const auto pickAt = pick;
            for (int l = 0; l < nl; ++l) draw(laneFlips[(size_t)l]);
            runLanes(from, T, until, (size_t)nl);
            for (int l = 0; l < nl; ++l) {
                const Lane& L = lanes[(size_t)l];
                ++rollouts;
                steps += L.steps;
                if (L.goal || L.lived > best) {
                    best = L.lived;
                    bestFlips = laneFlips[(size_t)l];
                    bestGoal = L.goal;
                }
                if (bestGoal || best - T > g_mcWindow / 2) {
                    rng = rngAt;
                    pick = pickAt;
                    for (int k = 0; k <= l; ++k) draw(f2);
                    roundDone = true;
                    break;
                }
            }
        }
        if (bestGoal) {
            resetAuto(T);
            commit(T, bestFlips, best);
            reached = true;
            maxGap = std::max(maxGap, rollouts - gainAt);
            break;
        }
        if (best - T >= 3) {
            const long long commitT = T + (best - T) / 3;
            resetAuto(T);
            commit(T, bestFlips, commitT);
            if (commitT > deepest) {
                deepest = commitT;
                back = 5;
                maxGap = std::max(maxGap, rollouts - gainAt);
                gainAt = rollouts;
            }
            continue;
        }
        ++backoffs;   // stuck: back off, further each time, never past the start
        const long long keepT = std::max(t0, T - back);
        path.resize((size_t)(keepT - t0) + 1);
        back += (back > 100) ? 50 : 5;
        if (back > 20000) back = 5;
    }
    const long long lastT = t0 + (long long)path.size() - 1;
    resetAuto(lastT);   // the witness walks the committed path, with its triggers' firing
    if (!reached) maxGap = std::max(maxGap, rollouts - gainAt);
    std::printf("search: montecarlo %s at t=%lld x=%.1f in %.3fs rounds=%lld rollouts=%lld "
                "backoffs=%lld steps=%lld deepest=%lld maxgap=%lld%s\n",
                reached ? "SOLVED" : "GAVE UP", lastT, (double)path.back().s.xAbs, elapsedS(),
                rounds, rollouts, backoffs, steps, deepest, maxGap,
                valve ? " -- stopped by the wall-clock safety valve: NOT deterministic"
                      : stalled ? " -- stalled: no deeper commit in --mcstall rollouts" : "");
    // The committed path into the arena: one node per tick, its action the input in effect.
    uint32_t parent = init.parent;
    for (size_t k = 1; k < path.size(); ++k) {
        arena.push_back(Node{parent | ((uint32_t)(path[k].curIn & 1) << 31)});
        if (checkWanted) arenaMode.push_back(path[k].s.mode);
        parent = (uint32_t)(arena.size() - 1);
        path[k].s.parent = parent;
        path[k].s.action = (uint8_t)path[k].curIn;
    }
    out.deterministic = !valve;
    bestT = lastT;
    bestX = (double)path.back().s.xAbs;
    out.t = bestT;
    out.x = bestX;
    if (reached) {
        solved = true;
        goalState = path.back().s;
        out.kind = SearchResult::Solved;
        out.goal = goalState;
        if (!rules.reached(goalState)) {   // alive at twice the horizon, not at the goal
            g_outcome.horizonCut = true;
            planCut = t0 + horizon;
            std::printf("search: montecarlo alive at t=%lld = t0 + 2 x horizon; the plan is cut "
                        "to t=%lld\n", lastT, planCut);
        }
    } else if (path.size() > 1) {
        out.kind = SearchResult::Partial;
        out.goal = path.back().s;
    } else {
        out.kind = SearchResult::NoPlan;
    }
    if (out.kind != SearchResult::NoPlan) planFromArena(arena, out.goal, out.inputs);
    return -1;
}

}  // namespace dp
