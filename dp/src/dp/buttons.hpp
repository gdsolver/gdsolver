#pragma once
// The buttons phase of a step, as GD runs it, on the player's byte image (collide.hpp's Mem): GJBaseGameLayer::processCommands -> handleButton ->
// PlayerObject::pushButton 0x4f6200 / releaseButton 0x4f64c0 / releaseAllButtons 0x4fd450 / enablePlayerControls 0x4fd940, with ringJump 0x4f6f50 and
// resetTouchedRings 0x4f6600. Read from the 22091 decompile (notes/pushbutton-state-table-2091-1027.md, sections 3.1-3.3 and 4); the offsets are the
// decompile's (2.209 Windows). Same shape as collide.hpp: a pure function of the image (+ the few things that live outside it), written in place, and
// nothing else read -- the model's own press registers (State::gdHS, step.hpp hsTop ...) are a different thing and are not touched.
//
// One step is: buttons (processCommands) -> resetTouchedRings -> update -> collision pass (the pass registers rings with RingTouches::touched). So the
// buttons phase scans the array the PREVIOUS step's pass left, as resetTouchedRings of the step before has trimmed it.
//
// What lives outside the image and is passed in:
//   RingTouches        m_touchingRings (+0xa40, the CCArray in first-contact order) and the set at +0xa48
//   ButtonsBody::holding   m_holdingButtons (+0xbc0, a map<int,bool>): pushButton / releaseButton write it, enablePlayerControls reads it
//   usedRings          the set at +0x850 (the rings this press has jumped off; pushButton empties it)
//   ButtonsHooks       the calls that are not byte work of this file: updateJump(0) (upd.hpp), spiderTestJump, dashing, teleport, reverse, spawn orb. An empty one is counted
//                      as "not ported" (Ctx::unsupported) and the bytes it would write are left alone, except where the line says what the shadow assumes.
#include "dp/collide.hpp"
#include <algorithm>
#include <functional>

