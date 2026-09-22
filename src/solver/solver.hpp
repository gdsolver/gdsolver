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
// uid = m_uniqueID. Only the coins fill it, and only so that GD's own pickup
// call -- which hands back the OBJECT, not an index -- can be matched to a row
// here. Position cannot do that job: a coin under group control is not where
// the dump says it is (lv21's third moves -120, lv22's third +180).
// hw = half the object rect's width, coins only: how far past the coin's centre the player can
// still be touching it, which is where cfg `coinroute` calls a coin missed.
struct Poi { float x; float y; int id = 0; bool dash = false; int uid = -1; float hw = 0.f; };
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
// [2026-08-31] MEASURED against GD at last, by replaying all 22 stored solutions
// with the pickupItem hook on (24 credited pickups). The half of the old comment
// that survives is the direction: not one pickup was claimed here that GD did not
// also credit. The rest of it was wrong in a way that matters.
//   * IT IS NOT A CIRCLE AND IT IS NOT 20. GD credits the coin the moment the
//     boxes overlap: the largest offsets seen at the crediting tick are
//     |dx| 34.80 (lv5) and |dy| 34.86 (lv9), each with the other axis near zero,
//     while lv6's is credited at (33.82, 32.13) AT ONCE -- 46.7 px away, which
//     no circle that also stops at 34.9 on an axis can reach.
//   * SO IT MISSES REAL PICKUPS: 4 of the 24 (lv4 #2, lv7 #0, lv10 #2, lv12 #1),
//     one in six, all of them beyond 20 px in one axis or both.
//   * AND IT IS LATE: 5 to 34 ticks behind GD, always behind, because it waits
//     for the centres to close to 20 after the boxes have already touched.
// THE RULE ITSELF was then measured on the coincal rig, which carries the table
// (py/mklevel.py): the player's axis-aligned box against THE COIN'S OWN ORIENTED
// box, the same 4-axis SAT the model already runs for portal firing. Touching
// counts. Unturned it collapses to one number per axis, confirmed eight ways --
// cube 35, mini 29, ball 35, robot 35, spider 33.5 (its half is 13.5 and it
// rests at y=103.5, which is what made the first spider run look like a
// different rule), and by scaling the COIN to move the boundary to 25, to 55,
// and to 55-in-x-25-in-y with a non-uniform scale.
// TURNED, THE BOUNDING BOX IS NOT IT: a coin at rot=45 is reported w=h=56.5686
// but its three stations are credited at |dx| 43.05 / 38.06 / 14.91 -- three
// different separating axes -- where the bounding box would say 43.28 for all
// three. Wrong by 28 px at the far station, and wrong in the direction that
// makes a route claim a coin GD never credits.
// So on the COIN's side there is no constant to carry at all, only the object:
// w0,h0, the per-axis scale and the rotation are all exported already. (64 of
// the 66 official coins are a plain 40x40 at rot 0 and the other two are scaled
// with rot 0, so the distinction costs the corpus nothing and is entirely about
// custom levels.)
// On the PLAYER's side it is a per-mode half, measured for all eight at both
// sizes: 15 for cube/ship/ball/ufo/robot/swing, 13.5 for the spider, and 5 for
// the WAVE -- which is not a box the model holds for anything else, and not the
// one the wave sits on either (it rests 10 px above the floor). Mini is x0.6
// throughout. The table with its brackets is in py/mklevel.py.
// COIN_RADIUS is left at 20 all the same. Nothing reads it but the HUD and this
// file's own bookkeeping, and the place to put the rule is the search, not a
// second copy of it here.
constexpr float COIN_RADIUS = 20.f;
// ...and GD's OWN verdict on the same coins, one entry per g_coins row, written
// by the pickupItem hook (hooks_gamelayer.cpp). -1 = GD never credited it.
//
// The two exist side by side because the sentence above -- "our detection means
// the real game surely picks it up too" -- is an ASSERTION, and until this
// column was added nothing had ever checked it against GD. A coin route is
// built on that claim, so it has to become a measurement first.
inline std::vector<long long> g_coinGdTick;
// pickupItem calls that matched no row in g_coins. GD collects unique ITEMS
// through the same call, so a non-zero count is information rather than an
// error: it counts the collectibles the coin list does not know about (lv21's
// ten id-1840 pickups are exactly this).
inline int g_coinGdUnmatched = 0;
// cfg `coinroute`: this attempt has already been told to end at a missed coin. GD does not
// always take the order -- a player in the "moving zombie" state (alive, advancing, killable
// by nothing; the loop's overlong guard is what ends those) refuses its own hazards' kills
// too -- and without a latch the request, and its result line, repeat every tick: 45,606
// lines on the 2026-09-20 run, 25,250 of them from one attempt.
inline bool g_coinMissFired = false;
// One `coinlive:` line per coin per attempt (hooks_gamelayer.cpp): where the
// object actually is when the player draws level with it.
inline std::vector<uint8_t> g_coinLiveSaid;
// ...and whether this level turns the gameplay frame (id 2900 anywhere). Where it
// does, travel is not one-way and a coin the player has passed can be reached
// again -- lv22 runs -x through the maze its third coin sits in -- so the missed
// verdict above has no bound to stand on and is withheld. The search's own miss
// prune is gated on the same fact (dp cli.hpp).
inline bool g_hasRotGameplay = false;
// GD's own item counters, as its updateCounters tells them (hooks_gamelayer.cpp).
// What a Count trigger compares against, and therefore what a re-anchored search
// has to be told (repair.hpp passes it as --itembase): the pickups behind the
// anchor are not in the window the model's own mask numbers. Per attempt.
inline std::map<int, int> g_itemCounts;
// Where a coin that only a counting tap's gate switches on is lost for good (dp
// Outcome::coinGates, refreshed after every solve): the Stop that shuts the
// tap's window, in GD's terms -- the rotation channel it sits on, its point and
// that channel's direction -- and the count the gate needs. Per level.
struct CoinGate {
    int uid = 0, chan = 0, dir = 4, item = 0, need = 0;
    double x = 0.0, y = 0.0;
    // ...and, when the coin lies ahead of the shut point on that channel's run,
    // the coin's far edge on it: passed without the coin, it is gone for good.
    bool miss = false;
    double mx = 0.0, my = 0.0;
};
inline std::vector<CoinGate> g_coinGates;
// Line budget for the two observation hooks, reset per SESSION rather than left
// as a function-local static: --one-session runs every level in one process, and
// a static that ran out on level 1 would leave later levels silently unobserved.
inline int g_coinLogLines = 0;
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
    // - touch / spawn: EffectGameObject's m_isTouchTriggered (property 11) and
    //   m_isSpawnTriggered (62), on every trigger row. THE X-CROSSING QUEUE DOES
    //   NOT ADMIT THEM: the effective gate GD applies is
    //   `m_isTrigger && [+0x4e8]==1 && !touch && !spawn` (isSpecialSpawnObject,
    //   which the model's queue was reading, is a constant false on all 37
    //   trigger classes). A queue that holds objects GD never enqueues fires
    //   things that never fire, which is part of why the parked entry-burst
    //   model came out too wide.
    // - chan: m_channelValue (170), the rotation channel a trigger switches to.
    //   Same queue question, and already dumped per-object in rotgameplay.txt --
    //   here so the ordinary trigger rows carry it too.
    // - axis: m_moveTargetMode (101), MoveTargetType {Both=0, XOnly=1, YOnly=2}:
    //   which axis a Static Camera (1914) actually touches.
    // - exstat: CameraTriggerGameObject::m_exitStatic (110).
    //   The 1914 rows are already in the dump as geometry; these two are the
    //   properties that say what they do, and the band's height depends on them.
          "editvel,vmodx,vmody,ovrvel,force,free,touch,spawn,chan,axis,exstat\n";
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
        // The trigger properties the x-crossing queue and the band need. Read
        // from the typed members like every other column here, not from a raw
        // property table: an EffectGameObject parses them at load and the
        // members are what the game itself then reads.
        int touch = 0, spawn = 0, chan = 0, axis = 0, exstat = 0, freem = 0;
        if (auto* e = geode::cast::typeinfo_cast<EffectGameObject*>(obj)) {
            touch = e->m_isTouchTriggered ? 1 : 0;
            spawn = e->m_isSpawnTriggered ? 1 : 0;
            chan = e->m_channelValue;
            axis = (int)e->m_moveTargetMode;
            // Free Mode, property 111. The name reads as a camera setting and
            // the field is one -- it frees the camera from the mode's band,
            // which is exactly why the band stops being written -- and the
            // bindings' own source carries `// property 111` directly above it,
            // which is a stronger identification than matching an offset.
            freem = e->m_cameraIsFreeMode ? 1 : 0;
        }
        if (auto* c = geode::cast::typeinfo_cast<CameraTriggerGameObject*>(obj))
            exstat = c->m_exitStatic ? 1 : 0;
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
           << "," << freem << "," << touch << "," << spawn << "," << chan
           << "," << axis << "," << exstat
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
    g_hasRotGameplay = false;
    if (!l || !l->m_objects) return;
    for (auto* obj : CCArrayExt<GameObject*>(l->m_objects)) {
        if (!obj) continue;
        // Coin extraction (142=secret, 1329=user)
        if (obj->m_objectID == 142 || obj->m_objectID == 1329) {
            auto p = obj->getPosition();
            // The rect is the bounding box, so for a turned coin this is an over-estimate --
            // the safe side for a "missed" verdict, which must never come early.
            const float hw = obj->getObjectRect().size.width * 0.5f;
            g_coins.push_back({p.x, p.y, obj->m_objectID, false, obj->m_uniqueID, hw});
            // ...and whether GD has it switched OFF at load. A disabled coin is
            // not credited however close the player passes (measured on the rig
            // calib_coingate), so a route planned for one that is disabled is a
            // route the game refuses without saying why -- and the model reads
            // no initial disabled state from the level at all.
            {
                char cb[160];
                snprintf(cb, sizeof(cb),
                         "coinstate: uid=%d id=%d at (%.1f,%.1f) disabled=%d tempdisabled=%d",
                         obj->m_uniqueID, obj->m_objectID, p.x, p.y,
                         obj->m_isGroupDisabled ? 1 : 0,
                         obj->m_isGroupDisabledTemp ? 1 : 0);
                writeResult(cb);
            }
        }
        if (obj->m_objectID == 2900) g_hasRotGameplay = true;   // see the flag
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
    // x, then y: the same order dp's loader gives L.coins, because cfg `coinroute` hands the
    // search a bitmask indexed by it (AnchorRow::coins). x alone would leave a tie unordered.
    std::sort(g_coins.begin(), g_coins.end(), [](const Poi& a, const Poi& b) {
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    });
    g_coinPickupTick.assign(g_coins.size(), -1);
    g_coinGdTick.assign(g_coins.size(), -1);
    g_coinGdUnmatched = 0;
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
              "ease,erate,lockx,locky,grav,gravmod,"
              // [2026-09-01] Appended, never inserted: a reader that stops at
              // gravmod keeps working, and the model does (nothing reads these
              // yet -- 008/009 do).
              // deg: the Rotate trigger's angle (id 1346). The one thing the
              // dump was missing to close the Rotate+Move composition; without
              // it the rotation has to be inferred from the recording.
              // t360/lockrot: [2026-09-01, second pass] deg ALONE IS NOT THE
              // ANGLE. m_rotationDegrees is only the remainder: whole turns
              // live in m_times360, and a Rotate that turns a group by two full
              // revolutions dumps deg=0. Measured on lv21 uid14789 -- deg=0,
              // while its group's uid15367 is recorded turning from +90 to
              // -629.997, i.e. -720 degrees. Eight of lv21's eighty Rotates are
              // this shape, and they are ALL THREE of the rotates whose group
              // also gets moved, so every subject that could test the
              // composition was uncomputable from the dump. The total is
              // m_times360 * 360 + m_rotationDegrees.
              // m_lockObjectRotation says whether each object's own rotation
              // follows the orbit, which decides its hitbox, not its centre.
              // ord/chan: the 2.2 trigger queue's ordering value and channel
              // (m_ordValue / m_channelValue). The queue's own build order is
              // NOT dumped -- setupLevelStart is not read yet -- so the
              // hypothesis 008/009 start from is "editor order = uid order",
              // and these two columns are what will confirm or refute it.
              // sord/sordd: the spawn ordering pair, same family.
              "deg,ord,chan,sord,sordd,t360,lockrot,"
              // [2026-09-04] Appended, never inserted, same rule as above: a
              // reader that stops at lockrot keeps working and every existing
              // dump stays valid.
              // sdelay: THE SPAWN TRIGGER'S DELAY (m_spawnTriggerDelay, key
              // 63). The `dur` column is m_duration, which a Spawn does not
              // use -- lv22's 141 spawns all read dur=0.5 because that is the
              // constructor's default, and the delay that actually schedules
              // the chain was not dumped at all. GD fires the chain
              // ceil(delay * 240) ticks later (0 = the same tick).
              // mvtgt/mvaxis/tmodctr/dirsnap/dirdist/dynmode: MOVE-TO. A Move
              // with m_useMoveTarget does not apply ox,oy at all -- it samples
              // getRealPosition(B) - getRealPosition(A) once, at the tick it
              // fires, so its offset depends on where the groups were by then.
              // lv22's uid18093 is one, and its displacement subtracts the
              // group's own accumulated travel (its source marker uid18094 is
              // itself a member of the group it moves). No static column can
              // express that; these say when to compute it.
              // silent: m_isSilent, which suppresses the trigger's effect.
              // What is still MISSING is the Stop trigger's mode (stop /
              // pause / resume). It is not a named member in the bindings --
              // the disassembly finds the flag on the COMMAND (cmd+0x72), not
              // on the trigger -- so lv22's twelve Stops are dumped without
              // saying which of the three they are.
              "sdelay,mvtgt,mvaxis,tmodctr,dirsnap,dirdist,dynmode,silent,togon,"
              // [2026-09-16] remap: a Spawn's (1268) group remap, property 442
              // (SpawnTriggerGameObject::m_remapObjects). A spawned trigger
              // acts on the REMAPPED group, not the one it names, so a chain
              // walked without this moves the template's group and misses the
              // real one. lv22's touch box uid17771 spawns group 493, whose
              // Moves target group 100 (six decorations) -- remapped to 508,
              // the block row the ball stands on. The level strings of lv1-22
              // hold 45 remaps, all in lv22. Written as the four raw ints of
              // each ChanceObject joined by ':' and entries by ';' ("-" when
              // empty), because which field holds the source and which the
              // target is checked against the level string, not assumed from
              // the member names.
              // [2026-09-20] THE ITEM COUNTERS, appended for the coin gates.
              // Four of the corpus' 66 coins are behind one: lv21's third waits
              // for ten id-1840 pickups (an Instant Count at target 10 spawns
              // the Move that brings it into reach), lv22's first waits for a
              // Count, and lv22's third for an Item Compare fed by a Touch. None
              // of that is visible in the columns above -- a Count trigger looks
              // like any other spawn with a target group -- so the search saw the
              // chain but never what opens it.
              //   item/item2: m_itemID / m_itemID2 (properties 80 / 95)
              //   count:      CountTriggerGameObject::m_pickupCount (77), the
              //               target the counter is compared against
              //   subcount:   m_subtractCount (78), a pickup that counts down
              //   actgrp:     m_activateGroup (56) for EVERY id, not just the
              //               Toggle the `togon` column above reports
              //   thold/ttog/tdual: the Touch trigger's mode (81 / 82 / 89),
              //               which decides whether a tap toggles or holds
              //   cmode:      CountTriggerGameObject::m_pickupTriggerMode (88),
              //               which of equals / larger / smaller the target is
              //               compared with. Exported rather than assumed: every
              //               Count in this corpus reads 0, and a rule written
              //               from that alone would be a guess about the other
              //               two.
              //   the ITEM COMPARE / ITEM EDIT block (id 3620 / 3619,
              //   ItemTriggerGameObject). lv22's third coin is behind one: a
              //   tap chain feeds it and it switches the coin's group on. Every
              //   field is exported rather than the two that look relevant,
              //   because which of them carries the comparison is exactly what
              //   is not known -- the names are the bindings' (properties
              //   476-486, 578/579), the meanings are to be read off the level
              //   this corpus already has.
              "remap,item,item2,count,subcount,actgrp,thold,ttog,tdual,cmode,"
              "i1mode,i2mode,tgtmode,mod1,mod2,res1,res2,res3,tol,rnd1,rnd2,"
              "sgn1,sgn2\n";
        // uid → groups it belongs to. One object can belong to several groups,
        // so the mapping is many-to-many
        std::ofstream gf(std::string(DATA_DIR) + "/objgroups.txt", std::ios::trunc);
        gf << "uid,groups\n";
        // ...and the PICKUPS, which are what a Count trigger is waiting for.
        // A collectible carries an item id but no target group, so the trigger
        // filter below drops it -- and it must not be written into triggers.txt
        // instead: dp's chain walk treats every uid in that file as a trigger
        // (the 2899/2900 pair cost lv22 a touch chain that way, see the note
        // there), so this family gets its own file as they did.
        std::ofstream itf(std::string(DATA_DIR) + "/items.txt", std::ios::trunc);
        itf << "uid,id,cx,cy,w,h,item,points,pickup,toggle,subcount\n";
        long long nItem = 0;
        // Is m_isGroupDisabled the byte toggleGroup writes (+0x28e, 0x223cbc)? The
        // bindings carry no offsets, and grouptrace's `on` column reads this field,
        // so whether `on` can witness a toggle depends on the answer. Read off a
        // live object rather than inferred from the member order. Logged once.
        for (auto* obj : CCArrayExt<GameObject*>(l->m_objects)) {
            if (!obj) continue;
            log::info("offsets: GameObject::m_isGroupDisabled=+{:#x} "
                      "m_isGroupDisabledTemp=+{:#x}",
                      (size_t)(reinterpret_cast<char const*>(&obj->m_isGroupDisabled)
                               - reinterpret_cast<char const*>(obj)),
                      (size_t)(reinterpret_cast<char const*>(&obj->m_isGroupDisabledTemp)
                               - reinterpret_cast<char const*>(obj)));
            break;
        }
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
            // TRIED AND REVERTED (2026-09-01): letting the rotation-gameplay
            // pair (2899/2900) through this filter, so that 009 could read
            // their channel and ordering. They have no target group, so all
            // thirty of lv22's were being dropped -- but putting them in
            // triggers.txt is NOT inert. dp's chain walks resolve a uid by
            // asking whether it is a KNOWN TRIGGER (trig.find) and otherwise
            // treat it as a moved object, so thirty new keys turned thirty
            // objects into triggers, one touch chain came out empty, and lv22
            // went 153 touch triggers to 152 -- quick_regress FAILED (tracked
            // 16,886 -> 16,666). The family gets its own file below instead,
            // which leaves this one byte-identical.
            if (e && e->m_targetGroupID == 0
                && (e->m_collectibleIsPickupItem || e->m_itemID != 0)) {
                auto ir = obj->getObjectRect();
                itf << obj->m_uniqueID << "," << obj->m_objectID << ","
                    << (ir.origin.x + ir.size.width * 0.5f) << ","
                    << (ir.origin.y + ir.size.height * 0.5f) << ","
                    << ir.size.width << "," << ir.size.height << ","
                    << e->m_itemID << "," << e->m_collectiblePoints << ","
                    << (e->m_collectibleIsPickupItem ? 1 : 0) << ","
                    << (e->m_collectibleIsToggleTrigger ? 1 : 0) << ","
                    << (e->m_subtractCount ? 1 : 0) << "\n";
                ++nItem;
            }
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
               << e->m_gravityValue << "," << e->m_gravityMod << ","
               << e->m_rotationDegrees << "," << e->m_ordValue << ","
               << e->m_channelValue << "," << e->m_spawnOrder << ","
               << (e->m_spawnOrdered ? 1 : 0) << ","
               << e->m_times360 << ","
               << (e->m_lockObjectRotation ? 1 : 0) << ","
               << e->m_spawnTriggerDelay << ","
               << (e->m_useMoveTarget ? 1 : 0) << ","
               << (int)e->m_moveTargetMode << ","
               << e->m_targetModCenterID << ","
               << (e->m_isDirectionFollowSnap360 ? 1 : 0) << ","
               << e->m_directionModeDistance << ","
               << (e->m_isDynamicMode ? 1 : 0) << ","
               << (e->m_isSilent ? 1 : 0) << ","
               // togon: a Toggle's (1049) m_activateGroup -- 1 turns its target
               // group ON, 0 turns it OFF; -1 on every other id. dp needs it to
               // tell which way a touched toggle moves a group's disabled state
               // (toggleGroup 0x223bc0: +0x28e = counter < 0), because a group
               // switched off makes the rotation queue consume its 2900s without
               // firing them (checkSpawnObjects 0x21aad8). Appended at the end:
               // loadTrigRows reads the first 26 columns by position and nothing
               // else parses this file.
               << (obj->m_objectID == 1049 ? (e->m_activateGroup ? 1 : 0) : -1) << ",";
            {
                std::string rm;
                if (auto* sp = geode::cast::typeinfo_cast<SpawnTriggerGameObject*>(obj)) {
                    for (auto const& c : sp->m_remapObjects) {
                        if (!rm.empty()) rm += ";";
                        rm += std::to_string(c.m_groupID) + ":"
                            + std::to_string(c.m_oldGroupID) + ":"
                            + std::to_string(c.m_chance) + ":"
                            + std::to_string(c.m_unk00c);
                    }
                }
                tf << (rm.empty() ? std::string("-") : rm) << ",";
            }
            {
                // The item columns (see the header). m_pickupCount lives on the
                // Count subclass only, so it is -1 where the object is not one --
                // never 0, which is a legitimate target.
                int cnt = -1, cmode = -1;
                if (auto* c = geode::cast::typeinfo_cast<CountTriggerGameObject*>(obj)) {
                    cnt = c->m_pickupCount;
                    cmode = c->m_pickupTriggerMode;
                }
                tf << e->m_itemID << "," << e->m_itemID2 << "," << cnt << ","
                   << (e->m_subtractCount ? 1 : 0) << ","
                   << (e->m_activateGroup ? 1 : 0) << ","
                   << (e->m_touchHoldMode ? 1 : 0) << ","
                   << (int)e->m_touchToggleMode << ","
                   << (e->m_isDualMode ? 1 : 0) << "," << cmode;
                if (auto* it = geode::cast::typeinfo_cast<ItemTriggerGameObject*>(obj))
                    tf << "," << it->m_item1Mode << "," << it->m_item2Mode << ","
                       << it->m_targetItemMode << "," << it->m_mod1 << ","
                       << it->m_mod2 << "," << it->m_resultType1 << ","
                       << it->m_resultType2 << "," << it->m_resultType3 << ","
                       << it->m_tolerance << "," << it->m_roundType1 << ","
                       << it->m_roundType2 << "," << it->m_signType1 << ","
                       << it->m_signType2 << "\n";
                else
                    tf << ",-1,-1,-1,0,0,-1,-1,-1,0,-1,-1,-1,-1\n";
            }
            ++nTrig;
        }
        log::info("triggers: {} triggers with a target, {} grouped objects, "
                  "{} pickups", nTrig, nGrp, nItem);
    }
    // ---- rotgameplay.txt: the 2.2 trigger queue's inputs ---------------------
    // 2900 (rotate gameplay) and 2899 carry no target group, so triggers.txt
    // cannot hold them (see the note at that filter -- putting them there
    // changes how dp's chain walks resolve a uid, and it cost lv22 a touch
    // trigger). A file of their own, the way levelsettings.txt is.
    //
    // What the queue needs (checkSpawnObjects, 0x21a8f0): the ACTIVE CHANNEL is
    // layer+0x33c and only rotateGameplay writes it, so each 2900 carries the
    // channel it switches to; each tick the game walks that one channel's array
    // from a per-channel counter and fires everything the player has passed.
    // chan is m_channelValue and ord is m_ordValue; the array's own build order
    // is NOT dumped (setupLevelStart is unread), which is the open question the
    // model has to answer from uid order or from a measurement.
    //
    // spx = m_spawnXPosition, dumped to settle whether the fire point is a
    // stored value or the object's own position: measured 0.000 on all thirty
    // of lv22's, while their cx spans 2,143..22,603, so it is the position.
    {
        std::ofstream rf(std::string(DATA_DIR) + "/rotgameplay.txt",
                         std::ios::trunc);
        // swarm/chanOnly/swch: the flag that gates a channel switch, the
        // "channel only" flag, and THE CHANNEL A 2900 SWITCHES TO.
        //
        // [2026-09-10] These were read raw at +0x754/+0x755/+0x758 because the
        // comment here said "this bindings version has neither m_changeChannel
        // nor m_targetChannelID". That was true of EffectGameObject, which is
        // what the cast below asks for, and FALSE of the class a 2900 actually
        // is: RotateGameplayGameObject declares m_changeChannel, m_channelOnly
        // and m_targetChannelID by name. The fields were missing from the base,
        // not from the bindings.
        //
        // Reading them off the base had one consequence, and it is visible in
        // the file: a 2899 is a GameOptionsTrigger, a DIFFERENT subclass that
        // has none of the three, so those offsets land outside it. Five of
        // lv22's ten 2899 rows came out 255/255/-1 and the other five came out
        // 1/0/1 -- the same three values on five objects sitting on five
        // different channels, which is what reading somebody else's memory
        // looks like rather than a field.
        //
        // Nothing downstream consumed it: every consumer is gated on either
        // `id == 2900` (step.hpp:390) or `rotIdx >= 0` (step.hpp:398,
        // cli.hpp:1825, frames.hpp:386), and rotIdx is -1 for a 2899 because
        // only id 2900 enters g_rotTrig (level_loader.hpp:1038). So this is
        // hygiene, not a behaviour fix -- but the numbers were in a file people
        // read, and one reader did take them for values.
        //
        // Asking for the derived type instead makes the distinction structural:
        // the cast fails for a 2899 and the row says "-", which is neither 0 nor
        // 255 and cannot be mistaken for either.
        rf << "uid,id,cx,cy,chan,ord,sord,sordd,spx,target,"
              "chanChanged,swarm,chanOnly,swch\n";
        long long n = 0;
        for (auto* obj : CCArrayExt<GameObject*>(l->m_objects)) {
            if (!obj || (obj->m_objectID != 2900 && obj->m_objectID != 2899))
                continue;
            auto* e = geode::cast::typeinfo_cast<EffectGameObject*>(obj);
            if (!e) continue;
            auto tr = obj->getObjectRect();
            rf << obj->m_uniqueID << "," << obj->m_objectID << ","
               << (tr.origin.x + tr.size.width * 0.5f) << ","
               << (tr.origin.y + tr.size.height * 0.5f) << ","
               << e->m_channelValue << "," << e->m_ordValue << ","
               << e->m_spawnOrder << "," << (e->m_spawnOrdered ? 1 : 0) << ","
               << e->m_spawnXPosition << "," << e->m_targetGroupID << ","
               << (e->m_channelChanged ? 1 : 0) << ",";
            if (auto* r = geode::cast::typeinfo_cast<RotateGameplayGameObject*>(obj))
                rf << (r->m_changeChannel ? 1 : 0) << ","
                   << (r->m_channelOnly ? 1 : 0) << ","
                   << r->m_targetChannelID << "\n";
            else
                // A 2899 has no such fields. "-" rather than a number: a 0 here
                // would read as "does not switch the channel", which is a claim
                // about a field that does not exist on this object.
                rf << "-,-,-\n";
            ++n;
        }
        if (n) log::info("rotgameplay: {} rotation-gameplay objects", n);
    }
    // ---- levelsettings.txt: the level's own compatibility flags -------------
    // Their own file rather than a column: they are ONE value per level, and
    // objrects is per object. Nothing reads it yet.
    //
    // kA39 = m_fixRadiusCollision is the one with a known consequence: it
    // chooses which of the two saw-collision branches GD takes (survey C1), and
    // every official level is expected to leave it and its siblings at 0. The
    // point of writing them down is the day one of them is not -- the same
    // reason the dump carries an out-of-bounds column nobody reads either.
    if (auto* ls = l->m_levelSettings) {
        std::ofstream sf(std::string(DATA_DIR) + "/levelsettings.txt",
                         std::ios::trunc);
        sf << "fixRadiusCollision=" << (ls->m_fixRadiusCollision ? 1 : 0)
           << "\nfixGravityBug=" << (ls->m_fixGravityBug ? 1 : 0)
           << "\nfixNegativeScale=" << (ls->m_fixNegativeScale ? 1 : 0)
           << "\nfixRobotJump=" << (ls->m_fixRobotJump ? 1 : 0)
           << "\nenableImpulseFix=" << (ls->m_enableImpulseFix ? 1 : 0)
           << "\ndynamicLevelHeight=" << (ls->m_dynamicLevelHeight ? 1 : 0)
           << "\nplatformerMode=" << (ls->m_platformerMode ? 1 : 0)
           << "\nreverseGameplay=" << (ls->m_reverseGameplay ? 1 : 0)
           << "\n";
        log::info("levelsettings: fixRadiusCollision={} platformer={} "
                  "reverse={}", (int)ls->m_fixRadiusCollision,
                  (int)ls->m_platformerMode, (int)ls->m_reverseGameplay);
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
