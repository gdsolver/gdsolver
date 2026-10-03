#pragma once
#include "dp/step.hpp"
#include "dp/search_key.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace dp {

// --searchcensus N: what the search spends its layers on, every N-th layer. Print only --
// nothing here is read back by the search, so the plan is the same with the flag on or off.
//
// Two questions, both counted rather than timed (a count does not depend on what else the
// machine is running):
//   1. which parts of the dedupe key divide the frontier. For each group of key terms, the
//      pre-cap layer is keyed again with that group put back to its resting value; the cells
//      that disappear are the cells that group alone was keeping apart.
//   2. what the alive cap keeps. On a layer the cap cuts, the (y, vy) bins the whole layer
//      covers against the bins the kept states cover, and against what a stride over the same
//      class sorted by (y, vy) would have kept.
inline int g_searchCensus = 0;

// --twinskip (ON by default; --no-twinskip turns it off): a parent whose pressed child provably
// comes out as a TWIN of its released one --
// same dedupe key, same vy, same fate, same touch mask -- is not stepped twice. The released
// child is copied into the pressed slot instead (cli.hpp stepKid). A twin can never be accepted
// by the dedupe (emit() replaces a representative only on a vy strictly beyond it, and the twin's
// sibling sits in the same cell with the same vy), so the plan, the born/died counts and the
// band rows are the same as with both children stepped.
// --twinaudit: make the same prediction, step both children anyway, and count every predicted
// pair that did NOT come out a twin. Zero over the corpus is the licence for --twinskip.
// On since 2026-09-25: --twinaudit found no mispredicted pair over the 22 official levels, 7
// customs and 176 loop calls rebuilt from divdb kits; plan, trace and band were byte-identical
// with it on and off, and a lv22 cold arm printed the same [fp] lines as its control.
// (The --twinskip switch is gone since the 0.4.0 clean-up; --twinaudit stays.)
inline bool g_twinAudit = false;

// Input quantisation (off at 0/1; a search restriction, not physics -- holding and letting go
// stay free, only how soon the button may change again is limited):
//   --minpulse K   after the button changes, it keeps its new level for at least K ticks
//                  (State::edgeAge), so no press or release shorter than K ticks is planned.
//   --inputgrid K  the button may change only on a tick t with t % K == 0.
// Where the rule forbids the one child an existing prune would have kept (an airborne cube whose
// press is moot and whose release is locked), the held child is kept instead: a parent is never
// left with no child by the rule.
inline int g_minPulse = 0;
inline int g_inputGrid = 0;
// --gridmap x0:k0,x1:k1,...: the input grid as a step function of the frontier's leading x (the
// previous layer's, as --capmap reads it). From x_i on the grid is k_i, where 0 means
// --inputgrid's value and 1 means none; before the first entry it is --inputgrid. Empty =
// --inputgrid everywhere. --capladder writes it so that the stretches it raises the cap over
// are searched at full timing precision (see cliMain).
inline std::vector<std::pair<double, int>> g_gridMap;
inline int gridAtX(double x) {
    int k = g_inputGrid;
    for (const auto& e : g_gridMap)
        if (x >= e.first) k = e.second ? e.second : g_inputGrid;
    return k;
}

