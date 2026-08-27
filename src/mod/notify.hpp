#pragma once
// The notifications the mod puts on screen, and the watchdog that takes them back off.
#include "mod/trace_io.hpp"

using namespace p1;

// ---- The notifications the mod puts on screen -------------------------------
// Geode hides a notification with a CCAction chain (Notification.cpp: fade in, CCDelayTime(time),
// hide, then showNextNotification pops a STATIC queue). Two properties of that matter here:
//
//   * the chain only advances while the action manager runs on that node. A node's actions are
//     paused by CCNode::onExit -- which a scene change performs -- so a notification raised just
//     before one (the solve's own "solved", "gave up" and end-of-session lines all sit next to a
//     reset or a scene swap) can be left on screen with its hide frozen, forever.
//   * the queue is shared and only its head is ever shown. One stuck notification silently
//     swallows every later one, ours and every other mod's.
//   * the POP is the last link of the hide chain (Notification::hide ends in
//     showNextNotification). So a notification that has been asked to hide is not finished with:
//     for the half second its fade-out runs, it is still the head of the queue, and losing that
//     chain wedges everything behind it. That is what g_retiring below exists for.
//
// So the mod owns exactly one at a time: showing a new one cancels ours that is still up, a
// watchdog un-freezes and hides one that has outlived its time, and a session end clears it.
// Everything here is bookkeeping around Geode's own calls -- no notification is created anywhere
// else in the mod (see notify::show's callers).
namespace notify {

// cfg `notifyfix=0` turns off BOTH halves of the repair (the re-arm after a purge and the
// watchdog), so that "was there anything to fix" can be answered by measurement rather than by
// reading Geode's source. Measured with it off, on the self-test below: FAIL, the notification
// is still on screen nine seconds after a purge that should have left it three.
inline bool g_fix = true;
inline Ref<Notification> g_cur;
inline std::chrono::steady_clock::time_point g_shownAt;
inline bool g_curSeenShowing = false;  // its own clock starts when it reaches the head, not when
                                       // it was created: a queued one has not begun
inline float g_secs = 0.f;
inline int g_stage = 0;             // how far the watchdog has escalated on the current one
inline long long g_forced = 0;      // how many needed forcing (reported at session end)

// The one we have asked to hide and whose fade-out has not finished. It has to be held onto
// separately from g_cur, because at exactly the moment it needs repairing g_cur is already the
// notification we raised NEXT -- which is queued, has no actions yet, and is therefore the one
// with nothing to repair. Losing this one's chain leaves it on screen for good and wedges every
// later notification behind it (see the note at the top of the file).
// [2026-08-27] The failure that made this necessary: a level solved by its FIRST plan raises
// "first plan ... now testing it" and then, a frame or two later, both "solution saved" and
// "solved - replaying the solution" -- and the render toggle purges between them. The purge
// landed on the fade-out of the first, which was no longer g_cur, so nothing re-armed it: it
// stayed on screen for the rest of the session and no notification appeared after it. Levels
// that take repair rounds do not show it, because there the first notification is minutes old
// and long since popped. lv1 and lv3 reproduce it every time.
inline Ref<Notification> g_retiring;
inline std::chrono::steady_clock::time_point g_retiredAt;
inline int g_retireStage = 0;

inline double aliveSec() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - g_shownAt).count();
}
inline double retireSec() {
    return std::chrono::duration<double>(
        std::chrono::steady_clock::now() - g_retiredAt).count();
}

// Hand one over to its fade-out, and keep watching it until the pop at the end of that chain has
// actually happened.
inline void retire(Notification* n) {
    if (!n) return;
    n->resumeSchedulerAndActions();
    n->hide();
    g_retiring = n;
    g_retiredAt = std::chrono::steady_clock::now();
    g_retireStage = 0;
}

// Let go of ours, hiding it first if it is still up. Called when the session ends and when the
// level is left -- the two places a scene swap is about to freeze whatever is on screen.
inline void clear() {
    if (g_cur) {
        if (g_cur->isShowing()) {
            retire(g_cur);        // only the head can be showing, so nothing is displaced here
        } else {
            g_cur->cancel();      // still queued: this takes it out of the queue
        }
    }
    g_cur = nullptr;
    g_stage = 0;
    g_curSeenShowing = false;
}

inline void show(const std::string& text, NotificationIcon icon, float seconds) {
    clear();                       // never stack ours; the queue is what wedges
    auto* n = Notification::create(text, icon, seconds);
    if (!n) return;
    g_cur = n;
    g_secs = seconds;
    g_shownAt = std::chrono::steady_clock::now();
    g_stage = 0;
    g_curSeenShowing = false;
    n->show();
}

