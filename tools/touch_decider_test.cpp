// Fixture for dp/src/dp/trigtouch.hpp: a touch trigger is called on the rising edge of the overlap, not on every pass it is overlapped.
//   cl /nologo /std:c++17 /EHsc /I dp\src tools\touch_decider_test.cpp /Fe:touch_decider_test.exe && touch_decider_test.exe
#include <cstdio>
#include "dp/trigtouch.hpp"

int main() {
    using dp::gdtouch::TouchDecider;
    int bad = 0;
    auto expect = [&](bool ok, const char* what) { std::printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) ++bad; };
    auto fired = [](std::initializer_list<std::pair<int, int>> l) { return std::vector<std::pair<int, int>>(l); };
    TouchDecider d;
    d.touchUids = {10, 11};
    // pass 1: uid 10 (a Move, type 0x14) and 11 (type 0x2d) newly overlapped, uid 12 is not a touch trigger, uid 13 is a solid (type 1)
    auto c = d.step(fired({{0x14 + 1000, 10}, {0x2d + 1000, 11}, {0x14, 12}, {1, 13}}));
    expect(c.size() == 2 && c[0] == 10 && c[1] == 11, "newly overlapped touch triggers are called, in the pass's order; others are not");
    // pass 2: still overlapped, nothing new
    c = d.step(fired({{0x14, 10}, {0x2d, 11}}));
    expect(c.empty(), "still overlapped: no second call");
    // pass 3: 10 left, 11 stays
    c = d.step(fired({{0x2d, 11}}));
    expect(c.empty(), "one leaves, the other stays: nothing");
    // pass 4: 10 comes back
    c = d.step(fired({{0x14, 10}, {0x2d, 11}}));
    expect(c.size() == 1 && c[0] == 10, "re-entering is a new touch");
    // once only (not Multi Activate): 10 may not be called twice in an attempt
    TouchDecider o; o.touchUids = {10}; o.onceOnly = true;
    o.step(fired({{0x14, 10}})); o.step(fired({})); c = o.step(fired({{0x14, 10}}));
    expect(c.empty(), "onceOnly: a second touch is not called");
    o.multi = {10}; o.step(fired({})); c = o.step(fired({{0x14, 10}}));
    expect(c.size() == 1, "onceOnly + Multi Activate: called again");
    o.startAttempt(); c = o.step(fired({{0x14, 10}}));
    expect(c.size() == 1, "a new attempt starts again");
    // the Tap (1595), the sequence the game logged for a custom level uid 7545 (target 62, hold mode, toggle mode 2): togl at 3767:on 3839:off 3873:on 3891:off 3893:on, then the Stop at 3917 ends it
    using dp::gdtouch::TapDecider;
    using dp::gdtouch::TapEvent;
    TapDecider t; t.taps[7545] = {62, 1, 2};
    auto one = [&](long long tick, int edge, bool want, const char* what) {
        auto e = t.step(tick, edge);
        expect(e.size() == 1 && e[0].uid == 7545 && e[0].target == 62 && e[0].on == want, what);
    };
    expect(t.step(3765, 1).empty(), "Tap: a press before the crossing does nothing");
    t.crossed(7545, 3766);
    one(3767, 2, true, "Tap: the release of a button held across the crossing reverses the press (group on)");
    one(3839, 1, false, "Tap: toggle mode 2, a press switches the group off");
    one(3873, 2, true, "Tap: hold mode, the release switches it on");
    one(3891, 1, false, "Tap: every press acts, not only the first");
    one(3893, 2, true, "Tap: ...and every release");
    t.stopAt(7545, 3917);
    expect(t.step(3939, 1).empty() && t.step(3943, 2).empty(), "Tap: after its Stop no edge acts");
    TapDecider u; u.taps[15522] = {331, 0, 1}; u.crossed(15522, 2365);
    auto e1 = u.step(2387, 1); auto e2 = u.step(2393, 2);
    expect(e1.size() == 1 && e1[0].on && e1[0].target == 331 && e2.empty(), "Tap: not hold mode (custom E, toggle mode 1): a press switches on, the release does nothing");
    TapDecider z; z.taps[139299] = {100, 1, 0}; z.crossed(139299, 9987);
    auto f1 = z.step(10033, 1); auto f2 = z.step(10035, 2);
    expect(f1.size() == 1 && !f1[0].on && f2.size() == 1 && f2[0].on, "Tap: toggle mode 0 in hold mode (custom C): press off, release on");
    z.startAttempt();
    expect(z.step(10037, 1).empty(), "Tap: a new attempt disarms it until the player crosses again");
    std::printf(bad ? "FAILED\n" : "ALL PASS\n");
    return bad ? 1 : 0;
}