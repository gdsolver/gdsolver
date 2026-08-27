// EnhancedGameObject (pad trace) and PlayerObject hooks: state dump, physics tracing.
#include "mod/playlayer_helpers.hpp"

using namespace p1;

class $modify(PadTraceGameObject, EnhancedGameObject) {
    void activatedByPlayer(PlayerObject* p) {
        if (g_cfg.padTrace) {
            const int ty = (int)this->m_objectType;
            if (ty == 8 || ty == 9 || ty == 10) {
                auto* l = GJBaseGameLayer::get();
                if (l && p == l->m_player1 && ++padtrace::g_lines <= 400) {
                    char buf[224];
                    snprintf(buf, sizeof(buf),
                        "padact: t=%lld id=%d uid=%d ty=%d o=(%.3f,%.3f) "
                        "p=(%.4f,%.4f) vy=%.4f used=%d",
                        (long long)g_tick, this->m_objectID, this->m_uniqueID,
                        ty, this->getPositionX(), this->getPositionY(),
                        p->getPositionX(), p->getPositionY(),
                        (float)p->m_yVelocity,
                        this->m_activatedByPlayer1 ? 1 : 0);
                    writeResult(buf);
                }
            }
        }
        EnhancedGameObject::activatedByPlayer(p);
    }
};

class $modify(PlayerObject) {
    // Death visual effects are not created during fast mode (cfg `deathfx=1` restores the old
    // behaviour). The reason is recorded at the declaration of g_deathFx
    void playDeathEffect() {
        if (!g_deathFx && renderSuppressed()) { ++g_deathFxSkipped; return; }
        PlayerObject::playDeathEffect();
    }

    void update(float dt) {
        auto* l = GJBaseGameLayer::get();
        bool isP1 = l && this == l->m_player1;
        if (isP1) ev("PO_update_pre", dt);
        PlayerObject::update(dt);
        if (isP1) traceModifierCounters(this);
    }

    // Name the spider's teleport target ON THE SPOT (cfg `standtrace=1`).
    //
    // Reading the raw fields from GJBaseGameLayer::update is too late: by the time the
    // layer update runs, `[this+0x960]` is back to the player's bottom edge, `[this+0x968]`
    // is 0 and `[this+0x5e8]` is null (measured, 7 points). Wrapping the function directly
    // captures the values IMMEDIATELY AFTER the search.
    //
    // [Correction] Those three are not spiderTestJump's but the bookkeeping of
    // updateCollide. The installed GD is 2.2081, not 2.208, and in 2.2081:
    //   0x393ff0 updateCollide / 0x394340 spiderTestJump /
    //   0x3943f0 spiderTestJumpInternal
    // The search body is 0x3943f0: it builds a rect, extends it to the band
    // (getMinPortalY / getMaxPortalY) and queries TWICE — staticObjectsInRect and
    // damagingObjectsInRect. The from/to/gravity emitted by this instrumentation
    // (`spidertp:` lines) were the evidence that pinned down those two rects.
    void spiderTestJump(bool dynamic) {
        auto* l = GJBaseGameLayer::get();
        const bool isP1 = l && this == l->m_player1;
        const float y0 = this->getPositionY();
        PlayerObject::spiderTestJump(dynamic);
        if (!isP1 || !g_cfg.standTrace || !g_started || g_sessionOver) return;
        auto* tg = *reinterpret_cast<GameObject**>((char*)this + 0x5e8);
        const double lo = *reinterpret_cast<double*>((char*)this + 0x960);
        const double hi = *reinterpret_cast<double*>((char*)this + 0x968);
        char tb[192] = "-";
        if (tg) {
            auto q = tg->getPosition();
            auto r = tg->getObjectRect();
            snprintf(tb, sizeof(tb),
                     "uid%d id%d type%d @%.2f,%.2f %.2fx%.2f",
                     tg->m_uniqueID, tg->m_objectID, (int)tg->m_objectType,
                     q.x, q.y, r.size.width, r.size.height);
        }
        char b[320];
        snprintf(b, sizeof(b),
                 "spidertp: t=%lld dyn=%d y %.3f -> %.3f up=%d lo=%.3f hi=%.3f "
                 "tgt=[%s]",
                 (long long)g_tick, (int)dynamic, y0, this->getPositionY(),
                 (int)this->m_isUpsideDown, lo, hi, tb);
        writeResult(b);
    }

