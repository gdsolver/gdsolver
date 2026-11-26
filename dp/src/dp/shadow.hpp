#pragma once
// The contact shadow: GD's collision pass (collide.hpp) run beside a trajectory, read-only. It keeps the player image of its own between ticks -- the words the
// game carries from one pass to the next (contact uids, the ground bytes, the once-per-attempt portal memory, the rect cache) are the shadow's own output of the
// tick before -- and takes from its caller only what the UPDATE phase produced: the position and velocity the pass is entered with, the mode, the size, the
// gravity and the speed. Nothing here is read by the model; the replay's observer that printed its columns (--gdcollideshadow) went with the old tick that fed it.
//
// What PlayerObject::update does to the image between two passes, besides the integration the caller supplies (measured on the calibration rigs, notes
// section 12.6): the ground byte G is dropped, the bottom-contact uid goes back to -1, the integer tick counters count down, the clock advances, the previous
// position is the last exit, and the position moves (which marks the cached rect dirty).
#include "dp/collide.hpp"
#include "dp/collide_tmpl.gen.hpp"
#include <algorithm>
#include <functional>

namespace dp {
namespace gdcol {

// The ground bytes the first image starts from (-1: from the body's grounded flag). (--shadowseed set them for the replay's observer; the switch went with it, so they stay -1.) A flying body in the air keeps the ground byte it last
// set (a ship takes off with G still up), which the model's `grounded` cannot say; the section runs of the fidelity tables start mid-air and hand it over here.
inline int g_shadowSeedG = -1, g_shadowSeedG2 = -1;
inline int g_shadowSeedBoost = -1;  // the BOOST byte (a jump begun before the first tick is still rising)

struct ShadowIn {
    long long t = 0;
    float x = 0, y = 0;          // the position the update phase wrote
    double vy = 0;               // ...and the velocity (world frame)
    int mode = 0;                // 0 cube, 1 ship, 2 ball, 3 ufo, 4 wave, 5 robot, 6 spider, 7 swing
    bool mini = false, flip = false;
    float speed = 0.9f;          // the speed multiplier (0.7 / 0.9 / 1.1 / 1.3 / 1.6)
    bool grounded = false;       // only to seed a fresh image
    float minPortalY = 90.0f, maxPortalY = 90.0f;   // the flying band (GJBaseGameLayer::getMinPortalY / getMaxPortalY)
    bool freeMode = false;
    bool dual = false;           // the layer's dual flag (both bodies are in play)
    float w764 = tmpl::kW764, ceilWord = tmpl::kCeilWord, speedA = tmpl::kSpeedA, speedB = tmpl::kSpeedB;   // layer words the pass reads
    bool held = false;           // the button level the physics sees (+0x98d)
    bool jumped = false;         // (a test can force it)
    int hsPass = -1;             // the press registers the pass sees (bit 0 H, 1 S, 2 Hm, 3 Sm: the model's --hsshadow Old register after the update's tail); -1 = unknown
    bool hsEdge = false;         // the buttons phase of this step pressed: it empties the set of rings the press has used before the next pass
    bool inferJump = false;      // read a jump off the motion (see applyUpdate)
    bool noAutoJump = true;      // true: only jumped starts a jump; the model's input is not read for it yet
    bool unusedJump = false;         // updateJump started a jump on this tick (the cube / robot leaves the ground: BOOST up, G and G2 down)
    int ringBp = 0;              // the GD type of the ring the BUTTONS phase of this step fired (pushButton's scan; 0 = none): ringJump's byte writes land after the pass
    float rot = 0.0f;            // the body's sprite rotation (degrees) after the update phase, written to the node's +0x38 when useRot (the pass's oriented box turns by it)
    bool useRot = false;
    bool gridOrder = false;      // walk the candidates in GD's section-cell order (see step) rather than the caller's order
    bool dynHeight = false;      // the level keeps dynamic level height (the layer's y section scale is 0.01, else 0: one row of cells)
    bool fixRadius = false;      // the level's fixRadiusCollision (LevelSettingsObject +0x1cf, the byte playerCircleCollision reads): a saw hits by centre distance <= half the player's width + radius
    bool fixGrav = false;        // the level's fixGravityBug (PlayerObject +0x561): playerIsFallingBugged reads the sideways arm with it
    bool p22 = true;             // the level's player 1 Enable22Changes (a spawn orb clears the armed press only then)
};

struct ShadowOut {
    bool ran = false;            // the pass was run (the shadow had an image to run it on)
    bool seeded = false;         // ...from a fresh image this tick (the first ticks after a seed are not comparable)
    int ret = 0;
    bool died = false;
    int why = 0, whyUid = -1;    // the source line of the first kill and the object it was under (-1 = none, e.g. the bounds kill)
    float x = 0, y = 0;
    double vy = 0;
    uint8_t g = 0, g2 = 0, boost = 0, onSlope = 0, upside = 0;
    int fl = -1, ce = -1, slope = -1, snap = -1, left = -1, right = -1;   // the uids the pass left in the image
    int unsupported = 0;         // calls of code that is not ported
    bool bornFired = false;      // a dual portal fired in this pass: bornY / bornVy are the first body's y and vy at that moment
    float bornY = 0; double bornVy = 0;
    int ringBp = 0;              // the buttons-phase ring this step applied (its GD type)
    std::vector<std::pair<int, int>> fired;   // (type, uid) of the non-solid objects the pass overlapped (1000 added to the type when the once-only gate let it act)
};

class ContactShadow {
public:
    Mem img;
    bool seeded = false;
    bool seededPrev = false, tappedBall = false;
    long long lastT = -100;
    FlatSet<int> usedRings;     // +0x850: the rings this press has jumped off (a press empties it, before the next pass: usedAfterPress)
    bool usedAfterPress = false;
    FlatSet<int> ever, gate;    // the once-per-attempt memory and the last tick's overlaps (Ctx::everIn / gateIn)
    std::map<int, GObj> known;   // the objects the image's pointer fields may name
    std::function<void(Mem&)> tweak;   // a test hook: runs on the image just before the pass
    Mem entry;                   // the image as the last pass was entered with (for the test's diagnostics)

