#pragma once

namespace solver {

struct TickInfo {
    float x;
    float y;
    uint8_t grounded;
    uint8_t mode; // 0=cube 1=ship 2=ball 3=ufo 4=wave 5=robot 6=spider 7=swing
    uint8_t grounded2; // m_isOnGround2 (the checkpoint does not restore it, so it is
                       // filled in from here on restore)
    float yvel;        // for boundary selection: m_isOnGround can be set even while falling
    // In dual mode one input moves both bodies at once, so even if p1 is airborne
    // the press is meaningful if p2 is grounded. Keep p2's grounded state as
    // branch-point information too
    uint8_t dual = 0;      // m_gameState.m_isDualMode
    uint8_t p2Grounded = 0;
};

// POI: orbs extracted from the level's live memory (resolved real coordinates).
// id=kind (36=yellow 84=blue 141=pink).
// dash (DashRing/GravityDashRing) is not a tap but "rides the rail only while
// held" = the hold length is what matters
struct Poi { float x; float y; int id = 0; bool dash = false; };
inline std::vector<Poi> g_pois;
inline bool g_poisBuilt = false;
// Latch to report dual entry only once (diagnostics)
inline bool g_dualSeen = false;
// In dual mode both bodies can die at the same time; latch so one run's death
// is not booked twice
inline bool g_deathBooked = false;
// Coins (ID142=secret/1329=user). Pickup detection is our own: a coordinate
// collision with the player centre is tested every tick and the pickup tick is
// recorded. Does not depend on GD's coin state at all
inline std::vector<Poi> g_coins;
inline std::vector<long long> g_coinPickupTick; // -1=not picked up
constexpr float COIN_RADIUS = 20.f; // more conservative than GD's real hitbox: our
                                    // detection=pickup ⇒ the real game surely picks it up too
// Hazard/solid geometry (sorted by x). Used by the clearance table (clearance.hpp)
struct Obj { float x; float y; int id; };
inline std::vector<Obj> g_hazards;
inline std::vector<Obj> g_solids;

// Observation of moving gates (cfg `gatetrace=x0,x1` + `gatetick=t0,t1`).
// The only way to know when and where a moving block is, is to look at its real
// position every tick
inline bool g_gateTrace = false;
inline float g_gateX0 = 0.f, g_gateX1 = 0.f;
inline long long g_gateT0 = 0, g_gateT1 = 0;
inline long long g_gateStride = 1;   // record every how many ticks
inline std::vector<GameObject*> g_gateObjs;
// On the first recording, emit once the m_uniqueID and the real getObjectRect()
// rect of each target (to match against lcol = the unique ID of m_lastCollision*)
inline bool g_gateHdrDone = false;

// State-injection probe (cfg `colprobe=T,y0,ystep,vy,n`). Piggybacks on the
// replay of a known solution; at tick T of attempt k it injects
// y = y0 + (k-1)*ystep and m_yVelocity = vy and observes the resolution over n
// ticks (y', vy', death, lcol). Uses GD itself as the transition oracle.
// Note: put T in a window without plan input (the plan keeps running after injection)
inline bool g_colProbe = false;
inline long long g_cpT = 0;
inline double g_cpY0 = 0, g_cpYStep = 0, g_cpVy = 0;
inline int g_cpN = 30;
inline long long g_cpEndTick = -1;
inline bool g_cpInjected = false;   // injected in this attempt already? (false at attempt start)

inline std::vector<TickInfo> g_log;       // per-tick recording of the current attempt

// Largest object x in the level (computed in buildPois). Reference for rejecting
// physically impossible reach points produced by the exponential divergence in
// the end zone (plausibleDeathX)
inline float g_levelMaxX = 0;
// The true goal x = position of GJBaseGameLayer::m_endPortal. On levels whose
// ending runs backwards the goal is far short of the last object. 0 = not
// obtained (fallback is levelMaxX)
inline float g_goalX = 0;

// x of the previous tick. Passing within ±20px of a coin during the end-of-level
// suck-in animation gives a false pickup, so coin pickup is restricted to "normal
// movement". Normal movement (dx≈1.3px/tick) vs suck-in (dx>5px) / stall (dx≈0)
// are cleanly separated by dx
inline float g_prevTickX = -1e9f;

// Is x physically possible as a reach point (excludes end-zone divergence / NaN).
// Margin +60px (2 blocks): the only legitimate way past the last object is during
// the clear animation's suck-in
inline bool plausibleDeathX(float x) {
    if (!std::isfinite(x)) return false;
    if (g_levelMaxX > 0 && x > g_levelMaxX + 60.f) return false;
    return true;
}
inline long long g_totalAttempts = 0;
// Wall time spent in resetLevel (fixed cost per attempt. Used for resets= in stall.txt)
inline long long g_resetNanos = 0;
inline long long g_resetCalls = 0;
inline std::chrono::steady_clock::time_point g_solveStart;

// Restore tick of the most recent respawn (0=full run). Always 0 in serve/replay
// (diag emits it)
inline long long g_restoreTickDbg = -1;

inline uint8_t modeOf(PlayerObject* p) {
    if (p->m_isShip) return 1;
    if (p->m_isBall) return 2;
    if (p->m_isBird) return 3;
    if (p->m_isDart) return 4;
    if (p->m_isRobot) return 5;
    if (p->m_isSpider) return 6;
    if (p->m_isSwing) return 7;
    return 0;
}

// Is the plan input holding at tick T
inline bool holdingAt(const std::vector<InputCmd>& plan, long long t) {
    bool h = false;
    for (auto& c : plan) {
        if (c.step > t) break;
        h = c.down;
    }
    return h;
}

// Write the objrects table: one row per GameObject, with GD's own hitbox numbers.
//
// Split out of buildPois so that the SAME writer serves both consumers: the dump on
// disk that the leveldp CLI reads, and the in-memory buffer the mod hands to the solver
// core (src/mod/dp_bridge.cpp). One writer and one parser is what keeps 'the level the
// mod solves' and 'the level the CLI solves' the same object.
// 0 for everything that is not a force block, which is what leveldp reads it as.
inline float forceOf(GameObject* obj) {
    auto* fb = geode::cast::typeinfo_cast<ForceBlockGameObject*>(obj);
    return fb ? fb->m_force : 0.f;
}

inline void writeObjRects(std::ostream& rf, GJBaseGameLayer* l) {
    // ---- objrects.txt: GD's own real hitbox dimensions -----------------------
    // Columns: id,type,cx,cy,w,h,groups,uid,radius,rot,sy0,sy1,shz,sdir,sup,w0,h0,
    //     tpy,tpg,tpix,tpiy  (columns are only ever appended at the end: existing
    //     readers only look at the leading columns)
    // - cx,cy,w,h: getObjectRect(). Axis-aligned bounding box, so objects whose
    //   rotation is not a multiple of 90 degrees are much larger than their real
    //   size (using it for activation fires tens of px early)
    // - groups: identifies moving floors/gates (the static dump alone misreads
    //   them as "impassable")
    // - uid: m_uniqueID. Stair snap (checkSnapJumpToObject) processes the solids
    //   in contact in descending uniqueID order and the last one becomes
    //   m_objectSnappedTo. It is assigned in load order and cannot be derived
    //   from the geometry, so emit it
    // - radius: m_objectRadius. Sawblades (88/89/98) are tested as circles, not
    //   rects (0=rect)
    // - rot: getRotation(). Pads/orbs have an orientation (0=upward, 180=downward)
    // - sy0,sy1: surface height of a slope (type 25). A segment from sampling
    //   GD's slopeYPos() twice, at the left and right edges of the rect.
    //   shz=m_slopeIsHazard, sdir=m_slopeDirection, sup=m_slopeUphill (which
    //   side of the line is solid cannot be determined from the line alone)
    // - w0,h0: m_width/m_height = real size before rotation (back-computing
    //   from bounding box + angle is ambiguous and unreliable)
    // - tpy,tpg,tpix,tpiy: teleport portal (type 28, from
    //   GJBaseGameLayer::teleportPlayer). tpy is the absolute target y =
    //   getRealPosition().y + m_teleportYOffset. A closed form that needs no
    //   partner portal; the reference is getRealPosition() (a different point
    //   from the bounding-box centre cx,cy, so it is emitted as an absolute
    //   value, not an offset). tpg=m_gravityMode (1=force normal 2=force flipped
    //   3=toggle 0=unchanged), tpix/tpiy=m_ignoreX/Y. Fires on a plain rect
    //   overlap; vy/x are unchanged.
    //   m_saveOffset (prop 351) is not emitted (leveldp warns if a level using
    //   it shows up)
    //   [2026-08-18 correction] The closed form for tpy is for id 747 ONLY.
    //   When m_orangePortal (portal+0x748) is linked, teleportPlayer overwrites
    //   m_teleportYOffset on the spot with "exit.y − entry.y" (0x20fe44), and
    //   only id 747 uses portal.y + yOffset (compared with 0x2eb at 0x20ff5e).
    //   For everything else (2902) the target is the exit object's real position
    //   itself (0x20ff6a). The saved yOffset stays 0 for 2902, so tpy just
    //   mirrors the entry's y — lv22 uid6425 has tpy=705 while GD's real landing
    //   is 1905.000 (the position of the exit half). Hence:
    // - tpex,tpey: m_orangePortal->getRealPosition(). Unlinked is 0,0 (leveldp
    //   falls back to tpy and warns). The orange half does not appear in
    //   m_objects (it is not exported), so it can only be reached via the link
    // - tw: time-warp (id 1935) factor = EffectGameObject::m_timeWarpTimeMod.
    //   This is a factor on "time itself": dx, the integration of y and the vy
    //   increment ALL shrink by the same coefficient (measured at lv22 x=4,535:
    //   dx 1.95019->0.39014, dy 3.375->0.675, dvy 0.215->0.043; each exactly
    //   0.2x). Distinct from speed portals; the dump's `speed` column stays at
    //   1.3 and does not move. There is a partner with factor 1.0 at x=4,615
    //   where it returns. lv1-21 have none; only lv22 has 2
    // - dis: m_isDisabled (initial toggle state at load). Portals under Toggle
    //   (id 1049) control (lv22 group 309) start OFF and do not fire until ON.
    //   The transition is named by the MOD's `togl:` line (hitboxtrace=1)
    rf << "id,type,cx,cy,w,h,groups,uid,radius,rot,sy0,sy1,shz,sdir,sup,w0,h0,"
          "tpy,tpg,tpix,tpiy,tw,zoom,zdur,zease,zrate,mvdir,gnddir,"
          "optp1,optp2,flipx,flipy,nofx,notouch,tpex,tpey,dis,"
    // - force: ForceBlockGameObject::m_force, the push strength the level's
    //   author set on this instance (0 for everything that is not a force
    //   block). It is what the uid table in level_loader.hpp was standing in
    //   for: the strength is a per-object editor property, so no amount of
    //   geometry recovers it and the model had to write one number per object.
    //   The full settings (min/max/relative/range/id) are in forceblocks.txt;
    //   only the strength is needed here, and appending one column is the
    //   documented-safe way to reach leveldp.
          "editvel,vmodx,vmody,ovrvel,force\n";
    for (auto* obj : CCArrayExt<GameObject*>(l->m_objects)) {
        if (!obj) continue;
        auto r = obj->getObjectRect();
        double sy0 = 0.0, sy1 = 0.0;
        int shz = 0, sdir = 0, sup = 0;
        if ((int)obj->m_objectType == 25) {
            sy0 = obj->slopeYPos(r.origin.x);
            sy1 = obj->slopeYPos(r.origin.x + r.size.width);
            shz = obj->m_slopeIsHazard ? 1 : 0;
            sdir = obj->m_slopeDirection;
            sup = obj->m_slopeUphill ? 1 : 0;
        }
        double tpy = 0.0, tpex = 0.0, tpey = 0.0;
        int tpg = 0, tpix = 0, tpiy = 0;
        if ((int)obj->m_objectType == 28) {
            auto* tp = static_cast<TeleportPortalObject*>(obj);
            tpy = obj->getRealPosition().y + tp->m_teleportYOffset;
            tpg = tp->m_gravityMode;
            tpix = tp->m_ignoreX ? 1 : 0;
            tpiy = tp->m_ignoreY ? 1 : 0;
            if (tp->m_orangePortal) {
                auto ep = tp->m_orangePortal->getRealPosition();
                tpex = ep.x;
                tpey = ep.y;
            } else if (tp->m_targetGroupID > 0) {
                // 2nd path of teleportPlayer (0x20fe8a): without a partner it
                // picks one member of the m_targetGroupID group. With several
                // members it picks RANDOMLY via an LCG, but with one it is
                // deterministic at index 0 (the cmp eax,1 at 0x20feb6 skips the
                // RNG). Both 2902s in lv22 were single-member groups (the
                // decorations uid 6435/6436).
                auto* grp = l->getGroup(tp->m_targetGroupID);
                const int n = grp ? (int)grp->count() : 0;
                if (n >= 1) {
                    auto* ex = static_cast<GameObject*>(grp->objectAtIndex(0));
                    auto ep = ex->getRealPosition();
                    tpex = ep.x;
                    tpey = ep.y;
                }
                if (n != 1)
                    log::info("solver: teleport uid {} target group {} has {} "
                              "members (random exit - NOT deterministic)",
                              obj->m_uniqueID, tp->m_targetGroupID, n);
            }
        }
        double tw = 0.0;
        if (obj->m_objectID == 1935)
            tw = static_cast<EffectGameObject*>(obj)->m_timeWarpTimeMod;
        // Zoom (id 1913). The invisible ceiling is "270 / camera zoom", not a
        // per-mode constant (measured: (pmax-pmin) * camscale = exactly 270.000
        // holds for cube/spider/ball/swing/robot alike). The value is eased, so
        // duration and easing are needed too. lv1-21 have 0 of 1913; only lv22
        // has 20
        double zm = 0.0, zdur = 0.0, zrate = 0.0;
        int zease = 0;
        if (obj->m_objectID == 1913) {
            auto* e = static_cast<EffectGameObject*>(obj);
            zm = e->m_zoomValue;
            zdur = e->m_duration;
            zease = (int)e->m_easingType;
            zrate = e->m_easingRate;
        }
        // [Important] id 2900 is not a "rotation": it SETS THE DIRECTION AS AN
        // ABSOLUTE VALUE. m_groundDirection = new travel direction (1=up 2=down
        // 3=left 4=right), m_moveDirection = new gravity direction. From rot
        // alone, two objects with the same orot=0 cannot be told apart when one
        // travels +X and the other -X (= backwards).
        // Measured 2026-08-15, matches for all 20 activations in lv22 (details
        // in memory).
        int mvdir = 0, gnddir = 0;
        // Velocity change (2026-08-19, target 5): when editvel=m_editVelocity(169)
        // is set, vy at the switch tick becomes "travel speed × vmody" (the
        // default for a missing key is 0.0, and that was the real cause of the
        // "starts from rest" at lv22 t=4,665).
        // ovrvel=m_overrideVelocity(584) is an absolute assignment, not a
        // multiplication. The parsed member values are dumped, so GD's own
        // defaults carry over as-is.
        int editvel = 0, ovrvel = 0;
        float vmodx = 0.f, vmody = 0.f;
        if (obj->m_objectID == 2900) {
            auto* rg = static_cast<RotateGameplayGameObject*>(obj);
            mvdir = rg->m_moveDirection;
            gnddir = rg->m_groundDirection;
            editvel = rg->m_editVelocity ? 1 : 0;
            ovrvel = rg->m_overrideVelocity ? 1 : 0;
            vmodx = rg->m_velocityModX;
            vmody = rg->m_velocityModY;
        }
        // [Important] id 2899 is NOT backwards travel but the Options trigger
        // (GameOptionsTrigger). All 10 in lv22 exist to "kill / restore the
        // player's controls"; the m_controlsDisabled transitions measured in GD
        // (endtrace=1, 2026-08-18) are
        //   t=13,461 x=15,255 ON / t=14,225 x=15,765 OFF /
        //   t=18,259 x=22,275 ON / t=18,613 x=22,305 OFF /
        //   t=19,152 x=21,435 ON / t=19,577 x=21,465 OFF /
        //   t=20,110 x=22,325 ON / t=20,488 x=22,275 OFF
        // which match the x of the 10 exactly. During this the button is
        // COMPLETELY IGNORED (neither presses nor the cube's held re-jump
        // happen). No vertical overlap is needed (the trigger is at cy=1,995
        // and the player fired it at 1,890-1,920).
        // Values are GameOptionsSetting: On=1 (= disable controls) / Off=-1
        // (= restore) / Disabled=0 (= leave this item alone).
        // Flip flags and the "no effects / no touch" flags. A pad's orientation
        // does NOT show in rot: a pad attached to the ceiling still has rot=0,
        // and up/down is held by m_isFlipY. The blue pad uid1823 (11085,327) in
        // lv10 has a box that fully overlaps the player yet GD never fires it,
        // while the other 67 in the same level fire even on a |dy|=17.8 graze —
        // geometry cannot explain it, so emit the per-object differences.
        const int flx = obj->m_isFlipX ? 1 : 0;
        const int fly = obj->m_isFlipY ? 1 : 0;
        const int nofx = obj->m_hasNoEffects ? 1 : 0;
        const int notch = obj->m_isNoTouch ? 1 : 0;
        int optp1 = 0, optp2 = 0;
        if (obj->m_objectID == 2899) {
            auto* go = static_cast<GameOptionsTrigger*>(obj);
            optp1 = (int)go->m_disableP1Controls;
            optp2 = (int)go->m_disableP2Controls;
        }
        rf << obj->m_objectID << "," << (int)obj->m_objectType << ","
           << (r.origin.x + r.size.width * 0.5f) << ","
           << (r.origin.y + r.size.height * 0.5f) << ","
           << r.size.width << "," << r.size.height << ","
           << (int)obj->m_groupCount << "," << obj->m_uniqueID << ","
           << obj->m_objectRadius << "," << obj->getRotation() << ","
           << sy0 << "," << sy1 << "," << shz << "," << sdir << "," << sup
           << "," << obj->m_width << "," << obj->m_height
           << "," << tpy << "," << tpg << "," << tpix << "," << tpiy
           << "," << tw
           << "," << zm << "," << zdur << "," << zease << "," << zrate
           << "," << mvdir << "," << gnddir
           << "," << optp1 << "," << optp2
           << "," << flx << "," << fly << "," << nofx << "," << notch
           << "," << tpex << "," << tpey
           << "," << (obj->m_isDisabled ? 1 : 0)
           << "," << editvel << "," << vmodx << "," << vmody << "," << ovrvel
           << "," << forceOf(obj)
           << "\n";
    }
}

// Extract orb POIs etc. from the level's live memory (m_objects).
// No parser needed: the real coordinates, with rotation, scale and placement
// already resolved, can be used as-is.
inline void buildPois(GJBaseGameLayer* l) {
    g_pois.clear();
    g_hazards.clear();
    g_solids.clear();
    g_coins.clear();
    if (!l || !l->m_objects) return;
    for (auto* obj : CCArrayExt<GameObject*>(l->m_objects)) {
        if (!obj) continue;
        // Coin extraction (142=secret, 1329=user)
        if (obj->m_objectID == 142 || obj->m_objectID == 1329) {
            auto p = obj->getPosition();
            g_coins.push_back({p.x, p.y});
        }
        // Hazard/solid extraction (for the clearance table + diagnostics)
        if (obj->m_objectType == GameObjectType::Hazard) {
            auto p = obj->getPosition();
            g_hazards.push_back({p.x, p.y, obj->m_objectID});
        } else if (obj->m_objectType == GameObjectType::Solid) {
            auto p = obj->getPosition();
            g_solids.push_back({p.x, p.y, obj->m_objectID});
        }
        // Observation targets for moving gates (cfg `gatetrace`). A full scan
        // every tick is expensive, so at entry pick up only the grouped solids
        // inside the window.
        // Hazards are included too (2026-08-10). Moving spikes hurt more than
        // moving gates: what was killing at x=24,843 in lv20 was spike uid 13495
        // of group 37, which never showed up in this window while it looked at
        // Solid only.
        if (g_gateTrace && obj->m_groupCount > 0
            && (obj->m_objectType == GameObjectType::Solid
                || obj->m_objectType == GameObjectType::Hazard)) {
            auto p = obj->getPosition();
            if (p.x >= g_gateX0 && p.x <= g_gateX1) g_gateObjs.push_back(obj);
        }
        switch (obj->m_objectType) {
            case GameObjectType::YellowJumpRing:
            case GameObjectType::PinkJumpRing:
            case GameObjectType::GravityRing:
            case GameObjectType::GreenRing:
            case GameObjectType::RedJumpRing:
            case GameObjectType::CustomRing:
            case GameObjectType::DashRing:
            case GameObjectType::GravityDashRing:
            case GameObjectType::DropRing:
            case GameObjectType::SpiderOrb:
            case GameObjectType::TeleportOrb: {
                auto p = obj->getPosition();
                bool dash = (obj->m_objectType == GameObjectType::DashRing
                          || obj->m_objectType == GameObjectType::GravityDashRing);
                g_pois.push_back({p.x, p.y, obj->m_objectID, dash});
                break;
            }
            default:
                break;
        }
    }
    std::sort(g_pois.begin(), g_pois.end(),
        [](const Poi& a, const Poi& b) { return a.x < b.x; });
    auto byX = [](const Obj& a, const Obj& b) { return a.x < b.x; };
    std::sort(g_hazards.begin(), g_hazards.end(), byX);
    std::sort(g_solids.begin(), g_solids.end(), byX);
    std::sort(g_coins.begin(), g_coins.end(),
        [](const Poi& a, const Poi& b) { return a.x < b.x; });
    g_coinPickupTick.assign(g_coins.size(), -1);
    g_levelMaxX = 0;
    for (auto* obj : CCArrayExt<GameObject*>(l->m_objects))
        if (obj) g_levelMaxX = std::max(g_levelMaxX, obj->getPositionX());
    log::info("solver: levelMaxX={}", g_levelMaxX);
    g_goalX = (l->m_endPortal) ? l->m_endPortal->getPositionX() : 0.f;
    log::info("solver: goalX={} (endPortal), levelMaxX={}", g_goalX, g_levelMaxX);
    log::info("solver: {} orbs, {} hazards, {} solids, {} coins extracted",
        g_pois.size(), g_hazards.size(), g_solids.size(), g_coins.size());
    std::ofstream pf(std::string(DATA_DIR) + "/pois.txt", std::ios::trunc);
    for (auto& p : g_pois) pf << p.x << "," << p.y << "\n";
    // Dump of all objects (objects.txt): id, type, x, y (centre coordinates only)
    std::ofstream of(std::string(DATA_DIR) + "/objects.txt", std::ios::trunc);
    for (auto* obj : CCArrayExt<GameObject*>(l->m_objects)) {
        if (!obj) continue;
        auto p = obj->getPosition();
        of << obj->m_objectID << "," << (int)obj->m_objectType
           << "," << p.x << "," << p.y << "\n";
    }
    {
        std::ofstream rf(std::string(DATA_DIR) + "/objrects.txt", std::ios::trunc);
        writeObjRects(rf, l);
    }
    // ---- obb.txt: GD's own oriented hitboxes ---------------------------------
    // Activation of rotated objects can be explained neither by the bounding box
    // nor by a real-size SAT, so emit the 4 corners of GD's OBB2D as-is (this
    // settles the shape). Objects whose rotation is a multiple of 90 degrees
    // coincide with the bounding box and are not emitted
    {
        std::ofstream bf(std::string(DATA_DIR) + "/obb.txt", std::ios::trunc);
        // oob = m_shouldUseOuterOb. A column so that this is not guessed.
        // The hazard test is a dedicated loop in checkCollisions (win 0x2137f0),
        // two-stage: "passes the AABB, and if m_shouldUseOuterOb is set, OBB vs
        // OBB SAT". Objects without the flag kill on the AABB alone, so applying
        // an OBB there makes the model fail to kill. Which one it is can only be
        // learned from the real object.
        bf << "uid,id,type,cx,cy,rot,x0,y0,x1,y1,x2,y2,x3,y3,oob\n";
        long long n = 0;
        for (auto* obj : CCArrayExt<GameObject*>(l->m_objects)) {
            if (!obj) continue;
            const float rot = obj->getRotation();
            const float m = std::fabs(std::fmod(rot, 90.f));
            if (m < 0.5f || m > 89.5f) continue;
            auto* ob = obj->getOrientedBox();
            if (!ob) continue;
            auto p = obj->getPosition();
            bf << obj->m_uniqueID << "," << obj->m_objectID << ","
               << (int)obj->m_objectType << "," << p.x << "," << p.y << ","
               << rot;
            for (int i = 0; i < 4; ++i)
                bf << "," << ob->m_corners[i].x << "," << ob->m_corners[i].y;
            bf << "," << (obj->m_shouldUseOuterOb ? 1 : 0) << "\n";
            ++n;
        }
        log::info("obb: {} rotated objects", n);
    }
    // ---- forceblocks.txt: the force block's own settings ---------------------
    // ForceBlockGameObject (2069, and 3645 which is the same class) carries the
    // push as MEMBERS, and the model had none of them. It ended up with a table
    // keyed on m_uniqueID for the strength and two unrelated special cases for
    // the shape: a flat push for 2069, a linear ramp for 3645. Reading
    // ForceBlockGameObject::calculateForceToTarget (win 0x4c1ec0) they are one
    // formula:
    //     radius    = m_objectRadius > 0 ? m_objectRadius * max(m_scaleX, m_scaleY)
    //                                    : max(rect.w, rect.h) * k
    //     magnitude = m_forceRange ? lerp(m_minForce, m_maxForce, clamp01(t))
    //                              : m_force
    //     angle     = m_relativeForce ? atan2(target - self)   <- why it takes a
    //                                 : from the object's own rotation   target
    // So "the strength is per-instance and there is no scale law" was right about
    // the conclusion and wrong about the reason: it is m_force. And what looked
    // like a per-uid AND per-mode strength (uid17701 measured 0.172 as a ship and
    // 0.330 as a robot) is a magnitude that falls off with distance, sampled
    // wherever the player happened to pass -- 17701 is 154x117, so a ship and a
    // robot cross it at different heights and read different values off the same
    // ramp. The ramp the model already fitted for 3645 IS this lerp; the "no
    // position-linear ramp" concluded for 2069 was looked for across 10 px of a
    // 30 px box, which is where an m_forceRange=0 instance and a ramp too shallow
    // to see look the same.
    // NOTHING READS THIS YET. The dump is the measurement, and the values decide
    // what the formula should be before any of it reaches the model.
    {
        std::ofstream ff(std::string(DATA_DIR) + "/forceblocks.txt",
                         std::ios::trunc);
        ff << "uid,id,type,cx,cy,rot,scalex,scaley,radius,rectw,recth,"
              "force,minforce,maxforce,relative,range,forceid\n";
        long long n = 0;
        for (auto* obj : CCArrayExt<GameObject*>(l->m_objects)) {
            auto* fb = geode::cast::typeinfo_cast<ForceBlockGameObject*>(obj);
            if (!fb) continue;
            const auto p = fb->getPosition();
            const auto r = fb->getObjectRect();
            ff << fb->m_uniqueID << "," << fb->m_objectID << ","
               << (int)fb->m_objectType << "," << p.x << "," << p.y << ","
               << fb->getRotation() << "," << fb->m_scaleX << ","
               << fb->m_scaleY << "," << fb->m_objectRadius << ","
               << r.size.width << "," << r.size.height << ","
               << fb->m_force << "," << fb->m_minForce << ","
               << fb->m_maxForce << "," << (fb->m_relativeForce ? 1 : 0) << ","
               << (fb->m_forceRange ? 1 : 0) << "," << fb->m_forceID << "\n";
            ++n;
        }
        log::info("forceblocks: {} force blocks", n);
    }
    // ---- triggers.txt / objgroups.txt: the trigger mapping -------------------
    // grouptrace only emits "what was where and when". A touch-trigger gate does
    // not move until the player enters its box and so never shows in the
    // recording (a self-confirmation loop), so emit the relation "entering this
    // box releases the constraint on this set of uids".
    // The semantics are emitted too (added 2026-08-09). Previously the stance
    // was "do not implement easing/duration/move; leave verification of the
    // effect to the GD replay", but that was the real cause of the wall at
    // x=28,867 in lv20: of the 6 objects blocking the player's box, 5 had NOT A
    // SINGLE ROW in grouptrace, and the model, seeing the gate still closed,
    // wiped out the frontier in 7 ticks (maxAlive=12, capHits=0 = full
    // enumeration, all dead). GD flies 131px through the same spot.
    // The recording cannot be taken because of the self-confirmation loop: the
    // only way to record the gate opening is to reach it, and a model that sees
    // a closed gate cannot make a plan that reaches it.
    //
    // But 285 of the 292 triggers in lv20 are autonomous (touch=0/spawn=0) and
    // their effect is fully determined by the level data alone (of 62 Moves,
    // only 4 are touch). The culprit of the wall is one of them:
    //   uid=15728 id=901 cx=28545 target=40 dur=0.400185 oy=-105
    // The total displacement of uid 15929, the only one with a real recording,
    // matches at -104.975, and the 93 ticks it took roughly matches dur*240
    // (= GD 2.2 physics steps per second) = 96.
    //
    // Linear is NOT enough, though: matching the same recording against linear
    // interpolation deviates up to 12.4px midway, with residuals antisymmetric,
    // +11 at f=0.25 / -12 at f=0.75 = ease in-out. So the easing kind and rate
    // are emitted. lockToPlayer is emitted too (when set, the movement follows
    // the player, so it cannot be expressed as a static composition = a marker
    // to hand over to the recording side).
    // The activation condition was measured as "a plain overlap of the player's
    // hitbox and the trigger rect" (press / x crossing are irrelevant)
    {
        std::ofstream tf(std::string(DATA_DIR) + "/triggers.txt", std::ios::trunc);
        tf << "uid,id,cx,cy,w,h,target,center,touch,spawn,dur,ox,oy,"
              "ease,erate,lockx,locky,grav,gravmod\n";
        // uid → groups it belongs to. One object can belong to several groups,
        // so the mapping is many-to-many
        std::ofstream gf(std::string(DATA_DIR) + "/objgroups.txt", std::ios::trunc);
        gf << "uid,groups\n";
        long long nTrig = 0, nGrp = 0;
        for (auto* obj : CCArrayExt<GameObject*>(l->m_objects)) {
            if (!obj) continue;
            if (obj->m_groupCount > 0 && obj->m_groups) {
                gf << obj->m_uniqueID;
                const int ng = std::min((int)obj->m_groupCount, 10);
                for (int i = 0; i < ng; ++i) gf << " " << (int)(*obj->m_groups)[i];
                gf << "\n";
                ++nGrp;
            }
            auto* e = geode::cast::typeinfo_cast<EffectGameObject*>(obj);
            // TRIED (2026-08-13): "also emit triggers without a target". The
            // reading was that gravity-changing triggers act on the player, not
            // on a group, and so were dropped by target==0. Could NOT tell them
            // apart: `m_gravityValue` defaults to 1.0 on every EffectGameObject,
            // so it is no criterion, and the row count just swelled from 291 to
            // 1,395. lv22 has no id 2066 (the 2.2 gravity trigger) either.
            // Next: hook `GJBaseGameLayer::flipGravity` (win 0x212b00) and look
            // directly at "who called it" (that settled the rotation sign in one
            // shot).
            if (!e || e->m_targetGroupID == 0) continue;
            auto tr = obj->getObjectRect();
            tf << obj->m_uniqueID << "," << obj->m_objectID << ","
               << (tr.origin.x + tr.size.width * 0.5f) << ","
               << (tr.origin.y + tr.size.height * 0.5f) << ","
               << tr.size.width << "," << tr.size.height << ","
               << e->m_targetGroupID << "," << e->m_centerGroupID << ","
               << (e->m_isTouchTriggered ? 1 : 0) << ","
               << (e->m_isSpawnTriggered ? 1 : 0) << ","
               << e->m_duration << "," << e->m_moveOffset.x << ","
               << e->m_moveOffset.y << ","
               // easing is emitted as the raw integer (GD's EasingType: 0=None,
               // 1=EaseInOut, 2=EaseIn, 3=EaseOut, 4-6=Elastic*, 7-9=Bounce*,
               // 10-12=Exp*, 13-15=Sine*, 16-18=Back*). The curve is identified
               // by matching against real recordings, so pass it through
               // uninterpreted here.
               << (int)e->m_easingType << "," << e->m_easingRate << ","
               << (e->m_lockToPlayerX ? 1 : 0) << ","
               << (e->m_lockToPlayerY ? 1 : 0) << ","
               // Values of triggers that change the player's gravity (the
               // columns are at the end, so old readers keep working)
               << e->m_gravityValue << "," << e->m_gravityMod << "\n";
            ++nTrig;
        }
        log::info("triggers: {} triggers with a target, {} grouped objects",
                  nTrig, nGrp);
    }
}

// Measures which player a death in a dual section came from (destroyPlayer counts)
inline long long g_deathsP1 = 0, g_deathsP2 = 0, g_deathsOther = 0;

// Diagnostics (emitted by cmd `diag`). serve/replay have no onDeath path, so
// lastDeath/groundGap/injAtDeath stay 0
inline long long g_lastDeathTick = 0;
inline long long g_lastGroundGap = 0;
inline long long g_injThisAttempt = 0;   // number of injections applied in this attempt
inline long long g_injAtDeath = 0;       // number of injections at the most recent death

// ============================================================
// Observation of automatic checkpoint placement (cfg `ckpttrace=1`)
// Lines emitted (`ckpt:`): create/mark (checkpoint creation) / store (storage) /
//   flag (transitions of m_shouldTryPlacingCheckpoint / m_checkpointTimeout).
// Each one also lists the tick and the clock candidates (`lastCkptT` `totalT`)
// (so that whether placement is a function of tick or of wall time can be
// decided from the numbers)
// ============================================================
namespace ckpttrace {
inline bool g_on = false;
inline int g_lastTry = -1;      // previous-tick value of m_shouldTryPlacingCheckpoint
                                // (-1=uninitialised)
inline int g_lastTimeout = -1;  // previous-tick value of m_checkpointTimeout
inline long long g_flagLines = 0;        // flag lines emitted (counter against overflow)
constexpr long long kFlagCap = 5000;     // beyond this, flag lines are dropped (the cut-off
                                         // is stated explicitly)
}

} // namespace solver