    // When, and with which sign, the player's rotation speed gets written (cfg
    // `hitboxtrace=1`).
    //
    // Why this is needed: the hit test of a rotated portal is not an AABB but a SAT of
    // player OBB x object OBB (collisionCheckObjects 0x214960), so the model must reproduce
    // the player's rotation angle. The magnitude is closed by measurement
    // (full 1.730769 / mini 2.25 °/tick), but the SIGN ALONE cannot be determined.
    // Sweeps tested 3 hypotheses (flip-dependent / inverts on every gravity flip /
    // direction-dependent) and refuted all of them. Measuring by reading raw offsets took
    // the whole worker down (0xC0000005). Looking at the arguments directly is the only
    // safe and reliable way.
    // When, and by which trigger, a rotation trigger fired (cfg `hitboxtrace=1`, `rot:`
    // lines).
    //
    // The model's crossing test runs in the progress coordinate of the rotation frame, and
    // in frame 3 that becomes "the trigger's world Y", so uid5957 (8297,423), 1,500px away
    // in world space, fires spuriously at u=423 (lv22 t=4,688).
    // Replacing it with "cross on world X" broke the first section, so DO NOT GUESS THE
    // TEST AXIS — ASK GD. Once this line appears, the tick and the partner are settled.
    // RECORD THE RISING EDGE of the modifier counters
    // (m_stateNoAutoJump/DartSlide/HitHead/FlipGravity/Force). collisionCheckObjects
    // writes a 2 according to the id of the object it touched, and the tail of
    // PlayerObject::update decrements unconditionally, so "a 2 was read = touching it on
    // that tick" should hold. Yet at lv22 t=2,748 a 2 was read 40px away from the 2866 box
    // (fixed at 3735,255). Without measuring WHICH TICK and WHO raised it, the box cannot
    // be identified.
    // The source of the gravity flip at t=2,749. xref narrowed it to 23 sites, and the
    // caller was confirmed to be PlayerObject::didHitHead (win 0x393c30). The real thing is:
    //   if ((int)this[+0xb80] > 0) {
    //       flipGravity(!m_isUpsideDown, true);
    //       setYVelocity(m_isUpsideDown ? +2 : -2);
    //       this[+0xa1c] = 1; this[+0xa0c] = 0;
    //       if ((int)this[+0xb74] > 0) { this[+0x9c1] = 0; *(u16*)&this[+0x985] = 0; }
    //   }
    // The callers are 3 sites in collidedWithObjectInternal = when the head hits a solid.
    // In other words, a "hitting the ceiling flips gravity" path EXISTS, and it only takes
    // effect when [+0xb80] > 0. Its identity is measured here ([+0xb84] has already
    // appeared as the multiplier in runNormalRotation; they are adjacent).
    void didHitHead() {
        auto* l = GJBaseGameLayer::get();
        const bool watch = g_cfg.hitboxTrace && l && this == l->m_player1
                           && g_started && !g_sessionOver;
        if (watch) {
            static int lines = 0;
            if (++lines <= 400) {
                const char* pb = reinterpret_cast<const char*>(this);
                int b80 = 0, b74 = 0; float b84 = 0.f;
                memcpy(&b80, pb + 0xb80, 4);
                memcpy(&b74, pb + 0xb74, 4);
                memcpy(&b84, pb + 0xb84, 4);
                // Pin down what b80/b74 are by name. From the raw offset values alone it
                // cannot be decided what the "2" is a 2 of
                const char* self = pb;
                char o[160];
                snprintf(o, sizeof(o),
                         "dhhoff: stateFlipGravity=0x%X gravityMod=0x%X "
                         "gravity=0x%X isUpsideDown=0x%X",
                         (unsigned)((const char*)&this->m_stateFlipGravity - self),
                         (unsigned)((const char*)&this->m_gravityMod - self),
                         (unsigned)((const char*)&this->m_gravity - self),
                         (unsigned)((const char*)&this->m_isUpsideDown - self));
                writeResult(o);
                char b[224];
                snprintf(b, sizeof(b),
                         "dhh: t=%lld x=%.3f y=%.3f vy=%.3f up=%d gate[b80]=%d "
                         "[b74]=%d [b84]=%.4f topMinY=%.3f",
                         (long long)g_tick, this->getPositionX(),
                         this->getPositionY(), this->m_yVelocity,
                         (int)this->m_isUpsideDown, b80, b74, b84,
                         this->m_collidedTopMinY);
                writeResult(b);
            }
        }
        PlayerObject::didHitHead();
    }

