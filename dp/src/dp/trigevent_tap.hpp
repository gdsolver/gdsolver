#pragma once
// The Tap producer of restructure draft section 7.5 (P4): the Touch trigger 1595 (touch = 0 spawn = 0) decided from the player's button edges, as Events. A pure function over trigtouch.hpp's TapDecider (the
// measured rule: an edge after the crossing acts at its own tick, a press applies the trigger's toggle mode, a release the opposite in hold mode). Measured offline on a custom level's flight: the
// two Taps' calls (1,462 of them) given to the shadow reproduced every on / off of the level against the game (121,070,868 object-ticks, 0 differences). Not wired into the search yet.
#include <vector>

#include "dp/trigevent.hpp"
#include "dp/trigtouch.hpp"

namespace dp {
namespace gdtrig {

// `edge`: 1 = the button went down in game tick `tick`, 2 = it went up, 0 = none. The shadow applies a call of game tick T at its step T + 1.
inline std::vector<Event> tapEvents(const gdtouch::TapDecider& d, long long tick, int edge) {
    std::vector<Event> out;
    for (const gdtouch::TapEvent& e : d.step(tick, edge)) {
        Event ev;
        ev.kind = kEvTap; ev.step = tick + 1; ev.uid = e.uid; ev.group = e.target; ev.on = e.on; ev.late = false;
        out.push_back(ev);
    }
    return out;
}

// the edge from the button level of two successive ticks (the State's `held` before and after the tick's input)
inline int buttonEdge(int prevHeld, int curHeld) { return (curHeld && !prevHeld) ? 1 : (!curHeld && prevHeld) ? 2 : 0; }

}  // namespace gdtrig
}  // namespace dp