// Dual (player 2) for the self-running shadow (--shadowdual; selfshadow.hpp steps player 1 alone). One SelfShadow steps both players: the player who is not
// stepping waits in a DualShadow::two slot, and the slot is swapped in for player 2's phases, so the shadow's world, its section index and its Ctx stay one.
// The game's order (GJBaseGameLayer::update 0x3406d0, 22091):
//   - the buttons: handleButton 0x33c5b0 gives an edge to player 1, then, in a dual of a one-player level, the same edge to player 2; resetTouchedRings and the
//     collision logs for player 1, then player 2 (update :246-269). Here: player 1's stepHead, then player 2's on the same clock;
//   - player 2's +0x9d8 / +0x9e0 (the reverse correction) := player 1's, every step (:335-336);
//   - player 1's update and pass, then player 2's update and pass, the latter only when the layer was in a dual before player 1's pass AND still is (:290 takes
//     +0x422 before the step, :360 tests both): a player 2 born in this step's pass of player 1 is not stepped in it, nor one that player 1 ended;
//   - checkRepellPlayer (FUN_1403424e0, at the end of a whole step, :458): two balls of the same gravity that come within 15 * scale each + 5 in y;
//   - a death of either is a death of both (PlayLayer::destroyPlayer 0x512390 :343-357).
// The calls the pass and the buttons make into the other player go through Ctx::dualH (collide.hpp DualHooks): GJBaseGameLayer::flipGravity's partner, the
// gravity part of FUN_14031a210 (playerWillSwitchMode), toggleDualMode.
// Two-player level (levelSettings +0x154): player 2 takes its own edge (own2 / press2 / release2); not ported yet: the birth's extra bytes (toggleDualMode). Not ported: the Options trigger's Unlink Dual
// Gravity (+0x860, taken as 0), the click-between half step, player 1's rotation that the game runs after player 2's pass (here player 1's whole tail is first),
// FUN_1404f69a0 (a wave player 1 at the dual's end): counted. GObj::activated is one flag for both players.
#pragma once
#include "dp/selfshadow.hpp"

#include <cmath>
#include <utility>

