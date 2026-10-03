#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>

namespace p1::solver {

// Recover the forward input path after an ancestor, excluding the origin's held input.
template <class Node>
void sectionReplayPath(const std::vector<Node>& nodes, int leaf, int origin,
                       std::vector<int>& path) {
    path.clear();
    for (int i = leaf; i > 0 && i != origin; i = nodes[(std::size_t)i].parent)
        path.push_back(i);
    std::reverse(path.begin(), path.end());
}

struct SectionReplayResult {
    bool alive;
    int origin;
    bool headRetried;
};

// An anchored death cannot discard the protected route until one head replay also fails.
// The callback leaves the game at its final state; never restore the failed trial afterwards.
template <class Replay>
SectionReplayResult checkSectionReplay(int origin, bool protectedRoute, Replay&& replay) {
    const bool alive = replay(origin);
    if (alive || origin == 0 || !protectedRoute) return {alive, origin, false};
    return {replay(0), 0, true};
}

enum class SectionExitResult { Solved = 0, Unverified = 1, Continue = 2 };

// Failed fixed-input trials are inconclusive; restore a verified exit before branching on it.
template <class Replay>
SectionExitResult finishSectionExit(bool graceAlive, Replay&& replay) {
    if (graceAlive) return SectionExitResult::Solved;
    return replay() ? SectionExitResult::Continue : SectionExitResult::Unverified;
}

// Budget exhausted air exits still branch; controllable exits can earn a verified handoff.
inline bool sectionExitNeedsCheck(long long failed, long long budget, bool controllable) {
    return budget <= 0 || failed < budget || controllable;
}

// Bound costly shortcuts per layer, reserving most attempts for exits that regain control.
struct SectionExitChecks {
    int used = 0, airUsed = 0;

    // Skipping a shortcut must leave the child eligible for ordinary frontier expansion.
    bool take(long long failed, long long budget, bool controllable) {
        if (used >= 4 || (!controllable && airUsed >= 1)
            || !sectionExitNeedsCheck(failed, budget, controllable)) return false;
        ++used;
        if (!controllable) ++airUsed;
        return true;
    }
};

// Automatic rungs may look ahead briefly; explicit section horizons remain unchanged.
struct SectionForwardSearch {
    bool enabled;
    int initial, horizon, limit, stride, extensions = 0;

    // Reuse the window's spare depth in chunks, with at most 240 additional input ticks.
    SectionForwardSearch(int base, int target, int grace, bool automatic)
        : enabled(automatic && target > 0), initial(base), horizon(base),
          limit(base + (enabled ? std::min(240, std::max(0, grace)) : 0)),
          stride(std::max(1, std::min(50, base - target))) {}

    // Only a complete head replay that survives AND crosses the exit authorizes more work.
    bool extend(bool verifiedExit) {
        if (!verifiedExit || horizon >= limit) return false;
        horizon = std::min(limit, horizon + stride);
        ++extensions;
        return true;
    }
};

} // namespace p1::solver