    ContactShadow() : img(tmpl::kImageBytes) {}

    void reset() { seeded = false; lastT = -100; ever.clear(); gate.clear(); known.clear(); usedRings.clear(); usedAfterPress = false; }

    static int hex(char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }

    void seed(const ShadowIn& in) {
        img.b.assign(tmpl::kImageBytes, 0);
        for (size_t i = 0; i < tmpl::kImageBytes; ++i) img.b[i] = (uint8_t)(hex(tmpl::kImageHex[2 * i]) * 16 + hex(tmpl::kImageHex[2 * i + 1]));
        img.put<int64_t>(po::Cf, 0);
        img.u8(po::G, g_shadowSeedG >= 0 ? g_shadowSeedG : in.grounded ? 1 : 0);
        img.u8(po::G2, g_shadowSeedG2 >= 0 ? g_shadowSeedG2 : in.grounded ? 1 : 0);
        g_shadowSeedG = g_shadowSeedG2 = -1;
        img.u8(po::Boost, g_shadowSeedBoost > 0 ? 1 : 0);
        g_shadowSeedBoost = -1;
        img.u8(po::WasOnSlope, 0);
        img.u8(po::OnSlope, 0);
        img.f32(po::PrevX, in.x);
        img.f32(po::PrevY, in.y);
        img.put<int32_t>(po::CollisionBottomUid, -1);
        img.put<int32_t>(po::CollisionTopUid, -1);
        img.put<int32_t>(po::PrevBottomUid, -1);
        img.put<int32_t>(po::PrevTopUid, -1);
        img.put<int32_t>(po::CollisionLeftUid, -1);
        img.put<int32_t>(po::CollisionRightUid, -1);
        ever.clear();
        gate.clear();
        usedRings.clear();
        usedAfterPress = false;
        seeded = true;
    }