namespace dp {
namespace gdcol {

// What a player keeps between steps that is not the layer's (SelfShadow's own members of a player)
struct PlayerSlot {
    Mem img;
    RingTouches rt;
    FlatSet<int> usedRings, gate, ever;
    std::vector<int> fireOrder;
    SelfShadow::EverRule everRule;   // (ever's rule part, when an unpack left it out of `ever`)
    bool holding[4] = {false, false, false, false};
    float dashRot = 0.0f, dashElapsed = 0.0f, dashSpinRate = 0.0f;
    bool dashOn = false, dashFirstDone = false;
    bool speedDirty = false;
    float pendingSpeed = 1.0f;
    Mem atButtons{0}, atEntry{0}, atExit{0};
    SelfShadow::StepPoint ptButtons, ptEntry;
    PlayerSlot() {}   // (Mem's constructor is explicit: no aggregate PlayerSlot{})
};

inline void swapPlayer(SelfShadow& sh, PlayerSlot& s) {
    sh.p2Phase = !sh.p2Phase; sh.activatedChanged();   // the reused neighbourhood copies carry the other player's fired flags now
    std::swap(sh.img.b, s.img.b);
    std::swap(sh.rt, s.rt);
    std::swap(sh.usedRings, s.usedRings);
    std::swap(sh.fireOrder, s.fireOrder);
    std::swap(sh.gate, s.gate);
    std::swap(sh.ever, s.ever);
    std::swap(sh.everRule, s.everRule);
    for (int k = 0; k < 4; ++k) std::swap(sh.holding[k], s.holding[k]);
    std::swap(sh.dashRot, s.dashRot); std::swap(sh.dashElapsed, s.dashElapsed); std::swap(sh.dashSpinRate, s.dashSpinRate);
    std::swap(sh.dashOn, s.dashOn); std::swap(sh.dashFirstDone, s.dashFirstDone);
    std::swap(sh.speedDirty, s.speedDirty); std::swap(sh.pendingSpeed, s.pendingSpeed);
    std::swap(sh.atButtons.b, s.atButtons.b); std::swap(sh.atEntry.b, s.atEntry.b); std::swap(sh.atExit.b, s.atExit.b);
    std::swap(sh.ptButtons, s.ptButtons); std::swap(sh.ptEntry, s.ptEntry);
}

// PlayerObject::copyAttributes 0x4fea30: `d` takes `s`'s clock, positions, gravity, mode, speed, size, velocity and press bytes. +0x7e9 is up while it runs
// (flipGravity and the toggles then do not halve the velocity or restart the streak).
inline void copyAttributes(Mem& d, const Mem& s, const Layer& L, int& unsupported) {
    Ctx c(d);
    c.L = L;
    d.u8(0x7e9, 1);
    d.f64(po::Time, s.f64(po::Time));                                  // param_1[0x155]
    d.f32(0xa98, s.f32(0xa98)); d.f32(0xa9c, s.f32(0xa9c));            // +0x153 * 8
    setPosition(d, s.f32(po::PosX), s.f32(po::PosY));                  // vtable +0xb8 with the source's getPosition
    d.f32(0xa00, s.f32(0xa00)); d.f32(0xa04, s.f32(0xa04));            // +0x140 * 8
    playerFlipGravity(c, s.u8(po::Upside), true);
    setLeftward(d, s.u8(po::Leftward));                               // GD Win 2.2091: copyAttributes reads source +0x9ca at RVA 0x4feacf and calls 0x4f6830 at 0x4fead9.
    toggleMode(c, kShip, s.u8(kShip) != 0);                           // toggleFlyMode .. toggleSwingMode, in the function's order
    toggleMode(c, kUfo, s.u8(kUfo) != 0);
    toggleMode(c, kBall, s.u8(kBall) != 0);
    toggleMode(c, kWave, s.u8(kWave) != 0);
    toggleMode(c, kRobot, s.u8(kRobot) != 0);
    toggleMode(c, kSpider, s.u8(kSpider) != 0);
    toggleMode(c, kSwing, s.u8(kSwing) != 0);
    upd::updateTimeMod(d, s.f32(0x9fc));
    togglePlayerScale(c, s.f32(0x9f8) != 1.0f);
    setYVelocity(d, s.f64(po::Vy));
    d.u8(0x7e9, 0);
    d.u8(0x98d, s.u8(0x98d));
    d.u8(0x98e, s.u8(0x98e));
    unsupported += c.unsupported;
}

class DualShadow {
public:
    SelfShadow* sh = nullptr;
    PlayerSlot two;             // the player who is not stepping: player 2 between steps and in player 1's phases, player 1 in player 2's
    bool active = false;        // the layer's +0x422 as the hooks see it within a step
    bool twoPlayer = false;     // levelSettings +0x154
    bool own2 = false;          // twoPlayer: player 2 takes its OWN edge (press2 / release2, set by the caller before head()); handleButton 0x2338e0 gives player 1's button to player 1 only
    uint8_t press2 = 0, release2 = 0;
    bool ghost2 = false;        // a search's ghost (--ghostlevels): the second body's death ends nothing
    bool unlink = false;        // +0x860, the Options trigger's Unlink Dual Gravity (not bridged), or a two-player level (no partner flip: the callers below)
    bool unlinkOpt = false;     // +0x860 itself: ONLY the Options trigger's value. toggleDualMode (0x2168d0) reads this byte alone for the new body's gravity and velocity; a two-player level does not make it 1
    bool born = false;         // player 2 was born in this step (not stepped in it)
    bool ended = false;         // the dual ended in this step
    int endedIn = 0;            // ...and in which player's pass (1 or 2): the pass of the player who took the dual-off portal
    bool everBorn = false;      // player 2's image has been made (the first birth starts from the template)
    int inPass = 1;             // the player stepping now
    int unsupported = 0;        // what the dual calls met that is not ported (summed into player 1's SelfOut)
    std::vector<uint8_t> tmpl;  // a player 2 that was never born: collide_tmpl's image (the classic cube at rest)
    DualHooks hooks;

