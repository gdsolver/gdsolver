#pragma once
#include "dp/step_shared.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
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

// --censuslayers PATH: one row per layer of every search call, appended to PATH as tab-separated text
// (a "# call N layers=L" line first, then t, cap (0 = no cap on the layer), pre-cap and post-cap state
// counts, born, died, the largest x among the pre-cap states, and the pre-cap then post-cap counts by
// mode: cube ship ball ufo wave robot spider swing dual). Lighter than --searchcensus (none of the
// sampled key work) and independent of it. Print only: nothing is read back by the search.
inline bool g_censusLayersOn = false;
inline std::string g_censusLayersPath;

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
// --gridgroundpress (off; design v4 S2): the grid (--inputgrid / --gridmap) does not lock the button of a GROUNDED state, nor of a cube with a landing within reach (search_dp.hpp landingNear, the air press
// child --airpress keeps): the press on the landing tick is a GD jump too. The grid is a search restriction (fewer children), and the census (notes p9_refwatch_census) found it drops the child such a jump needs:
// the layo plan of lv11 jumps at t=1075 (from the ground) and t=2101 (on the landing tick), lv22's at t=261, odd ticks under --inputgrid 2, and a rung that keeps the grid has no lineage with that jump.
inline bool g_gridGroundPress = false;
// --gridgroundpresscube (in the default bundle since 2026-11-13, kDefaultBundle; --no-gridgroundpresscube is the off arm; implies --gridgroundpress): the grounded unlock only for a CUBE (mode 0). The cold of --gridgroundpress (gd-05, 11-13) made lv19 worse (4 -> 11 iterations, new deaths at 11354 / 12233 / 12254 /
// 12272 / 14865, all in robot sections where the model dies on the same tick as GD): a robot stands on the ground for hundreds of ticks and every one of them then takes a press child, which a cube's brief ground
// contact does not; the layers grow and the cap drops lineages. The jumps the census needed (lv11 t=1075 and 2101, lv22 t=261) are cube jumps.
inline bool g_gridGroundPressCube = false;
//   --wavepulse K  --minpulse for the WAVE only (State::mode 4 at the tick the button would change): a wave
//                  that has just pressed or let go keeps that level for at least K ticks. The user's
//                  wave-behaviour option (Avoid glitches, third choice): a hand cannot toggle every 1-3
//                  ticks, which is what the search plans when nothing asks it not to. Off at 0/1; a
//                  search restriction like --minpulse, not physics.
inline int g_wavePulse = 0;
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
                   uint64_t ka, uint64_t kb) {
        ++violations;
        if (printed >= 20) return;
        ++printed;
        std::printf("twinaudit: VIOLATION t=%lld mode=%d y=%.4f vy=%.4f x=%.3f g=%d flip=%d "
                    "| released fate=%d key=%016llx vy=%.6f y=%.4f trig=%llx | pressed fate=%d "
                    "key=%016llx vy=%.6f y=%.4f trig=%llx\n",
                    t, (int)p.mode, (double)p.y, (double)p.vy, (double)p.xAbs, (int)p.grounded,
                    (int)p.flip, fa, (unsigned long long)ka, (double)a.vy, (double)a.y,
                    (unsigned long long)a.trig.word(0), fb, (unsigned long long)kb, (double)b.vy,
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
    int slMask = -1;   // >= 0: the key is taken with only these parts of the shadow bits (shadow_lite.hpp g_slKeyMask)
    int slBytes = -1;  // >= 0: ...and with only these bytes of the press part (g_slPressBytes)
};

inline const CensusField kCensusFields[] = {
    {"xphase", [](State& s) { s.xAbs = 0.f; }, false},
    {"vy", [](State& s) { s.vy = 0.f; s.vy2 = 0.f; }, false},
    {"p2", [](State& s) {
         if (!s.dual) return;
         s.y2 = s.vy2 = 0.f; s.flip2 = 0; s.mode2 = s.mode; s.mini2 = s.mini;
         s.onSlope2 = 0; s.rideLanded2 = 0; s.slopeT2 = 0; s.boost2 = 0;
     }, false},
    {"held", [](State& s) { s.held = 0; }, false},
    {"grounded", [](State& s) { s.grounded = 0; }, false},
    {"press", [](State& s) { s.ringHold = 0; s.pressSpent = 0; }, true},
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
    {"coins", [](State& s) { s.coins = 0; s.items = 0; s.taps = 0; s.tapsX[0] = s.tapsX[1] = s.tapsX[2] = 0; }, true},
    {"latch", [](State& s) { s.portalLatch = GravLatch{}; s.portalLatch2 = GravLatch{}; }, true},
    {"fireB", [](State& s) { for (auto& f : s.fireB) f = 0; }, true},
    // --shadowstep: the shadow bits of the key (shadow_lite.hpp slKeyBits), all together and one part at a time
    {"slall", [](State& s) { s.slRef = 0; s.slRef2 = 0; }, true},
    {"slpress", [](State&) {}, false, 0xF & ~1},
    {"slhold", [](State&) {}, false, 0xF & ~2},
    {"sllast", [](State&) {}, false, 0xF & ~4},
    {"sldash", [](State&) {}, false, 0xF & ~8},
    // the press part one byte at a time (+0x98d H, +0x98e S, +0x98f Hp, +0x990 Rp, +0x991 Hm, +0x992 Sm, +0x993 slot A)
    {"sl:H", [](State&) {}, false, -1, 0x7F & ~1},
    {"sl:S", [](State&) {}, false, -1, 0x7F & ~2},
    {"sl:Hp", [](State&) {}, false, -1, 0x7F & ~4},
    {"sl:Rp", [](State&) {}, false, -1, 0x7F & ~8},
    {"sl:Hm", [](State&) {}, false, -1, 0x7F & ~16},
    {"sl:Sm", [](State&) {}, false, -1, 0x7F & ~32},
    {"sl:slotA", [](State&) {}, false, -1, 0x7F & ~64},
    {"sl:HS", [](State&) {}, false, -1, 0x7F & ~3},
    {"sl:HpRp", [](State&) {}, false, -1, 0x7F & ~12},
    {"sl:HmSmA", [](State&) {}, false, -1, 0x7F & ~(16 | 32 | 64)},
    {"sl:Hp+Rp+Hm+Sm+A", [](State&) {}, false, -1, 0x7F & ~(4 | 8 | 16 | 32 | 64)},
    // the quantisation of y / vy, coarser by 4x / 4x (a census of what a coarser bin would merge, not a rule)
    {"y4", [](State& s) { s.y = std::floor(s.y / 2.0f) * 2.0f; s.y2 = std::floor(s.y2 / 2.0f) * 2.0f; }, false},
    {"vy4", [](State& s) { s.vy = std::floor(s.vy / 0.4f) * 0.4f; s.vy2 = std::floor(s.vy2 / 0.4f) * 0.4f; }, false},
    // the old model's press terms together (held / action / ringHold / pressSpent / rHover / dashing): what the shadow bits make redundant, read with slall
    {"oldpress", [](State& s) { s.held = 0; s.action = 0; s.ringHold = 0; s.pressSpent = 0; s.rHover = 0; s.dashing = 0; s.dashing2 = 0; s.jumpBuf = 0; }, false},
    {"oldpress+sl", [](State& s) { s.held = 0; s.action = 0; s.ringHold = 0; s.pressSpent = 0; s.rHover = 0; s.dashing = 0; s.dashing2 = 0; s.jumpBuf = 0; s.slRef = 0; s.slRef2 = 0; }, false},
    // single old terms
    {"slopeT", [](State& s) { s.slopeT = 0; s.slopeT2 = 0; s.rideLanded = 0; s.rideLanded2 = 0; s.slopeUid0 = 0; s.slopeUidNow = 0; }, false},
    {"taps", [](State& s) { s.taps = 0; s.tapsX[0] = s.tapsX[1] = s.tapsX[2] = 0; }, false},
    {"seatT", [](State& s) { s.seatT = 0; }, false},
    {"dx2", [](State& s) { s.dx2 = 0.f; }, false},
    {"revOff", [](State& s) { s.revOff = 0.f; }, false},
};
constexpr int kCensusN = (int)(sizeof(kCensusFields) / sizeof(kCensusFields[0]));

struct SearchCensus {
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
    long long sib[9][2][7] = {};   // [6]: the cells differ only in the shadow bits
    long long single[9][2] = {};   // parents that expanded only one input (by the same split)
    long long twin[9][2] = {};     // same cell AND bit-equal y, vy, x, y2, vy2, rot
    // cap coverage, sampled cap layers: [0] 2 px x 0.5 vy bins, [1] 8 px x 2.0
    long long covLayers = 0, covPre[2] = {}, covKept[2] = {}, covAlt[2] = {};
    // sampled layers: the y / vy bins of the key. cells that have an adjacent-bin sibling with the rest of the key equal (boundary splits); states whose key equals another's while their primary terms
    // (yq, vq, xq, mini, flip, mode, dual, the pair's bins) differ (a collision of the xor-fold, counted by content); a two-stage key's estimate: the first stage = the rest of the key
    // (y and vy put to 0), the second = the continuous pair covered by boxes m x the current bin (greedy centres).
    long long bndCells = 0, bndWithNbr = 0, collStates = 0, collBad = 0, stage1 = 0;
    long long est[3] = {}, estCells = 0;   // m = 1, 2, 4
    long long estByMode[9][3] = {}, estCellsByMode[9] = {};
    // --censuslayers: one row per layer. preM / postM by mode ([8] = dual, counted as well as its mode).
    struct LayerRow {
        long long t = 0, born = 0, died = 0;
        size_t cap = 0, pre = 0, post = 0;
        float maxX = 0.f;
        long long preM[9] = {}, postM[9] = {};
    };
    std::vector<LayerRow> rows;
    LayerRow curRow;
    bool curOpen = false;

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
        if (g_censusLayersOn) {
            curRow = LayerRow();
            curRow.t = t;
            curRow.born = born;
            curRow.died = dead;
            curRow.pre = pre.size();
            for (const State& s : pre) {
                if (s.mode < 8) ++curRow.preM[s.mode];
                if (s.dual) ++curRow.preM[8];
                if (s.xAbs > curRow.maxX) curRow.maxX = (float)s.xAbs;
            }
            curOpen = true;
        }
        if (!sample) return;
        ++sampled;
        std::unordered_set<uint64_t> full, cor, w[kCensusN];
        std::unordered_set<uint64_t> mf[9], mx[9], mv[9], mc[9];
        std::vector<std::unordered_set<uint64_t>> mw(9 * kCensusN);
        full.reserve(pre.size() * 2);
        cor.reserve(pre.size() * 2);
        for (auto& s : w) s.reserve(pre.size() * 2);
        for (const State& s : pre) {
            const uint64_t g = groupOf(s, curDx) * 0xD6E8FEB86659FD93ull;
            const int m = s.dual ? 8 : (s.mode < 8 ? s.mode : 0);
            const uint64_t kf = keyOf(s, t) ^ g;
            full.insert(kf);
            mf[m].insert(kf);
            State c = s;
            for (int f = 0; f < kCensusN; ++f) {
                State d = s;
                kCensusFields[f].canon(d);
                const uint32_t savedMask = gdcol::g_slKeyMask, savedBytes = gdcol::g_slPressBytes;
                if (kCensusFields[f].slMask >= 0) gdcol::g_slKeyMask = (uint32_t)kCensusFields[f].slMask;
                if (kCensusFields[f].slBytes >= 0) gdcol::g_slPressBytes = (uint32_t)kCensusFields[f].slBytes;
                const uint64_t kd = keyOf(d, t) ^ g;
                gdcol::g_slKeyMask = savedMask;
                gdcol::g_slPressBytes = savedBytes;
                w[f].insert(kd);
                mw[(size_t)(m * kCensusN + f)].insert(kd);
                if (f == 0) mx[m].insert(kd);
                if (f == 1) mv[m].insert(kd);
                if (kCensusFields[f].core) kCensusFields[f].canon(c);
            }
            const uint64_t kc = keyOf(c, t) ^ g;
            cor.insert(kc);
            mc[m].insert(kc);
        }
        {   // boundary splits, collisions and the two-stage estimate (see the members)
            struct CellYV { int yq, vq; float y, vy; int m; };
            std::unordered_map<uint64_t, std::vector<CellYV>> rest;
            std::unordered_map<uint64_t, std::array<int64_t, 9>> prim;
            for (const State& s : pre) {
                const uint64_t g = groupOf(s, curDx) * 0xD6E8FEB86659FD93ull;
                const uint64_t kf = keyOf(s, t) ^ g;
                const bool flying = (s.mode == 1 || s.mode == 3);
                const double ys = flying ? g_shipYq : g_cubeYq, vs = flying ? g_shipVq : g_cubeVq;
                const bool flyX = (s.mode == 1 || s.mode == 3 || s.mode == 4 || s.mode == 7);
                const int32_t yq = (int32_t)std::lround(s.y * ys), vq = (int32_t)std::lround(s.vy * vs);
                const int32_t xq = (int32_t)std::lround(s.xAbs * (flyX ? g_keyXqFly : g_keyXq));
                std::array<int64_t, 9> tup = {yq, vq, xq, s.mini, s.flip, s.mode, s.dual,
                    s.dual ? (int64_t)std::lround(s.y2 * (s.freeHalf ? ys * g_dualFreeQ : ys)) : 0,
                    s.dual ? (int64_t)std::lround(s.vy2 * (s.freeHalf ? vs * g_dualFreeQ : vs)) : 0};
                ++collStates;
                auto pit = prim.find(kf);
                if (pit != prim.end()) { if (pit->second != tup) ++collBad; continue; }   // a later state of a cell: its content must be the cell's
                prim.emplace(kf, tup);
                State d = s;
                d.y = d.vy = d.y2 = d.vy2 = 0.f;
                rest[keyOf(d, t) ^ g].push_back({yq, vq, s.y, s.vy, s.dual ? 8 : (s.mode < 8 ? s.mode : 0)});
            }
            stage1 += (long long)rest.size();
            for (auto& kv : rest) {
                auto& v = kv.second;
                bndCells += (long long)v.size();
                estCells += (long long)v.size();
                std::unordered_set<uint64_t> have;
                for (const auto& c : v) have.insert(((uint64_t)(uint32_t)c.yq << 32) | (uint32_t)c.vq);
                for (const auto& c : v) {
                    bool nb = false;
                    for (int dy = -1; dy <= 1 && !nb; ++dy)
                        for (int dv = -1; dv <= 1 && !nb; ++dv)
                            if ((dy || dv) && have.count(((uint64_t)(uint32_t)(c.yq + dy) << 32) | (uint32_t)(c.vq + dv))) nb = true;
                    if (nb) ++bndWithNbr;
                }
                const int m0 = v.empty() ? 0 : v[0].m;
                estCellsByMode[m0] += (long long)v.size();
                std::sort(v.begin(), v.end(), [](const CellYV& a, const CellYV& b) { return a.y < b.y || (a.y == b.y && a.vy < b.vy); });
                for (int mi = 0; mi < 3; ++mi) {
                    const double mul = (double)(1 << mi);
                    const bool flying = (m0 == 1 || m0 == 3);
                    const double wy = mul / (flying ? g_shipYq : g_cubeYq), wv = mul / (flying ? g_shipVq : g_cubeVq);
                    std::vector<std::pair<float, float>> centres;
                    for (const auto& c : v) {
                        bool cov = false;
                        for (const auto& k : centres)
                            if (std::fabs((double)c.y - (double)k.first) <= wy * 0.5 && std::fabs((double)c.vy - (double)k.second) <= wv * 0.5) { cov = true; break; }
                        if (!cov) centres.emplace_back(c.y, c.vy);
                    }
                    est[mi] += (long long)centres.size();
                    estByMode[m0][mi] += (long long)centres.size();
                }
            }
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
                  uint64_t ka, uint64_t kb, long long t) {
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
        else if (State xs = ca, ys = cb; (xs.slRef = ys.slRef = 0, xs.slRef2 = ys.slRef2 = 0, keyOf(xs, t) == keyOf(ys, t))) c = 6;   // --shadowstep: apart only by what the shadow's buttons wrote
        else {
            State x = ca, y = cb;
            x.ringHold = y.ringHold = 0;
            x.pressSpent = y.pressSpent = 0;
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
        if (g_censusLayersOn && curOpen) {
            curRow.post = post.size();
            for (const State& s : post) {
                if (s.mode < 8) ++curRow.postM[s.mode];
                if (s.dual) ++curRow.postM[8];
            }
            rows.push_back(curRow);
            curOpen = false;
        }
    }

    // --censuslayers: append the call's rows to the file (print only).
    void writeRows() const {
        if (!g_censusLayersOn || g_censusLayersPath.empty()) return;
        static int call = 0;
        ++call;
        std::FILE* f = std::fopen(g_censusLayersPath.c_str(), "ab");
        if (!f) {
            std::printf("censuslayers: cannot open %s\n", g_censusLayersPath.c_str());
            return;
        }
        std::fprintf(f, "# call %d layers=%zu\n", call, rows.size());
        std::fprintf(f, "# t\tcap\tpre\tpost\tborn\tdied\tmaxX\tpre:cube ship ball ufo wave robot spider swing dual\tpost:(same)\n");
        for (const LayerRow& r : rows) {
            std::fprintf(f, "%lld\t%zu\t%zu\t%zu\t%lld\t%lld\t%.1f\t", r.t, r.cap, r.pre, r.post, r.born, r.died, (double)r.maxX);
            for (int m = 0; m < 9; ++m) std::fprintf(f, "%lld%c", r.preM[m], m == 8 ? '\t' : ' ');
            for (int m = 0; m < 9; ++m) std::fprintf(f, "%lld%c", r.postM[m], m == 8 ? '\n' : ' ');
        }
        std::fclose(f);
        std::printf("censuslayers: call %d, %zu layers -> %s\n", call, rows.size(), g_censusLayersPath.c_str());
    }

    void cap(size_t pre, size_t cap, long long deadThisLayer) {
        if (g_censusLayersOn && curOpen) curRow.cap = cap;
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
                const long long n = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6];
                if (!n && !single[m][g]) continue;
                std::printf("searchcensus: sib %-6s %s pairs=%lld single=%lld bothdead=%lld "
                            "onedead=%lld samecell=%lld pressonly=%lld heldonly=%lld distinct=%lld "
                            "slonly=%lld twin=%lld\n",
                            kModeName[m], g ? "ground" : "air   ", n, single[m][g], v[0], v[1],
                            v[2], v[3], v[4], v[5], v[6], twin[m][g]);
            }
        std::printf("searchcensus: y/vy bins: cells=%lld, with an adjacent-bin sibling (same rest of the key)=%lld (%.1f%%)\n", bndCells, bndWithNbr, bndCells ? 100.0 * (double)bndWithNbr / (double)bndCells : 0.0);
        std::printf("searchcensus: collisions: states=%lld, states whose key equals another's with other primary terms=%lld\n", collStates, collBad);
        std::printf("searchcensus: two-stage key: stage-1 groups=%lld for %lld cells (%.2f cells a group); stage-2 centres with boxes of 1x / 2x / 4x the current bins: %lld / %lld / %lld (%.1f%% / %.1f%% / %.1f%% of the cells)\n",
                    stage1, estCells, stage1 ? (double)estCells / (double)stage1 : 0.0, est[0], est[1], est[2],
                    estCells ? 100.0 * (double)est[0] / (double)estCells : 0.0, estCells ? 100.0 * (double)est[1] / (double)estCells : 0.0, estCells ? 100.0 * (double)est[2] / (double)estCells : 0.0);
        {
            static const char* kMN[9] = {"cube", "ship", "ball", "ufo", "wave", "robot", "spider", "swing", "dual"};
            for (int m = 0; m < 9; ++m)
                if (estCellsByMode[m])
                    std::printf("searchcensus: two-stage %-6s cells=%lld centres 1x/2x/4x = %.1f%% / %.1f%% / %.1f%%\n", kMN[m], estCellsByMode[m],
                                100.0 * (double)estByMode[m][0] / (double)estCellsByMode[m], 100.0 * (double)estByMode[m][1] / (double)estCellsByMode[m], 100.0 * (double)estByMode[m][2] / (double)estCellsByMode[m]);
        }
        std::printf("searchcensus: capcov layers=%lld bins2 pre=%lld kept=%lld sorted=%lld | "
                    "bins8 pre=%lld kept=%lld sorted=%lld\n",
                    covLayers, covPre[0], covKept[0], covAlt[0], covPre[1], covKept[1], covAlt[1]);
    }
};

}  // namespace dp