    // The EXECUTOR of gravity flips. GJBaseGameLayer::flipGravity is only the "via portal"
    // entry point, and the flip at lv22 t=2,749 did not go through it (hooked and
    // measured). This is what actually sets m_isUpsideDown.
    // The slope-derived flags are emitted as well — the flip happened in a slope section,
    // and GD has m_maybeUpsideDownSlope / m_slopeFlipGravityRelated.
    void flipGravity(bool flip, bool noEffects) {
        auto* l = GJBaseGameLayer::get();
        const bool watch = g_cfg.hitboxTrace && l && this == l->m_player1
                           && g_started && !g_sessionOver;
        const int before = watch ? (int)this->m_isUpsideDown : 0;
        PlayerObject::flipGravity(flip, noEffects);
        if (!watch) return;
        static int lines = 0;
        if (++lines > 2000) return;
        // Name the caller by RVA. flipGravity has 23 xrefs, and static reading alone cannot
        // decide "which one took effect on this tick".
        // Only frames inside the GD binary itself are picked up and printed (Geode's
        // trampolines live in another module, so they drop out naturally).
        {
            void* fr[24];
            const USHORT n = RtlCaptureStackBackTrace(0, 24, fr, nullptr);
            const auto base = (uintptr_t)GetModuleHandleA(nullptr);
            std::string cs;
            for (USHORT i = 0; i < n; ++i) {
                const uintptr_t a = (uintptr_t)fr[i];
                char t[32];
                if (a >= base && a - base < 0x600000)
                    snprintf(t, sizeof(t), " gd:%llX",
                             (unsigned long long)(a - base));
                else
                    snprintf(t, sizeof(t), " ?%llX", (unsigned long long)a);
                cs += t;
            }
            writeResult(("pfgstk: t=" + std::to_string((long long)g_tick)
                         + " n=" + std::to_string((int)n)
                         + " base=" + std::to_string((long long)base)
                         + cs).c_str());
        }
        char b[224];
        snprintf(b, sizeof(b),
                 "pfg: t=%lld flip=%d noFx=%d up %d->%d x=%.3f y=%.3f vy=%.3f "
                 "slopeUp=%d slopeRel=%d onSlope=%d grav=%.3f",
                 (long long)g_tick, flip ? 1 : 0, noEffects ? 1 : 0,
                 before, (int)this->m_isUpsideDown,
                 this->getPositionX(), this->getPositionY(), this->m_yVelocity,
                 (int)this->m_maybeUpsideDownSlope,
                 (int)this->m_slopeFlipGravityRelated,
                 (int)this->m_isOnSlope, (double)this->m_gravity);
        writeResult(b);
    }

    void runNormalRotation(bool notNormalMode, float speed) {
        auto* l = GJBaseGameLayer::get();
        const bool watch = g_cfg.hitboxTrace && l && this == l->m_player1
                           && g_started && !g_sessionOver;
        const float before = watch ? this->m_rotationSpeed : 0.f;
        PlayerObject::runNormalRotation(notNormalMode, speed);
        if (!watch) return;
        static int lines = 0;
        if (++lines > 4000) return;
        char b[192];
        snprintf(b, sizeof(b),
                 "rnr: t=%lld nnm=%d speed=%.4f rspd %.4f -> %.4f "
                 "flip=%d grounded=%d size=%.3f",
                 (long long)g_tick, notNormalMode ? 1 : 0, speed,
                 before, this->m_rotationSpeed,
                 (int)this->m_isUpsideDown, (int)this->m_isOnGround,
                 this->m_vehicleSize);
        writeResult(b);
    }

