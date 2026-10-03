#pragma once

namespace p1::solver {

// Consecutive near-wall deaths, even when point fixes keep inching the wall forward.
struct RepairProgress {
    static constexpr long long kMinAdvance = 30;
    static constexpr int kSlowRounds = 4;
    long long wall = -1;
    int rounds = 0;

    // A fresh run or a successful splice must earn its next handoff again.
    void reset() { wall = -1; rounds = 0; }

    // Count only deaths near the verified wall; a large advance or regression clears the streak.
    void observe(long long best, long long death) {
        const bool slow = wall >= 0 && best >= wall && best - wall < kMinAdvance;
        if (best < 0 || death < best - kMinAdvance || !slow) rounds = 0;
        else if (rounds < kSlowRounds) ++rounds;
        wall = best;
    }

    // This is a resource handoff, never a verdict that the model or level is unsolvable.
    bool shouldSearch() const { return rounds >= kSlowRounds; }
};

// A wall has a finite sequence of strictly earlier section heads, shared by normal and final tries.
struct RepairBacktrack {
    static constexpr long long kFirstBack = 200;
    static constexpr long long kDoubleUpTo = 800;
    long long wall = -1;
    long long lastHead = -1;
    int tries = 0;

    // A new session must not inherit another level's failed windows.
    void reset() { wall = -1; lastHead = -1; tries = 0; }

    // Small advances retain the failed heads; a new wall starts at the shortest window.
    int level(long long now) const {
        if (wall < 0 || now < wall || now - wall >= RepairProgress::kMinAdvance) return 0;
        return lastHead == 1 ? -1 : tries;
    }

    // Only an already-started, unfinished sequence may outlive the ordinary repair budget.
    bool canContinue(long long now) const { return lastHead > 1 && level(now) > 0; }

    // Book a valid level() result once, even if the requested window later cannot fit.
    void begin(long long now, int next) {
        if (next == 0) { wall = now; lastHead = -1; }
        tries = next + 1;
    }

    // Double short windows, then add a fixed span rather than doubling long searches.
    static long long back(int level) {
        long long span = kFirstBack;
        for (int i = 0; i < level; ++i)
            span = span < kDoubleUpTo ? span * 2 : span + kDoubleUpTo;
        return span;
    }

    // A fixup-selected head may already be earlier than the nominal window.
    long long earlierHead(long long head, int next) const {
        if (next > 0 && lastHead > 0 && head >= lastHead)
            return lastHead - (back(next) - back(next - 1));
        return head;
    }
};

}  // namespace p1::solver
