#pragma once
// P5-1 (restructure draft section 7.6): the world key of a State as a pure function of its own fields, and the table that interns it -- observation only for now (--worldkeytable).
// The key is a DERIVED value, so the State carries no second copy (draft 1.7). Components in this step: the touch history (trig mask + fireB), the item counts (coins, taps, tapsX, items), the Rotate Gameplay
// queue (rotSpent, rotChan, rotRev) and the camera summary dxHist. NOT yet in it: the group on / off bits (tog) and the x-crossing gate (xgate), which wait for the Tap / orb event entry point (P4), and the seed slot.
#include <cstdint>
#include <cstdio>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "dp/state.hpp"
#include "dp/trigevent_camera.hpp"

namespace dp {

inline uint64_t worldKeyOf(const State& s, bool withCamera = true, bool withTog = false) {
    using gdtrig::camMix;
    uint64_t h = 0x574f524c44ULL;
    for (int k = 0; k < (kTouchBits + 63) / 64; ++k) h = camMix(h, s.trig.word(k));
    for (int i = 0; i < kTouchBits; ++i) h = camMix(h, s.fireB[i]);
    h = camMix(h, (uint64_t)s.coins | ((uint64_t)s.taps << 8) | ((uint64_t)s.items << 16) | ((uint64_t)s.tapsX[0] << 32) | ((uint64_t)s.tapsX[1] << 40) | ((uint64_t)s.tapsX[2] << 48));
    h = camMix(h, s.rotSpent);
    h = camMix(h, (uint64_t)s.rotChan | ((uint64_t)s.rotRev << 8));
    if (withCamera) h = camMix(h, s.dxHist);
    // The groups' on / off bits and the x-gate digest (restructure draft section 7.7), for a caller that asks for them.
    if (withTog) { h = camMix(h, s.tog.w[0]); h = camMix(h, s.tog.w[1]); h = camMix(h, s.xgate); }
    return h;
}

// The criteria for giving up the camera summary in the world key (the draft's option (イ)), applied per 2000-tick bucket of an offline call, on ABSOLUTE numbers (the first version compared W with W0 -- the world key
// without the camera summary -- and flagged nearly every bucket because W0 is 1 wherever nothing has fired: a ratio against 1 says nothing about cost). A bucket is marked SWITCH when, with at least 20 states a layer on average,
//   (1) the mean number of worlds a layer is above 100 -- the per-layer cost of stepping and placing worlds is no longer small beside the states, or
//   (2) W / states is above 0.25 -- fewer than four states share a world on average, so sharing gains little, or
//   (3) more than 10% of the layers hold more worlds than half their states -- the layers where the world is nearly the state.
// W / W0 is still printed, as information. The ratio is the split under the OLD dedupe; the growth of later layers under a new key is measured with the arm that keeps the new key's survivors (P5-2), and until then
// "does not grow" is not claimed.
struct WorldTable {
    struct Info { long long first = 0, last = 0, layers = 0; };
    struct Bk { long long layers = 0, states = 0, w = 0, w0 = 0, maxW = 0, wOverHalf = 0; };
    std::unordered_map<uint64_t, uint32_t> ids;
    std::vector<Info> info;
    std::unordered_set<uint64_t> edges;
    std::unordered_map<uint32_t, uint32_t> prev, cur;   // arena node of a frontier state -> its world id
    std::unordered_map<uint32_t, std::unordered_set<uint32_t>> parentsOf;
    std::map<long long, Bk> bk;
    long long lastT = -2, lastLayerT = 0, gcResets = 0, unknownParent = 0, statesSeen = 0;
    size_t lastArena = 0;
    bool on = false;
    bool withTog = false;   // the key also holds State::tog / xgate (set by the search before each layer)

    template <class Arena>
    void layer(long long t, const std::vector<State>& nxt, const Arena& arena) {
        if (!on || nxt.empty()) return;
        if (arena.size() < lastArena) { prev.clear(); cur.clear(); ++gcResets; }
        lastArena = arena.size();
        prev.swap(cur); cur.clear();
        std::unordered_set<uint64_t> w, w0;
        for (const State& s : nxt) {
            const uint64_t k = worldKeyOf(s, true, withTog);
            w.insert(k); w0.insert(worldKeyOf(s, false, withTog));
            auto it = ids.find(k);
            uint32_t id;
            if (it == ids.end()) { id = (uint32_t)info.size(); ids.emplace(k, id); Info f; f.first = t; info.push_back(f); } else id = it->second;
            Info& f = info[id];
            if (f.last != t || f.layers == 0) ++f.layers;
            f.last = t;
            ++statesSeen;
            if (s.parent < arena.size()) {
                const auto pi = prev.find(arena[s.parent].parent());
                if (pi != prev.end()) {
                    if (edges.insert(((uint64_t)pi->second << 32) | id).second) parentsOf[id].insert(pi->second);
                } else ++unknownParent;
            }
            cur[s.parent] = id;
        }
        lastLayerT = t;
        Bk& b = bk[t / 2000];
        ++b.layers; b.states += (long long)nxt.size(); b.w += (long long)w.size(); b.w0 += (long long)w0.size();
        b.maxW = std::max(b.maxW, (long long)w.size());
        if (w.size() * 2 > nxt.size()) ++b.wOverHalf;
    }
    ~WorldTable() {
        if (!on || bk.empty()) return;
        long long dead = 0, multi = 0, lifeSum = 0, lifeMax = 0;
        for (size_t i = 0; i < info.size(); ++i) {
            const long long life = info[i].last - info[i].first + 1; lifeSum += life; lifeMax = std::max(lifeMax, life);
            if (info[i].last < lastLayerT - 50) ++dead;
        }
        for (const auto& kv : parentsOf) if (kv.second.size() > 1) ++multi;
        std::printf("worldtable: %zu worlds seen (finished earlier %lld, alive at the end %zu; life in layers mean %.1f max %lld), %zu distinct parent edges, %lld worlds with more than one parent, %lld states with no known parent, %lld arena compactions\n",
                    info.size(), dead, info.size() - (size_t)dead, info.empty() ? 0.0 : (double)lifeSum / (double)info.size(), lifeMax, edges.size(), multi, unknownParent, gcResets);
        for (const auto& kv : bk) {
            const Bk& b = kv.second;
            const double wr = b.w0 ? (double)b.w / (double)b.w0 : 0.0, sr = b.states ? (double)b.w / (double)b.states : 0.0;
            const double meanStates = b.layers ? (double)b.states / (double)b.layers : 0.0, meanW = b.layers ? (double)b.w / (double)b.layers : 0.0;
            const bool sw = meanStates >= 20.0 && (meanW > 100.0 || sr > 0.25 || (double)b.wOverHalf > 0.10 * (double)b.layers);
            std::printf("worldtable: bucket t=%lld..%lld: %lld layers, %.1f states a layer, worlds %.2f (max %lld), without the camera summary %.2f, W/W0 %.4f, W/states %.4f, %lld layers with worlds over half the states%s\n",
                        kv.first * 2000, kv.first * 2000 + 1999, b.layers, b.layers ? (double)b.states / (double)b.layers : 0.0, b.layers ? (double)b.w / (double)b.layers : 0.0, b.maxW,
                        b.layers ? (double)b.w0 / (double)b.layers : 0.0, wr, sr, b.wOverHalf, sw ? "   SWITCH" : "");
        }
    }
};
inline WorldTable g_worldTable;

}  // namespace dp