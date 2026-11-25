#pragma once
// A port of GD 2.209's (22091) update phase of the player as a pure function of the player's bytes: PlayerObject::update 0x4e6fc0 (with updateJump 0x4e9b80, which the
// Ghidra project mislabels GameStatsManager::checkAchievement), updateTimeMod 0x4ff370's constants, and the rotation functions updateRotation 0x4ef390, FUN_1404ef5b0 (the
// rotation step), FUN_1404eefa0, updateSlopeRotation 0x4eef20, convertToClosestRotation 0x4eb3b0.
//
// Same rules as collide.hpp: self-contained apart from collide.hpp (Mem, po::, Ctx, setYVelocity, flipGravity, runNormalRotation, runBallRotation), the player is the byte image
// [0, 0xc80), float / double follow the decompile (a float expression stays float until it is converted), and what is not ported is counted in Ctx::unsupported, never
// guessed. The sprite / particle / animation / sound / game-event code of the functions is left out: it writes nothing the physics reads. tools/upd_test.cpp compares the
// result with the game's own entry / exit bytes (mod cfg ctupd=t0,t1 -> ctupd.csv).
//
// Fields the game writes that no physics reads and that a byte comparison must leave out: +0x99c / +0x998 / +0x9a0 (rand(), updateJump's first lines), +0x800 (_ftime64),
// and the sprite / colour words (+0x8c..+0xa2, +0x111..+0x11a, ...).
#include "dp/collide.hpp"
#include "dp/spider.hpp"

