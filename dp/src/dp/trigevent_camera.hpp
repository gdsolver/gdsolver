#pragma once
// The camera summary of the world key and the --worldkey instrument's counts (restructure draft sections 7.6 / 8.1, audit AUD-857). Pure functions; dptest_main.cpp holds the fixtures.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace dp {
namespace gdtrig {

inline uint64_t camMix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); return h * 0xff51afd7ed558ccdULL; }

// dxHist: a digest of the CURVE x(t) the lineage has run, taken at its change points -- when the speed changes (dx), the TICK of the change and the new dx; when x jumps (a teleport, a step of the position far beyond
// one tick's advance), the tick and where it landed. A history A -> B at tick 100 is not A -> B at tick 200, and two histories that differ only in a jump of x are different. Not captured: a time-warp factor that
// scales the advance without changing dx (that step stays under the jump threshold of 8 px or 8 dx).
inline uint64_t dxHistStep(uint64_t hist, float parentDx, float kidDx, double parentX, double kidX, long long t) {
    if (kidDx > 0.f && kidDx != parentDx) {
        uint32_t db; std::memcpy(&db, &kidDx, 4);
        return camMix(camMix(camMix(hist, 1u), (uint64_t)t), db);
    }
    const double lim = 8.0 * (kidDx > 1.f ? (double)kidDx : 1.0);
    const double adv = kidX - parentX;
    if (adv > lim || adv < -lim) {
        float xf = (float)kidX; uint32_t xb; std::memcpy(&xb, &xf, 4);
        return camMix(camMix(camMix(hist, 2u), (uint64_t)t), xb);
    }
    return hist;
}

// The two counts of the instrument over a list of (dedupe key, dxHist): K = distinct keys, KD = distinct (key, dxHist). It runs on the CHILDREN of a group BEFORE its dedupe, so two children with the same key and
// different histories are two in KD and one in K; after the dedupe only one of them is left and the count could not see the split (the first version counted the survivors).
struct KeyCount { long long k = 0, kd = 0; };
inline KeyCount countKeys(const std::vector<std::pair<uint64_t, uint64_t>>& kh) {
    KeyCount c;
    std::vector<uint64_t> a, b;
    a.reserve(kh.size()); b.reserve(kh.size());
    for (const auto& p : kh) { a.push_back(p.first); b.push_back(camMix(p.first, p.second)); }
    std::sort(a.begin(), a.end()); a.erase(std::unique(a.begin(), a.end()), a.end());
    std::sort(b.begin(), b.end()); b.erase(std::unique(b.begin(), b.end()), b.end());
    c.k = (long long)a.size(); c.kd = (long long)b.size();
    return c;
}

}  // namespace gdtrig
}  // namespace dp