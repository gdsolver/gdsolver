#pragma once
#include "dp/state.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace dp {

// --portaudit (default off; print only -- nothing here is read back by the search, so the plan is the same with the flag on or off).
//
// The question (Orch 11-05, audit AUD-857): State::portSeen / portSeen2 (the portals the body overlapped on this tick and the one before: step.hpp's portal-once rule reads them on the NEXT tick) are not
// in the dedupe key, so two children that share a key and differ only in them are merged and one of the two is lost. Does the loss matter -- do the two part on the next tick?
//
// Counted on the CANDIDATES, before the dedupe (the children of one group as phase 1 stepped them): a candidate whose key was already taken by an earlier valid candidate of the group, and whose
// portSeen / portSeen2 differ from that first one's, is a collision "pair" (first, other). (Counting after the dedupe, on `nxt`, cannot see the child that was dropped.)
// A sample of the pairs (portAuditSample a layer) is kept and, in the next layer, when the group they belong to is stepped, BOTH states are stepped one more tick with each input the model allows, under
// that layer's own context, and the two children are compared: key (modelKeyOf ^ rules.keyBits), then y / vy / xAbs, then whether either died.
inline bool g_portAudit = false;
inline int g_portAuditSample = 8;

struct PortAuditStats {
    long long layers = 0;       // layers with at least one collision pair
    long long kids = 0;         // valid, alive candidates looked at
    long long keyShared = 0;    // candidates whose key an earlier candidate of the group already had
    long long pairs = 0;        // ...of those, with portSeen / portSeen2 different from that first candidate's
    long long pairsReal = 0;    // ...of those, pairs where either state names a real portal in portSeen / portSeen2 (an entry >= 0: a portal the body overlapped); the rest differ only by the -1 / -2 markers
    long long sampled = 0;      // pairs kept for the next layer
    long long outcomes = 0;     // (pair, input) steps compared
    long long same = 0;         // same key, same y / vy / xAbs, both alive
    long long keyDiffers = 0;   // the children's keys differ
    long long bodyDiffers = 0;  // same key, y / vy / xAbs differ
    long long deadDiffers = 0;  // one child died, the other did not
    long long bothDead = 0;
    long long noInput = 0;      // inputs the model does not allow for one of the two (skipped)
    // ISOLATED: the same two states with ONLY portSeen / portSeen2 swapped (a against a-with-b's-portSeen, and b against b-with-a's-portSeen), so a difference is the portSeen alone -- the pair above also
    // differs in y / vy inside the key's bins, which the dedupe has always merged
    long long isoOutcomes = 0, isoSame = 0, isoKeyDiffers = 0, isoBodyDiffers = 0, isoDeadDiffers = 0, isoBothDead = 0;
    long long expired = 0;      // sampled pairs whose group was not stepped in the next layer
};
inline PortAuditStats g_portAuditStats;

struct PortAuditPair {
    State a, b;          // first candidate with the key, the colliding candidate
    long long t = 0;     // the layer they were stepped in (their children are the next layer's parents)
    bool done = false;
};
inline std::vector<PortAuditPair> g_portAuditPending;

inline bool portSeenDiffers(const State& a, const State& b) {
    return std::memcmp(a.portSeen, b.portSeen, sizeof a.portSeen) != 0 || std::memcmp(a.portSeen2, b.portSeen2, sizeof a.portSeen2) != 0;
}

// does the state name a real portal (a uid) in either list? (-1 = empty slot, -2 = the born-last-tick marker)
inline bool portSeenReal(const State& s) {
    for (int i = 0; i < 3; ++i)
        if (s.portSeen[i] >= 0 || s.portSeen2[i] >= 0) return true;
    return false;
}

enum class PortOutcome { Same, KeyDiffers, BodyDiffers, DeadDiffers, BothDead };

inline PortOutcome portAuditClassify(bool deadA, bool deadB, uint64_t keyA, uint64_t keyB, const State& a, const State& b) {
    if (deadA && deadB) return PortOutcome::BothDead;
    if (deadA != deadB) return PortOutcome::DeadDiffers;
    if (keyA != keyB) return PortOutcome::KeyDiffers;
    if (a.y != b.y || a.vy != b.vy || a.xAbs != b.xAbs) return PortOutcome::BodyDiffers;
    return PortOutcome::Same;
}