    // the mode, size, gravity and speed bytes the caller owns
    void applyBody(const ShadowIn& in) {
        Mem& P = img;
        for (int off : {0x9c1, 0x9c2, 0x9c3, 0x9c4, 0x9c5, 0x9c6, 0x9cc}) P.u8((size_t)off, 0);
        switch (in.mode) {
        case 1: P.u8(0x9c1, 1); break;
        case 2: P.u8(0x9c3, 1); break;
        case 3: P.u8(0x9c2, 1); break;
        case 4: P.u8(0x9c4, 1); break;
        case 5: P.u8(0x9c5, 1); break;
        case 6: P.u8(0x9c6, 1); break;
        case 7: P.u8(0x9cc, 1); break;
        default: break;
        }
        const float size = in.mini ? 0.6f : 1.0f;
        P.f32(0x9f8, size);
        P.f32(0x394, size);
        P.f32(0x398, size);
        P.f32(0x40, size);
        P.f32(0x44, size);
        const float hb = in.mode == 4 ? 10.0f : in.mode == 6 ? 27.0f : 30.0f;
        P.f32(0x2fc, hb);
        P.f32(0x300, hb);
        P.f32(0xab8, in.mode == 4 ? 20.0f : in.mode == 6 ? 27.0f : 30.0f);
        P.u8(po::Upside, in.flip ? 1 : 0);
        P.u8(0x561, in.fixGrav ? 1 : 0);
        P.f32(0x9fc, in.speed);
    }