    // Stair snap observation (cfg `snaptrace=1`). The identity of the extra dx on the
    // landing tick. Called only from the cube landing branch of collidedWithObjectInternal.
    // "Called but did not move" cases are emitted too (otherwise the non-firing conditions
    // become unknowable)
    void checkSnapJumpToObject(GameObject* object) {
        auto* l = GJBaseGameLayer::get();
        bool watch = g_cfg.snapTrace && l && this == l->m_player1 && object;
        if (!watch) { PlayerObject::checkSnapJumpToObject(object); return; }
        GameObject* prev = this->m_objectSnappedTo;
        CCPoint pp = prev ? prev->getRealPosition() : CCPoint(0, 0);
        CCPoint op = object->getRealPosition();
        float x0 = this->getPositionX();
        double sd0 = this->m_snapDistance;
        PlayerObject::checkSnapJumpToObject(object);
        float x1 = this->getPositionX();
        static int lines = 0;
        if (++lines > 40000) return;
        char b[320];
        snprintf(b, sizeof(b),
            "snap: t=%lld y=%.3f spd=%.1f size=%.2f flip=%d "
            "prev=%d,type=%d,(%.2f,%.2f) obj=%d,type=%d,(%.2f,%.2f) "
            "dx=%.2f dy=%.2f x=%.4f->%.4f d=%+.4f sd=%.4f->%.4f",
            (long long)g_tick, this->getPositionY(), (float)this->m_playerSpeed,
            this->m_vehicleSize, this->m_isUpsideDown ? 1 : 0,
            prev ? prev->m_uniqueID : -1, prev ? (int)prev->getType() : -1, pp.x, pp.y,
            object->m_uniqueID, (int)object->getType(), op.x, op.y,
            prev ? op.x - pp.x : 0.f, prev ? op.y - pp.y : 0.f,
            x0, x1, x1 - x0, sd0, this->m_snapDistance);
        writeResult(b);
    }

    // Orb fire observation (cfg `orbtrace=1`). Records the offset between the player
    // position and the orb centre at fire time. addToTouchedRings is defined inline and
    // cannot be hooked, but the difference between press tick and fire tick tells whether
    // "the press was buffered and fired at the moment of contact"
    void ringJump(RingObject* object, bool skipCheck) {
        auto* l = GJBaseGameLayer::get();
        bool isP1 = object && l && this == l->m_player1;
        bool watch = g_cfg.orbTrace && isP1;
        if (!watch) {
            PlayerObject::ringJump(object, skipCheck); return;
        }
        // This function is a test path called every tick for the whole contact, not the
        // fire point. Whether it actually fired is decided by whether vy jumped
        float ox = object->getPositionX(), oy = object->getPositionY();
        float vyBefore = (float)this->m_yVelocity;
        PlayerObject::ringJump(object, skipCheck);
        float vyAfter = (float)this->m_yVelocity;
        if (std::abs(vyAfter - vyBefore) < 1e-4f) return; // did not fire
        // Log-flood guard: only near the targeted x + a total cap
        if (g_cfg.orbTraceX > 0 && std::abs(ox - g_cfg.orbTraceX) > 300.f) return;
        if (++orbtrace::g_lines > 400) return;
        long long lag = (orbtrace::g_lastPress >= 0)
                      ? (long long)g_tick - orbtrace::g_lastPress : -1;
        char buf[256];
        snprintf(buf, sizeof(buf),
            "orb: id=%d mode=%d size=%.2f flip=%d spd=%.1f "
            "t=%lld press=%lld lag=%lld dxx=%.1f dyy=%.1f "
            "orb=(%.0f,%.0f) vy=%.4f->%.4f",
            object->m_objectID, (int)solver::modeOf(this),
            this->m_vehicleSize, this->m_isUpsideDown ? 1 : 0,
            (float)this->m_playerSpeed,
            (long long)g_tick, (long long)orbtrace::g_lastPress, lag,
            this->getPositionX() - ox, this->getPositionY() - oy,
            ox, oy, vyBefore, vyAfter);
        writeResult(buf);
    }