    DualShadow() {
        hooks.self = this;
        hooks.flip = [](void* s, Ctx& c, uint8_t up, bool p3) { static_cast<DualShadow*>(s)->partnerFlip(c, up, p3); };
        hooks.willSwitch = [](void* s, Ctx& c, int type) { static_cast<DualShadow*>(s)->willSwitch(c, type); };
        hooks.portal = [](void* s, Ctx& c, bool on) { static_cast<DualShadow*>(s)->portal(c, on); };
        tmpl.assign(tmpl::kImageBytes, 0);
        auto hv = [](char ch) { return ch <= '9' ? ch - '0' : (ch | 0x20) - 'a' + 10; };
        for (size_t i = 0; i < tmpl::kImageBytes; ++i) tmpl[i] = (uint8_t)(hv(tmpl::kImageHex[2 * i]) * 16 + hv(tmpl::kImageHex[2 * i + 1]));
        freshen(tmpl);
        two.img.b = tmpl;
    }

    // collide_tmpl's image is player 1 in the middle of a classic level; a player 2 that was never stepped holds the constructor's values in the words below
    // instead (measured: the first player-2 pass of o_lv20_17270 and a_113256289_18100_20700 agree on them; checked on c_119550490_4800_5110):
    // the counters +0xae4, +0xaec..+0xaf8, +0xb7c..+0xb88, +0xb90 and +0x9d0 are 0 (the first update counts them down to -1, and on: a player 2 of 326 passes holds -326),
    // +0x690 is -1, the cached rect
    // (+0x358..+0x367) is empty, the first-pass byte +0x671 (checkCollisions) is clear, the y history index / clock (+0xac4, +0xae0) are 0, +0xa18 is 0.0,
    // +0xa30, +0x7f0, +0xb40, +0xb41 are 0, +0xaa0 is 1, +0xc44 is 0.0.
    static void freshen(std::vector<uint8_t>& b) {
        Mem m; m.b = b;
        for (size_t o : {(size_t)0xae4, (size_t)0xaec, (size_t)0xaf0, (size_t)0xaf4, (size_t)0xaf8, (size_t)0xb7c, (size_t)0xb80, (size_t)0xb84, (size_t)0xb88, (size_t)0xb90, (size_t)0x9d0}) m.i32(o, 0);
        m.i32(0x690, -1);
        for (size_t o = 0x358; o < 0x368; o += 4) m.i32(o, 0);
        m.u8(0x671, 0);
        m.i32(0xac4, 0); m.f32(0xae0, 0.0f);
        m.f64(0xa18, 0.0);
        m.u8(0xa30, 0); m.u8(0x7f0, 0); m.u8(0xb40, 0); m.u8(0xb41, 0);
        m.u8(0xaa0, 1);
        m.f32(0xc44, 0.0f);
        b = m.b;
    }

    // player 2 from an image the game recorded (a window, or an anchor, that starts inside a dual)
    void seedTwo(const std::vector<uint8_t>& img) { two.img.b = img; everBorn = true; active = true; }

    // the buttons of a step: player 1's head, then player 2's on the same clock (the layer advances it once)
    void head(const SelfIn& in1, SelfOut& o1, SelfOut& o2) {
        sh->dualH = &hooks;
        inPass = 1;
        const double lt0 = sh->layerTime; const int fs0 = sh->frameSteps, pf0 = sh->prevFrameSteps;
        sh->stepHead(in1, o1);
        if (!active) return;
        const double lt1 = sh->layerTime; const int fs1 = sh->frameSteps, pf1 = sh->prevFrameSteps;
        SelfIn in2 = stepIn(in1);
        if (twoPlayer && own2) { in2.press1 = press2; in2.release1 = release2; }   // handleButton: a two-player level's player 2 takes its own edge
        else { in2.press1 = in1.press1; in2.release1 = in1.release1; }            // handleButton: a one-player level's edge goes to both
        in2.ctrl = in1.ctrl2;                                      // --shadowdualctrl: player 2's own Options event
        swapPlayer(*sh, two); inPass = 2;
        sh->layerTime = lt0; sh->frameSteps = fs0; sh->prevFrameSteps = pf0;
        sh->stepHead(in2, o2);
        swapPlayer(*sh, two); inPass = 1;
        sh->layerTime = lt1; sh->frameSteps = fs1; sh->prevFrameSteps = pf1;
    }

