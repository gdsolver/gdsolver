// Fixture for tools/touch_truth.hpp: two attempts with different calls on the same tick; the chosen attempt's calls are the truth.
//   cl /nologo /std:c++17 /EHsc /I tools tools\touch_truth_test.cpp /Fe:touch_truth_test.exe && touch_truth_test.exe
#include <cstdio>
#include "touch_truth.hpp"

int main() {
    const char* csv =
        "attempt,tick,uid,id,px,spawnTriggered,via\n"
        "1,100,7,901,1.0,0,0\n"      // A: attempt 1 only
        "2,100,8,901,1.0,0,0\n"      // B: attempt 2 only, the same tick
        "2,120,8,901,1.0,1,0\n"      // spawn-made: not a touch call
        "2,130,9,901,1.0,0,0\n"      // not a touch uid
        "2,140,8,1268,2.5,0,1\n";    // via = 1: a Spawn / Stop trigger's touch call goes through the override and counts
    const std::set<int> touch = {7, 8};
    int bad = 0;
    auto expect = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++bad; };
    {   std::istringstream in(csv); TouchTruth t = loadTouchTruth(in, touch);
        expect(t.attempt == 2 && t.attempts == 2, "default attempt is the last (2 of 2)");
        expect(t.rows.size() == 2 && t.rows.count({100, 8}) == 1 && t.rows.count({140, 8}) == 1, "default: B (tick 100, uid 8) and the via = 1 call; A, the spawn-made row and the non-touch uid are not truth");
        expect(t.px.count({100, 8}) == 1 && t.px.at({100, 8}) == 1.0, "the player's x of the call is kept"); }
    {   std::istringstream in(csv); TouchTruth t = loadTouchTruth(in, touch, 1);
        expect(t.attempt == 1 && t.rows.size() == 1 && t.rows.count({100, 7}) == 1, "attempt 1 asked for: only A"); }
    {   std::istringstream in(csv); TouchTruth t = loadTouchTruth(in, touch, 3);
        expect(t.attempt == 3 && t.rows.empty(), "an attempt the recording does not have: no rows, and the attempt reported"); }
    {   // the game's own calls (ptt lines of result.txt): only touch uids, the tick and uid read from the line
        const char* res = "stat: t=5 uid=1\nptt: t=8106 uid=15941 id=901 orect=(1,2,3,4) prect=(5,6,7,8)\nptt: t=8106 uid=15942 id=901 orect=(1,2,3,4) prect=(5,6,7,8)\nptt: t=9000 uid=77 id=901 orect=(1,2,3,4) prect=(5,6,7,8)\nkiller: t=1\n";
        std::istringstream in(res); TouchTruth t = loadPttTruth(in, {15941, 15942});
        expect(t.rows.size() == 2 && t.rows.count({8106, 15941}) == 1 && t.rows.count({8106, 15942}) == 1, "ptt: both touch uids at tick 8106; a uid that is not a touch trigger and other lines are not truth"); }
    {   // scoring: the same (tick, uid) in the decider's calls and in the truth, the recording's player 80 px from where the window has him -> neither matched nor extra, one skipped (AUD-715)
        const std::set<std::pair<long long, int>> truth = {{100, 7}, {110, 8}, {120, 9}}, calls = {{100, 7}, {110, 8}, {130, 4}};
        auto same = [](const std::pair<long long, int>& p) { return p.first != 100; };   // tick 100's row is another flight (px differs >= 60)
        TouchScore s = scoreTouchCalls(calls, truth, 0, 200, same);
        expect(s.hit == 1 && s.skipped == 1, "another-flight row: matched 1 (tick 110), skipped 1 (tick 100: its call is not matched)");
        expect(s.extra == 1 && s.extras.size() == 1 && s.extras[0].first == 130, "a call with no truth row stays extra");
        expect(s.miss == 1 && s.misses.size() == 1 && s.misses[0].first == 120, "a comparable row without a call is missed; the skipped one is not");
        TouchScore only = scoreTouchCalls({{100, 7}}, {{100, 7}}, 0, 200, same);
        expect(only.hit == 0 && only.extra == 0 && only.miss == 0 && only.skipped == 1, "same tick and uid in both, other flight: matched 0, extra 0, missed 0, skipped 1");
        TouchScore range = scoreTouchCalls({{50, 7}, {250, 7}}, {{50, 7}, {250, 7}}, 50, 200, [](const std::pair<long long, int>&) { return true; });
        expect(range.hit == 0 && range.miss == 0 && range.extra == 1, "the first step is not judged and truth past the window's last step is not compared (a call there would be extra)"); }
    std::printf(bad ? "FAILED\n" : "ALL PASS\n");
    return bad ? 1 : 0;
}