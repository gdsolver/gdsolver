#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace p1::solver {

// Identities are allocated after exact-byte comparisons, not inferred from file hashes.
struct PlanContext {
    uint64_t level = 0;
    unsigned fp = 0;
    std::string config;
    std::vector<std::pair<std::string, uint64_t>> files;
    std::vector<std::string> args;
    bool valid = false;
    // Missing inputs and incomplete contexts must never suppress a game verification.
    bool operator==(const PlanContext& b) const {
        return valid && b.valid && level == b.level && fp == b.fp
            && config == b.config && files == b.files && args == b.args;
    }
};
using PlanEdges = std::vector<std::pair<long long, int>>;

// A bounded, cold-session-only list of full plans the game actually killed.
class FailedPlans {
    struct Entry { PlanContext context; PlanEdges edges; long long death; };
    std::vector<Entry> entries_;
public:
    // New sessions, including explicit resumes, do not inherit failure claims.
    void clear() { entries_.clear(); }
    // Compare every edge, not a plan hash or just the new tail of a different prefix.
    long long failedAt(const PlanContext& context, const PlanEdges& edges) const {
        for (const auto& e : entries_)
            if (e.context == context && e.edges == edges) return e.death;
        return -1;
    }
    // Only the caller's genuine collision verdict may enter this list.
    void remember(const PlanContext& context, const PlanEdges& edges, long long death) {
        if (!context.valid || death < 1 || failedAt(context, edges) >= 0) return;
        if (entries_.size() == 16) entries_.erase(entries_.begin());
        entries_.push_back({context, edges, death});
    }
};
}  // namespace p1::solver