    // the rest of the step. `o2.ran` says whether player 2 was stepped.
    void tail(const SelfIn& in1, SelfOut& o1, SelfOut& o2) {
        sh->dualH = &hooks;
        born = false; ended = false;
        const bool wasActive = active;
        std::memcpy(two.img.b.data() + 0x9d8, sh->img.b.data() + 0x9d8, 16);   // update :335-336: player 2's reverse correction is player 1's
        inPass = 1;
        sh->stepTail(in1, o1);
        if (wasActive && active) {
            SelfIn in2 = stepIn(in1);
            auto wp = std::move(sh->worldPhase);                    // (the world moved once, for player 1)
            sh->worldPhase = nullptr;
            swapPlayer(*sh, two); inPass = 2;
            sh->stepTail(in2, o2);
            swapPlayer(*sh, two); inPass = 1;
            sh->worldPhase = std::move(wp);
            if (ended && endedIn == 2) {
                // The dual ended in player 2's pass: the body left is player 1's image, which copyAttributes just gave player 2's position, previous position and frame's position (the frame's position is the one
                // stored at player 2's last frame end, up to a frame old). What the layer does to the players after the passes (update :387-402, :587-595: the previous position and, at a frame's end, the frame's
                // position) is done to the body that is left as it stands now, not as it stood in player 1's own tail before the copy (that tail ran first). Without it the next frame head puts the body back
                // at the old frame's position: a custom level t=4633, GD's body stays at y 212.368 and the shadow's went to y 219.840 (player 2's position of t=4629).
                Mem& d = sh->img;
                d.f32(0xa00, d.f32(po::PosX));
                d.f32(0xa04, d.f32(po::PosY));
                if (in1.end >= 0 ? in1.end != 0 : in1.tick % sh->frameLen == sh->frameEndMod) { d.f32(0xa98, d.f32(po::PosX)); d.f32(0xa9c, d.f32(po::PosY)); }
            }
        } else if (born) {
            // a player 2 born in this pass is not updated in this step, but what the layer does to both players after the passes reaches it, the layer being
            // in a dual by then (update :387-402, :587-595): +0xa10 := its rotation, updateRotation, the previous position, and at a frame's end the frame's position.
            // (o_lv20_17270 t=17545: without the frame's store the next frame head put the newborn a frame behind)
            Mem& d = two.img;
            d.f32(0xa10, d.f32(0x38));
            upd::updateRotation(d, in1.dt);
            d.f32(0xa00, d.f32(po::PosX));
            d.f32(0xa04, d.f32(po::PosY));
            if (in1.end >= 0 ? in1.end != 0 : in1.tick % sh->frameLen == sh->frameEndMod) { d.f32(0xa98, d.f32(po::PosX)); d.f32(0xa9c, d.f32(po::PosY)); }
        }
        // a speed portal sets the layer's speed: both players take it at the next head (update :239-241)
        if (sh->speedDirty && !two.speedDirty) { two.speedDirty = true; two.pendingSpeed = sh->pendingSpeed; }
        else if (two.speedDirty && !sh->speedDirty) { sh->speedDirty = true; sh->pendingSpeed = two.pendingSpeed; }
        if (active && !born && !o1.died && !o2.died) repel();
        if (o2.died && !ghost2) o1.died = true;                     // destroyPlayer: both (a search's ghost ends nothing)
        o1.unsupported += unsupported;
        if (unsupported) o1.unsupportedBy[1104] += unsupported;
        unsupported = 0;
    }

private:
    static SelfIn stepIn(const SelfIn& in1) {
        SelfIn in2;
        in2.tick = in1.tick; in2.dt = in1.dt; in2.head = in1.head; in2.end = in1.end; in2.recordGameEvents = in1.recordGameEvents;
        return in2;
    }
    static bool sameModes(const Mem& a, const Mem& b) {   // the six bytes GJBaseGameLayer::flipGravity compares (not the wave's)
        for (size_t f : {(size_t)kShip, (size_t)kBall, (size_t)kUfo, (size_t)kSpider, (size_t)kRobot, (size_t)kSwing}) if (a.u8(f) != b.u8(f)) return false;
        return true;
    }
    // GJBaseGameLayer::flipGravity 0x319e20, after this player's own flip to `up`: the other one is flipped to !up when the layer is in a dual of a one-player
    // level, the dual gravity is linked (+0x860 == 0) and the two are in the same mode (the wave not compared)
    void partnerFlip(Ctx& c, uint8_t up, bool p3) {
        if (!active || unlink || twoPlayer) return;
        Mem& O = two.img;
        if (!sameModes(c.P, O)) return;
        Ctx co(O);
        co.L = c.L;
        playerFlipGravity(co, up ? 0 : 1, p3);
        unsupported += co.unsupported;
    }
    // FUN_14031a210's gravity part (playerWillSwitchMode): a mode portal taken while the other player is already in that mode turns this one to the other's
    // opposite gravity (PlayerObject::flipGravity, no partner). The wave's portal has no case.
    void willSwitch(Ctx& c, int type) {
        if (!active || twoPlayer || unlink) return;
        const Mem& O = two.img;
        bool same = false;
        switch (type) {
        case 5: same = O.u8(kShip) != 0; break;
        case 6: same = !O.u8(kShip) && !O.u8(kUfo) && !O.u8(kWave) && !O.u8(kSwing) && !O.u8(kBall) && !O.u8(kRobot) && !O.u8(kSpider); break;
        case 0x10: same = O.u8(kBall) != 0; break;
        case 0x13: same = O.u8(kUfo) != 0; break;
        case 0x1b: same = O.u8(kRobot) != 0; break;
        case 0x21: same = O.u8(kSpider) != 0; break;
        case 0x29: same = O.u8(kSwing) != 0; break;
        default: break;
        }
        if (same) playerFlipGravity(c, O.u8(po::Upside) ? 0 : 1, true);
    }
    // GJBaseGameLayer::toggleDualMode 0x31de10 (from collisionCheckObjects, the portals 23 / 24)
    void portal(Ctx& c, bool on) {
        if (on == active) return;                                   // +0x422 already so: nothing
        if (on) {
            Mem& d = two.img;                                       // (player 1 is stepping: player 2 is the slot)
            if (!everBorn) { d.b = tmpl; everBorn = true; }
            const Mem& p1 = c.P;
            copyAttributes(d, p1, c.L, unsupported);
            Ctx cd(d);
            cd.L = c.L;
            // [R] GJBaseGameLayer::toggleDualMode 2.2081 base+0x2168d0: `cVar1 = layer[+0x860]; copyAttributes(p2, p1); if (cVar1 == 0) { flipGravity(p2, !p1.upside, 1); vy = -p1.vy } else { flipGravity(p2, p1.upside, 1); vy = p1.vy }` -- no term of the level's two-player flag (+0x154, read
            // below only for the button / streak part). [M] Grief a custom level t=22658 (a two-player level, no Unlink trigger in it): GD's dump has the new body flipped (p2up 1) with vy +2.093 against the first body's -2.093; the shadow born it
            // unflipped with the same vy (the two-player flag had made `unlink` true), and every later tick of that window moved it the wrong way (edy2 = 2 x |dy|).
            if (!unlinkOpt) { playerFlipGravity(cd, p1.u8(po::Upside) ? 0 : 1, true); setYVelocity(d, -p1.f64(po::Vy)); }
            else { playerFlipGravity(cd, p1.u8(po::Upside), true); setYVelocity(d, p1.f64(po::Vy)); }
            unsupported += cd.unsupported;
            d.u8(po::G, 0);                                         // +0x9c9
            d.u8(po::G2, 0);                                        // +0xa14
            if (d.u8(kWave)) d.u8(0x5a0, 0);                        // FUN_1404fec10 with its position
            d.u8(0xaa1, 1);
            if (g_fixGravityBug != 0) d.u8(0x561, 1);   // --shadowp2fixgrav (constants.hpp)
            two.holding[1] = two.holding[2] = two.holding[3] = false;   // (a player that was hidden holds nothing: releaseAllButtons at the last dual's end)
            if (twoPlayer) {   // toggleDualMode (2.2081 0x2168d0; the offsets are +8 in 22091), a two-player level's player 2: its button-1 entry is cleared unless its inputs are locked (+0xbc8: InputsLocked);
                               // unless its controls are disabled (+0xa2b: ControlsDisabled) the streak ends (+0x985 / +0x986 -> +0x98d / +0x98e := 0), +0x99c (-> +0x9a4) := 1 and a dash stops
                if (!d.u8(bo::InputsLocked)) two.holding[1] = false;
                if (!d.u8(bo::ControlsDisabled)) {
                    d.u8(0x98d, 0); d.u8(0x98e, 0); d.u8(0x9a4, 1);
                    if (d.u8(bo::Dashing)) { Ctx ce(d); ce.L = c.L; upd::stopDashing(ce); two.dashOn = false; unsupported += ce.unsupported; }
                }
            }
            active = true; born = true;
            return;
        }
        // the end: the player who took the portal is kept (copyAttributes into player 1 when it was player 2), player 2 lets every button go
        if (inPass == 2) {
            Mem& p1 = two.img;                                      // (player 2 is stepping: player 1 is the slot)
            copyAttributes(p1, c.P, c.L, unsupported);
            if (c.P.u8(kWave) && p1.u8(kWave)) upd::setRot(p1, upd::getRot(c.P));   // --shadowdualendrot: a wave that ends the dual as the second body hands over its angle (the old tick's --dualendrot, folded into its default)
            for (int k = 1; k <= 3; ++k) sh->holding[k] = false;
        } else {
            if (c.P.u8(kWave)) ++unsupported;                       // FUN_1404f69a0
            for (int k = 1; k <= 3; ++k) two.holding[k] = false;
        }
        two.img.u8(0xaa1, 0);
        active = false; ended = true; endedIn = inPass;
    }
    // GJBaseGameLayer::checkRepellPlayer (FUN_1403424e0): two balls of the same gravity in a one-player level's dual, within 15 * scale + 15 * scale + 5 in y:
    // the one that is not on the ground (+0xa14) by the order player 1 then player 2 is flipped and given 2 away from its new floor
    void repel() {
        Mem& A = sh->img;
        Mem& B = two.img;
        if (twoPlayer || !A.u8(kBall) || !B.u8(kBall) || A.u8(po::Upside) != B.u8(po::Upside)) return;
        const float r = A.f32(0x9f8) * 30.0f * 0.5f + B.f32(0x9f8) * 30.0f * 0.5f + 5.0f;   // --shadowrepelscale (constants.hpp)
        if (!(std::fabs(A.f32(po::PosY) - B.f32(po::PosY)) < r)) return;
        Mem* t = nullptr;
        if (A.u8(po::G2)) t = &B;
        else if (B.u8(po::G2)) t = &A;
        if (!t) return;
        Ctx ct(*t);
        ct.L = sh->L;
        playerFlipGravity(ct, t->u8(po::Upside) ? 0 : 1, true);
        setYVelocity(*t, t->u8(po::Upside) ? 2.0 : -2.0);
        unsupported += ct.unsupported;
    }
};

}  // namespace gdcol
}  // namespace dp
