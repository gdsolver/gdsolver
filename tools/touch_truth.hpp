#pragma once
// The game's own calls of touch-triggered triggers, from gd-04's trigfire.csv (attempt,tick,uid,id,px,spawnTriggered,via), for tools/self_test.cpp's ST_TOUCHCMP.
// A recording holds one block of rows per attempt; the capture a window comes from is one attempt, and trig_replay reads the LAST one, so the default here is the last as well (an explicit attempt can be asked for).
#include <cstdlib>
#include <istream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

struct TouchTruth {
    std::set<std::pair<long long, int>> rows;   // (tick, uid) of the chosen attempt's calls that no spawn made, for the uids in `touchUid` (a Spawn / Stop trigger's touch call is logged by the overriding triggerObject, via = 1, and counts)
    std::map<std::pair<long long, int>, double> px;   // the player's x the recording had at that call (a recording of another flight of the same level shows a different one)
    int attempt = -1;                            // the attempt used
    int attempts = 0;                            // how many attempts the recording holds (the highest attempt number)
};

inline TouchTruth loadTouchTruth(std::istream& in, const std::set<int>& touchUid, int attemptWanted = -1) {
    struct Row { int attempt; long long tick; int uid; int spawn; int via; double px; };
    std::vector<Row> all;
    int maxAttempt = 0;
    std::string ln;
    std::getline(in, ln);   // header
    while (std::getline(in, ln)) {
        std::istringstream is(ln);
        std::string c;
        std::vector<std::string> v;
        while (std::getline(is, c, ',')) v.push_back(c);
        if (v.size() < 7) continue;
        Row r{std::atoi(v[0].c_str()), std::atoll(v[1].c_str()), std::atoi(v[2].c_str()), std::atoi(v[5].c_str()), std::atoi(v[6].c_str()), std::atof(v[4].c_str())};
        if (r.attempt > maxAttempt) maxAttempt = r.attempt;
        all.push_back(r);
    }
    TouchTruth t;
    t.attempts = maxAttempt;
    t.attempt = attemptWanted > 0 ? attemptWanted : maxAttempt;
    for (const Row& r : all) {
        if (r.attempt != t.attempt || r.spawn != 0 || !touchUid.count(r.uid)) continue;
        t.rows.insert({r.tick, r.uid});
        t.px[{r.tick, r.uid}] = r.px;
    }
    return t;
}

// The game's own calls, from the same run as the window's capture: the `ptt:` lines of result.txt (cfg hitboxtrace=1, GJBaseGameLayer::playerTouchedTrigger's entry: tick, uid, id, the object's
// and the player's rect). This is the right truth for the decider, which says when the player's box first overlaps a touch trigger. trigfire.csv is NOT: it is another run's record of when a trigger
// EXECUTED, and a recording of another flight of the level has the same x at a tick (x is the tick's) but another y, so the x test cannot tell that its calls belong to other flights (measured:
// 13 of 17 "misses" of 2026-10-31, where the game's own ptt calls were exactly the decider's). The log keeps 2,000 lines of each kind; rows past the cap are simply absent.
inline TouchTruth loadPttTruth(std::istream& in, const std::set<int>& touchUid) {
    TouchTruth t;
    t.attempt = 1; t.attempts = 1;
    std::string ln;
    while (std::getline(in, ln)) {
        if (ln.compare(0, 7, "ptt: t=") != 0) continue;
        const long long tick = std::atoll(ln.c_str() + 7);
        const auto u = ln.find(" uid=");
        if (u == std::string::npos) continue;
        const int uid = std::atoi(ln.c_str() + u + 5);
        if (touchUid.count(uid)) t.rows.insert({tick, uid});
    }
    return t;
}

// The score of the decider's calls against the game's, over ONE set of comparable truth rows: those inside the window's steps (first < tick <= last) whose recording had the player where the
// window's pass has him (`sameFlight`; a recording of another flight of the level has the player elsewhere at that tick). A truth row that fails it is skipped, and nothing is scored against
// it: a call of the decider on that (tick, uid) is neither matched nor extra, and it cannot be missed. (The first version applied sameFlight to the missed loop only, so a prediction on a skipped
// row could still be counted as matched: AUD-715.)
struct TouchScore {
    long long hit = 0, extra = 0, miss = 0, skipped = 0;
    std::vector<std::pair<long long, int>> extras, misses;
};
template <class SameFlight>
inline TouchScore scoreTouchCalls(const std::set<std::pair<long long, int>>& calls, const std::set<std::pair<long long, int>>& truth, long long first, long long last, SameFlight sameFlight) {
    TouchScore s;
    std::set<std::pair<long long, int>> comparable, excluded;
    for (const auto& p : truth) {
        if (p.first <= first || p.first > last) continue;
        if (sameFlight(p)) comparable.insert(p); else { excluded.insert(p); ++s.skipped; }
    }
    for (const auto& p : calls) {
        if (p.first <= first || excluded.count(p)) continue;
        if (comparable.count(p)) ++s.hit; else { ++s.extra; s.extras.push_back(p); }
    }
    for (const auto& p : comparable) if (!calls.count(p)) { ++s.miss; s.misses.push_back(p); }
    return s;
}