#pragma once
#include "dp/stairs.hpp"

namespace dp {

// --groups <file>: the MOD's `grouptrace=1` output, `tick,uid,cx,cy,w,h`.
// Keyed by uid because that is the only identifier both dumps share.
// --groups can be given more than once, and a later file OVERRIDES an earlier
// one for as far as it reaches. That is not a convenience -- it is the only way
// the timeline can be right.
//
// The cheap way to record it is one bootstrap pass with an empty plan and
// `nodeath=1`, which reaches the level end and fires everything that fires off
// the screen. But not every trigger does: lv19's gate at x=27,705 is opened by
// the player getting there, and in the bootstrap pass (where the player is dead
// and falling) it never opens at all. Measured on the two recordings of the
// same level: the bootstrap has ONE sample for those two blocks, the real
// replay has 99 each, sliding the 60 px gap open from t=19,606 to t=19,704 --
// exactly when the player arrives.
//
// So the driver records again on every GD replay of its own plan, and that
// recording wins wherever it reaches (its last tick). Beyond that the bootstrap
// fills in, and beyond THAT every object holds its last known rect.
using GroupTimeline = std::unordered_map<int, std::vector<DynSample>>;
inline GroupTimeline loadGroupTimeline(const std::string& path) {
    GroupTimeline g;
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "groups: cannot open %s\n", path.c_str());
        return g;
    }
    std::string line;
    std::getline(in, line);   // header
    long long rows = 0;
    while (std::getline(in, line)) {
        int t = 0, uid = 0, on = 1;
        float cx = 0, cy = 0, w = 0, h = 0, rot = 0;
        // a run that is cut off mid-write leaves one short line; skip it.
        // `on` and `rot` are optional so a trace recorded before those columns
        // existed still loads (as "always there, never turned", which is what it
        // used to mean).
        const int n = std::sscanf(line.c_str(), "%d,%d,%f,%f,%f,%f,%d,%f", &t,
                                  &uid, &cx, &cy, &w, &h, &on, &rot);
        if (n < 6) continue;
        g[uid].push_back({t, cx, cy, w * 0.5f, h * 0.5f,
                          (uint8_t)(on ? 1 : 0), rot});
        ++rows;
    }
    for (auto& kv : g)
        std::sort(kv.second.begin(), kv.second.end(),
                  [](const DynSample& a, const DynSample& b) { return a.t < b.t; });
    std::printf("groups: %lld samples for %zu objects (%s)\n", rows, g.size(),
                path.c_str());
    return g;
}

// `over` wins for every tick it covers; `base` supplies the rest.
inline void overlayGroupTimeline(GroupTimeline& base, const GroupTimeline& over) {
    // How far the overriding recording actually reaches. One number for the
    // whole file, not per object: an object that simply did not move inside the
    // covered window has one sample and must NOT be treated as uncovered.
    int cover = -1;
    for (const auto& kv : over)
        if (!kv.second.empty()) cover = std::max(cover, kv.second.back().t);
    if (cover < 0) return;
    for (const auto& kv : over) {
        std::vector<DynSample> merged = kv.second;
        auto it = base.find(kv.first);
        if (it != base.end())
            for (const DynSample& s : it->second)
                if (s.t > cover) merged.push_back(s);
        std::sort(merged.begin(), merged.end(),
                  [](const DynSample& a, const DynSample& b) { return a.t < b.t; });
        base[kv.first] = std::move(merged);
    }
    std::printf("groups: overlay covers t<=%d\n", cover);
}

}  // namespace dp