struct TwinStats {
    long long predicted = 0, skipped = 0, violations = 0;
    long long byMode[9] = {};
    int printed = 0;
    void violation(long long t, const State& p, int fa, int fb, const State& a, const State& b,
                   const SearchKey& ka, const SearchKey& kb) {
        ++violations;
        if (printed >= 20) return;
        ++printed;
        std::printf("twinaudit: VIOLATION t=%lld mode=%d y=%.4f vy=%.4f x=%.3f g=%d flip=%d "
                    "| released fate=%d key=%016llx vy=%.6f y=%.4f trig=%llx | pressed fate=%d "
                    "key=%016llx vy=%.6f y=%.4f trig=%llx\n",
                    t, (int)p.mode, (double)p.y, (double)p.vy, (double)p.xAbs, (int)p.grounded,
                    (int)p.flip, fa, (unsigned long long)SearchKeyHash{}(ka), (double)a.vy, (double)a.y,
                    (unsigned long long)a.trig.word(0), fb, (unsigned long long)SearchKeyHash{}(kb), (double)b.vy,
                    (double)b.y, (unsigned long long)b.trig.word(0));
    }
    // The skip count only under --searchcensus: --twinskip is meant to leave the output as it
    // was, stdout included.
    void print() const {
        if (g_twinAudit)
            std::printf("twinaudit: predicted=%lld violations=%lld (cube=%lld ball=%lld robot=%lld "
                        "spider=%lld)\n",
                        predicted, violations, byMode[0], byMode[2], byMode[5], byMode[6]);
        if (!g_twinAudit && g_searchCensus > 0)
            std::printf("twinskip: skipped=%lld (cube=%lld ball=%lld robot=%lld spider=%lld)\n",
                        skipped, byMode[0], byMode[2], byMode[5], byMode[6]);
    }
};

struct CensusField {
    const char* name;
    void (*canon)(State&);
    bool core;   // part of the "everything but the physics core" key
};

inline const CensusField kCensusFields[] = {
    {"xphase", [](State& s) { s.xAbs = 0.f; }, false},
    {"vy", [](State& s) { s.vy = 0.f; s.vy2 = 0.f; }, false},
    {"p2", [](State& s) {
         if (!s.dual) return;
         s.y2 = s.vy2 = 0.f; s.flip2 = 0; s.mode2 = s.mode; s.mini2 = s.mini;
         s.onSlope2 = 0; s.rideLanded2 = 0; s.slopeT2 = 0; s.boost2 = 0;
         s.ringHold2 = 0; s.pressSpent2 = 0;
     }, false},
    {"held", [](State& s) { s.held = 0; }, false},
    {"grounded", [](State& s) { s.grounded = 0; }, false},
    {"press", [](State& s) {
         s.ringHold = 0; s.pressSpent = 0; s.ringHold2 = 0; s.pressSpent2 = 0;
     }, true},
    {"band", [](State& s) { s.bandFloor = 0.f; s.bandBranch = 0; }, true},
    {"hoverdash", [](State& s) { s.rHover = 0; s.dashing = 0; }, true},
    {"action", [](State& s) { s.action = 0; }, true},
    {"slope", [](State& s) { s.onSlope = 0; s.onSlope2 = 0; }, true},
    {"oneshot", [](State& s) {
         s.pFlap = 0; s.pSpiderTap = 0; s.pNoTerm = 0; s.pExitVy = 0.f; s.frameChg = 0;
         s.ceilT = 0; s.ceilM4 = 0; s.jumpBuf = 0;
     }, true},
    {"boost", [](State& s) { s.boost = 0; s.boost2 = 0; s.a1cLatch = 0; }, true},
    {"arms", [](State& s) { s.fgArm = 0; s.ogLinger = 0; s.holdDead = 0; s.armT = 255; }, true},
    {"coins", [](State& s) { s.coins = 0; s.items = 0; s.taps = 0; }, true},
    {"latch", [](State& s) { s.portalLatch = GravLatch{}; s.portalLatch2 = GravLatch{}; }, true},
    {"fireB", [](State& s) { for (auto& f : s.fireB) f = 0; }, true},
};
constexpr int kCensusN = (int)(sizeof(kCensusFields) / sizeof(kCensusFields[0]));

