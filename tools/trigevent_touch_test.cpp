// trigevent_touch_test: dp/src/dp/trigevent_touch.hpp's touchEvents against trigtouch.hpp's TouchDecider on random overlap sequences (touch uids, Multi Activate sets, onceOnly, attempts).
// cl /nologo /std:c++17 /O2 /EHsc /I dp/src tools/trigevent_touch_test.cpp
#include <cstdio>
#include <cstdlib>
#include <random>
#include "dp/trigtouch.hpp"
#include "dp/trigevent_touch.hpp"

int main() {
    std::mt19937 rng(12345);
    long long ticks = 0, calls = 0, bad = 0;
    for (int round = 0; round < 400; ++round) {
        const int nUid = 1 + (int)(rng() % 9);
        dp::gdtouch::TouchDecider ref;
        dp::gdtrig::TouchEventRules rules;
        for (int u = 0; u < nUid; ++u) ref.touchUids.insert(100 + u);
        for (int u = 0; u < nUid; ++u) if (rng() % 3 == 0) { ref.multi.insert(100 + u); rules.multi.insert(100 + u); }
        ref.onceOnly = rules.onceOnly = (rng() % 2) != 0;
        std::set<int> prev, calledEver;
        ref.startAttempt();
        for (int t = 0; t < 600; ++t) {
            if (t == 300) { ref.startAttempt(); prev.clear(); calledEver.clear(); }   // a new attempt
            std::vector<int> cur;
            for (int u = 0; u < nUid; ++u) if (rng() % 3 == 0) cur.push_back(100 + u);
            std::shuffle(cur.begin(), cur.end(), rng);
            if (!cur.empty() && rng() % 7 == 0) cur.push_back(cur.front());          // a uid listed twice by the pass
            std::vector<std::pair<int, int>> fired;
            for (int u : cur) fired.push_back({0x14 + (int)(rng() % 2) * (0x2d - 0x14), u});   // type 0x14 or 0x2d
            if (rng() % 5 == 0) fired.push_back({0x05, 999});                         // an object that is not a touch trigger
            const std::vector<int> a = ref.step(fired);
            const std::vector<dp::gdtrig::Event> b = dp::gdtrig::touchEvents(t, prev, cur, rules, calledEver);
            bool ok = a.size() == b.size();
            for (size_t i = 0; ok && i < a.size(); ++i) ok = a[i] == b[i].uid && b[i].step == t + 1 && b[i].kind == dp::gdtrig::kEvTouch && !b[i].late && b[i].group == -1;
            if (!ok) { ++bad; if (bad < 5) std::printf("MISMATCH round %d t %d: ref %zu calls, new %zu\n", round, t, a.size(), b.size()); }
            prev.clear(); for (int u : cur) prev.insert(u);
            ++ticks; calls += (long long)a.size();
        }
    }
    std::printf("trigevent_touch_test: %lld ticks, %lld calls, %lld mismatches\n", ticks, calls, bad);

    // the latch form against the overlap form: a state's latch (first entry of each box, with its tick) is touchEvents() with onceOnly and no Multi Activate
    long long lticks = 0, lcalls = 0, lbad = 0, lorder = 0;
    for (int round = 0; round < 400; ++round) {
        const int nBits = 1 + (int)(rng() % 40);
        std::vector<int> boxUid(nBits);
        for (int b = 0; b < nBits; ++b) boxUid[b] = (rng() % 11 == 0) ? -1 : 500 + b;   // a bit with no trigger is skipped
        dp::gdtrig::TouchEventRules rules; rules.onceOnly = true;
        std::set<int> prev, calledEver;
        uint64_t latch = 0;
        uint16_t fireB[64] = {};
        for (int t = 0; t < 400; ++t) {
            std::vector<int> cur;
            for (int b = 0; b < nBits; ++b) if (boxUid[b] >= 0 && rng() % 6 == 0) cur.push_back(boxUid[b]);
            std::shuffle(cur.begin(), cur.end(), rng);
            const std::vector<dp::gdtrig::Event> a = dp::gdtrig::touchEvents(t, prev, cur, rules, calledEver);
            const uint64_t parent = latch;
            for (int u : cur) { const int b = u - 500; if (!((latch >> b) & 1ull)) { latch |= 1ull << b; fireB[b] = (uint16_t)t; } }   // the state enters the box: bit set, entry tick kept
            const std::vector<dp::gdtrig::Event> c = dp::gdtrig::touchEventsFromLatch(parent, latch, fireB, boxUid);
            bool ok = a.size() == c.size();
            std::set<int> sa, sc;
            for (const auto& e : a) sa.insert(e.uid);
            for (const auto& e : c) { sc.insert(e.uid); if (e.step != t + 1 || e.kind != dp::gdtrig::kEvTouch) ok = false; }
            if (sa != sc) ok = false;
            if (ok) { bool asc = true; for (size_t i = 1; i < a.size(); ++i) if (a[i].uid < a[i - 1].uid) asc = false; if (!asc) ++lorder; }   // same calls, the pass's order differs from the latch's ascending order
            if (!ok) { ++lbad; if (lbad < 5) std::printf("LATCH MISMATCH round %d t %d: overlap form %zu calls, latch form %zu\n", round, t, a.size(), c.size()); }
            prev.clear(); for (int u : cur) prev.insert(u);
            ++lticks; lcalls += (long long)c.size();
        }
    }
    std::printf("latch form: %lld ticks, %lld calls, %lld mismatches; %lld ticks where the same calls come in a different order than the pass's\n", lticks, lcalls, lbad, lorder);
    return (bad || lbad) ? 1 : 0;
}