// Once per frame, from the same place audio::sync runs.
inline void watchdog() {
    if (!g_fix) return;
    // The retiring one first: everything else, ours and everyone else's, is queued behind its
    // pop. Its budget is the 0.5 s fade-out; three seconds is a long frame plus slack.
    if (g_retiring) {
        if (!g_retiring->isShowing()) {
            g_retiring = nullptr;          // it popped; the queue has moved on
            g_retireStage = 0;
        } else if (g_retireStage == 0 && retireSec() > 3.0) {
            g_retireStage = 1;
            g_retiring->resumeSchedulerAndActions();
            g_retiring->hide();            // re-issues the whole chain, pop included
            ++g_forced;
            writeResult("notify: a hidden notification never popped the queue ("
                        + std::to_string((int)retireSec()) + "s) - re-issued its hide");
        } else if (g_retireStage == 1 && retireSec() > 8.0) {
            g_retireStage = 2;
            // Same last resort as below, and the same caveat: this takes it off the screen but
            // cannot pop the queue (showNextNotification is Geode's, and protected).
            writeResult("notify: the re-issued hide did not take either - removing the node "
                        "directly (Geode's queue may stay wedged until the scene changes)");
            g_retiring->removeFromParent();
            g_retiring = nullptr;
        }
    }
    if (!g_cur) return;
    if (!g_cur->isShowing()) {
        // Either it has hidden itself normally, or it is queued -- behind one of ours that the
        // block above is unwedging, or behind another mod's. A queued one cannot be overdue: its
        // own clock has not started.
        if (g_stage > 0 || (!g_retiring && aliveSec() > (double)g_secs + 2.0)) {
            g_cur = nullptr;
            g_stage = 0;
        }
        return;
    }
    // It has reached the head: this is when its time begins. Without this, one that waited in
    // the queue is judged overdue the moment it appears.
    if (!g_curSeenShowing) {
        g_curSeenShowing = true;
        g_shownAt = std::chrono::steady_clock::now();
    }
    // Geode's own budget is fade-in (0.3) + time + fade-out (0.5). Two seconds past that is
    // slack for a long frame, not a symptom.
    const double over = aliveSec() - ((double)g_secs + 0.8);
    if (over < 2.0) return;
    if (g_stage == 0) {
        g_stage = 1;
        // The suspected freeze, and the fix for it: a paused target never advances its actions.
        // Resuming and re-issuing hide() also runs showNextNotification, so the shared queue is
        // released properly rather than left with a head that never pops.
        g_cur->resumeSchedulerAndActions();
        g_cur->hide();
        ++g_forced;
        writeResult("notify: a notification outlived its time by "
                    + std::to_string((int)over) + "s - resumed its actions and forced the hide");
        return;
    }
    if (over < 5.0) return;
    // hide() did not take either, so the action manager is not running this node at all. Take it
    // off the screen directly. The queue may stay wedged; say so, because the next notification
    // going missing would otherwise look like a separate bug.
    writeResult("notify: the forced hide did not take - removing the node directly "
                "(Geode's notification queue may be wedged until the scene changes)");
    g_cur->removeFromParent();
    g_cur = nullptr;
    g_stage = 0;
}

// ---- self-test (cfg `notifytest=1`) -----------------------------------------
// The failure this file exists for needs a purge to land ON a notification that is currently
// up, and in a real run that is a coincidence -- three solve runs with the screen toggling
// every two seconds produced not one overlap, which is evidence of nothing either way. So the
// overlap is made on purpose here: show one, purge under it a second later, and say whether it
// still went away. Worth keeping rather than deleting after one green: the mechanism lives in
// Geode's Notification.cpp, which is somebody else's file and can change under us.
inline bool g_test = false;
inline int g_testStage = 0;
inline std::chrono::steady_clock::time_point g_testT0;