namespace dp {
namespace gdcol {
namespace upd {

// the player's setPosition (vtable +0xb8): the position, and both rect caches (+0x368, +0x369) marked dirty
inline void setPlayerPosition(Mem& P, float x, float y) {
    setPosition(P, x, y);
    P.u8(0x369, 1);
}

// PlayerObject::updateTimeMod 0x4ff370: the speed word and the three constants it selects (the particle effect is left out).
inline void updateTimeMod(Mem& P, float speed) {
    P.f32(0x9fc, speed);
    uint64_t c7c8 = 0, c7d0 = 0, c7c0 = 0x4017147b60000000ULL;
    bool setHead = false;
    if (speed == 0.9f) { c7c8 = 0x40265c2d20000000ULL; c7d0 = 0x3feea99100000000ULL; setHead = true; }
    else if (speed == 0.7f) { c7c8 = 0x40253d74d594f26bULL; c7d0 = 0x3fee161c36976bc2ULL; c7c0 = 0x4017eb85a4f00ef1ULL; setHead = true; }
    else if (speed == 1.1f) { c7c8 = 0x4026d70e6f2e8c05ULL; c7d0 = 0x3feea15fcc1871e7ULL; c7c0 = 0x40177ae1cde5d181ULL; setHead = true; }
    else if (speed == 1.3f || speed == 1.6f) { c7c8 = 0x402675c6c11a1123ULL; c7d0 = 0x3feec22467be553bULL; c7c0 = 0x401800008637bd06ULL; setHead = true; }
    if (setHead) { P.put<uint64_t>(0x7c8, c7c8); P.put<uint64_t>(0x7d0, c7d0); }
    P.put<uint64_t>(0x7c0, c7c0);
    // (FUN_1404ff700, the particle rates, is visual and not ported.) A ball that is not locked or dashing starts its rotation over at the new speed: the stake fields are cleared and runBallRotation(1.0)
    // sets the spin again (0x4ff6c5); a speed portal passed in the air as a ball gives +0x728 = 750.0 at the next step (wtcap w_145755012_3430 t=3524).
    if (P.u8(po::ModeBall) && P.u8(0xa32) == 0 && P.u8(0x9ec) == 0) {
        P.put<uint16_t>(0x730, 0);
        P.u8(0x670, 0);
        P.i32(0x728, 0);
        runBallRotation(P, 1.0f);
    }
}

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------
// The rotation functions (read from the disassembly, not the decompile: the decompile lost their float arguments). Everything is float except where said; the libm calls are
// the UCRT's cosf / sinf / acosf / atan2f / atan2, which a build with the same CRT reproduces bit for bit.
inline float getRot(const Mem& P) { return P.f32(0x38); }
// PlayerObject::setRotation 0x501e70 (vtable +0x160): CCNode keeps the angle twice (+0x38, +0x3c); a change marks both rect caches dirty (+0x368, +0x369), sets +0x350 and decides the 90-degree flag +0x390
inline void setRot(Mem& P, float r) {
    if (P.f32(0x38) != r || P.f32(0x3c) != r) {
        P.put<uint16_t>(0x368, 0x101);
        P.u8(0x350, 1);
        P.f32(0x38, r);
        P.f32(0x3c, r);
        P.u8(0x390, (r == 90.0f || r == -90.0f || r == 270.0f || r == -270.0f) ? 1 : 0);
    }
}

// FUN_140084c80: a spherical blend of two angles (radians), t in [0, 1]
inline float angleSlerp(float a, float b, float t) {
    const float c1 = cosf(a * 0.5f), s1 = sinf(a * 0.5f);
    float c2 = cosf(b * 0.5f), s2 = sinf(b * 0.5f);
    float dot = s2 * s1 + c2 * c1;
    if (0.0f > dot) { dot = -dot; s2 = -s2; c2 = -c2; }
    float w1 = 1.0f - t, w2 = t;
    if (1.0f - dot > 9.99999975e-05f) {
        const float th = acosf(dot);
        const float sn = sinf(th);
        w1 = sinf(th * (1.0f - t)) / sn;
        w2 = sinf(th * t) / sn;
    }
    const float y = s1 * w1 + w2 * s2;
    const float x = c1 * w1 + w2 * c2;
    const double r = std::atan2((double)y, (double)x);
    return (float)(r + r);
}

// PlayerObject::convertToClosestRotation 0x4eb3b0
inline float convertToClosestRotation(const Mem& P, float A) {
    if (P.u8(po::ModeShip) || P.u8(po::ModeUfo) || P.u8(po::ModeWave) || P.u8(po::ModeSwing) || P.u8(po::ModeRobot) || P.u8(po::ModeSpider) || P.u8(0x9ec)) return A;
    int c = (int)getRot(P);
    c = c - (c / 360) * 360;
    const float R = (float)c;
    float last = A, cur = A;
    if (A > R) { do { last = cur; cur += -90.0f; } while (cur > R); }
    else if (R > A) { do { last = cur; cur += 90.0f; } while (R > cur); }
    else return A;
    return (std::fabs(R - last) > std::fabs(R - cur)) ? cur : last;
}

// FUN_1404ef5b0: one step of the current rotation toward `tgtDeg`
inline void rotateStep(Mem& P, float dt, float tgtDeg) {
    if (P.u8(0x9ec)) return;
    const float cur = getRot(P);
    const float curRad = cur * 0.0174532924f;
    float f = P.f32(0x9fc) * 0.174999997f;
    if (P.u8(po::ModeShip)) f *= 0.5f;
    else if (!P.u8(po::ModeUfo) && !P.u8(po::ModeWave) && !P.u8(po::ModeSwing) && !P.u8(po::ModeBall) && P.u8(0xa14) && !P.u8(po::OnSlope) && !P.u8(po::WasOnSlope)) f *= 3.0f;
    const float fd = f * dt;
    const float t = (dt < fd) ? dt : fd;                           // minss: ties and NaN take f * dt
    float r = angleSlerp(curRad, tgtDeg * 0.0174532924f, t);
    r = r * 57.2957802f;
    if (P.u8(po::Platformer) && (P.u8(po::ModeShip) || P.u8(po::ModeUfo) || P.u8(po::ModeWave) || P.u8(po::ModeSwing)) && P.u8(po::OnSlope) && P.i32(0x9d0) > 0) r = tgtDeg;
    setRot(P, r);
}

// FUN_1404eefa0: the flying modes turn toward their direction of motion (the previous tick's position is the floats at +0xa00 / +0xa04, stored by GJBaseGameLayer::update after
// updateRotation)
inline void turnToMotion(Mem& P, float dt) {
    if (P.u8(po::OnSlope) || P.u8(0x9ec)) return;
    const float px = P.f32(po::PosX), py = P.f32(po::PosY);
    const bool any = P.u8(po::ModeUfo) || P.u8(po::ModeShip) || P.u8(po::ModeSwing) || P.u8(po::ModeWave);
    float X, Y;
    if (!P.u8(po::Sideways)) {
        X = px - P.f32(0xa00);
        if (any && P.u8(po::Leftward)) X = std::fabs(X);
        Y = -(py - P.f32(0xa04));
    } else {
        X = py - P.f32(0xa04);
        if (any && P.u8(po::Leftward)) X = std::fabs(X);
        Y = px - P.f32(0xa00);
    }
    const bool body = (Y * Y + X * X >= dt * 1.20000005f) || (P.u8(po::Platformer) && (P.u8(po::ModeShip) || P.u8(po::ModeUfo)));
    if (!body) return;
    float ang = std::atan2(Y, X);                                  // atan2f
    const float cur = getRot(P);
    const float curRad = cur * 0.0174532924f;
    float blend = 0.150000006f;
    if (P.u8(po::ModeUfo)) ang *= -0.400000006f;                  // 0x4ef139
    if (P.u8(0x660) && any) ang = 0.0f;
    if (any && P.u8(po::Leftward)) ang *= -1.0f;
    if (P.u8(po::ModeUfo)) {
        blend = 0.07f;
        if (P.u8(po::Leftward) != P.u8(po::Upside)) ang = (0.1f < ang) ? 0.1f : ang;
        else ang = (-0.1f > ang) ? -0.1f : ang;
        if (P.u8(po::Platformer) && (std::fabs(P.f64(0xb00)) <= 0.5 || P.u8(0xa14))) ang = 0.0f;
    } else if (P.u8(po::ModeWave)) blend = (P.f32(0x9f8) == 1.0f) ? 0.25f : 0.4f;
    else if (P.u8(po::Platformer) && P.u8(po::ModeShip)) {
        if (!P.u8(0xb09) && !P.u8(0xb08) && !P.u8(0xb61) && !P.u8(0xb62)) { blend = 0.06f; ang = 0.0f; }
        else {
            const float s = (float)((P.u8(po::Upside) == P.u8(po::Sideways)) ? 1 : -1) * (float)((P.u8(po::Leftward) == 0) ? 1 : -1);
            blend = 0.07f; ang = s * 0.52359879f;
            if (0.1f > std::fabs(ang) - std::fabs(curRad)) { blend = 0.015f; ang = s * 0.785398185f; }
        }
    }
    if (P.u8(0x660) && any) blend *= 0.25f;
    float t = blend * dt;
    t = (dt < t) ? dt : t;
    const float r = angleSlerp(curRad, ang, t);
    setRot(P, r * 57.2957802f);
}

// PlayerObject::updateSlopeRotation 0x4eef20 (dt: the step the caller of collidedWithSlopeInternal runs at)
inline void updateSlopeRotation(Mem& P, float dt) {
    const float a = (float)(P.f64(0x938) * 57.295780181884766);
    float r = convertToClosestRotation(P, a);
    if (P.u8(po::Platformer) && P.f64(0xb68) > 80.0 && (P.u8(0xb61) || P.u8(0xb62))) r = 0.0f;
    rotateStep(P, dt, r);
}

// PlayerObject::updateRotation 0x4ef390 (dt: GJBaseGameLayer::update passes float((D / n) * 60), the sub-step in 1/60 s units)
inline void updateRotation(Mem& P, float dt) {
    if (P.u8(0xa32) || P.u8(0x9ec)) return;
    if (!P.u8(po::ModeBall)) {
        if (P.u8(po::OnSlope)) {
            const float a = (float)(P.f64(0x938) * 57.295780181884766);
            float r = convertToClosestRotation(P, a);
            if (P.u8(po::Platformer) && P.f64(0xb68) > 80.0 && (P.u8(0xb61) || P.u8(0xb62))) r = 0.0f;
            rotateStep(P, dt, r);
        } else if (P.u8(po::ModeShip) || P.u8(po::ModeUfo) || P.u8(po::ModeWave) || P.u8(po::ModeSwing)) {
            turnToMotion(P, dt);
        } else {
            bool done = false;
            if (P.u8(0xa14)) {
                const double v = P.f64(po::Vy);
                const bool lo = P.u8(po::Upside) ? (v > 0.25) : (v < -0.25);
                if (!lo && !P.u8(po::WasOnSlope)) {
                    if (!P.u8(po::Platformer) || !P.u8(0x730)) {
                        P.put<uint16_t>(0x730, 0);
                        P.u8(0x670, 0);
                        P.f32(0x728, 0.0f);
                        rotateStep(P, dt, convertToClosestRotation(P, 0.0f));
                    }
                    done = true;
                }
            }
            if (!done && (P.u8(po::ModeRobot) || P.u8(po::ModeSpider)) && !P.u8(po::WasOnSlope)) rotateStep(P, dt * 0.5f, 0.0f);
        }
    }
    if (P.f32(0x728) != 0.0f) {
        float s = 1.0f;
        if (P.u8(0x670) && P.u8(po::ModeBall)) s = P.f32(0x72c);
        setRot(P, getRot(P) + (((dt / 60.0f) * P.f32(0x728)) * s));
    }
}

// PlayerObject::startDashing(ring) 0x4f3b20 (the sprite, particle and sound calls are left out), and the part of 0x4f43c0 that turns the player. A dash ring (type 0x25 / 0x26) calls it from ringJump.
// The dash vector is read from the ring's rotation: a = -rot (flipped by 180 for a flipX ring, which is the vtable +0x4f8 byte -- assumed, no window has one), brought into (-180, 180], turned by the
// player's sideways / leftward frame, mirrored into (-90, 90] and clamped to +-70; +0x580 keeps that angle, +0x570 / +0x578 the slope |tan| / -tan of it (read from the disassembly; the decompile
// loses the float arguments). The platformer arm (FUN_1404eb320, the ring's +0x748 speed) is counted as not ported. Checked on w_128992032_710 t=834: 570 = 0.398151, 578 = -0.398151, 580 = -21.71, rot 21.71.
inline void startDashing(Ctx& c, const GObj* ring) {
    Mem& P = c.P;
    if (P.u8(0x9ec)) return;
    P.u8(0x9ec, 1);
    P.put<uint64_t>(0xa18, 0);
    P.put<uint16_t>(0x730, 0);
    P.u8(0x670, 0);
    P.i32(0x728, 0);
    if (P.u8(po::Platformer)) { ++c.unsupported; ++c.unsupportedBy[2007]; }
    if (!P.u8(po::ModeWave) && P.u8(0x980)) P.u8(0x980, 0);   // (FUN_1404fe600, the sprite scale, is visual)
    float ang9 = (float)P.f64(0x580);
    if (ring) {
        P.put<int64_t>(0x590, (int64_t)ring->uid + 1);
        P.f64(0x588, P.f64(po::Time));
        float a = ring->rot;
        if (ring->flipX) a += 180.0f;
        float x9 = std::fmod(-a, 360.0f);
        auto norm = [](float v) { if (180.0f < v) v += -360.0f; else if (v < -180.0f) v += 360.0f; return v; };
        x9 = norm(x9);
        if (!P.u8(po::Platformer)) {
            float off = 0.0f;
            if (P.u8(0x9cb)) off = -90.0f;
            if (P.u8(0x9ca)) off += 180.0f;
            if (off != 0.0f) x9 = norm(x9 + off);
            if (90.0f < std::fabs(x9)) { x9 = norm((x9 > 0.0f ? 180.0f : -180.0f) - x9); }
            if (70.0f < x9 || x9 < -70.0f) {
                if (x9 > 0.0f) x9 = (x9 > 70.0f && x9 < 180.0f) ? 70.0f : -70.0f;
                else x9 = (x9 < 70.0f && x9 > -180.0f) ? -70.0f : 70.0f;
            }
            if (off != 0.0f) x9 = norm(x9 - off);
            const float rad = x9 * 0.01745329238474369f;
            const float vx = std::cos(rad), vy = std::sin(rad);
            const bool sideways = P.u8(0x9cb) != 0;
            const float a1 = sideways ? vy : vx, b1 = sideways ? vx : vy;   // xmm2 / xmm1
            const double d578 = (double)(b1 / std::fabs(a1));
            P.f64(0x578, d578);
            P.f64(0x570, std::fabs(d578));
        } else {
            ++c.unsupported; ++c.unsupportedBy[2007];
        }
        ang9 = x9;
        P.f64(0x580, (double)ang9);
    }
    setDashBodyRotation(P);
    P.f32(0xa34, 0.0f);
    P.f32(0xa38, 0.0f);
}
// PlayerObject::stopDashing 0x4f4850, the byte work: the dash flag and its start time go, the ring is forgotten, a ball starts its rotation over. For a cube / robot-less mode the player is turned to
// the signed rotation of its child sprite (GD Win 2.2091 RVAs 0x4f4cc7..0x4f4d49).
// The platformer arm (the carried speed) is counted as not ported.
inline void stopDashing(Ctx& c) {
    Mem& P = c.P;
    if (!P.u8(0x9ec)) return;
    const float childRot = c.dashRot ? *c.dashRot : 0.0f;
    P.u8(0x9ec, 0);
    P.put<uint64_t>(0xa18, 0);
    if (P.u8(po::Platformer)) { ++c.unsupported; ++c.unsupportedBy[2008]; }
    P.put<int64_t>(0x590, 0);
    if (!P.u8(po::ModeShip) && !P.u8(po::ModeUfo) && !P.u8(po::ModeWave) && !P.u8(po::ModeSwing) && !P.u8(po::ModeRobot) && !P.u8(po::ModeSpider)) setRot(P, childRot);
    if (P.u8(po::ModeBall)) {
        if (P.u8(po::Platformer)) { P.put<uint16_t>(0x730, 0); P.u8(0x670, 0); P.i32(0x728, 0); }
        runBallRotation(P, 1.0f);
    }
}
// PlayerObject::playerIsFallingBugged 0x4f86d0 (the non-platformer arm and the sideways / swing arm; gravity is the double at +0x7d0)
inline bool fallingBugged(const Mem& P) {
    const double g = P.f64(0x7d0), vy = P.f64(po::Vy);
    if (P.u8(po::Sideways) || P.u8(po::Platformer) || P.u8(po::ModeSwing) || P.u8(0x561)) {
        const float t = (float)(g + g);
        return P.u8(po::Upside) == 0 ? vy < (double)t : (double)-t < vy;
    }
    const double d2 = P.u8(0xaa1) ? -g : g;
    return P.u8(po::Upside) == 0 ? vy < (double)((float)d2 + (float)d2) : g + g < vy;
}

// The head of updateJump's flying branch (below, `clear95a`), asked of an image as a step leaves it: will the body's next update clear the velocity-limit exemption (+0x95a) before either
// reader of it (the ship's thrust branch and the terminal clamp, both further down that branch)? A ship / ufo / wave / swing whose vy is inside the plain band, alive, not locked (playerUpdate
// skips updateJump then) and not dashing (a release ends the dash in the buttons phase, before the update). The caller adds the one thing the image does not hold: no ring to press in the
// buttons phase that comes first (a yellow / pink / blue / green ring writes vy and leaves the byte: buttons.hpp ringJump). With those the byte's value now is never read.
// The band asked is the part the upright band (-6.4 / f, 8 / f) and the upside-down one (-8 / f, 6.4 / f) share: the body may be flipped before its update without the byte being written
// (the other player's flip in a linked dual, selfshadow_dual.hpp partnerFlip; a teleport's gravity at the head), and a flip keeps vy or halves it (playerFlipGravity), so a vy inside the
// shared part is inside the band the update will ask whichever way up the body is by then.
inline bool exemptionClearedNext(const Mem& P) {
    if (!(P.u8(po::ModeShip) || P.u8(po::ModeUfo) || P.u8(po::ModeWave) || P.u8(po::ModeSwing))) return false;
    if (P.u8(0x9c8) || P.u8(0xa32) || P.u8(0x9ec)) return false;
    const float f22 = P.f32(0x9f8) != 1.0f ? 0.85f : 1.0f;
    const double v = P.f64(po::Vy);
    return (double)(-6.4f / f22) < v && v < (double)-(-6.4f / f22);
}

// PlayerObject::updateJump 0x4e9b80. dt is the step (0 when pushButton calls it). The sprite / sound / game-event calls, rand() and _ftime64 are left out; what they would write
// (+0x998 +0x99c +0x9a0 +0x800) is not written. spiderTestJump is spider.hpp; a call not ported is counted: animatePlatformerJump.
inline void updateJump(Ctx& c, float dt) {
    Mem& P = c.P;
    bool left = P.u8(0xb09) != 0, right = P.u8(0xb08) != 0;   // CVar14, CVar10
    if (left && right) { if (P.u8(0xb0a) == 0) left = false; else right = false; }
    if (P.u8(po::Platformer) && P.u8(po::OnSlope)
        && (80.0f <= std::fabs(P.f32(0x6a0) * 57.29578f) || (40.0f <= std::fabs(P.f32(0x6a0) * 57.29578f) && P.u8(0x519 + 0) == 0 && false)))
        P.u8(po::G, 0);
    // (the second half of that condition reads the slope object's byte +0x519 through the pointer at +0x680: platformer only, not carried)
    const uint32_t uVar9 = P.u8(po::G) ? 0xd5u : 0u;           // +0x9a0
    float fVar23 = (float)P.f64(0x7d0);                          // the gravity word
    uint8_t H = P.u8(0x98d);                                     // CVar11
    if (P.u8(po::ModeRobot)) H = (P.u8(0x98e) && H) ? 1 : 0;
    if (P.u8(po::ModeBall) || P.u8(po::ModeShip) || P.u8(po::ModeUfo) || P.u8(po::ModeWave) || P.u8(po::ModeSwing) || P.u8(po::ModeSpider)) fVar23 = 0.958199f;
    float fVar24 = fVar23;
    if (P.f32(0xb8c) != 1.0f) fVar24 = fVar23 * P.f32(0xb8c);
    float fVar20 = 0.8f, fVar19 = 0.8f, fVar22 = 1.0f;
    if (P.f32(0x9f8) != 1.0f) fVar22 = fVar19;
    const bool upside = P.u8(po::Upside) != 0;
    const int dir = upside ? -1 : 1;
    const bool wave = P.u8(po::ModeWave) != 0, ship = P.u8(po::ModeShip) != 0, ufo = P.u8(po::ModeUfo) != 0, swing = P.u8(po::ModeSwing) != 0;
    const bool ball = P.u8(po::ModeBall) != 0, robot = P.u8(po::ModeRobot) != 0, spider = P.u8(po::ModeSpider) != 0;
    const bool plat = P.u8(po::Platformer) != 0;
    auto finish = [&]() { P.u8(0x98f, P.u8(0x98d)); P.u8(0x990, P.u8(0x9a4)); };
    if (ship || ufo || wave || swing) {
        // ---- the flying modes ----
        if (P.f32(0x9f8) != 1.0f) fVar22 = 0.85f;
        double v = P.f64(po::Vy);
        bool clear95a;
        if (!upside) clear95a = (0.0 <= v && v < (double)(8.0f / fVar22)) || (v <= 0.0 && (double)(-6.4f / fVar22) < v);
        else clear95a = (v <= 0.0 && (double)-(8.0f / fVar22) < v) || (0.0 <= v && v < (double)-(-6.4f / fVar22));
        if (clear95a) P.u8(0x95a, 0);
        if (!ship) {
            if (!ufo) {
                if (!swing) {                                      // wave
                    const int iVar2 = upside ? -1 : 1, iVar13 = P.u8(0x98d) ? 1 : -1;
                    setYVelocity(P, (double)((float)iVar2 * 8.0f * (float)iVar13));
                    setYVelocity(P, (double)P.f32(0x9fc) * P.f64(0x7c0) * (double)iVar2 * (double)iVar13);
                    goto clampTail;
                }
                // swing
                if (P.u8(0x98e) && P.u8(0x98d)) {
                    P.u8(0x98e, 0);
                    const double v0 = P.f64(po::Vy);
                    playerFlipGravity(c, upside ? 0 : 1, true);
                    setYVelocity(P, (double)((float)v0 * 0.8f));
                }
                fVar22 = 1.0f;
                fVar20 = 1.0f;
            } else {                                              // ufo
                if (P.u8(0x98e) && P.u8(0x98d)) {
                    P.u8(0x98e, 0);
                    float fv = P.f32(0x9f8) != 1.0f ? 8.0f : 7.0f;
                    double target; bool eq, lt;
                    if (!upside) { target = (double)(fv * 1.0f * fVar22); eq = target == P.f64(po::Vy); lt = target < P.f64(po::Vy); }
                    else { target = (double)(fv * -1.0f * fVar22); eq = P.f64(po::Vy) == target; lt = P.f64(po::Vy) < target; }
                    if (!lt && !eq) {
                        setYVelocity(P, target);                   // (the decompile lost the argument; the disassembly: D((sgnUD * f7) * F22), 0x4eaa35)
                        if ((P.u8(po::WasOnSlope) || P.u8(po::OnSlope)) && 0.0f < P.f32(0x9bc)) {
                            const double v1 = P.f64(po::Vy);
                            setYVelocity(P, (double)(P.f32(0x9bc) * 0.5f) + v1);
                            const double d21 = (double)(float)(v1 * 1.399999976158142);
                            double d17 = P.f64(po::Vy);
                            if (d21 <= P.f64(po::Vy)) d17 = d21;
                            setYVelocity(P, d17);
                        }
                    }
                }
            }
            {   // the shared tail 0x4eac53 (decompile line 199, whose argument the decompile lost; read from the disassembly): the gravity step of the UFO and the swing
                float t;
                if (ufo) {
                    float m = 0.8f;
                    if (!fallingBugged(P)) m = 1.2f;
                    t = (((float)(P.u8(po::Upside) ? -1 : 1) * (dt * fVar24)) * m) * 0.5f / fVar22;   // (the game reads +0x9c7 again here: the swing's flip above has turned it)
                } else {
                    const float s = P.f32(0x9f8) != 1.0f ? 0.6f : 0.4f;
                    t = ((float)(P.u8(po::Upside) ? -1 : 1) * (dt * fVar24)) * s;
                }
                setYVelocity(P, (double)(-t) + P.f64(po::Vy));
            }
            if (P.u8(0x98d)) P.u8(0xa14, 0);
        } else {                                                  // ship
            float fv19 = fVar19;
            uint8_t Hh = P.u8(0x98d);
            bool take = false;
            if (!Hh) { if (P.u8(0x95a)) take = true; else { /* ea89d */ if (!fallingBugged(P)) fv19 = 1.2f; } }
            else if (!P.u8(0x95a)) { fv19 = -1.0f; }
            else take = true;
            if (take) {
                // ea872
                const double vv = P.f64(po::Vy);
                if (!upside) { if (vv <= 0.0 && vv != 0.0) fv19 = -1.0f; }
                else if (0.0 < vv) fv19 = -1.0f;
                // ea89d
                Hh = P.u8(0x98d);
                if (!Hh && !fallingBugged(P)) fv19 = 1.2f;
            }
            if (plat) fVar24 = fVar24 * 0.8f;
            if (0.0f <= fv19) fVar23 = fVar24;
            float f24 = 0.4f;
            if (Hh && fallingBugged(P)) f24 = 0.5f;
            setYVelocity(P, (double)-(((float)dir * dt * fVar23 * fv19 * f24) / fVar22) + P.f64(po::Vy));
            if (P.u8(0x98d)) P.u8(0xa14, 0);
        }
    clampTail:
        if (P.u8(0x95a) == 0 && !wave) {
            double d17 = P.f64(po::Vy);
            float fhi;
            if (!P.u8(po::Upside)) {
                const double d21 = (double)((fVar20 * -8.0f) / fVar22);
                if (d17 <= d21) d17 = d21;
                setYVelocity(P, d17);
                fhi = 8.0f / fVar22;
            } else {
                if (d17 <= (double)(-8.0f / fVar22)) d17 = (double)(-8.0f / fVar22);
                setYVelocity(P, d17);
                fhi = (fVar20 * 8.0f) / fVar22;
            }
            d17 = P.f64(po::Vy);
            if ((double)fhi <= P.f64(po::Vy)) d17 = (double)fhi;
            setYVelocity(P, d17);
        }
        if (fallingBugged(P)) P.u8(0xa24, 0);
        finish();
        return;
    }
    // ---- cube, ball, robot, spider ----
    fVar23 = 1.0f;
    if (ball) fVar23 = 0.6f; else if (spider) fVar23 = 0.6f; else if (robot) fVar23 = 0.9f;
    bool bVar15, bVar16;
    {
        const uint8_t b61 = P.u8(0xb61);
        if ((b61 == 0 || left) && (P.u8(0xb62) == 0 || right)) { bVar15 = false; bVar16 = true; }
        else {
            bVar15 = true;
            if (P.u8(0xb63) != b61 && P.u8(0x5fc) == 0) P.i32(0xb44, 0);
            bVar16 = P.i32(0xb44) < 2;
        }
    }
    if (H && P.u8(po::G)) {
        if (!spider) {
            if (P.u8(0x9ec) == 0 && (!plat || bVar16)) {
                P.u8(0xa24, 1);
                P.u8(0xa14, 0);
                P.u8(po::G, 0);
                P.u8(0x98e, 0);
                P.u8(0x9a4, 0);
                P.f64(0x838, 0.0);
                if (P.u8(0xb61) == 0 && P.u8(0xb62) == 0 && 0 < P.i32(0xb44)) { P.i32(0xb44, 0); P.f64(0xb48, P.f64(po::Time)); }
                float fj = (float)P.f64(0x7c8);
                if (robot) fj = fj * 0.5f;
                if (uVar9 != 0xd5u) { fj = 9999.0f; P.u8(0x95a, 1); }
                if (plat && bVar15) c.unsupported += 1, ++c.unsupportedBy[2001];     // the platformer's jump ramp (+0xb44, +0xb60, +0xb00)
                setYVelocity(P, (double)((float)dir * fj * fVar22));
                if (P.u8(po::WasOnSlope) || P.u8(po::OnSlope)) {
                    const float f9bc = P.f32(0x9bc);
                    const int i13 = upside ? -1 : 1;
                    if (0.0f < (float)i13 * f9bc && (!plat || 4.0 < std::fabs(P.f64(0xb00)))) {
                        const double v1 = P.f64(po::Vy);
                        double d21 = 1.0;
                        if (P.f64(po::Time) - P.f64(0x598) < 0.10000000149011612) {
                            const double d18 = (P.f64(po::Time) - P.f64(0x598)) * 10.0;
                            d21 = 0.4000000059604645;
                            if (0.4000000059604645 <= d18) d21 = d18;
                        }
                        float f24 = (float)((double)f9bc * d21);
                        if (ball) f24 = f9bc;
                        setYVelocity(P, (double)(f24 * 0.25f) + v1);
                        const double d21b = (double)(float)(v1 * 1.399999976158142);
                        double d17 = P.f64(po::Vy);
                        if (!upside) { if (d21b <= d17) d17 = d21b; }
                        else if (d17 <= d21b) d17 = d21b;
                        setYVelocity(P, d17);
                    }
                }
                P.u8(0x7eb, 1);
                // _ftime64 -> +0x800 (wall clock): not written
                if (P.u8(0x7ec)) P.u8(0xa90, 1);                       // incrementJumps 0x4eae50 (the layer's jump statistics and rand() aside)
                if (!ball) {
                    if (!robot) {
                        if (!spider && P.u8(0xa32) == 0 && P.u8(0x9ec) == 0) {
                            P.put<uint16_t>(0x730, 0);
                            P.u8(0x670, 0);
                            P.i32(0x728, 0);
                            runNormalRotation(P, false, 1.0f);
                        }
                    }
                } else {
                    playerFlipGravity(c, upside ? 0 : 1, true);
                    P.f64(po::Vy, P.f64(po::Vy) * 0.6000000238418579);
                    P.u8(0x7f0, 0);
                    P.u8(0x98d, 0);
                }
                if (plat) c.unsupported += 1, ++c.unsupportedBy[2002];       // animatePlatformerJump
                finish();
                return;
            }
        } else if (P.u8(0x9ec) == 0) {
            spider::spiderTestJump(c);                                              // 0x4f2540
            finish();
            return;
        }
    }
    if (P.u8(0xa24)) {                                                       // rising out of a jump
        if (robot && P.u8(0x98d) && P.u8(0x9a4) == 0 && P.f64(0x838) < 1.5) {
            P.f64(0x838, (double)(dt * 0.1f) + P.f64(0x838));
            setYVelocity(P, (double)((float)dir * dt * fVar24 * fVar23) + P.f64(po::Vy));
        }
        setYVelocity(P, P.f64(po::Vy) - (double)((float)dir * dt * fVar24 * fVar23));
        if (!fallingBugged(P)) {
            if (!plat) { finish(); return; }
            const double vv = P.f64(po::Vy);
            const bool b = upside ? 0.0 < vv : vv < 0.0;
            if (!b && vv != 0.0) { finish(); return; }
        }
        P.u8(0xa24, 0);
        P.u8(0x7ea, 1);
        P.u8(0xa14, 0);
        P.f32(0xc44, P.f32(po::PosY));
        finish();
        return;
    }
    if (P.u8(po::G)) P.f32(0xc44, P.f32(po::PosY));
    if (fallingBugged(P) && (!plat || 0.05000000447034836 <= P.f64(po::Time) - P.f64(0xa18) || !bVar16)) P.u8(po::G, 0);
    {
        const double d17 = (double)((float)dir * dt * fVar24 * fVar23);
        setYVelocity(P, P.f64(po::Vy) - d17);                                // (0x4ea569: [0x9a8] - D((sgnUD * (dt * G11)) * M); the decompile scrambles the operands)
    }
    {
        double d17 = P.f64(po::Vy);
        if (!upside) { if (d17 <= -15.0) d17 = -15.0; }
        else if (15.0 <= d17) d17 = 15.0;
        const double d21 = (double)(int)d17;
        if (d17 != d21) d17 = std::round((d17 - d21) * 1000.0) / 1000.0 + d21;
        P.f64(po::Vy, d17);
        const bool below = !upside ? (d17 < -0.25 && !(d17 == -0.25)) : (0.25 < d17);
        // `bVar16 = -0.25 < d17; bVar15 = d17 == -0.25` for an upright body: the branch is taken when d17 < -0.25 (upside: d17 > 0.25)
        if (below && !ball && !spider && !robot && P.u8(0x730) == 0 && P.u8(po::OnSlope) == 0 && P.u8(0x660) == 0
            && (!plat || P.u8(0xa14) == 0) && P.u8(0xa32) == 0 && P.u8(0x9ec) == 0) {
            P.put<uint16_t>(0x730, 0);
            P.u8(0x670, 0);
            P.i32(0x728, 0);
            if (!ball) runNormalRotation(P, false, 1.0f); else runBallRotation(P, 1.0f);
        }
    }
    if (fallingBugged(P)) {
        const double d17 = P.f64(po::Vy);
        const bool bv = !upside ? (d17 < -4.0) : (4.0 < d17);
        if (bv) P.u8(0xa14, 0);
    }
    finish();
}

// PlayerObject::update 0x4e6fc0 (the physics; the colour fades, the particle systems, the streak, the texture and the platformer's dash stop are left out).
// `updateJumpFn` is updateJump 0x4e9b80 (it is called first, only when the body is not locked).
template <class UpdateJump>
inline void playerUpdate(Ctx& c, float dt, UpdateJump&& updateJumpFn) {
    Mem& P = c.P;
    const float fVar39 = dt * 0.9f;
    double vy = P.f64(po::Vy);
    if (vy > 1000.0) P.put<uint64_t>(po::Vy, 0x408f400000000000ULL);
    else if (vy < -1000.0) P.put<uint64_t>(po::Vy, 0xc08f400000000000ULL);
    if (P.u8(po::Platformer)) {
        const double b00 = P.f64(0xb00);
        if (b00 > 1000.0) P.put<uint64_t>(0xb00, 0x408f400000000000ULL);
        else if (b00 < -1000.0) P.put<uint64_t>(0xb00, 0xc08f400000000000ULL);
    }
    if (P.u8(0x9c8)) return;                                  // dead
    // ccpDistance(&prev, &pos) is unused; prev := pos
    P.f32(po::PrevX, P.f32(po::PosX));
    P.f32(po::PrevY, P.f32(po::PosY));
    P.f32(0xabc, 0.0f);
    if (P.u8(0xa32) == 0) {                                   // not locked
        updateJumpFn(c, fVar39);                              // (the disassembly passes dt * 0.9, the decompile hides the argument)
        if (P.i32(0xb90) > 0) {
            float fVar33 = 1.0f;
            if (P.u8(po::ModeShip)) fVar33 = 0.47f;
            else if (P.u8(po::ModeUfo)) fVar33 = 0.58f;
            else if (P.u8(po::ModeSwing)) fVar33 = 0.4f;
            else if (P.u8(po::ModeBall)) fVar33 = 0.6f;
            else if (P.u8(po::ModeSpider)) fVar33 = 0.6f;
            else if (P.u8(po::ModeRobot)) fVar33 = 0.9f;
            if (P.f32(0x9f8) != 1.0f) {
                if (P.u8(po::ModeShip)) fVar33 = 0.5875f;
                else if (P.u8(po::ModeUfo)) fVar33 = 0.72499996f;
                else if (P.u8(po::ModeSwing)) fVar33 = 0.61538464f;
            }
            const double dVar36 = (double)(fVar39 * P.f32(0xb98) * fVar33);
            setYVelocity(P, dVar36 + P.f64(po::Vy));
            if (dVar36 != 0.0) P.u8(0x95a, 1);
            bool acc = false;
            if (P.u8(po::Upside) == 0) { if (0.0 < dVar36) acc = true; }
            else if (dVar36 < 0.0) acc = true;
            if (acc) P.f64(0x838, (dVar36 / 12.9399995803833) * 1.5 + P.f64(0x838));
            if (P.u8(po::Platformer)) {
                float f = P.f32(0xb94);
                P.f64(0xb00, (double)(f * fVar39) + P.f64(0xb00));
                if (f != 0.0f) P.u8(0xb9c, 1);
                if (!P.u8(po::ModeShip) && !P.u8(po::ModeUfo) && !P.u8(po::ModeWave) && !P.u8(po::ModeSwing) && !P.u8(po::ModeBall) && !P.u8(po::ModeRobot)
                    && !P.u8(po::ModeSpider) && P.u8(0x730) == 0 && 0.1f < std::fabs(f)) {
                    const float fv = P.f32(0x9f8) == 1.0f ? 0.43333334f : 0.33333334f;
                    const int iVar11 = P.u8(po::Sideways) ? -1 : 1;
                    const int iVar29 = P.u8(po::Leftward) ? -0xb4 : 0xb4;
                    const int iVar12 = P.u8(po::Upside) ? -1 : 1;
                    P.f32(0x728, ((float)(iVar29 * iVar12) * (float)iVar11 * P.f32(0xb8c)) / fv);
                    P.u8(0x730, 1);
                }
            }
        }
        if (P.u8(0x9ec)) P.f64(po::Vy, 0.0);                  // dashing: no vertical speed
        const double dVar30 = (double)fVar39;
        double dVar36 = dVar30 * P.f64(po::Vy);
        if (P.u8(po::Platformer)) c.unsupported += 1, ++c.unsupportedBy[2000];   // updateMove 0x4e8310 is not ported
        const bool dash = P.u8(0x9ec) != 0;
        const bool plat = P.u8(po::Platformer) != 0;
        double dVar38 = plat ? P.f64(0xb00) : (double)P.f32(0x9fc) * P.f64(0x7c0);
        dVar38 = dVar38 * (double)dt;
        const double dVar8 = dVar38 * 0.019999999552965164;
        bool dashArm = false;
        if (!P.u8(po::ModeWave)) { if (dash) dashArm = true; }
        else if (!dash) {
            const int iVar11 = P.u8(po::Upside) ? -1 : 1;
            const int iVar29 = P.u8(0x98d) ? 1 : -1;
            dVar36 = std::fabs(dVar38) * (double)iVar11 * (double)iVar29;
            if (P.f32(0x9f8) != 1.0f) dVar36 = dVar36 + dVar36;
        } else dashArm = true;
        if (dashArm) {
            const double dVar2 = P.f64(0x578);
            if (!plat) dVar36 = dVar2 * dVar38;
            else {
                dVar38 = P.f64(0x570) * dVar30;
                dVar36 = dVar2 * dVar30;
                P.f64(0xb00, P.f64(0x570));
                P.f64(po::Vy, dVar2);
            }
        }
        P.f32(0xabc, (float)dVar36);
        if (P.u8(po::Leftward) && !plat) dVar38 = dVar38 * -1.0;
        double dx = dVar38 + P.f64(0x9e0);
        double dy = dVar36;
        if (P.u8(po::Sideways)) { dx = dVar36; dy = dVar38 + P.f64(0x9e0); }
        setPosition(P, P.f32(po::PosX) + (float)dx, P.f32(po::PosY) + (float)dy);
        P.u8(0x369, 1);                                   // CCNode / GameObject::setPosition marks both rect caches dirty (collide.hpp's setPosition writes 0x368 only)
        P.f64(0x9e0, 0.0);
        const double pend = P.f64(0x9d8);
        if (pend != 0.0) {
            double d = std::fabs(pend);
            if (dVar8 <= d) d = dVar8;
            if (pend <= 0.0) d = -d;
            P.f64(0x9e0, d);
            P.f64(0x9d8, pend - d);
        }
    }
    // the tail of update: the registers the next step's buttons read, and the counters
    P.u8(0x8b8, 0);
    P.u8(0x991, P.u8(0x98d));
    P.u8(0x992, P.u8(0x98e));
    P.i32(0x993, 0);
    for (size_t off : {(size_t)0xb7c, (size_t)0xb80, (size_t)0xb88, (size_t)0xb84, (size_t)0xae4, (size_t)0xaec, (size_t)0xaf0, (size_t)0xaf4, (size_t)0xaf8, (size_t)0xb90})
        P.i32(off, P.i32(off) - 1);
    P.f32(0xb94, 0.0f);
    P.f32(0xb98, 0.0f);
    P.put<uint64_t>(0xba8, 0);   // the force ids' map is emptied (FUN_14016edd0)
}

}  // namespace upd
}  // namespace gdcol
}  // namespace dp