    // PlayerObject::update's bookkeeping between two passes (updateJump 0x4e9b80 and the clock), then the position and velocity the caller integrated.
    // Only updateJump's gravity arm and its BOOST arm are written down so far (cube, ball, robot, spider); the flying modes have arms of their own.
    void applyUpdate(const ShadowIn& in) {
        Mem& P = img;
        const bool upside = P.u8(po::Upside) != 0;
        const float prevX = P.f32(po::PosX), prevY = P.f32(po::PosY);
        P.f32(po::PrevX, prevX);
        P.f32(po::PrevY, prevY);
        P.put<int32_t>(po::CollisionBottomUid, -1);
        P.put<int32_t>(po::CollisionTopUid, -1);
        for (int off : {0xae4, 0xaec, 0xaf0, 0xaf4, 0xaf8, 0xb7c, 0xb80, 0xb84, 0xb88, 0xb90}) {
            const int32_t v = P.i32((size_t)off);
            if (v > -0x40000000) P.i32((size_t)off, v - 1);
        }
        P.f64(po::Time, (double)(in.t + 240) / 240.0);
        for (int off : {0x993, 0x994, 0x995}) P.u8((size_t)off, 0);   // updateJump's end: the ring slots are per press
        const bool boost = P.u8(po::Boost) != 0;
        const bool flying = in.mode == 1 || in.mode == 3 || in.mode == 4 || in.mode == 7;   // ship, UFO, wave, swing: updateJump's flying arms
        P.u8(0x98d, in.held ? 1 : 0);
        if (in.useRot) P.f32(0x38, in.rot);
        if (flying) {
            // (lines 85-291) G stays, a held ship / UFO / swing drops G2, and a velocity that has fallen past twice the gravity ends BOOST
            P.f32(po::PosX, in.x);
            P.f32(po::PosY, in.y);
            P.f64(po::Vy, in.vy);
            P.u8(0x368, 1);
            P.u8(0x369, 1);
            if (in.held && in.mode != 4) P.u8(po::G2, 0);
            if (fallingBugged(P)) P.u8(po::Boost, 0);
            return;
        }
        // updateJump (lines 323-336): a held button on the ground starts the jump (cube, robot: the second flag too; not the spider, not a dash)
        const bool pressed = in.mode == 5 ? (P.u8(0x98e) && in.held) : in.held;
        const bool jumpNow = pressed && P.u8(po::G) && in.mode != 6 && P.u8(0x9ec) == 0 && P.u8(po::Platformer) == 0;
        // A caller that does not know the button's timing (the model: its press lands a tick before the game's update sees it) is told by the motion: a body the last pass left
        // on the ground that now rises faster than the ground can lift it has jumped in this update.
        const bool risingOff = in.inferJump && P.u8(po::G) && (in.mode == 5 ? (in.vy > 4.0 || in.vy < -4.0) : (in.vy > 5.0 || in.vy < -5.0)) && (in.mode == 0 || in.mode == 5);   // (the robot's jump is 4.568 at full size)
        if (in.jumped || risingOff || (!in.noAutoJump && jumpNow && (in.mode == 0 || in.mode == 5))) {   // a jump that starts on this tick
            P.u8(po::Boost, 1);
            P.u8(po::G2, 0);
            P.u8(po::G, 0);
            P.u8(0x98e, 0);
            P.u8(0x9a4, 0);
            P.f64(0x838, 0.0);
        } else if (!boost) {                              // the gravity arm, lines 536-627
            if (P.u8(po::G)) P.f32(0xc44, prevY);
            if (fallingBugged(P)) P.u8(po::G, 0);         // (the platformer's 0.05 s grace is not written down)
        }
        P.f32(po::PosX, in.x);
        P.f32(po::PosY, in.y);
        P.f64(po::Vy, in.vy);
        P.u8(0x368, 1);                                   // the move marks the rect cache dirty
        P.u8(0x369, 1);
        if (tappedBall) { P.u8(po::G, 0); P.u8(po::G2, 0); }
        if (in.jumped) return;
        if (boost) {
            // while BOOST is up the velocity falling past twice the gravity ends it
            if (fallingBugged(P)) {
                P.u8(po::Boost, 0);
                P.u8(0x7ea, 1);
                P.u8(po::G2, 0);
                P.f32(0xc44, P.f32(po::PosY));
            }
            return;
        }
        const double vy = in.vy;
        const bool slow = upside ? !(vy < 0.25) && vy != 0.25 : !(-0.25 < vy) && vy != -0.25;   // |vy| has fallen past a quarter
        if (slow && !P.u8(po::ModeBall) && !P.u8(po::ModeSpider) && !P.u8(po::ModeRobot) && P.u8(0x730) == 0 && P.u8(po::OnSlope) == 0 && P.u8(0x660) == 0
            && (P.u8(po::Platformer) == 0 || P.u8(po::G2) == 0) && P.u8(0xa32) == 0 && P.u8(0x9ec) == 0) {
            rotReset(P);
            runNormalRotation(P, false, 1.0f);
        }
        if (fallingBugged(P) && (upside ? vy > 4.0 : vy < -4.0)) P.u8(po::G2, 0);
    }
    // PlayerObject::playerIsFallingBugged 0x4f86d0 (the non-platformer arm and the sideways / swing arm; gravity is the double at +0x7d0)
    static bool fallingBugged(const Mem& P) {
        const double g = P.f64(0x7d0), vy = P.f64(po::Vy);
        if (P.u8(po::Sideways) || P.u8(po::Platformer) || P.u8(po::ModeSwing) || P.u8(0x561)) {
            const float t = (float)(g + g);
            return P.u8(po::Upside) == 0 ? vy < (double)t : (double)-t < vy;
        }
        const double d2 = P.u8(0xaa1) ? -g : g;
        return P.u8(po::Upside) == 0 ? vy < (double)((float)d2 + (float)d2) : g + g < vy;
    }

