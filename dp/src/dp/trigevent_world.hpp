#pragma once
// The world's small state parts of restructure draft section 7.5 / 7.6, as pure functions: the group on / off bits a Tap or Toggle Orb sets (last write wins) and the x-crossing gate digest of the
// triggers that sit in a toggleable group. Nothing here is wired into the search yet (P4 / P5); dptest_main.cpp holds the fixtures.
#include <cstdint>

namespace dp {
namespace gdtrig {

inline uint64_t wkMix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); return h * 0xff51afd7ed558ccdULL; }

// xgate: a trigger T that sits in a group a state-kind toggle (Toggle Orb 1594, Tap 1595) can switch does not fire when it is crossed while its group is off (checkSpawnObjects, obj+0x28e), so whether
// it fired depends on the toggle's TIME. The gate is the sum, over every crossing of such a trigger, of a hash of (T, fired). A SUM and not an xor: crossing the same T twice (a teleport, --tpback,
// a re-fire) adds 2 * H and does not cancel to 0; the sum does not depend on the order of the crossings. 0 for a level with no such trigger.
inline uint64_t xgateHash(int uid, bool fired) { return wkMix(wkMix(0x58474154ULL, (uint64_t)(uint32_t)uid), fired ? 1u : 0u); }
inline uint64_t xgateAdd(uint64_t xg, int uid, bool fired) { return xg + xgateHash(uid, fired); }

// TogBits: the current on / off of the level's toggleable groups, indexed densely by the level (128 of them; the 65 levels measured need at most 111). A bit is 1 when the group's value
// DIFFERS from the level's initial one, so the all-zero value is "nothing switched" whichever groups start off, and a group switched back to where it started is 0 again: the value is a function of
// the groups' current state, not of the history of switches (last write wins).
struct TogBits {
    uint64_t w[2] = {0, 0};
    bool differs(int i) const { return (w[(i >> 6) & 1] >> (i & 63)) & 1u; }
    void set(int i, bool on, bool initialOn) {
        const uint64_t m = 1ULL << (i & 63);
        if (on != initialOn) w[(i >> 6) & 1] |= m; else w[(i >> 6) & 1] &= ~m;
    }
    uint64_t digest() const { return wkMix(wkMix(0x544f4742ULL, w[0]), w[1]); }
    bool operator==(const TogBits& o) const { return w[0] == o.w[0] && w[1] == o.w[1]; }
};

}  // namespace gdtrig
}  // namespace dp