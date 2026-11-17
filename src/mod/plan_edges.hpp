#pragma once
// A plan's input edges: the struct, the reader of one `input=` line and the writer of a plan's lines. Pure standard C++ (no game types), so tools/plan_roundtrip_test.cpp
// checks the pair without the game. Included by config.hpp (InputCmd) and plan_io.hpp (the file reader and writer).
#include <ostream>
#include <vector>

namespace p1 {

struct InputCmd { int step; bool down; bool p2 = false; };

// One `input=<tick>,<mask>` line as edges. The mask is the buttons held from that tick; `prevMask` is the one before it (0 at the start of a
// plan), and is updated. Player 1: a line is an edge, as it always was -- unless player 2 is in the plan, where a line that changes only bit 1
// must not press player 1's button again while it is held. Player 2: an edge only where bit 1 changes. False (nothing added) when the mask
// holds a bit this version does not drive. Both readers of an `input=` line -- a plan file and the autorun cfg -- come through here: a reader
// that tests the mask against "1" alone reads every mask with bit 1 as a release.
inline bool pushInputMask(std::vector<InputCmd>& dst, int t, int d, int& prevMask) {
    if (d & ~3) return false;
    if (((d | prevMask) & 2) == 0 || ((d ^ prevMask) & 1)) dst.push_back({t, (d & 1) != 0});
    if ((d ^ prevMask) & 2) dst.push_back({t, (d & 2) != 0, true});
    prevMask = d;
    return true;
}

// Write a plan back out in the same format loadInputsFile reads. Used to keep a solution the
// mod solved and then SAW CLEAR THE LEVEL -- a plan that has not cleared is not a solution and
// must not be filed as one (the repository already carries 72 dead lineages from the days when
// anything got saved).
//
// A plan with no player 2 edge is written one line per edge, as ever. With one, a line is the buttons held from that tick (bit 0 player 1's
// jump, bit 1 player 2's), the edges of a tick merged into one line, which loadInputsFile reads back to the same edges.
inline void writeInputLines(std::ostream& f, const std::vector<InputCmd>& plan) {
    bool any2 = false;
    for (const InputCmd& c : plan) any2 = any2 || c.p2;
    if (!any2) {
        for (const InputCmd& c : plan) f << "input=" << c.step << "," << (c.down ? 1 : 0) << "\n";
        return;
    }
    // The edges of a tick are merged into one line, unless a button changes twice within the tick (a release and a press of player 2's button in one
    // tick, which GD takes as a press): the merged mask would then equal the one before the tick and loadInputsFile reads no edge at all. Such a tick is
    // written one line per edge, each with the mask after it.
    int mask = 0;
    for (size_t i = 0; i < plan.size();) {
        size_t j = i;
        while (j + 1 < plan.size() && plan[j + 1].step == plan[i].step) ++j;
        int toggles[2] = {0, 0};
        for (size_t k = i; k <= j; ++k) ++toggles[plan[k].p2 ? 1 : 0];
        const bool split = toggles[0] > 1 || toggles[1] > 1;
        for (size_t k = i; k <= j; ++k) {
            const int bit = plan[k].p2 ? 2 : 1;
            mask = plan[k].down ? (mask | bit) : (mask & ~bit);
            if (split || k == j) f << "input=" << plan[k].step << "," << mask << "\n";
        }
        i = j + 1;
    }
}

}  // namespace p1