    // ringJump 0x4f6f50's writes on the player's bytes (notes: pushbutton-state-table 3.3 / 4.3), for the ring the buttons phase fired. The velocity and the position are the
    // caller's next update (it writes them); the flip of a gravity ring goes through flipGravity. A spider orb is taken to succeed (H cleared).
    void applyRingButton(Ctx& c, const ShadowIn& in) {
        if (in.ringBp == 0) return;
        c.p22 = in.p22;
        ringJumpWrites(c, in.ringBp);
    }
    ShadowOut step(const ShadowIn& in, const std::vector<GObj>& cands, bool ignoreDamage = false) {
        ShadowOut out;
        if (!seeded || in.t != lastT + 1) {
            seededPrev = false;
            seed(in);
            img.f32(po::PosX, in.x);
            img.f32(po::PosY, in.y);
            out.seeded = true;
        }
        // a ball whose gravity the caller turned since the last pass was tapped (the tap is a button's work, after the pass): it left the ground
        tappedBall = in.mode == 2 && seededPrev && (img.u8(po::Upside) != 0) != in.flip && img.u8(po::G);
        // ...and in a dual, GJBaseGameLayer::flipGravity turns the partner too (before its own update): the body's gravity changed under it without a button
        const uint8_t oldUp = img.u8(po::Upside);
        const bool partnerFlip = in.dual && in.mode != 2 && seededPrev && (oldUp != 0) != in.flip;
        seededPrev = true;
        applyBody(in);
        if (partnerFlip) { img.u8(po::Upside, oldUp); Ctx pc(img); playerFlipGravity(pc, in.flip ? 1 : 0, true); }
        applyUpdate(in);
        if (in.hsPass >= 0) {                              // the registers the pass is entered with (the update's tail has mirrored H, S into Hm, Sm and let the slots down)
            img.u8(0x98d, (in.hsPass & 1) ? 1 : 0);
            img.u8(0x98e, (in.hsPass & 2) ? 1 : 0);
            img.u8(0x991, (in.hsPass & 4) ? 1 : 0);
            img.u8(0x992, (in.hsPass & 8) ? 1 : 0);
            for (int off : {0x993, 0x994, 0x995}) img.u8((size_t)off, 0);
        }
        if (usedAfterPress) { usedRings.clear(); usedAfterPress = false; }
        if (tweak) tweak(img);
        entry = img;
        lastT = in.t;
        // GJBaseGameLayer::checkCollisions 0x31aae0 walks the section cells around the player, x - 1 .. x + 1 outward and y - 1 .. y + 1 inward (a cell is (int)(position * scale),
        // 100 px wide, one row without dynamic level height), and calls collisionCheckObjects once per cell with that cell's objects in uid order (FUN_14030b120 compares +0x39c);
        // the solids and hazards queue up across the calls. An object is in the one cell of its centre (an object outside the nine is not a candidate), except that one with
        // Extended Collision (property 511, GObj::extCol) is in no cell: addToSection files it in a flat list the pass walks once more after the cells, whatever the cell (read from
        // addToSection 0x32f280 and checkCollisions lines 432-436; the candidate-list watch, cfg watchuid, saw a 424 px block with the flag in the game's list for a player in cells 15 to 19).
        // The flat list is in the order the objects were filed (uid order here).
        auto cellOf = [](double v, float s) -> int { if (!(v > 0.0)) return 0; return (int)(v < 1e7 ? (double)s * v : (double)(s * 1e7f)); };
        const float sxs = tmpl::kScaleX, sys = in.dynHeight ? 0.01f : 0.0f;
        std::vector<GObj> ordered;
        {
            const float pxs = in.x, pys = in.y;
            const int pcx = pxs > 0.0f ? (int)(pxs < 1e7f ? pxs * sxs : sxs * 1e7f) : 0, pcy = pys > 0.0f ? (int)(pys < 1e7f ? pys * sys : sys * 1e7f) : 0;
            for (int ix = std::max(0, pcx - 1); ix <= pcx + 1; ++ix)
                for (int iy = std::max(0, pcy - 1); iy <= pcy + 1; ++iy) {
                    std::vector<GObj> cell;
                    for (const GObj& o : cands)
                        if (!o.extCol && cellOf(o.realX(), sxs) == ix && cellOf(o.realY(), sys) == iy) cell.push_back(o);
                    std::sort(cell.begin(), cell.end(), [](const GObj& a, const GObj& b) { return a.uid < b.uid; });
                    ordered.insert(ordered.end(), cell.begin(), cell.end());
                }
            std::vector<GObj> flat;
            for (const GObj& o : cands) if (o.extCol) flat.push_back(o);
            std::sort(flat.begin(), flat.end(), [](const GObj& a, const GObj& b) { return a.uid < b.uid; });
            ordered.insert(ordered.end(), flat.begin(), flat.end());
        }
        // the objects: this tick's candidates, plus what the image still names
        std::vector<GObj> objs = in.gridOrder ? ordered : cands;
        for (size_t off : {(size_t)po::SlopeObj, (size_t)po::SlopeObjB, (size_t)po::Collided, (size_t)po::GroundObj, (size_t)po::LeftObj, (size_t)po::RightObj, (size_t)po::Cf, (size_t)po::Cd0, (size_t)po::SnappedTo}) {
            const int uid = (int)img.get<int64_t>(off) - 1;
            if (uid < 0) continue;
            bool have = false;
            for (const GObj& o : objs) if (o.uid == uid) { have = true; break; }
            if (!have) { auto it = known.find(uid); if (it != known.end()) objs.push_back(it->second); }
        }
        known.clear();
        for (const GObj& o : objs) known[o.uid] = o;
        std::vector<GObj*> cand;
        const size_t nCand = in.gridOrder ? ordered.size() : cands.size();
        for (size_t i = 0; i < nCand; ++i) cand.push_back(&objs[i]);
        Ctx c(img);
        c.objs = &objs;
        c.L.speedA = in.speedA;
        c.L.speedB = in.speedB;
        c.L.w764 = in.w764;
        c.L.scaleX = tmpl::kScaleX;
        c.L.scaleY = tmpl::kScaleY;
        c.L.ceilWord = in.ceilWord;
        c.L.maxPortalY = in.maxPortalY;
        c.L.minPortalY = in.minPortalY;
        c.L.freeMode = in.freeMode;
        c.L.f1cf = in.fixRadius;
        c.L.dual = in.dual;
        c.gateIn = &gate;
        c.everIn = &ever;
        c.hsKnown = in.hsPass >= 0;
        c.p22 = in.p22;
        c.usedRings = &usedRings;
        out.ret = checkCollisions(c, 0.25f, ignoreDamage, cand);
        usedAfterPress = in.hsEdge;
        gate = c.gateNow;
        ever.insert(c.everNow.begin(), c.everNow.end());
        applyRingButton(c, in);                           // (GD's order: the pass of this step, then the buttons phase's ring)
        out.ran = true;
        out.ringBp = in.ringBp;
        out.fired = c.fired;
        out.died = c.died;
        out.why = c.why;
        out.whyUid = c.whyUid;
        out.x = img.f32(po::PosX);
        out.y = img.f32(po::PosY);
        out.vy = img.f64(po::Vy);
        out.g = img.u8(po::G);
        out.g2 = img.u8(po::G2);
        out.boost = img.u8(po::Boost);
        out.onSlope = img.u8(po::OnSlope);
        out.upside = img.u8(po::Upside);
        auto uidAt = [&](size_t off) { return (int)img.get<int64_t>(off) - 1; };
        out.fl = img.i32(po::CollisionBottomUid);
        out.ce = img.i32(po::CollisionTopUid);
        out.left = img.i32(po::CollisionLeftUid);
        out.right = img.i32(po::CollisionRightUid);
        out.slope = img.u8(po::OnSlope) ? uidAt(po::SlopeObj) : -1;   // (the pointer outlives the ride; the game's record names the ramp only while it is ridden)
        out.snap = uidAt(po::SnappedTo);
        out.unsupported = c.unsupported;
        out.bornFired = c.bornFired; out.bornY = c.bornY; out.bornVy = c.bornVy;
        return out;
    }
};

}  // namespace gdcol
}  // namespace dp
