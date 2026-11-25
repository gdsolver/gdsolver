#pragma once
// The touch-triggered triggers the shadow decides for itself: a trigger object with Touch Triggered set (triggers.txt `touch` = 1) is called when the player's box starts to overlap it
// (GJBaseGameLayer::collisionCheckObjects, case 0x14 / 0x2d -> playerTouchedTrigger 0x31f6b0: the player's own set of touched triggers lets a call through once per touch). The overlap is the
// pass's own (SelfOut::fired lists every non-solid object the pass overlapped), so the rect it is made against must be where the object IS: a trigger object that a Move carries
// (group 325 in the level where a Move / Rotate carried three touch triggers 90 px away from their dump position) is only right with its tracked position (cfg grouptracksmart, wtrace.csv).
//
// Measured against the game's own calls (trigfire.csv of gd-04's trig_replay, rows no spawn made): a call at trigfire tick T is the rising edge of the overlap in the pass of tick T.
// Separate from dp/src/dp/trigshadow.hpp (gd-04's closed-form trigger world): this file only decides WHICH touch triggers are called in a step; trigshadow's setTouchEvents takes the
// answer (key = the shadow's step = trigfire tick + 1) and runs what they do.
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace dp {
namespace gdtouch {

struct TouchDecider {
    std::set<int> touchUids;    // the uids of the trigger objects with Touch Triggered set (triggers.txt, touch = 1)
    std::set<int> multi;        // the ones that may be called again after the player left (Multi Activate); empty = every call counts (a touch is a rising edge)
    bool onceOnly = false;      // true: a trigger not in `multi` is called at most once per attempt
    std::set<int> prev;         // what the last pass overlapped
    std::set<int> calledEver;

    void startAttempt() { prev.clear(); calledEver.clear(); }

    // `fired`: SelfOut::fired of this step, (type + 1000 for a first act, uid). Returns the uids called in this step, in the pass's order.
    std::vector<int> step(const std::vector<std::pair<int, int>>& fired) {
        std::set<int> now;
        std::vector<int> order;
        for (const auto& f : fired) {
            const int ty = f.first % 1000;
            if ((ty == 0x14 || ty == 0x2d) && touchUids.count(f.second) && now.insert(f.second).second) order.push_back(f.second);
        }
        std::vector<int> calls;
        for (int u : order) {
            if (prev.count(u)) continue;
            if (onceOnly && !multi.count(u) && calledEver.count(u)) continue;
            calls.push_back(u);
            calledEver.insert(u);
        }
        prev = now;
        return calls;
    }
};

// The Touch trigger itself (id 1595, triggers.txt touch = 0 / spawn = 0: "Tap"), which is not the trigger marked Touch Triggered above: it is armed when the player crosses it
// (trigfire.csv logs only that crossing, id 1595) and then acts on the player's own button edges.
//
// Measured against the game's own group toggles (cfg hitboxtrace=1 `togl:` lines, trig = the Tap's uid; 12 Taps in 10 levels, 4,580 toggles: 0 missed, 0 extra):
//   - an edge at game tick T > the crossing tick acts in tick T itself (the toggle is logged at the tick of the pushButton / releaseButton call). A button already held when the player crosses
//     is no press: its release is the first edge (a custom level uid 7545: press 3765, crossing 3766, release 3767 -> the group goes back on at 3767).
//   - a press applies the Tap's toggle mode (triggers.txt ttog): 1 = group on, 2 = group off, 0 = group off (measured: ttog 0, thold 1: press -> off, release -> on).
//   - hold mode (thold = 1): the release applies the opposite; without it a release does nothing (a custom level, thold 0: every press switches on, no release acts).
//   - every press acts, not only the first (Multi Activate, multi.txt, is 0 on all twelve), until a Stop (1616) whose target group the Tap's own object belongs to has been activated: no edge
//     after the Stop's tick acts (a custom level: Stop uid 7543 at tick 3108 ends uid 7541 after its single press; the release at 3113 does nothing). An edge exactly at the Stop's tick is not
//     measured (no data); stopAt() lets it count.
// Not measured: Dual Mode (tdual), a Tap in a level with a second player, an edge at the crossing tick itself.
//
// Separate from trigshadow.hpp (gd-04): its fireTouch() skips every trigger whose `touch` is 0, so these toggles are returned here as (uid, target, on) for it to apply.
struct TapDef {
    int target = 0;
    int thold = 0;    // 1: hold mode (the release reverses the press)
    int ttog = 0;     // toggle mode, triggers.txt ttog
};
struct TapEvent {
    int uid, target;
    bool on;          // the group goes on (true) or off (false)
};
struct TapDecider {
    std::map<int, TapDef> taps;           // by uid
    std::map<int, long long> armedAt;     // uid -> the tick of the crossing (an edge acts from the next tick)
    std::map<int, long long> stopTick;    // uid -> the tick of the Stop that ends it

    void startAttempt() { armedAt.clear(); stopTick.clear(); }
    void crossed(int uid, long long tick) { if (taps.count(uid) && !armedAt.count(uid)) armedAt[uid] = tick; }
    void stopAt(int uid, long long tick) { if (!stopTick.count(uid)) stopTick[uid] = tick; }

    // `edge`: 1 = the button went down in game tick `tick`, 2 = it went up, 0 = no edge. Returns the toggles of this tick in ascending uid order.
    std::vector<TapEvent> step(long long tick, int edge) const {
        std::vector<TapEvent> out;
        if (edge == 0) return out;
        for (const auto& kv : armedAt) {
            if (kv.second >= tick) continue;
            const auto st = stopTick.find(kv.first);
            if (st != stopTick.end() && tick > st->second) continue;
            const TapDef& d = taps.at(kv.first);
            const bool pressOn = d.ttog == 1;
            if (edge == 1) out.push_back({kv.first, d.target, pressOn});
            else if (d.thold) out.push_back({kv.first, d.target, !pressOn});
        }
        return out;
    }
};

}  // namespace gdtouch
}  // namespace dp