// The collision pairs of one group's candidates (before the dedupe): (index of the first candidate with the key, index of the colliding one), in candidate order. `Kid` has .s (State), .key,
// .dead and .valid, as search_dp.hpp's Child does. Pure: it counts nothing and keeps nothing.
template <class Kid>
std::vector<std::pair<size_t, size_t>> portAuditFind(const std::vector<Kid>& kids, long long* aliveKids = nullptr, long long* keyShared = nullptr) {
    std::vector<std::pair<size_t, size_t>> out;
    std::unordered_map<uint64_t, size_t> first;
    first.reserve(kids.size());
    for (size_t i = 0; i < kids.size(); ++i) {
        if (!kids[i].valid || kids[i].dead) continue;
        if (aliveKids) ++*aliveKids;
        const auto it = first.emplace(kids[i].key, i);
        if (it.second) continue;
        if (keyShared) ++*keyShared;
        if (portSeenDiffers(kids[it.first->second].s, kids[i].s)) out.emplace_back(it.first->second, i);
    }
    return out;
}

// Step a recorded pair one tick with each input. `stepFn(state, input, out, key)` returns -1 (the model does not allow this input), 0 (the child died) or 1 (alive; `out` and `key` set).
template <class StepFn>
void portAuditStepPair(PortAuditPair& p, StepFn&& stepFn) {
    State aSwap = p.a, bSwap = p.b;
    std::memcpy(aSwap.portSeen, p.b.portSeen, sizeof aSwap.portSeen);   std::memcpy(aSwap.portSeen2, p.b.portSeen2, sizeof aSwap.portSeen2);
    std::memcpy(bSwap.portSeen, p.a.portSeen, sizeof bSwap.portSeen);   std::memcpy(bSwap.portSeen2, p.a.portSeen2, sizeof bSwap.portSeen2);
    for (int input = 0; input < 2; ++input) {
        for (int which = 0; which < 2; ++which) {   // the isolated comparisons
            const State& orig = which ? p.b : p.a;
            const State& swapped = which ? bSwap : aSwap;
            State co, cs;
            uint64_t ko = 0, ks = 0;
            const int ro = stepFn(orig, input, co, ko), rs = stepFn(swapped, input, cs, ks);
            if (ro < 0 || rs < 0) continue;
            ++g_portAuditStats.isoOutcomes;
            switch (portAuditClassify(ro == 0, rs == 0, ko, ks, co, cs)) {
                case PortOutcome::Same: ++g_portAuditStats.isoSame; break;
                case PortOutcome::KeyDiffers: ++g_portAuditStats.isoKeyDiffers; break;
                case PortOutcome::BodyDiffers: ++g_portAuditStats.isoBodyDiffers; break;
                case PortOutcome::DeadDiffers: ++g_portAuditStats.isoDeadDiffers; break;
                case PortOutcome::BothDead: ++g_portAuditStats.isoBothDead; break;
            }
        }
        State ca, cb;
        uint64_t ka = 0, kb = 0;
        const int ra = stepFn(p.a, input, ca, ka), rb = stepFn(p.b, input, cb, kb);
        if (ra < 0 || rb < 0) { ++g_portAuditStats.noInput; continue; }
        ++g_portAuditStats.outcomes;
        switch (portAuditClassify(ra == 0, rb == 0, ka, kb, ca, cb)) {
            case PortOutcome::Same: ++g_portAuditStats.same; break;
            case PortOutcome::KeyDiffers: ++g_portAuditStats.keyDiffers; break;
            case PortOutcome::BodyDiffers: ++g_portAuditStats.bodyDiffers; break;
            case PortOutcome::DeadDiffers: ++g_portAuditStats.deadDiffers; break;
            case PortOutcome::BothDead: ++g_portAuditStats.bothDead; break;
        }
    }
    p.done = true;
}

inline void portAuditPrint() {
    if (!g_portAudit) return;
    const PortAuditStats& s = g_portAuditStats;
    std::printf("portaudit: layers-with-pairs=%lld alive-candidates=%lld key-shared=%lld pairs(portSeen differs)=%lld of-which-real-portal=%lld sampled=%lld expired=%lld | next-tick outcomes=%lld same=%lld key-differs=%lld"
                " body-differs=%lld dead-differs=%lld both-dead=%lld input-not-allowed=%lld | isolated (portSeen swapped, nothing else) outcomes=%lld same=%lld key-differs=%lld body-differs=%lld"
                " dead-differs=%lld both-dead=%lld\n",
                s.layers, s.kids, s.keyShared, s.pairs, s.pairsReal, s.sampled, s.expired, s.outcomes, s.same, s.keyDiffers, s.bodyDiffers, s.deadDiffers, s.bothDead, s.noInput,
                s.isoOutcomes, s.isoSame, s.isoKeyDiffers, s.isoBodyDiffers, s.isoDeadDiffers, s.isoBothDead);
}

}  // namespace dp
