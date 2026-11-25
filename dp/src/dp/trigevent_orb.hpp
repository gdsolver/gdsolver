#pragma once
// The Toggle Orb producer of restructure draft section 7.5 (P4): when a Toggle Orb (1594, object type 36) is clicked, as a pure function of the player's place at two rows, the button state and the orb's place.
// A transcription of the measured rule in tools/trig_replay.cpp (--pickup, notes/trigshadow-wiring-plan-1104.md sections 18 / 21 / 22 / 24; PlayerObject::pushButton / ringJump / playerTouchedRing): two ways in --
//  (1) the PUSH: pushButton fires the rings the player touches at the click's tick; the box tested is the one of the row BEFORE the call row (the orb has already moved the player at the call row), and the call is
//      made in the button phase, after the collision pass (late);
//  (2) the CONTACT: playerTouchedRing calls ringJump when the player reaches the orb while a push armed it (armed from a push that was not on the ground until the release has been seen by an earlier tick), once per push
//      (consumed), unless the mode forbids it (ship / ufo / wave / swing: noContact); the box is the one of the call row.
// An orb already used does not fire again unless it is Multi Activate. The player's box is 30 x 30 times the size (27 x 27 for the spider); the orb's is its rect, or its oriented box when it is rotated.
// Nothing here is wired into the search yet; dptest_main.cpp holds the fixtures.
#include <cmath>

#include "dp/trigevent.hpp"

namespace dp {
namespace gdtrig {

struct OrbRect { double cx = 0, cy = 0, w = 0, h = 0; double rot = 0, w0 = 0, h0 = 0; };   // w / h: the bounding rect, w0 / h0 the unrotated size times the object's scale
struct OrbPlayer { double x = 0, y = 0, vsize = 1.0; bool spider = false; bool valid = true; };

// the player's half box against the orb's rect (and, for a rotated orb with `obb`, its oriented box: the separating-axis test)
// `halfOverride` > 0 replaces the player's half box (times the size): trig_replay --orbhalf.
inline bool orbOverlap(const OrbRect& r, const OrbPlayer& p, bool obb = true, bool spiderBox = true, double halfOverride = 0.0) {
    if (!p.valid) return false;
    const double vs = p.vsize > 0.0 ? p.vsize : 1.0;
    const double half = halfOverride > 0.0 ? halfOverride * vs : (spiderBox && p.spider) ? 13.5 * vs : 15.0 * vs;
    bool hit = std::fabs(p.x - r.cx) <= half + r.w * 0.5 && std::fabs(p.y - r.cy) <= half + r.h * 0.5;
    const double m = std::fabs(std::fmod(std::fabs(r.rot), 90.0));
    if (hit && obb && r.w0 > 0.0 && m > 0.01 && std::fabs(m - 90.0) > 0.01) {
        const double th = r.rot * 3.14159265358979323846 / 180.0, co = std::cos(th), si = std::sin(th);
        const double hx = r.w0 * 0.5, hy = r.h0 * 0.5, dx = p.x - r.cx, dy = p.y - r.cy;
        const double axs[4][2] = {{1, 0}, {0, 1}, {co, si}, {-si, co}};
        for (const auto& a : axs) {
            const double ra = half * (std::fabs(a[0]) + std::fabs(a[1]));
            const double rb = hx * std::fabs(a[0] * co + a[1] * si) + hy * std::fabs(-a[0] * si + a[1] * co);
            if (std::fabs(dx * a[0] + dy * a[1]) > ra + rb) { hit = false; break; }
        }
    }
    return hit;
}

struct OrbInput {
    bool pushNow = false;      // a press whose pushButton call is this step's (the click of game tick F = step - lag)
    bool armed = false;        // a push armed the contact path: the last button event before F is a press, and that push was not on the ground
    bool consumed = false;     // one Toggle Orb per push: an orb has already fired since that push
    bool noContact = false;    // the mode (ship / ufo / wave / swing) does not call ringJump from playerTouchedRing
    bool done = false;         // this orb has fired before in the attempt
    bool multi = false;        // ... and it is Multi Activate
    bool contactPath = true;   // --orbheld: the contact path exists at all
};

struct OrbCall { bool fire = false; bool push = false; };

// `prevRow` is the player's place one row before the call row, `curRow` at the call row
inline OrbCall orbDecide(const OrbRect& orb, const OrbPlayer& prevRow, const OrbPlayer& curRow, const OrbInput& in, bool obb = true, bool spiderBox = true, double halfOverride = 0.0) {
    OrbCall c;
    if (in.done && !in.multi) return c;
    const bool hPush = in.pushNow && orbOverlap(orb, prevRow, obb, spiderBox, halfOverride);
    const bool hContact = in.contactPath && in.armed && !in.consumed && !in.noContact && orbOverlap(orb, curRow, obb, spiderBox, halfOverride);
    c.fire = hPush || hContact;
    c.push = hPush;
    return c;
}

// the Event for a call: step = the shadow step of the call row, late = the push path (applied after the collision pass)
inline Event orbEvent(long long step, int uid, bool push) {
    Event e;
    e.kind = kEvOrb; e.step = step; e.uid = uid; e.group = -1; e.on = false; e.late = push;
    return e;
}

}  // namespace gdtrig
}  // namespace dp