    // Pad launch measurement (cfg `padtrace=1`). Pads have no hook that carries the target
    // the way ringJump does; the effect converges here (propellPlayer). The partner is
    // identified by matching against the padact: line of the same tick (activatedByPlayer
    // side)
    void propellPlayer(float yVelocity, bool noEffects, int objectType) {
        auto* l = GJBaseGameLayer::get();
        bool watch = g_cfg.padTrace && l && this == l->m_player1;
        if (!watch) {
            PlayerObject::propellPlayer(yVelocity, noEffects, objectType); return;
        }
        float vyBefore = (float)this->m_yVelocity;
        PlayerObject::propellPlayer(yVelocity, noEffects, objectType);
        if (++padtrace::g_lines > 400) return;
        char buf[224];
        snprintf(buf, sizeof(buf),
            "padfire: t=%lld p=(%.4f,%.4f) arg=%.4f otype=%d "
            "vy=%.4f->%.4f flip=%d mode=%d spd=%.1f",
            (long long)g_tick, this->getPositionX(), this->getPositionY(),
            yVelocity, objectType, vyBefore, (float)this->m_yVelocity,
            this->m_isUpsideDown ? 1 : 0, (int)solver::modeOf(this),
            (float)this->m_playerSpeed);
        writeResult(buf);
    }

    bool collidedWithObject(float dt, GameObject* obj, cocos2d::CCRect rect, bool skip) {
        auto* l = GJBaseGameLayer::get();
        // The player rect BEFORE resolution. This function pushes the player out in the
        // hit branch, so reading getObjectRect() after the call only shows the state AFTER
        // resolution. How deep, and along which axis, the player penetrated exists only in
        // the pre-resolution rect, and without it the gate for "which axis GD resolves on"
        // cannot be decided (the swing push-out at lv22 t=3,670 would not close because of
        // that).
        const bool hbWatch = g_cfg.hitboxTrace && l && this == l->m_player1 && obj
                             && g_tick >= g_cfg.hbFrom
                             && (g_cfg.hbTo <= 0 || g_tick <= g_cfg.hbTo);
        CCRect prePr = hbWatch ? this->getObjectRect() : CCRect();
        bool r = PlayerObject::collidedWithObject(dt, obj, rect, skip);
        // Hitbox observation (cfg `hitboxtrace=1`). Emits the partner rect exactly as GD
        // passed it, plus the player rect of the same tick and the test result (without
        // both, it cannot be told whether the shrunk side is the partner or the player)
        if (g_cfg.hitboxTrace && l && this == l->m_player1 && obj
            && g_tick >= g_cfg.hbFrom
            && (g_cfg.hbTo <= 0 || g_tick <= g_cfg.hbTo)) {
            static int lines = 0;
            if (++lines <= 40000) {
                CCRect pr = this->getObjectRect();
                CCRect orr = obj->getObjectRect();
                char b[352];
                snprintf(b, sizeof(b),
                    "hbox: t=%lld obj=%d type=%d hit=%d size=%.2f "
                    "arg=(%.2f,%.2f,%.2f,%.2f) objrect=(%.2f,%.2f,%.2f,%.2f) "
                    "player=(%.2f,%.2f,%.2f,%.2f) ppre=(%.2f,%.2f,%.2f,%.2f)",
                    (long long)g_tick, obj->m_uniqueID, (int)obj->getType(),
                    r ? 1 : 0, this->m_vehicleSize,
                    rect.origin.x, rect.origin.y, rect.size.width, rect.size.height,
                    orr.origin.x, orr.origin.y, orr.size.width, orr.size.height,
                    pr.origin.x, pr.origin.y, pr.size.width, pr.size.height,
                    prePr.origin.x, prePr.origin.y,
                    prePr.size.width, prePr.size.height);
                writeResult(b);
            }
        }
        return r;
    }

