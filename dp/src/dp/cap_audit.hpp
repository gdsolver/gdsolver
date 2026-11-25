#pragma once
#include "dp/state.hpp"
#include "dp/worldkey.hpp"
#include "dp/modifiers.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <unordered_map>
#include <vector>

namespace dp {

// --capaudit (default off; print only -- nothing here is read back by the search, so the plan is the same with the flag on or off).
//
// What the alive cap does to the WORLDS of a layer (restructure draft 7.6, P5-3). The cap is one count for the whole layer; it keeps states by a water-fill over classes (search_dp.hpp classOf) and a stride
// inside each class, and knows nothing of worlds. On every --capauditevery-th layer the cap cut, the layer's states are grouped by worldKeyOf (the derived world key, with the camera summary), and the kept set
// (keepIdx) is laid over it:
//   worlds       distinct world keys among the layer's states (the pre-cap frontier `nxt`)
//   wiped        worlds none of whose states the cap kept (their states are all dropped)
//   wipedStates  the states those worlds held
//   kept ratio   per world, kept / held: 0, (0, 25 %), [25, 50 %), [50, 100 %), 100 %
// per 2000-tick bucket of the layer's tick, and in all.
//
// WHICH WORLD (audit AUD-861): the key is worldKeyOf (touch history, item counts, rotation queue, and the camera summary dxHist) plus State::tog / xgate when the capaudit asks for them. Only components the run
// actually UPDATES count: dxHist is kept only under --worldkey (without it the camera part is always 0, so the "world" is coarser than the full key), tog / xgate in every run. worldKeyOf itself
// does not read tog / xgate, so capaudit mixes them in. capAuditPrint states which components were live in the run; a result from a run without them is a result for a COARSE world and says nothing about
// the full key (a wiped-world count there is a lower bound for it).
inline bool g_capAudit = false;
inline int g_capAuditEvery = 10;

struct CapAuditBucket {
    long long layers = 0, states = 0, kept = 0, worlds = 0, wiped = 0, wipedStates = 0, layersWithWiped = 0, maxWorlds = 0, maxShareMilli = 0;
    long long ratio[5] = {0, 0, 0, 0, 0};
};
struct CapAuditStats {
    long long capped = 0;   // layers the cap cut
    long long sampled = 0;  // ...of which looked at
    std::map<long long, CapAuditBucket> bk;
};
inline CapAuditStats g_capAuditStats;

// One capped layer. `states` = the pre-cap layer, `keep` = indices into it of the states the cap kept. Pure besides the counters.
// the world of a state as capaudit counts it
inline uint64_t capAuditWorld(const State& s, bool withCamera) {
    uint64_t k = worldKeyOf(s, withCamera);
    k ^= (s.tog.w[0] * 0x9E3779B97F4A7C15ull) ^ (s.tog.w[1] * 0xC2B2AE3D27D4EB4Full) ^ (s.xgate * 0x165667B19E3779F9ull);
    return k;
}

inline void capAuditLayer(long long t, const std::vector<State>& states, const std::vector<uint32_t>& keep, bool withCamera = true) {
    ++g_capAuditStats.capped;
    if (g_capAuditEvery > 1 && (g_capAuditStats.capped % g_capAuditEvery) != 1) return;
    ++g_capAuditStats.sampled;
    std::vector<uint8_t> kept(states.size(), 0);
    for (uint32_t i : keep) if (i < kept.size()) kept[i] = 1;
    std::unordered_map<uint64_t, std::pair<uint32_t, uint32_t>> w;   // world key -> (states, kept)
    w.reserve(states.size() / 4 + 8);
    for (size_t i = 0; i < states.size(); ++i) {
        auto& e = w[capAuditWorld(states[i], withCamera)];
        ++e.first;
        e.second += kept[i];
    }
    CapAuditBucket& b = g_capAuditStats.bk[t / 2000 * 2000];
    ++b.layers;
    b.states += (long long)states.size();
    b.kept += (long long)keep.size();
    b.worlds += (long long)w.size();
    b.maxWorlds = std::max(b.maxWorlds, (long long)w.size());
    long long wipedHere = 0, biggest = 0;
    for (const auto& kv : w) {
        const uint32_t n = kv.second.first, k = kv.second.second;
        biggest = std::max<long long>(biggest, n);
        if (k == 0) { ++b.wiped; ++wipedHere; b.wipedStates += n; }
        const double r = (double)k / (double)n;
        ++b.ratio[k == 0 ? 0 : r < 0.25 ? 1 : r < 0.5 ? 2 : r < 1.0 ? 3 : 4];
    }
    if (wipedHere) ++b.layersWithWiped;
    if (!states.empty()) b.maxShareMilli = std::max(b.maxShareMilli, biggest * 1000 / (long long)states.size());
}

inline void capAuditPrint() {
    if (!g_capAudit) return;
    const CapAuditStats& s = g_capAuditStats;
    std::printf("capaudit: capped layers %lld, sampled %lld (every %d)\n", s.capped, s.sampled, g_capAuditEvery);
    std::printf("capaudit: the world key counted = touch history + items + rotation queue + camera summary (dxHist: %s) + tog / xgate (%s)%s\n", g_worldKey ? "UPDATED (--worldkey)" : "NOT updated (no --worldkey: always 0)",
                "UPDATED", g_worldKey ? "" : "  => a COARSE world: not the full key");
    CapAuditBucket tot;
    for (const auto& kv : s.bk) {
        const CapAuditBucket& b = kv.second;
        std::printf("capaudit bucket t=%lld..%lld: %lld layers, states %.1f kept %.1f worlds %.1f (max %lld) wiped %.2f (in %lld layers; %.2f%% of the states), largest world %.1f%% of a layer (max), worlds by kept ratio 0 / <25%% / <50%% / <100%% / 100%%: %lld %lld %lld %lld %lld\n",
                    kv.first, kv.first + 1999, b.layers, (double)b.states / b.layers, (double)b.kept / b.layers, (double)b.worlds / b.layers, b.maxWorlds, (double)b.wiped / b.layers, b.layersWithWiped,
                    b.states ? 100.0 * (double)b.wipedStates / (double)b.states : 0.0, 0.1 * (double)b.maxShareMilli, b.ratio[0], b.ratio[1], b.ratio[2], b.ratio[3], b.ratio[4]);
        tot.layers += b.layers; tot.states += b.states; tot.kept += b.kept; tot.worlds += b.worlds; tot.wiped += b.wiped; tot.wipedStates += b.wipedStates; tot.layersWithWiped += b.layersWithWiped;
        tot.maxWorlds = std::max(tot.maxWorlds, b.maxWorlds); tot.maxShareMilli = std::max(tot.maxShareMilli, b.maxShareMilli);
        for (int i = 0; i < 5; ++i) tot.ratio[i] += b.ratio[i];
    }
    if (tot.layers)
        std::printf("capaudit total: %lld layers, states %.1f kept %.1f worlds %.1f (max %lld) wiped %.2f (in %lld layers; %.2f%% of the states), largest world %.1f%% (max), worlds by kept ratio: %lld %lld %lld %lld %lld\n",
                    tot.layers, (double)tot.states / tot.layers, (double)tot.kept / tot.layers, (double)tot.worlds / tot.layers, tot.maxWorlds, (double)tot.wiped / tot.layers, tot.layersWithWiped,
                    100.0 * (double)tot.wipedStates / (double)tot.states, 0.1 * (double)tot.maxShareMilli, tot.ratio[0], tot.ratio[1], tot.ratio[2], tot.ratio[3], tot.ratio[4]);
}

}  // namespace dp