inline void selfTest() {
    if (!g_test) return;
    const double el = (g_testStage == 0) ? 0.0
        : std::chrono::duration<double>(std::chrono::steady_clock::now() - g_testT0).count();
    if (g_testStage == 0) {
        g_testStage = 1;
        g_testT0 = std::chrono::steady_clock::now();
        show("gdsolver: notification self-test", NotificationIcon::Info, 3.f);
        writeResult("notifytest: shown (3s), purging under it in 1s");
        return;
    }
    if (g_testStage == 1 && el >= 1.0) {
        g_testStage = 2;
        // Exactly what the render toggle does (session_hotkeys renderTogglePress).
        purgeDanglingActions();
        writeResult(std::string("notifytest: purged - still showing=")
                    + (g_cur && g_cur->isShowing() ? "yes" : "no")
                    + " rearmed=" + std::to_string(g_forced));
        return;
    }
    // Well past the notification's own life (3s) plus its fade, and past the watchdog's own
    // 2s grace, so a survivor here is a survivor for good.
    if (g_testStage == 2 && el >= 9.0) {
        g_testStage = 3;
        const bool stuck = g_cur && g_cur->isShowing();
        writeResult(std::string("notifytest: ") + (stuck ? "FAIL - still on screen "
                                                           "9s after a purge"
                                                         : "PASS - gone after the purge")
                    + " (rearmed=" + std::to_string(g_forced) + ")");
    }
    // ---- scenario 2: a purge on a notification that is already RETIRING ------
    // The failure that outlived scenario 1 (2026-08-27). Scenario 1 purges under the one the mod
    // currently owns, and that one was always repairable. This is the other shape, and it is the
    // one a level solved by its FIRST plan produces every time: one of ours is still fading out
    // when the next is raised, so the fading one is no longer what the mod owns -- and it is the
    // fading one whose chain ends in the queue pop.
    if (g_testStage == 3 && el >= 11.0) {
        g_testStage = 4;
        g_testT0 = std::chrono::steady_clock::now();
        show("gdsolver: notification self-test A", NotificationIcon::Info, 3.f);
        writeResult("notifytest2: A shown (3s)");
        return;
    }
    if (g_testStage == 4 && el >= 0.6) {
        g_testStage = 5;
        // A is still up, so show() retires it and B goes into the queue behind A's fade-out...
        show("gdsolver: notification self-test B", NotificationIcon::Info, 3.f);
        purgeDanglingActions();   // ...and the purge lands on that fade-out, pop and all
        writeResult(std::string("notifytest2: B queued, purged under A - A=")
                    + (g_retiring && g_retiring->isShowing() ? "showing" : "gone")
                    + " forced=" + std::to_string(g_forced));
        return;
    }
    if (g_testStage == 5 && el >= 12.0) {
        g_testStage = 6;
        // Well past A (3 s) + B (3 s) and both fades. Anything of ours still up here is up for
        // good, and a queue left wedged would have kept B from ever being shown at all.
        const bool stuckA = g_retiring && g_retiring->isShowing();
        const bool stuckB = g_cur && g_cur->isShowing();
        writeResult(std::string("notifytest2: ")
                    + ((stuckA || stuckB) ? "FAIL - still on screen 12s after the purge"
                                          : "PASS - both gone, queue released")
                    + " (A=" + (stuckA ? "stuck" : "gone")
                    + " B=" + (stuckB ? "stuck" : "gone")
                    + " forced=" + std::to_string(g_forced) + ")");
    }
}

}  // namespace notify

namespace p1 {

// Declared in config.hpp, called at the end of purgeDanglingActions. The purge removes every
// action the director holds, ours included; setTime re-issues the wait-then-hide chain (Geode's
// setTime calls waitThenHide), so the notification still goes away on its own and the queue is
// still released by its pop. What is left of its time is kept, with a floor so a purge landing
// on the last moment does not make it vanish mid-sentence.
inline void notifyActionsPurged() {
    if (!notify::g_fix) return;
    // The retiring one FIRST, and unconditionally: the chain the purge has just destroyed is the
    // one that ends in the queue pop, and nothing else will ever re-create it. This is the half
    // that was missing -- the test below is g_cur's, and at this moment g_cur is the notification
    // raised after it, which is queued and has nothing to repair, so the function returned having
    // done nothing and left the visible one frozen.
    if (notify::g_retiring) {
        if (notify::g_retiring->isShowing()) {
            notify::g_retiring->resumeSchedulerAndActions();
            notify::g_retiring->hide();
            notify::g_retiredAt = std::chrono::steady_clock::now();
            notify::g_retireStage = 0;
            ++notify::g_forced;
        } else {
            notify::g_retiring = nullptr;
        }
    }
    if (!notify::g_cur || !notify::g_cur->isShowing()) return;
    const double left = (double)notify::g_secs - notify::aliveSec();
    notify::g_cur->setTime((float)std::max(0.4, left));
    ++notify::g_forced;
}

}  // namespace p1