    // Which ramp the player is riding (cfg `hitboxtrace=1`, `slp:` lines).
    // `hbox` does NOT pass type 25, so the slope partner can only be learned here.
    // At lv22 t=4,584 GD reports `onSlope=1`, but none of the nearby ramps is within reach
    // of the player's 15px half-width, and the work stalled because the partner could not
    // be identified.
    void collidedWithSlopeInternal(float dt, GameObject* obj, bool forced) {
        auto* l = GJBaseGameLayer::get();
        const bool watch = g_cfg.hitboxTrace && l && this == l->m_player1 && obj
                           && g_started && !g_sessionOver
                           && g_tick >= g_cfg.hbFrom
                           && (g_cfg.hbTo <= 0 || g_tick <= g_cfg.hbTo);
        const double yBefore = watch ? this->getPositionY() : 0.0;
        PlayerObject::collidedWithSlopeInternal(dt, obj, forced);
        if (!watch) return;
        static int lines = 0;
        if (++lines > 4000) return;
        CCRect orr = obj->getObjectRect();
        char b[288];
        snprintf(b, sizeof(b),
                 "slp: t=%lld uid=%d id=%d forced=%d onSlope=%d up=%d top=%d "
                 "y %.3f->%.3f "
                 "vy=%.3f rect=(%.2f,%.2f,%.2f,%.2f) rot=%.1f syAtX=%.3f",
                 (long long)g_tick, obj->m_uniqueID, obj->m_objectID,
                 forced ? 1 : 0, (int)this->m_isOnSlope,
                 (int)this->m_isUpsideDown, (int)this->m_isCurrentSlopeTop,
                 yBefore, this->getPositionY(), this->m_yVelocity,
                 orr.origin.x, orr.origin.y, orr.size.width, orr.size.height,
                 obj->getRotation(), obj->slopeYPos(this->getPositionX()));
        writeResult(b);
    }

    // Hazards do not go through collidedWithObject; the actual test is here. The rect GD
    // passes in is the hitbox itself, so comparing it with getObjectRect() exposes the
    // shrink directly
    bool collidedWithObjectInternal(float dt, GameObject* obj, cocos2d::CCRect rect,
                                    bool skip) {
        bool r = PlayerObject::collidedWithObjectInternal(dt, obj, rect, skip);
        auto* l = GJBaseGameLayer::get();
        if (g_cfg.hitboxTrace && l && this == l->m_player1 && obj
            && g_tick >= g_cfg.hbFrom
            && (g_cfg.hbTo <= 0 || g_tick <= g_cfg.hbTo)) {
            static int lines = 0;
            if (++lines <= 40000) {
                CCRect pr = this->getObjectRect();
                CCRect orr = obj->getObjectRect();
                char b[352];
                snprintf(b, sizeof(b),
                    "hbin: t=%lld obj=%d type=%d hit=%d size=%.2f "
                    "arg=(%.2f,%.2f,%.2f,%.2f) objrect=(%.2f,%.2f,%.2f,%.2f) "
                    "player=(%.2f,%.2f,%.2f,%.2f)",
                    (long long)g_tick, obj->m_uniqueID, (int)obj->getType(),
                    r ? 1 : 0, this->m_vehicleSize,
                    rect.origin.x, rect.origin.y, rect.size.width, rect.size.height,
                    orr.origin.x, orr.origin.y, orr.size.width, orr.size.height,
                    pr.origin.x, pr.origin.y, pr.size.width, pr.size.height);
                writeResult(b);
            }
        }
        return r;
    }

    bool pushButton(PlayerButton button) {
        auto* l = GJBaseGameLayer::get();
        if (l && this == l->m_player1) ev("PO_pushButton", (int)button);
        return PlayerObject::pushButton(button);
    }

    bool releaseButton(PlayerButton button) {
        auto* l = GJBaseGameLayer::get();
        if (l && this == l->m_player1) ev("PO_releaseButton", (int)button);
        return PlayerObject::releaseButton(button);
    }
};