struct SearchCensus {
    // Census cells retain the exact outer group as well as the exact inner key.
    struct Cell {
        SearchKey key;
        uint32_t dx;
        uint64_t trig;
        uint8_t frame, rev;
        // Do not fold two speed/trigger groups merely because their hashes agree.
        bool operator==(const Cell& b) const {
            return key == b.key && dx == b.dx && trig == b.trig
                   && frame == b.frame && rev == b.rev;
        }
    };
    struct CellHash {
        // Hash only locates a cell; Cell::operator== supplies its identity.
        size_t operator()(const Cell& c) const {
            uint64_t h = SearchKeyHash{}(c.key);
            h = mix(h, c.dx); h = mix(h, c.trig);
            h = mix(h, c.frame); h = mix(h, c.rev);
            return (size_t)h;
        }
    };
    // every layer
    long long layers = 0, kids = 0, died = 0, preCap = 0, postCap = 0;
    long long capLayers = 0, quietCapLayers = 0, quietCapDropped = 0, capDropped = 0;
    long long byMode[9] = {};   // post-cap states (= next layer's parents) by mode; [8] = dual
    // sampled layers
    long long sampled = 0, cells = 0, core = 0;
    long long without[kCensusN] = {};
    // ...per mode ([8] = any dual): cells, and cells without the x phase / vy / non-core terms
    long long mCells[9] = {}, mNoX[9] = {}, mNoVy[9] = {}, mCore[9] = {};
    long long mWithout[9][kCensusN] = {};
    // Sibling census, every layer: for each parent that expanded both inputs, by the parent's mode
    // ([8] = dual) and grounded (0/1): [0] both died, [1] one died, [2] same dedupe cell,
    // [3] cells differ only in ringHold/pressSpent, [4] ...only in held/action, [5] otherwise.
    long long sib[9][2][6] = {};
    long long single[9][2] = {};   // parents that expanded only one input (by the same split)
    long long twin[9][2] = {};     // same cell AND bit-equal y, vy, x, y2, vy2, rot
    // cap coverage, sampled cap layers: [0] 2 px x 0.5 vy bins, [1] 8 px x 2.0
    long long covLayers = 0, covPre[2] = {}, covKept[2] = {}, covAlt[2] = {};

    static uint64_t mix(uint64_t h, uint64_t v) {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return h;
    }
    // The speed group (cli.hpp GKey): the dedupe runs per group, so a key only means
    // something next to the group it was taken in.
    static uint64_t groupOf(const State& s, float curDx) {
        uint64_t h = 0;
        float d = (s.dx > 0.f) ? s.dx : curDx;
        uint32_t di;
        std::memcpy(&di, &d, 4);
        h = mix(h, di);
        for (int k = 0; k < TouchMask::kWords; ++k) h = mix(h, s.trig.word(k));
        h = mix(h, s.frame);
        h = mix(h, s.rev);
        return h;
    }
    static uint64_t binOf(const State& s, uint64_t g, int scale) {
        const double yb = scale ? 8.0 : 2.0, vb = scale ? 2.0 : 0.5;
        uint64_t h = g;
        h = mix(h, ((uint64_t)s.mode << 24) | ((uint64_t)s.flip << 16) | ((uint64_t)s.mini << 8)
                       | (uint64_t)s.dual);
        h = mix(h, (uint64_t)(int64_t)std::floor((double)s.y / yb));
        h = mix(h, (uint64_t)(int64_t)std::floor((double)s.vy / vb));
        return h;
    }