namespace dp {
namespace gdcol {

// m_touchingRings and m_touchedRings of one player. playerTouchedRing 0x31f5a0 adds a ring to both (the array only when it is not in it); resetTouchedRings(false) 0x4f6600 drops from the array every
// ring the set does not hold, then clears the set; ringJump takes the ring it fires out of the array (the set keeps it).
struct RingTouches {
    std::vector<int> arr;     // uids, first-contact order
    FlatSet<int> now;
    void touched(int uid) {
        if (std::find(arr.begin(), arr.end(), uid) == arr.end()) arr.push_back(uid);
        now.insert(uid);
    }
    void reset() {            // resetTouchedRings(false)
        std::vector<int> keep;
        for (int u : arr) if (now.count(u)) keep.push_back(u);
        arr.swap(keep);
        now.clear();
    }
    void clearAll() { arr.clear(); now.clear(); }   // resetTouchedRings(true)
    void remove(int uid) { arr.erase(std::remove(arr.begin(), arr.end(), uid), arr.end()); }
};

struct ButtonsIn {
    uint8_t press1 = 0, release1 = 0, press2 = 0, release2 = 0;   // the edges this step's processCommands hands to each body (button 1 = jump; 2 / 3 = platformer left / right; 5 = release all)
    bool dual = false;
};

struct ButtonsBody {
    Ctx* c = nullptr;                    // the image (c->P), the level's objects (ring lookup), p22
    RingTouches* touch = nullptr;
    bool holding[4] = {false, false, false, false};   // m_holdingButtons[1..3]
    FlatSet<int>* usedRings = nullptr;  // +0x850
};

struct ButtonsHooks {
    std::function<void(Ctx&, float)> updateJump;               // upd.hpp: pushButton calls updateJump(0)
    std::function<bool(Ctx&, const GObj*)> spiderTestJump;     // true = the spider found a surface and jumped (the caller's geometry); empty: taken to succeed
    std::function<void(Ctx&)> stopDashing;
    std::function<void(Ctx&, const GObj*)> startDashing;       // the dash ring's activatedByPlayer
    std::function<void(Ctx&, const GObj*)> teleportPlayer, reversePlayer, spawnOrb;
    std::function<void(Ctx&, int)> switchedDirTo;              // platformer direction buttons
};

// ----------------------------------------------------------------------------------------------------------------------------------------
// The decompile's offsets, by name
namespace bo {
enum : size_t {
    H = 0x98d, S = 0x98e, Hm = 0x991, Sm = 0x992, SlotA = 0x993, SlotB = 0x994, SlotC = 0x995, SlotD = 0x996,
    Hb = 0x9a4,                 // m_touchedPad: "released / touched a pad"
    Rf = 0x7e8,                 // class A ring fired in this press
    RingJumpRelated = 0x848,
    Dead = 0x9c8, Dashing = 0x9ec, Locked = 0xa32, ControlsDisabled = 0xa33, InputsLocked = 0xbd0,
    Jf = 0x95a, Streak = 0xa91,
    PlatB09 = 0xb09, PlatB08 = 0xb08, PlatB0a = 0xb0a, PlatB60 = 0xb60, PlatB21 = 0xb21, PlatB63 = 0xb63, PlatB28 = 0xb28, PlatDir = 0xb00,
    JumpCoef = 0x7c8,           // the double the ring's jump speed is a multiple of (param_1[0xf9])
    Size = 0x9f8,
};
}  // namespace bo

// [ring + 0x740], RingObject::m_claimTouch (a spawn orb's Claim Touch, property 445; RingObject::customObjectSetup 0x643d70 sets it only for type 0x24): pushButton fires such a ring alone and takes H (0x98d := 0);
// playerTouchedRing (0x31f5a0) does not fire it on a touch. The objrects `claim` column carries it (a dump that predates the column has none: false).
inline bool ringPressOnly(const GObj& o) { return o.claim; }

inline void hookCount(Ctx& c, int key) { unsup(c, key); }

// FUN_1404f6830 (22091): the player's direction byte +0x9ca set to `nw`; a change starts the turn counter +0x9d0 at 2, restarts the wave's streak (+0x5a0) and re-runs a ball's spin
// PlayerObject::ringJump 0x4f6f50. `ring` is the object pushButton found in the array (or playerTouchedRing's); the gate is the decompile's, the effects by type are its arms.
// Returns true when the gate let the ring through.
inline bool ringJump(ButtonsBody& b, GObj* ring, const ButtonsHooks& hk) {
    Ctx& c = *b.c;
    Mem& P = c.P;
    if (P.u8(bo::Dead)) return false;
    if (!ring) return false;
    if (b.usedRings && b.usedRings->count(ring->uid)) return false;
    const int ty = ring->type;
    const bool classA = !(ty == 0x24 || ty == 0x2e);
    if (!P.u8(bo::Sm)) return false;
    if (P.u8(bo::Dashing)) return false;
    if (!P.u8(bo::Hm)) return false;
    if ((P.u8(bo::SlotA) || !classA) && (P.u8(bo::SlotB) || ty != 0x24)) {
        if (P.u8(bo::SlotC)) return false;
        if (ty != 0x2e) return false;
    }
    if (ring->reverse) { if (hk.reversePlayer) hk.reversePlayer(c, ring); else reversePlayerPort(c, ring); }
    P.u8(bo::RingJumpRelated, 1);
    if (b.usedRings) b.usedRings->insert(ring->uid);
    noteFired(c.fireOrder, ring->uid);
    gdev::noteOrbEvents(c.gameEvents, ring->uid, ty);   // GD Win 2.2091 ringJump: calls at 0x4f7123 / 0x4f7153 precede spiderTestJump at 0x4f7265.
    // Native listeners spawn synchronously here (0x33ac80 -> 0x322370 -> 0x3227a0); the DP feeds these tokens in the next layer, so this search cannot yet see their immediate geometry changes.
    P.u8(ty == 0x24 ? bo::SlotB : ty == 0x2e ? bo::SlotC : bo::SlotA, 1);
    if (b.touch) b.touch->remove(ring->uid);
    if (classA) P.u8(bo::Rf, 1);
    const bool upside = P.u8(po::Upside) != 0;
    auto flip = [&]() { const uint8_t u0 = P.u8(po::Upside); playerFlipGravity(c, upside ? 0 : 1, true); dualRingFlip(c, u0); };   // (the partner: GJBaseGameLayer::flipGravity, --shadowdual)
    auto ringRotation = [&]() {
        if (!P.u8(po::ModeBall)) {
            if (P.u8(bo::Locked) == 0 && P.u8(bo::Dashing) == 0) {
                P.put<uint16_t>(0x730, 0);
                P.u8(0x670, 0);
                P.put<uint32_t>(0x728, 0);
                runNormalRotation(P, false, 1.0f);
            }
        } else {
            runBallRotation2(P);
        }
    };
    if (ty == 0x24) {
        if (hk.spawnOrb) hk.spawnOrb(c, ring); else hookCount(c, 2002);
        if (c.p22) P.u8(bo::S, 0);
    } else if (ty == 0x2e) {
        if (hk.teleportPlayer) hk.teleportPlayer(c, ring); else hookCount(c, 2003);
        P.u8(bo::S, 0);
    } else if (ty == 0x2b) {
        // the spider orb (ringJump 0x4f6f50, the 0x2b arm): gravity is flipped to the orb's facing (isFacingLeft for a turned frame, isFacingDown otherwise), then spiderTestJump
        {
            const bool facing = P.u8(po::Sideways) ? objFacingLeft(*ring) : objFacingDown(*ring);
            if (facing != (P.u8(po::Upside) != 0)) flip();
        }
        const bool ok = hk.spiderTestJump ? hk.spiderTestJump(c, ring) : true;
        if (ok) P.u8(bo::H, 0);
    } else if (ty == 0x25 || ty == 0x26) {
        if (ty == 0x26) flip();
        if (hk.startDashing) hk.startDashing(c, ring); else hookCount(c, 2005);
    } else {
        P.u8(bo::S, 0);
        if (ty == 0x20) {                                           // drop
            const int dir = upside ? -1 : 1;
            float v = (float)dir * -15.0f;
            const bool fly = P.u8(po::ModeShip) || P.u8(0x9c2) || P.u8(po::ModeWave) || P.u8(po::ModeSwing);
            if (fly) {
                v = (float)(upside ? -1 : 1) * -14.0f;
                if (P.u8(0x9c2)) v *= 0.8f;
            } else if (!P.u8(po::ModeRobot) && P.u8(po::ModeSpider)) {
                v *= 1.1f;
            }
            setYVelocity(P, (double)v);
            ringRotation();
            streakResume(P, &c.L);
            P.u8(bo::Streak, 1);
            P.u8(bo::Jf, 1);
            if (P.u8(po::ModeBall) || P.u8(po::ModeSwing)) P.u8(bo::H, 0);
        } else {                                                    // yellow 0xb, pink 0xc, blue 0xd, green 0x1d, red 0x23 (and any other type that reaches here)
            P.u8(po::Boost, 1);
            P.u8(po::G2, 0);
            P.u8(po::G, 0);
            const bool mini = P.f32(bo::Size) != 1.0f;
            float sizeK = mini ? 0.8f : 1.0f;
            float coef = (float)P.f64(bo::JumpCoef);
            const bool ship = P.u8(po::ModeShip), ufo = P.u8(0x9c2), ball = P.u8(po::ModeBall), robot = P.u8(po::ModeRobot), spider = P.u8(po::ModeSpider);
            if (ty == 0xd) {
                coef *= 0.8f;
            } else if (ty == 0x1d) {
                if (ship) coef *= 0.7f;
            } else if (ty == 0xc) {
                if (!ship) {
                    if (!ufo) coef *= ball ? 0.77f : 0.72f;
                    else coef *= 0.42f;
                } else {
                    coef *= 0.37f;
                }
            } else if (ty == 0x23) {
                if (!ship) {
                    if (!ufo) {
                        if (!ball) coef *= robot ? 1.28f : spider ? 1.34f : 1.38f;
                        else coef *= 1.34f;
                    } else {
                        coef *= mini ? 1.36f : 1.02f;
                    }
                } else if (mini) {
                    coef *= 1.4f;
                }
            } else if (robot) {
                coef *= 0.9f;
            }
            if (ty == 0x1d) flip();                                 // green: the flip comes before the velocity
            const bool up2 = P.u8(po::Upside) != 0;
            setYVelocity(P, (double)((float)(up2 ? -1 : 1) * coef * sizeK));
            ringRotation();
            P.f32(0xa34, P.f32(po::PosX));
            P.f32(0xa38, P.f32(po::PosY));
            streakResume(P, &c.L);
            P.u8(bo::Streak, 1);
            if (!ball) {
                if (P.u8(po::ModeSwing)) { P.f64(po::Vy, 0.6000000238418579 * P.f64(po::Vy)); P.u8(bo::H, 0); }
                else if (spider) { P.f64(po::Vy, 0.699999988079071 * P.f64(po::Vy)); P.u8(bo::H, 0); }
            } else {
                P.f64(po::Vy, 0.699999988079071 * P.f64(po::Vy));
                P.u8(bo::H, 0);
            }
            if (ty == 0xd) flip();                                  // blue: after the velocity
            if (ty == 0x23) P.u8(bo::Jf, 1);
        }
    }
    // the end of every ring: activatedByPlayer (+0x568) marks the ring, so a ring that is not multi-activate is skipped by the next collision passes
    if (!ring->multiAct) { ring->activated = true; if (c.actLog) c.actLog->push_back(ring); }   // (actLog: selfshadow.hpp writes back only the copies a step marked)
    return true;
}

// PlayerObject::pushButton 0x4f6200
inline bool pushButton(ButtonsBody& b, int btn, const ButtonsHooks& hk) {
    Ctx& c = *b.c;
    Mem& P = c.P;
    if (P.u8(bo::InputsLocked) == 0 && btn >= 1 && btn <= 3) b.holding[btn] = true;
    if (P.u8(bo::Locked) || P.u8(bo::ControlsDisabled)) return false;
    if (btn == 1) {
        if (!P.u8(bo::H)) P.u8(0x5a0, 0);                     // FUN_1404fec10 (the streak's restart at the player's position) clears this byte, as flipGravity's call of it does
        P.u8(bo::H, 1);
        P.u8(bo::S, 1);
        P.u8(bo::Rf, 0);
        if (b.usedRings) b.usedRings->clear();
        P.u8(bo::Hm, P.u8(bo::H));
        P.u8(bo::Sm, P.u8(bo::S));
        for (size_t o : {(size_t)bo::SlotA, (size_t)bo::SlotB, (size_t)bo::SlotC, (size_t)bo::SlotD}) P.u8(o, 0);
        const std::vector<int> touching = b.touch ? b.touch->arr : std::vector<int>();
        if (touching.empty()) {
            if (!P.u8(po::ModeBall)) {
                if (P.u8(po::ModeShip) || P.u8(0x9c2) || P.u8(po::ModeWave) || P.u8(po::ModeSwing)) return true;
                if (!P.u8(bo::H)) return true;
            }
            if (P.u8(po::G)) {
                if (hk.updateJump) hk.updateJump(c, 0.0f); else hookCount(c, 2010);
            }
        } else {
            std::vector<GObj*> spawn, other;
            for (int uid : touching) {
                GObj* o = c.byUid(uid);
                if (!o) continue;
                if (ringPressOnly(*o)) {
                    ringJump(b, o, hk);
                    P.u8(bo::H, 0);
                    return true;
                }
                (o->type == 0x24 ? spawn : other).push_back(o);
            }
            for (GObj* o : spawn) ringJump(b, o, hk);
            for (GObj* o : other) ringJump(b, o, hk);
        }
    } else if (btn == 2 || btn == 3) {
        if (hk.switchedDirTo) hk.switchedDirTo(c, btn); else hookCount(c, 2011);
    }
    return true;
}

// PlayerObject::releaseButton 0x4f64c0
inline bool releaseButton(ButtonsBody& b, int btn, const ButtonsHooks& hk) {
    Ctx& c = *b.c;
    Mem& P = c.P;
    if (btn == 5) {
        releaseButton(b, 2, hk);
        releaseButton(b, 3, hk);
        return P.u8(bo::ControlsDisabled) == 0;
    }
    if (P.u8(bo::InputsLocked) == 0 && btn >= 1 && btn <= 3) b.holding[btn] = false;
    if (P.u8(bo::ControlsDisabled)) return false;
    if (P.u8(bo::H)) P.u8(0x5a0, 0);                          // the streak call (see pushButton)
    if (btn == 1) {
        P.put<uint16_t>(bo::H, 0);
        P.u8(bo::Hb, 1);
        if (P.u8(bo::Dashing)) {
            if (hk.stopDashing) hk.stopDashing(c); else hookCount(c, 2012);
            return true;
        }
    } else {
        const double d = P.f64(bo::PlatDir);
        if (btn == 2) {
            P.u8(bo::PlatB09, 0);
            if (0.0 < d || d == 0.0) return true;
        } else {
            if (btn != 3) return true;
            P.u8(0xb08, 0);
            if (d <= 0.0) return true;
        }
        P.u8(bo::PlatB60, 0);
    }
    return true;
}

// PlayerObject::releaseAllButtons 0x4fd450
inline void releaseAllButtons(ButtonsBody& b, const ButtonsHooks& hk) {
    Ctx& c = *b.c;
    Mem& P = c.P;
    const bool locked = P.u8(bo::InputsLocked) != 0;
    if (!locked) b.holding[1] = false;
    if (P.u8(bo::ControlsDisabled) == 0) {
        if (P.u8(bo::H)) P.u8(0x5a0, 0);
        P.put<uint16_t>(bo::H, 0);
        P.u8(bo::Hb, 1);
        if (P.u8(bo::Dashing)) { if (hk.stopDashing) hk.stopDashing(c); else hookCount(c, 2012); }
    }
    if (!P.u8(bo::InputsLocked)) b.holding[2] = false;
    if (P.u8(bo::ControlsDisabled) == 0) {
        P.u8(bo::PlatB09, 0);
        const double d = P.f64(bo::PlatDir);
        if (d <= 0.0 && d != 0.0) P.u8(bo::PlatB60, 0);
    }
    if (!P.u8(bo::InputsLocked)) b.holding[3] = false;
    if (P.u8(bo::ControlsDisabled) == 0) {
        P.u8(0xb08, 0);
        if (0.0 < P.f64(bo::PlatDir)) P.u8(bo::PlatB60, 0);
    }
    releaseButton(b, 2, hk);
    releaseButton(b, 3, hk);
}

// PlayerObject::enablePlayerControls 0x4fd940: the window lifts; a button held through it presses again
inline void enablePlayerControls(ButtonsBody& b, const ButtonsHooks& hk) {
    Ctx& c = *b.c;
    Mem& P = c.P;
    P.u8(bo::ControlsDisabled, 0);
    bool done = false;
    if (!b.holding[2]) {
        if (!b.holding[3]) {
            done = true;
        } else {
            if (P.u8(bo::InputsLocked) == 0) { b.holding[3] = true; b.holding[2] = false; }
            if (P.u8(bo::ControlsDisabled)) done = true;
            else {
                P.u8(bo::PlatB08, 1);
                P.u8(bo::PlatB0a, 0);
                if (P.u8(bo::PlatB21) && P.u8(bo::PlatB63)) { P.u8(bo::PlatB21, 0); P.put<uint64_t>(bo::PlatB28, 0); }
                const double d = P.f64(bo::PlatDir);
                if (0.0 < d || d == 0.0) done = true;
                else P.u8(bo::PlatB60, 0);
            }
        }
    } else {
        if (P.u8(bo::InputsLocked) == 0) { b.holding[3] = false; b.holding[2] = true; }
        if (P.u8(bo::ControlsDisabled)) done = true;
        else {
            P.put<uint16_t>(bo::PlatB09, 0x101);
            if (P.u8(bo::PlatB21) && !P.u8(bo::PlatB63)) { P.u8(bo::PlatB21, 0); P.put<uint64_t>(bo::PlatB28, 0); }
            if (P.f64(bo::PlatDir) <= 0.0) done = true;
            else P.u8(bo::PlatB60, 0);
        }
    }
    (void)done;
    if (b.holding[1]) pushButton(b, 1, hk);
}

// The routine GJBaseGameLayer::processOptionsTrigger calls to take the controls from the player (FUN_1404fd790 in the 2.2091 exe, read from its decompile): the lock byte goes up while the buttons are let go,
// the queued press (+0x98d) and the dash end, the platformer buttons 2 and 3 are released, and +0xa33 stays 1. Player 1 only; the caller applies it in the step the Options trigger names, before the
// step's own button edges (btraw: the releases of buttons 2 and 3 come before the plan's release of button 1).
inline void disablePlayerControls(ButtonsBody& b, const ButtonsHooks& hk) {
    Ctx& c = *b.c;
    Mem& P = c.P;
    P.u8(bo::ControlsDisabled, 0);
    P.u8(bo::InputsLocked, 1);
    if (P.u8(bo::H)) P.u8(0x5a0, 0);                          // FUN_1404fec10: the streak call
    P.put<uint16_t>(bo::H, 0);
    P.u8(bo::Hb, 1);
    if (P.u8(bo::Dashing)) { if (hk.stopDashing) hk.stopDashing(c); else hookCount(c, 2012); }
    // (the holding map's entries 2 and 3 are cleared only while the lock is down: it is, so they are left)
    {
        P.u8(bo::PlatB09, 0);
        const double d = P.f64(bo::PlatDir);
        if (d <= 0.0 && d != 0.0) P.u8(bo::PlatB60, 0);
    }
    {
        P.u8(bo::PlatB08, 0);
        if (0.0 < P.f64(bo::PlatDir)) P.u8(bo::PlatB60, 0);
    }
    releaseButton(b, 2, hk);
    releaseButton(b, 3, hk);
    P.u8(bo::InputsLocked, 0);
    P.u8(bo::ControlsDisabled, 1);
}

// GJBaseGameLayer::handleButton: the edges of one step, for one body, in the order the game feeds them (a release before a press when both come in one step)
inline void applyEdges(ButtonsBody& b, uint8_t press, uint8_t release, const ButtonsHooks& hk) {
    if (release) releaseButton(b, release, hk);
    if (press) pushButton(b, press, hk);
}

// The buttons phase of one step. Then the caller runs resetTouchedRings (RingTouches::reset on each body), the update phase and the collision pass.
inline void buttonsPhase(ButtonsBody& p1, ButtonsBody* p2, const ButtonsIn& in, const ButtonsHooks& hk) {
    applyEdges(p1, in.press1, in.release1, hk);
    if (p2 && in.dual) applyEdges(*p2, in.press2, in.release2, hk);
}

}  // namespace gdcol
}  // namespace dp