    void layer(const std::vector<State>& pre, long long t, float curDx, long long born,
               long long dead, bool sample) {
        ++layers;
        kids += born;
        died += dead;
        preCap += (long long)pre.size();
        if (!sample) return;
        ++sampled;
        using Cells = std::unordered_set<Cell, CellHash>;
        Cells full, cor, w[kCensusN];
        Cells mf[9], mx[9], mv[9], mc[9];
        std::vector<Cells> mw(9 * kCensusN);
        full.reserve(pre.size() * 2);
        cor.reserve(pre.size() * 2);
        for (auto& s : w) s.reserve(pre.size() * 2);
        for (const State& s : pre) {
            const float dx = s.dx > 0.f ? s.dx : curDx;
            uint32_t di;
            std::memcpy(&di, &dx, sizeof(di));
            auto cell = [&](const State& state) {
                return Cell{keyOf(state, t), di, s.trig.word(0), s.frame, s.rev};
            };
            const int m = s.dual ? 8 : (s.mode < 8 ? s.mode : 0);
            const Cell kf = cell(s);
            full.insert(kf);
            mf[m].insert(kf);
            State c = s;
            for (int f = 0; f < kCensusN; ++f) {
                State d = s;
                kCensusFields[f].canon(d);
                const Cell kd = cell(d);
                w[f].insert(kd);
                mw[(size_t)(m * kCensusN + f)].insert(kd);
                if (f == 0) mx[m].insert(kd);
                if (f == 1) mv[m].insert(kd);
                if (kCensusFields[f].core) kCensusFields[f].canon(c);
            }
            const Cell kc = cell(c);
            cor.insert(kc);
            mc[m].insert(kc);
        }
        cells += (long long)full.size();
        core += (long long)cor.size();
        for (int f = 0; f < kCensusN; ++f) without[f] += (long long)w[f].size();
        for (int m = 0; m < 9; ++m) {
            mCells[m] += (long long)mf[m].size();
            mNoX[m] += (long long)mx[m].size();
            mNoVy[m] += (long long)mv[m].size();
            mCore[m] += (long long)mc[m].size();
            for (int f = 0; f < kCensusN; ++f)
                mWithout[m][f] += (long long)mw[(size_t)(m * kCensusN + f)].size();
        }
    }

    // One parent's two children (cli.hpp stepKid slots 2i / 2i+1). a/b: 0 not expanded,
    // 1 dead, 2 alive (kidFlag).
    void siblings(const State& parent, int fa, int fb, const State& ca, const State& cb,
                  const SearchKey& ka, const SearchKey& kb, long long t) {
        const int m = parent.dual ? 8 : (parent.mode < 8 ? parent.mode : 0);
        const int g = parent.grounded ? 1 : 0;
        if (!fa || !fb) {
            if (fa || fb) ++single[m][g];
            return;
        }
        int c;
        if (fa == 1 && fb == 1) c = 0;
        else if (fa == 1 || fb == 1) c = 1;
        else if (ka == kb) {
            c = 2;
            // ...and of those, the ones whose continuous state came out bit-equal: the second
            // step bought nothing at all.
            if (ca.y == cb.y && ca.vy == cb.vy && ca.xAbs == cb.xAbs && ca.y2 == cb.y2
                && ca.vy2 == cb.vy2 && ca.rot == cb.rot)
                ++twin[m][g];
        }
        else {
            State x = ca, y = cb;
            x.ringHold = y.ringHold = 0;
            x.pressSpent = y.pressSpent = 0;
            x.ringHold2 = y.ringHold2 = 0;
            x.pressSpent2 = y.pressSpent2 = 0;
            if (keyOf(x, t) == keyOf(y, t)) c = 3;
            else {
                x.held = y.held = 0;
                x.action = y.action = 0;
                c = (keyOf(x, t) == keyOf(y, t)) ? 4 : 5;
            }
        }
        ++sib[m][g][c];
    }

    void kept(const std::vector<State>& post) {
        postCap += (long long)post.size();
        for (const State& s : post) {
            if (s.mode < 8) ++byMode[s.mode];
            if (s.dual) ++byMode[8];
        }
    }

    void cap(size_t pre, size_t cap, long long deadThisLayer) {
        ++capLayers;
        capDropped += (long long)(pre - cap);
        if (deadThisLayer == 0) {
            ++quietCapLayers;
            quietCapDropped += (long long)(pre - cap);
        }
    }

    void coverage(const std::vector<State>& pre, const std::vector<uint32_t>& keep,
                  const std::vector<uint32_t>& alt, float curDx) {
        ++covLayers;
        for (int sc = 0; sc < 2; ++sc) {
            std::unordered_set<uint64_t> a, b, c;
            for (const State& s : pre) a.insert(binOf(s, groupOf(s, curDx), sc));
            for (uint32_t i : keep) b.insert(binOf(pre[i], groupOf(pre[i], curDx), sc));
            for (uint32_t i : alt) c.insert(binOf(pre[i], groupOf(pre[i], curDx), sc));
            covPre[sc] += (long long)a.size();
            covKept[sc] += (long long)b.size();
            covAlt[sc] += (long long)c.size();
        }
    }

    void print() const {
        std::printf("searchcensus: layers=%lld kids=%lld died=%lld precap=%lld postcap=%lld "
                    "caplayers=%lld quietcap=%lld dropped=%lld quietdropped=%lld\n",
                    layers, kids, died, preCap, postCap, capLayers, quietCapLayers, capDropped,
                    quietCapDropped);
        std::printf("searchcensus: parents by mode cube=%lld ship=%lld ball=%lld ufo=%lld "
                    "wave=%lld robot=%lld spider=%lld swing=%lld dual=%lld\n",
                    byMode[0], byMode[1], byMode[2], byMode[3], byMode[4], byMode[5],
                    byMode[6], byMode[7], byMode[8]);
        std::printf("searchcensus: sampled=%lld cells=%lld core=%lld (%.1f%% of the cells are "
                    "kept apart only by the non-core terms)\n",
                    sampled, cells, core, cells ? 100.0 * (double)(cells - core) / (double)cells : 0.0);
        for (int f = 0; f < kCensusN; ++f)
            std::printf("searchcensus: without %-9s cells=%lld (-%.2f%%)%s\n", kCensusFields[f].name,
                        without[f],
                        cells ? 100.0 * (double)(cells - without[f]) / (double)cells : 0.0,
                        kCensusFields[f].core ? "" : " [physics core]");
        static const char* kModeName[9] = {"cube", "ship", "ball", "ufo", "wave",
                                           "robot", "spider", "swing", "dual"};
        for (int m = 0; m < 9; ++m)
            if (mCells[m])
                std::printf("searchcensus: mode %-6s cells=%lld noxphase=%lld (x%.2f) novy=%lld "
                            "(x%.2f) core=%lld (x%.2f)\n",
                            kModeName[m], mCells[m], mNoX[m],
                            mNoX[m] ? (double)mCells[m] / (double)mNoX[m] : 0.0, mNoVy[m],
                            mNoVy[m] ? (double)mCells[m] / (double)mNoVy[m] : 0.0, mCore[m],
                            mCore[m] ? (double)mCells[m] / (double)mCore[m] : 0.0);
        // mode x term: the share of that mode's cells each term alone keeps apart (%)
        for (int m = 0; m < 9; ++m) {
            if (!mCells[m]) continue;
            std::printf("searchcensus: split %-6s", kModeName[m]);
            for (int f = 0; f < kCensusN; ++f) {
                const double p = 100.0 * (double)(mCells[m] - mWithout[m][f]) / (double)mCells[m];
                if (p >= 0.05) std::printf(" %s=%.1f", kCensusFields[f].name, p);
            }
            std::printf("\n");
        }
        for (int m = 0; m < 9; ++m)
            for (int g = 0; g < 2; ++g) {
                const long long* v = sib[m][g];
                const long long n = v[0] + v[1] + v[2] + v[3] + v[4] + v[5];
                if (!n && !single[m][g]) continue;
                std::printf("searchcensus: sib %-6s %s pairs=%lld single=%lld bothdead=%lld "
                            "onedead=%lld samecell=%lld pressonly=%lld heldonly=%lld distinct=%lld "
                            "twin=%lld\n",
                            kModeName[m], g ? "ground" : "air   ", n, single[m][g], v[0], v[1],
                            v[2], v[3], v[4], v[5], twin[m][g]);
            }
        std::printf("searchcensus: capcov layers=%lld bins2 pre=%lld kept=%lld sorted=%lld | "
                    "bins8 pre=%lld kept=%lld sorted=%lld\n",
                    covLayers, covPre[0], covKept[0], covAlt[0], covPre[1], covKept[1], covAlt[1]);
    }
};

}  // namespace dp
