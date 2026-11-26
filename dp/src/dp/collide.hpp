#pragma once
// A port of GD 2.209's collision pass (22091) as a pure function of the player's bytes and the level's objects.
// Read from the decompile (notes/collision-state-table-2091-1027.md): GJBaseGameLayer::checkCollisions 0x31aae0, collisionCheckObjects 0x31bce0,
// PlayerObject::collidedWithObject(Internal) 0x4efc90/0x4efd50, collidedWithSlopeInternal 0x4edb90, preSlopeCollision 0x4ed660,
// handleRotatedCollisionInternal 0x4ed4c0, hitGround 0x4fa250, hitGroundNoJump 0x4fa1f0, updateCollide(Left/Right) 0x4f21f0.., postCollision 0x4eb830,
// didHitHead 0x4f1e30, bumpPlayer/propellPlayer/boostPlayer 0x4fdae0/0x4fdc90/0x4fe2f0, checkSnapJumpToObject 0x4f1eb0.
//
// Kept independent of the search: tools/collide_test.cpp compiles it alone in seconds and checks it against the game's own
// entry / exit bytes (mod cfg ctraw=t0,t1 -> ctraw.csv) before anything in the model depends on it. The player is a byte image of [0, 0xc80) of the
// PlayerObject (the offsets are the decompile's, i.e. 2.209 Windows), written in place; objects are the fields the pass reads.
//
// Float / double follow the decompile: rects and positions are float, the contact lines and velocities double.
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include "dp/flat_set.hpp"
#include <vector>
#include "dp/game_event.hpp"
#define GDCOL_DIED(c) do { (c).died = true; if (!(c).why) { (c).why = __LINE__; (c).whyUid = (c).curUid; } } while (0)
namespace dp {
namespace gdcol {

// ----------------------------------------------------------------------------------------------------------------------------------------
// the player's bytes
struct Mem {
    std::vector<uint8_t> b;
    explicit Mem(size_t n = 0xc80) : b(n, 0) {}
    template <class T> T get(size_t o) const { T v; std::memcpy(&v, b.data() + o, sizeof(T)); return v; }
    template <class T> void put(size_t o, T v) { std::memcpy(b.data() + o, &v, sizeof(T)); }
    uint8_t u8(size_t o) const { return b[o]; }
    void u8(size_t o, uint8_t v) { b[o] = v; }
    float f32(size_t o) const { return get<float>(o); }
    void f32(size_t o, float v) { put<float>(o, v); }
    double f64(size_t o) const { return get<double>(o); }
    void f64(size_t o, double v) { put<double>(o, v); }
    int32_t i32(size_t o) const { return get<int32_t>(o); }
    void i32(size_t o, int32_t v) { put<int32_t>(o, v); }
};

// PlayerObject offsets (2.209 Windows; the decompile's `param_1[n]` is 8n)
namespace po {
enum : size_t {
    PosX = 0x4c, PosY = 0x50,                // CCNode::m_obPosition
    PrevX = 0x4d0, PrevY = 0x4d4,            // the position at the start of the update
    CollisionBottomUid = 0x5d0, CollisionTopUid = 0x5d4, CollisionLeftUid = 0x5d8, CollisionRightUid = 0x5dc,   // m_lastCollision*
    PrevBottomUid = 0x5e0, PrevTopUid = 0x5e4,
    SlopeObj = 0x5e8, SlopeObjB = 0x5f0, Collided = 0x600, GroundObj = 0x608, LeftObj = 0x610, RightObj = 0x618,
    Slope5fc = 0x5fc, NodeFlag671 = 0x671, Cf = 0x678, Cd0 = 0x680, Slope694 = 0x694,
    SnappedTo = 0x890, SnapDist = 0x840,
    CeilTopY = 0x960, FloorBottomY = 0x968, LeftX = 0x970, RightX = 0x978,
    Vy = 0x9a8, VyPrev = 0x9b0,
    OnSlope = 0x9b8, WasOnSlope = 0x9b9, OnSlopeUnder = 0x9c0,
    ModeShip = 0x9c1, ModeUfo = 0x9c2, ModeBall = 0x9c3, ModeWave = 0x9c4, ModeRobot = 0x9c5, ModeSpider = 0x9c6, Upside = 0x9c7, ModeSwing = 0x9cc,
    Sideways = 0x9cb, Leftward = 0x9ca, G = 0x9c9, G2 = 0xa14, Boost = 0xa24,
    Time = 0xaa8, Platformer = 0xb78,
};
}  // namespace po

// ----------------------------------------------------------------------------------------------------------------------------------------

inline constexpr int kTpRedirectDash = 8, kTpDashUsesEntryAngle = 16;   // objrects tpfm

// a GD object as the pass reads it
struct GObj {
    int uid = -1;
    int type = 0;            // GameObjectType (+0x3a0): 0 solid, 2 hazard, 0x19 slope, ...
    float rx = 0, ry = 0, rw = 0, rh = 0;   // getObjectRect(): origin and size
    float cx = 0, cy = 0;                   // the object's centre (vtable +0xc8 / +0x4b8)
    double realDx = 0, realDy = 0; int spiderSecX = -2, spiderSecY = -2, spiderFixedSlot = -1;   // logical position relative to the float rect centre; spider cell: -2 = not captured, -1 = not filed
    float prevCx = 0, prevCy = 0;           // +0x4d0 / +0x4d4: where it was last tick
    float radius = 0;                       // +0x38c
    float scaleX = 1, scaleY = 1;
    int slopeDir = 0; float spiderSlopeBoxDx = 0, spiderSlopeBoxDy = 0;   // +0x444 m_slopeDirection; cached getBoxOffset for the spider's temporary rotation
    bool slopeUphill = false;               // +0x440 m_slopeUphill
    bool slopeHazard = false; bool spiderSlopeLockX = false, spiderSlopeCachedOffset = false;   // m_slopeIsHazard; native turn's X gate and known box offset
    bool inactive = false;                  // +0x28e
    bool farHold = false;                   // --shadowspiderrect: `inactive` only because a Follow Player Y object was sent away from the pass's cells (its place is real): the spider's rect queries still find it
    bool activated = false;                 // +0x3d2: used object or destroyed breakable
    bool passable = false;                  // +0x515
    bool oriented = false; int8_t spiderOuterOb = -1;   // rotation estimate; spiderOuterOb is the captured +0x2e8 latch, or -1 when unknown
    bool b517 = false, b518 = false, b519 = false, b51a = false, b51b = false, b51c = false, b51d = false, b51e = false;   // the flag bytes at +0x517.. the pass reads
    int groupCount = 0;                     // +0x498 m_groupCount: checkCollisions registers a platform as the rider's +0x600 only if it is in a group (0 = unknown / none: the registration is skipped)
    bool dynamic = false;                   // a moving object: its rect is re-read every tick
    bool flipY = false;                      // the object's vertical flip (GameObject vtable +0x500)
    bool reverse = false;                    // +0x704
    bool multiAct = false;                   // the editor's Multi Activate: a portal / pad / orb that may fire again
    bool claim = false;                      // a spawn / toggle orb's Claim Touch (RingObject::m_claimTouch, +0x740, property 445; objrects `claim`): pushButton fires it alone and takes the hold, a touch does not fire it
    bool extCol = false, spiderUiDetached = false;   // Extended Collision uses a flat list; addUIObject removes the object from the gameplay sections
    // a teleport portal (type 28), from the objrects columns: the exit and the keeps (GJBaseGameLayer::teleportPlayer 0x315f20)
    float cx0 = 0, cy0 = 0;                 // the box centre at load, before any group motion
    float tpEx = 0, tpEy = 0, tpY = 0, tpRX = 0, tpRY = 0;   // the linked exit's position, an id-747 portal's target y, the portal's real position
    int tpGrav = 0, tpFm = 0;                                  // +0x774 (1 normal, 2 inverted, 3 toggle); the force flag (+0x759)
    float tpF = 0;
    float tpAng = 0, tpRmod = 0, tpRmin = 0, tpRmax = 0;      // a redirected force (tpFm bit 2): the direction (degrees), the factor, the floor and the ceiling of the speed
    bool tpKeepX = false, tpKeepY = false, tpSave = false;     // +0x771, +0x772, +0x770
    // a force block (ids 2069 / 3645: ForceBlockGameObject; forceblocks.txt)
    float fbForce = 0, fbMin = 0, fbMax = 0;
    bool fbRelative = false, fbRange = false, flipX = false;
    int fbId = 0;
    bool touchTrig = false; int rotMv = 0, rotGnd = 0; bool rotEdit = false, rotOvr = false, rotDont = false; float rotVmx = 0, rotVmy = 0;   // touchTrig: EffectGameObject::m_isTouchTriggered (objrects 'touch'): 1 on every ring of the 64 customs, so it separates nothing; kept as data only. rot*: a Rotate Gameplay trigger (2900) as GJBaseGameLayer 0x31f980 hands it to the player's rotateGameplay: +0x740 mvdir, +0x744 gnddir, +0x748 editvel, +0x749 ovrvel, +0x74c vmodx, +0x750 vmody, +0x75d dontslide (on this line: the __LINE__ of the kills below stay)

    float bw = 0, bh = 0;                    // the box's own size when known (the unrotated hitbox); 0 = solve it from the rect of the rotation
    bool hasGc = false;                      // GD's own four corners of the oriented box (obb.txt x0 y0 .. x3 y3), taken at the centre (gcx, gcy): used as they are, shifted by the object's move since
    float gc[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    float gcx = 0, gcy = 0;
    float gscale = 1.0f;                     // the uniform scale applied to gc about its centre (self_test: from the game's rect size; 1 = as taken)
    float rot = 0, w0 = 0, h0 = 0;           // the rotation in degrees and the unrotated hitbox size (objrects.txt)
    float slopeAngle = 0;                   // atan(h / w) of the slope's rect, radians
    double sy0 = 0, sy1 = 0;                // the slope line's y at the rect's left and right edge (objrects.txt)
    int objectId = 0;
    bool operator<(const GObj& o) const { return uid < o.uid; }
    double realX() const { return (double)cx + realDx; } double realY() const { return (double)cy + realDy; }
};
struct Layer {
    bool freeMode = false;      // +0x311
    bool dual = false;          // +0x422
    bool f317c = false, f317e = false;
    float scaleX = 0, scaleY = 0, ceilWord = 0;   // +0x37a0, +0x37a4, +0x37a8
    float w764 = 0;
    float maxPortalY = 0, minPortalY = 0;
    float speedA = 0, speedB = 0;                 // +0x1a8, +0x1ac
    bool betweenSteps = false;                    // layer+0x71e*8 (postCollision's param_3)
    bool f1cf = false;                            // layer+0xdc8 -> +0x1cf: the circle test's version (not in the image yet; 0)
    float flipAmt = 0;                            // +0x41c: the mirror portal's tween (neither 0 nor 1 = the level is flipping: PlayerObject::levelFlipping with +0x7ec set)
    bool dynHeight = false;                       // the level keeps dynamic level height: the layer's y section scale is 0.01 (else 0, one row of cells); the pass's calls are the section cells
};

// ----------------------------------------------------------------------------------------------------------------------------------------
struct TpEvt { int uid; float xBefore, xAfter; };
// --shadowfireorder (gd-04, P9): the rings a body has fired, newest first. noteFired puts a fire at the front (a ring that fires again on the next tick is the front already) and keeps kFireOrder.
inline constexpr int kFireOrder = 5;
inline void noteFired(std::vector<int>* v, int uid) {
    if (!v || uid < 0) return;
    if (!v->empty() && v->front() == uid) return;
    v->insert(v->begin(), uid);
    if ((int)v->size() > kFireOrder) v->resize((size_t)kFireOrder);
}
struct Ctx {
    Mem& P;
    Layer L;
    std::vector<GObj>* objs;          // every object that can matter (the caller picks the neighbourhood)
    bool died = false;                // destroyPlayer was called
    std::vector<int> touched;         // the map at +0x6e8 / list at +0xde: uids this tick's slope code registered
    int why = 0;                      // the line of the first kill, for the test
    int curUid = -1, whyUid = -1;     // the object being processed, and the one the first kill was under (the trace prints both)
    int unsupported = 0;              // calls of code that is not ported (flipGravity, the OBB test, ...)
    std::vector<TpEvt> tpLog;   // the teleports this pass made (teleportPortal), in order -- the new model's tpBack reads them
    std::map<int, int> unsupportedBy; // ... by object type (and 1000 + n for the code paths without an object)
    std::set<int> forceSeen;          // the map at +0xba0: force ids already added this step (the update empties it)
    std::set<int> topDict, botDict;   // the CCDictionaries at +0x5b0 / +0x5b8: uids the head / the foot touched this tick (the layer empties them every step)
    // The once-per-contact gate (FUN_14031ef50) as a rising edge: an object that was already overlapped on the last tick does not fire again.
    // `gateIn` is what the last tick left (null = nothing known: every portal / pad fires), `gateNow` is what this tick overlaps.
    const FlatSet<int>* gateIn = nullptr;
    FlatSet<int> gateNow;
    const FlatSet<int>* everIn = nullptr; bool (*everRule)(const void*, int) = nullptr; const void* everRuleCtx = nullptr; const FlatSet<int>* everBase = nullptr;     // (everBase: the anchor payload's `ever`, ever_payload.hpp: held by GD at the anchor, seen for certain) every object the gate has seen since the attempt began (FUN_14031ef50's map: nothing sweeps it); everRule (set: leveldp's shadow_step, shadow_state.hpp SlWorld::lazyEver) answers ever's rule part, which everIn then does not hold
    FlatSet<int> everNow;
    bool bornFired = false;           // a dual portal (type 23) fired in this pass: the first body's position and velocity at that moment are the second body's birth (toggleDualMode copies them)
    float bornY = 0; double bornVy = 0;
    std::vector<int> ringsTouched;    // playerTouchedRing's registrations this tick (the array at +0xa40)
    // The press registers the caller knows (H, S, Hm, Sm are in the image): with them a ring the pass touches is judged by ringJump's own gate and its byte writes are made; without
    // them (hsKnown false) a ring that could fire is only counted as not ported. `usedRings` is the set at +0x850 (the rings this press has already jumped off; a press empties it).
    bool hsKnown = false;
    bool (*ringJump)(Ctx&, GObj*) = nullptr;   // PlayerObject::ringJump 0x4f6f50 for a ring the pass touches (buttons.hpp's, set by selfshadow.hpp); empty = the short form below (ringJumpWrites)
    void (*slopeRotation)(Ctx&, float) = nullptr;   // PlayerObject::updateSlopeRotation 0x4eef20 (upd.hpp sets it): the sprite's turn to the slope at the end of collidedWithSlopeInternal; empty = not written
    void (*stopDash)(Ctx&) = nullptr;     // PlayerObject::stopDashing (upd.hpp, set by selfshadow.hpp): the "Special" object 1829 ends a dash; empty = counted as not ported
    float* dashRot = nullptr;             // signed child-sprite angle; stopDashing gives it to a cube; null = 0
    void (*restartDashSpin)(Ctx&) = nullptr;   // restart the dash's child rotation action
    bool (*spiderJump)(Ctx&) = nullptr;   // PlayerObject::spiderTestJump (spider.hpp sets it): the spider pad (type 0x2c) calls it; empty = counted as not ported
    bool p22 = true;                  // the level's player 1 Enable22Changes (a spawn orb clears the armed press only then)
    FlatSet<int>* usedRings = nullptr;
    std::vector<int>* fireOrder = nullptr; std::vector<gdev::EvTok>* gameEvents = nullptr;   // cumulative ring history (shadow_lite.hpp), and the optional ordered event log for this step
    bool everGuess = false;                  // --shadowrev: a Leftward pass overlapped an object the gate calls seen only by the seeding rule (everRule), not by the objects the player was known to touch
    std::vector<std::pair<int,int>> fired;   // (type, uid) of the non-solid objects that overlapped this tick, with 1000 added to the type when the gate let them fire (the test's --verbose prints it)
    explicit Ctx(Mem& p) : P(p) {}   std::vector<GObj*>* actLog = nullptr;   struct DualHooks* dualH = nullptr;   bool rotApply = false; bool primeWords = false; const std::vector<int>* rotSpent = nullptr; std::vector<int> rotFired;   // rotApply (--shadowrotapply): a touch Rotate Gameplay (2900) the pass overlaps turns the body at once (playerRotateGameplay, the end of this file); rotSpent = the uids the caller knows have fired, rotFired = the ones this pass turned on.   dualH: --shadowdual, the other player (selfshadow_dual.hpp; null = player 2 is not modelled and its calls are counted). non-null: buttons.hpp ringJump appends every object it marks activated (selfshadow.hpp writeBack; on this line so that the __LINE__ of the kills below, which self_test prints, stay; a member added here must be added to selfshadow.hpp ctxReset)
    GObj* byUidScan(int uid) { for (auto& o : *objs) if (o.uid == uid) return &o; return nullptr; }   static bool byUidCheck() { static const bool on = std::getenv("ST_BYUIDCHECK") != nullptr; return on; }
    // byUid's index (speed only; the answer is the scan's; this block keeps its five lines: GDCOL_DIED prints this file's line numbers into the trace). The owner of `objs` may say where the object of a uid stands: lkIndex(lkWorld, uid) is its index in the world the copies were taken from (-1: none, or a uid two objects of the world carry), and for the list `lkObjs` the object of world index i stands at lkPos[i] where lkStamp[i] == lkGen (lkPos null: the list IS that world).
    // The place is believed only where the object there carries the uid; anything else (no index, another list in `objs`, an object appended outside the indexed part, a uid not in the list) is answered by the scan. ST_BYUIDCHECK=1 runs the scan beside every indexed answer and aborts on a difference.
    const void* lkWorld = nullptr; long long (*lkIndex)(const void*, int) = nullptr; const std::vector<GObj>* lkObjs = nullptr; const uint32_t* lkStamp = nullptr; const uint32_t* lkPos = nullptr; uint32_t lkGen = 0;
    GObj* byUid(int uid) { if (uid < 0 || !objs) return nullptr; if (lkIndex && objs == lkObjs) { long long p = lkIndex(lkWorld, uid); if (p >= 0 && lkPos) p = lkStamp[p] == lkGen ? (long long)lkPos[p] : -1; if (p >= 0 && (size_t)p < objs->size() && (*objs)[(size_t)p].uid == uid) { GObj* r = &(*objs)[(size_t)p]; if (byUidCheck() && byUidScan(uid) != r) { std::fprintf(stderr, "ST_BYUIDCHECK: the indexed object of uid %d is not the scan's\n", uid); std::abort(); } return r; } } return byUidScan(uid); }
};

inline bool mode(const Mem& P, size_t m) { return P.u8(m) != 0; }
inline double dirOf(const Mem& P) { return P.u8(po::Upside) ? -1.0 : 1.0; }

// PlayerObject::setYVelocity 0x4e6f50: three decimals
inline void setYVelocity(Mem& P, double v) {
    const double ip = (double)(int)v;
    if (v != ip) P.f64(po::Vy, std::round((v - ip) * 1000.0) / 1000.0 + ip);
    else P.f64(po::Vy, v);
}

// ST_SLOPEDBG=x0,x1 (stderr or ST_SLOPEDBG_FILE): what the slope contact decides for a player whose x is in the range (diagnosis only)
inline bool slopeDbgOn(const Mem& P) {
    static double lo = 1.0, hi = 0.0;
    static const bool enabled = [] { if (const char* e = std::getenv("ST_SLOPEDBG")) { std::sscanf(e, "%lf,%lf", &lo, &hi); return true; } return false; }();
    if (!enabled) return false;
    const double x = (double)P.f32(po::PosX);
    return x >= lo && x <= hi;
}
inline FILE* slopeDbgOut() {
    static FILE* f = [] { const char* p = std::getenv("ST_SLOPEDBG_FILE"); FILE* h = p ? std::fopen(p, "a") : nullptr; if (h) std::setvbuf(h, nullptr, _IONBF, 0); return h ? h : stderr; }();
    return f;
}
inline void setPosition(Mem& P, float x, float y, int line = __builtin_LINE()) {
    if (slopeDbgOn(P)) std::fprintf(slopeDbgOut(), "slopedbg setPosition (collide.hpp or caller line %d): x %.4f -> %.4f, y %.4f -> %.4f\n", line, (double)P.f32(po::PosX), (double)x, (double)P.f32(po::PosY), (double)y);
    P.f32(po::PosX, x); P.f32(po::PosY, y);
    P.u8(0x368, 1);                                     // GameObject::setPosition marks the rect cache dirty
    P.u8(0x369, 1);                                     // ... and the oriented box (the player's setPosition, vtable +0xb8, marks both)
}
// GameObject::getObjectRect (2.2091 0x29484a..0x294938): use absolute scale for mirrored-by-scale objects, then swap the scaled sides at right angles.
inline std::pair<float, float> plainPlayerRectSize(const Mem& P, float fx, float fy) {
    const float sx = P.f32(0x488), sy = P.f32(0x48c); const bool mirrored = P.u8(0x36d) != 0;
    const float w = (P.f32(0x2fc) * (mirrored ? std::fabs(sx) : sx)) * fx, h = (P.f32(0x300) * (mirrored ? std::fabs(sy) : sy)) * fy;
    return P.u8(0x390) ? std::pair{h, w} : std::pair{w, h};
}
// getObjectRect2 (0x2949e0) only refreshes the cached rect when +0x368 is dirty; a scale change alone does not mark it.
inline void refreshRect(Mem& P) {
    if (!P.u8(0x368)) return;
    const auto [w, h] = plainPlayerRectSize(P, P.f32(0x394), P.f32(0x398));
    P.f32(0x360, w); P.f32(0x364, h);
    P.f32(0x358, P.f32(po::PosX) - w * 0.5f); P.f32(0x35c, P.f32(po::PosY) - h * 0.5f);   // the player's box offset is 0 (measured)
    P.u8(0x368, 0);
}
// the player's rectangle (vtable +0x4a0): centred on the position, size from the rect cache the game keeps (+0x358: x, y, w, h)
inline void playerRect(Mem& P, float& rx, float& ry, float& rw, float& rh) {
    refreshRect(P);
    rw = P.f32(0x360); rh = P.f32(0x364);
    rx = P.f32(po::PosX) - rw * 0.5f; ry = P.f32(po::PosY) - rh * 0.5f;
}
// PlayerObject::levelFlipping 0x4f97c0: +0x7ec set and the layer's mirror tween (+0x41c) neither 0 nor 1.
inline bool levelFlipping(const Mem& P, const Layer& L) { return P.u8(0x7ec) != 0 && L.flipAmt != 0.0f && L.flipAmt != 1.0f; }

// ----------------------------------------------------------------------------------------------------------------------------------------
struct Ctx;
inline bool rectsIntersect(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh);
inline bool collidedWithObjectInternal(Ctx& c, float dt, GObj* O, const float* rectArg, bool noOverlap);
inline bool collidedWithObject(Ctx& c, float dt, GObj* O, const float* rectArg, bool noOverlap);   // PlayerObject::collidedWithObject 0x4efc90: the turned frame goes through handleRotatedCollisionInternal
inline void runBallRotation(Mem& P, float f);
inline void runNormalRotation(Mem& P, bool force, float f);
// the three-field reset the ball's rotation code does (+0x730 u16, +0x670 u8, +0x728 i32)
inline void rotReset(Mem& P) {
    P.put<uint16_t>(0x730, 0);
    P.u8(0x670, 0);
    P.i32(0x728, 0);
}

// PlayerObject::hitGround 0x4fa250. `o` is the object landed on (its group word at +0x3fe goes to +0x9f4: not modelled).
inline void hitGround(Ctx& c, const GObj* o, bool p3) {
    (void)o;
    Mem& P = c.P;
    const double dir = dirOf(P);
    const double vy = P.f64(po::Vy);
    P.u8(0xb30, 0);
    if (P.u8(po::G2) == 0 && !p3 && P.u8(0x733) == 0 && !levelFlipping(P, c.L)) P.u8(0x908, P.u8(0x908) == 0 ? 1 : 0);   // the landing dust alternates between two emitters (not while the level turns: w_lv13_9210 t=9487)
    // events (gameEventTriggered) are not modelled; the fall-distance word is cleared unless the player is a wave
    if (!P.u8(po::ModeWave) && P.f32(0xc44) != 0.0f) P.f32(0xc44, 0.0f);
    P.f64(po::Vy, 0.0);
    P.u8(0xb21, 0);
    if (P.u8(0x5a0) == 0) P.u8(0x5a0, 1);
    P.u8(po::G2, 1);
    P.u8(0xa31, p3 ? 0 : 1);
    if (dir * vy <= 5.0) {
        P.f64(0xa18, P.f64(po::Time));
        P.u8(po::G, 1);
    }
    P.u8(0x7f0, 1);
    P.i32(0xae4, 2);
    if (!P.u8(po::ModeBall)) {
        if (P.u8(0x730) || P.u8(po::WasOnSlope)) rotReset(P);
    } else {
        if (P.u8(0x670) && !P.u8(po::OnSlope)) rotReset(P);
        if (P.u8(0x731)) rotReset(P);
        if (!P.u8(0x730) && !P.u8(0xa32) && !P.u8(0x9ec)) { rotReset(P); runBallRotation(P, 1.0f); }
    }
    // the cube / robot / spider block at the end (L254-265) resets the rotation words on an off-grid rotation: the sprite's rotation is not in the image
    P.f32(0xa34, P.f32(po::PosX));
    P.f32(0xa38, P.f32(po::PosY));
    if (!P.u8(po::ModeShip) && !P.u8(po::ModeUfo) && !P.u8(po::ModeWave) && !P.u8(po::ModeSwing) && P.u8(0x7eb)) {
        if (P.u8(0x931) == 0) { /* stopStroke */ }
        if (P.u8(0x980)) P.u8(0x980, 0);
        P.u8(0x981, 1);
    }
    P.u8(0x7eb, 0);
}

// hitGroundNoJump 0x4fa1f0: hitGround(0) with G, G2 and the landing time put back
inline void hitGroundNoJump(Ctx& c, bool p3 = false) {
    Mem& P = c.P;
    const uint8_t g = P.u8(po::G), g2 = P.u8(po::G2);
    const double t = P.f64(0xa18);
    hitGround(c, nullptr, p3);
    P.f64(0xa18, t);
    P.u8(po::G2, g2);
    P.u8(po::G, g);
}

// updateCollide 0x4f21f0: kind 0 = head, 1 = foot, 2 = left, 3 = right. The contact line keeps the extreme, the uid the last writer.
inline void updateCollide(Ctx& c, int kind, const GObj* o) {
    Mem& P = c.P;
    refreshRect(P);
    const float hw = P.f32(0x364) * 0.5f;   // the decompile reads vtable+0x4a0 .w / .h * 0.5 -- the cached rect's size
    const float hh = P.f32(0x360) * 0.5f;
    (void)hh;
    const bool flip = P.u8(po::Upside) != 0;
    const int uid = o ? o->uid : 0;
    const float y = P.f32(po::PosY);
    if (kind == 0) {
        const int s = flip ? -1 : 1;
        const double v = (double)((float)s * hw + y);
        double cur = P.f64(po::CeilTopY);
        double nv = v;
        if (cur != 0.0) { nv = cur; if (!flip) { if (v <= cur) nv = v; } else if (cur <= v) nv = v; }
        P.f64(po::CeilTopY, nv);
        if (uid == 0 || uid == P.i32(po::CollisionTopUid)) return;
        P.i32(po::CollisionTopUid, uid);
        c.topDict.insert(uid);
    } else if (kind == 1) {
        const int s = flip ? -1 : 1;
        const double v = (double)(y - (float)s * hw);
        double cur = P.f64(po::FloorBottomY);
        double nv = v;
        if (cur != 0.0) { nv = cur; if (!flip) { if (cur <= v) nv = v; } else if (v <= cur) nv = v; }
        P.f64(po::FloorBottomY, nv);
        if (uid == 0 || uid == P.i32(po::CollisionBottomUid)) return;
        P.i32(po::CollisionBottomUid, uid);
        c.botDict.insert(uid);
    }
}

// -------
// ----------------------------------------------------------------------------------------------------------------------------------------
// the pointer fields hold uid + 1 (0 = null) in the image
inline GObj* objAt(Ctx& c, size_t off) { return c.byUid((int)c.P.get<int64_t>(off) - 1); }
inline void setObj(Ctx& c, size_t off, const GObj* o) { c.P.put<int64_t>(off, o ? (int64_t)o->uid + 1 : 0); }

// PlayerObject::boostPlayer 0x4fe2f0 (the animation half is sprite-only and left out)
inline void boostPlayer(Ctx& c, float v) {
    Mem& P = c.P;
    P.u8(po::Boost, 1);
    P.u8(po::G2, 0);
    P.u8(po::G, 0);
    P.u8(0x7eb, 1);
    P.u8(0x95a, 1);
    const double ip = (double)(int)v;
    double d = (double)v;
    if (d != ip) d = std::round((d - ip) * 1000.0) / 1000.0 + ip;
    P.f64(po::Vy, d);
    if (!P.u8(0x9ec)) {
        if (!P.u8(po::ModeShip) && !P.u8(po::ModeUfo) && !P.u8(po::ModeWave) && !P.u8(po::ModeSwing) && !P.u8(po::ModeBall) && !P.u8(po::ModeRobot)) {
            if (!P.u8(po::ModeSpider)) {
                P.u8(0x731, 0);
                P.u8(0x670, 0);
                const float k = P.f32(0x9f8) == 1.0f ? 0.8666667f : 0.6666667f;
                const int i = P.u8(po::Upside) ? 0xb4 : -0xb4;
                P.f32(0x728, (float)i / k);
                P.u8(0x730, 1);
            }
        }
    }
    P.f32(0xa34, P.f32(po::PosX));
    P.f32(0xa38, P.f32(po::PosY));
}

// PlayerObject::didHitHead 0x4f1e30 (decompile 4f1e4f): with the "Special" 2866's counter (+0xb88) up the head hit turns the gravity -- flipGravity(!upside, 1) -- and then sets the velocity to -2 if the NEW
// gravity is upright (+0x9c7 == 0), else +2. w_116243793_1960 t=2231: a cube under a ceiling of solids that carry Special 2866 objects is flipped by the game and stands on the ceiling.
inline void playerFlipGravity(Ctx& c, uint8_t up, bool p3); inline void dualFlipSite(Ctx& c, uint8_t up0, bool p3, bool count = true); inline bool dualWillSwitch(Ctx& c, int type); inline bool dualPortalHook(Ctx& c, bool on); inline void playerRotateGameplay(Ctx& c, const GObj& o);   // (the dual calls and the rotation: defined at the end of this file)
inline void didHitHead(Ctx& c) {
    Mem& P = c.P;
    if (0 < P.i32(0xb88)) {
        playerFlipGravity(c, P.u8(po::Upside) == 0 ? 1 : 0, true);
        P.f64(po::Vy, P.u8(po::Upside) ? 2.0 : -2.0);
        P.u8(po::Boost, 1);
        P.u8(po::G2, 0);
        if (0 < P.i32(0xb7c)) { P.u8(po::G, 0); P.put<uint16_t>(0x98d, 0); }
    }
}

// ----------------------------------------------------------------------------------------------------------------------------------------
// PlayerObject::postCollision 0x4eb830 for a player that is not a platformer (the platformer's carry code, L260-370 and L494-529, is not ported)
inline void postCollision(Ctx& c, float dt, bool p3) {
    Mem& P = c.P;
    float fVar24 = dt * 4.0f;
    if (P.u8(0xc4e) == 0) fVar24 = p3 ? 0.5f : 1.0f;
    uint8_t cVar21 = P.u8(0xb09), cVar22 = P.u8(0xb08);
    if (cVar21 && cVar22) { if (!P.u8(0xb0a)) cVar21 = 0; else cVar22 = 0; }
    GObj* ground = objAt(c, po::GroundObj);
    if (ground && ground->inactive) { setObj(c, po::GroundObj, nullptr); ground = nullptr; }
    GObj* slopeObj = objAt(c, po::SlopeObj);
    if (slopeObj && slopeObj->inactive) setObj(c, po::SlopeObj, nullptr);
    if (ground) P.u8(0xb30, ground->b519 ? 1 : 0);
    if (P.u8(po::OnSlope) && P.get<int64_t>(po::Cd0) && !P.u8(0x9c0)) {
        GObj* d0 = objAt(c, po::Cd0);
        if (d0) P.u8(0xb30, d0->b519 ? 1 : 0);
    }
    const uint8_t cVar15 = (P.f64(po::CeilTopY) == 0.0 && P.f64(po::FloorBottomY) == 0.0) ? 0 : 1;
    if (P.u8(po::ModeWave) && P.u8(0xb41) && cVar15 == 0) P.u8(0x5a0, 0);   // FUN_1404fec10 (the wave's streak restarts when it leaves the ceiling / floor)
    P.u8(0xb41, cVar15);
    const bool flip = P.u8(po::Upside) != 0;
    if (P.u8(po::OnSlope) != P.u8(po::WasOnSlope) && P.u8(po::ModeWave)) P.u8(0x5a0, 0);   // FUN_1404fec10 (the wave's streak restarts at a ramp's start and end)

    if (P.u8(po::OnSlope) != P.u8(po::WasOnSlope) && P.u8(po::OnSlope) == 0) {        // left a ramp this tick (L104-229)
        P.f64(0xb58, P.f64(po::Time));
        const float v = P.f32(0x9bc);
        const double vy = P.f64(po::Vy);
        const bool is95b = P.u8(0x95b) != 0;
        bool bVar23;
        if (!flip) bVar23 = (v < 0.0f && (double)v < vy && is95b) || (0.0f < v && vy <= (double)v && (double)v != vy && !is95b);
        else bVar23 = !(((0.0f <= v) || (vy <= (double)v) || !is95b) && ((v <= 0.0f) || ((double)v <= vy) || is95b));
        const bool gate1 = P.u8(po::Boost) == 0 || P.u8(po::ModeShip) || P.u8(po::ModeUfo) || P.u8(po::ModeWave) || P.u8(po::ModeSwing) || P.u8(po::ModeBall);
        if (gate1 && bVar23) {
            double dVar28 = P.f64(po::Time) - P.f64(0x598);
            double dVar27 = 1.0;
            if (dVar28 < 0.10000000149011612) {
                dVar28 *= 10.0;
                dVar27 = 0.4000000059604645;
                if (0.4000000059604645 <= dVar28) dVar27 = dVar28;
            }
            const float fVar29 = (float)((double)v * dVar27);
            bool lt, eq;
            if (!flip) {
                if (!is95b || !P.u8(0x694)) { eq = (double)fVar29 == vy; lt = (double)fVar29 < vy; }
                else { eq = vy == (double)fVar29; lt = vy < (double)fVar29; }
            } else {
                if (is95b || !P.u8(0x694)) { eq = vy == (double)fVar29; lt = vy < (double)fVar29; }
                else { eq = (double)fVar29 == vy; lt = (double)fVar29 < vy; }
            }
            if (!lt && !eq) boostPlayer(c, fVar29);
        } else {
            const bool cond = !flip ? (v <= 0.0f && v != 0.0f) : (0.0f < v);
            if (cond && P.u8(po::Boost) == 0 && P.u8(0x98d) == 0 && P.u8(0x98e) == 0 && P.u8(0x95b) == P.u8(po::Upside)) {
                P.u8(po::G2, 0);
                const int iVar5 = flip ? -1 : 1;
                double d = (double)iVar5 * -P.f64(0x940);
                if (d != (double)(int)d) d = std::round((d - (double)(int)d) * 1000.0) / 1000.0 + (double)(int)d;
                P.f64(po::Vy, d);
                P.u8(po::G, 0);
                if (!P.u8(0xa32) && !P.u8(0x9ec)) { rotReset(P); if (!P.u8(po::ModeBall)) runNormalRotation(P, false, 1.0f); else runBallRotation(P, 1.0f); }
            }
        }
        if (P.f64(0xb28) > 0.0 && ((cVar21 && P.u8(0xb63)) || (cVar22 && !P.u8(0xb63)))) {
            P.u8(0xb21, 0);
            P.f64(0xb28, 0.0);
            P.u8(0xb60, 1);
        }
        P.f64(0x940, 0.0);
        P.f32(0x9bc, 0.0f);
        P.f64(0x938, 0.0);
        P.put<int64_t>(po::Cd0, 0);
    }

    // crushed: the contact lines of the head and the foot are under 0.7 of the height apart (L231-388)
    bool done = false;
    if (!(P.f64(po::CeilTopY) == 0.0 || P.f64(po::FloorBottomY) == 0.0 || P.u8(0xc48))) {
        const float fVar26 = P.u8(po::Platformer) ? 0.8f : 0.7f;
        float rx, ry, rw, rh;
        playerRect(P, rx, ry, rw, rh);
        bool bVar23 = false;
        const float fVar25 = (float)std::fabs(P.f64(po::CeilTopY) - P.f64(po::FloorBottomY));
        const float py = P.f32(po::PosY), pPrev = P.f32(po::PrevY);
        if (fVar25 == 0.0f) bVar23 = true;
        else if (!flip) { if (P.f64(po::CeilTopY) < (double)py && P.f64(po::CeilTopY) < (double)pPrev) bVar23 = true; }
        else if ((double)py < P.f64(po::CeilTopY) && (double)pPrev < P.f64(po::CeilTopY)) bVar23 = true;
        if (!(bVar23 || rh * fVar26 <= fVar25) && P.u8(0xc4c) == 0) {
            if (P.u8(0x7ed) == 0) GDCOL_DIED(c); else P.u8(0x98c, 1);
            done = true;
        }
    }
    if (P.u8(0xc4c) == 0) {    // the same object under the head and under the foot in one tick
        for (int u : c.topDict) {
            if (c.botDict.count(u)) {
                if (P.u8(0x7ed) != 0) P.u8(0x98c, 1); else GDCOL_DIED(c);
                return;
            }
        }
    }
    if (done) return;

    // L616-734: the ground and the ramp the player stood on are forgotten when the foot touched nothing this tick, or when the ground has moved away
    {
        bool moved = false;
        GObj* g = objAt(c, po::GroundObj);
        const bool sw = P.u8(po::Sideways) != 0;   // a turned frame: the ground has moved along X, not Y (postCollision 0x4eb830 L614-636: it reads the x pair when +0x9cb is up)
        if (g && P.get<int64_t>(po::SlopeObj) == 0 && !g->b518) {
            if (g->cx != g->prevCx || g->cy != g->prevCy) {
                const float was = sw ? g->prevCx : g->prevCy, now = sw ? g->cx : g->cy;
                if (!flip) { if (now < was) moved = true; }
                else if (was < now) moved = true;
            }
        }
        if (c.botDict.empty() || moved) {
            P.u8(0x660, 0);
            setObj(c, po::GroundObj, nullptr);
            if (g && moved) {
                float lim = (P.u8(po::Platformer) ? 10.0f : 5.0f) * dt;
                if (g->b51c) lim = dt * 20.0f;
                float px, py, pw, ph;
                playerRect(P, px, py, pw, ph);
                float tx = g->rx, ty = g->ry, tw = g->rw, th = g->rh;
                if (sw) tw += lim; else th += lim;
                bool near_;
                if (!flip) near_ = sw ? (px - (g->rx + g->rw)) < lim : (py - (g->ry + g->rh)) < lim;
                else { near_ = sw ? (g->rx - (px + pw)) < lim : (g->ry - (py + ph)) < lim; if (sw) tx -= lim; else ty -= lim; }
                bool inter = rectsIntersect(px, py, pw, ph, tx, ty, tw, th);
                GObj* landOn = g;
                if (!inter && P.get<int64_t>(po::Collided)) {   // 0x4ecd4e: no rect contact, but checkCollisions registered a platform as +0x600: that platform (rbx = [rdi+0x600]) is what collidedWithObject is called with
                    inter = true;
                    if (GObj* col = objAt(c, po::Collided)) landOn = col;
                }
                if (near_ && inter) { P.u8(0x660, 1); collidedWithObject(c, 0.0f, landOn, nullptr, false); }
            }
        }
        bool slopeMoved = false;
        GObj* s = objAt(c, po::SlopeObj);
        if (s && !s->b518) {
            if (s->cx != s->prevCx || s->cy != s->prevCy) {
                if (!flip) { if (s->cy < s->prevCy) slopeMoved = true; }
                else if (s->prevCy < s->cy) slopeMoved = true;
            }
        }
        if (c.botDict.empty() || slopeMoved) {
            P.u8(0x660, 0);
            setObj(c, po::SlopeObj, nullptr);
            if (slopeMoved) { P.u8(0x660, 1); ++c.unsupported; }
        }
    }    // L735-768: a platform that was rising and slowed throws the rider
    {
        double dVar27 = P.f64(0x638);
        const double dVar28 = P.f64(0x630);
        if (dVar27 != 0.0 && P.i32(0xaec) < 1 && fVar24 * 0.5f < (float)std::fabs(dVar27 - dVar28)) {
            if (!flip) {
                if (5.0 < dVar27 && dVar28 < dVar27) {
                    if (20.0 <= dVar27) dVar27 = 20.0;
                    if (dVar27 <= -20.0) dVar27 = -20.0;
                    P.f64(0x638, dVar27);
                    if (P.f64(po::Vy) <= dVar27) { boostPlayer(c, (float)dVar27); setObj(c, po::SlopeObj, nullptr); setObj(c, po::GroundObj, nullptr); }
                    else { setObj(c, po::SlopeObj, nullptr); setObj(c, po::GroundObj, nullptr); }
                }
            } else if (dVar27 < -5.0 && dVar27 < dVar28) {
                if (20.0 <= dVar27) dVar27 = 20.0;
                if (dVar27 <= -20.0) dVar27 = -20.0;
                P.f64(0x638, dVar27);
                if (dVar27 <= P.f64(po::Vy)) boostPlayer(c, (float)dVar27);
                setObj(c, po::SlopeObj, nullptr);
                setObj(c, po::GroundObj, nullptr);
            }
        }
    }
    // L840-905: the tail bookkeeping
    P.put<int64_t>(po::Collided, 0);
    const double c5 = P.f64(0x628);
    if (c5 != 0.0) P.f64(0xa28, P.f64(po::Time));
    if (P.f64(0x640) == 0.0 || c5 != 0.0) {
        P.f64(0x640, c5);
        P.u8(0x648, p3 ? 1 : 0);
    } else if (0.03333333507180214 <= P.f64(po::Time) - P.f64(0xa28)) {
        P.f64(0x640, 0.0);
    }
    P.f64(0x628, 0.0);
    P.f64(0x638, P.f64(0x630));
    P.f64(0x630, 0.0);
    P.f64(0x658, P.f64(0x650));
    P.f64(0x650, 0.0);
    P.f64(0xb38, 0.0);                       // `if (!bVar23) [0x167] = 0` with the platformer carry off
    if (P.u8(po::G2)) P.u8(0xb18, 0);
    if (!P.u8(po::OnSlope) && !P.u8(po::WasOnSlope)) P.i32(0x690, 0);
    P.f64(po::VyPrev, P.f64(po::Vy));
    P.i32(0x9d0, P.i32(0x9d0) - 1);
}


// ----------------------------------------------------------------------------------------------------------------------------------------
// small helpers for the slope code
inline bool slopeIsTop(int dir) { return dir == 1 || (((unsigned)(dir - 3)) & 0xfffffffcu) == 0 && dir != 4; }

// GameObject::slopeYPos 0x29e2b0 (the object's rect is the AABB of the slope; sy is the line through its corners)
inline double slopeYPos(const GObj& o, float x) {
    const float minX = o.rx, maxX = o.rx + o.rw, minY = o.ry, maxY = o.ry + o.rh;
    double d = (double)x, r;
    if ((double)minX < d) {
        d = (d - (double)maxX) * (double)(o.rh / o.rw);
        r = !o.slopeUphill ? (double)minY - d : d + (double)maxY;
    } else {
        d = ((double)minX - d) * (double)(o.rh / o.rw);
        r = !o.slopeUphill ? d + (double)maxY : (double)minY - d;
    }
    if (o.slopeHazard) r += slopeIsTop(o.slopeDir) ? -4.0 : 4.0;
    return r;
}

inline bool rectsIntersect(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh) {
    // CCRect::intersectsRect: the closed-interval test cocos2d uses
    return !(ax + aw < bx || bx + bw < ax || ay + ah < by || by + bh < ay);
}

// ----------------------------------------------------------------------------------------------------------------------------------------
inline bool collidedWithObjectInternal(Ctx& c, float dt, GObj* O, const float* rectArg /* x,y,w,h or null = the object's own */, bool noOverlap);
inline void collidedWithSlope(Ctx& c, float dt, GObj* o, bool forced);

// PlayerObject::checkSnapJumpToObject 0x4f1eb0: a cube landing on the block a fixed stride from the one it left is nudged by up to 1 (2 above 1x) px in x,
// so a row of equally spaced blocks is always met at the same offset. The strides are by speed: 0.7 -> 60 / 120, 0.9 -> 90 / 150, 1.1 -> 120 / 195, 1.3 -> 135 / 225.
inline void checkSnapJumpToObject(Ctx& c, const GObj* O) {
    Mem& P = c.P;
    if (!O) return;
    const GObj* S = objAt(c, po::SnappedTo);
    if (S && S->uid != O->uid && S->type == 0) {
        const bool flip = P.u8(po::Upside) != 0;
        const float fVar1 = P.f32(0x9fc);
        float fVar12 = 90.0f, fVar10 = 120.0f, fVar16 = 90.0f, fVar15 = 150.0f, fVar14 = 1.0f;
        const bool mini = P.f32(0x9f8) != 1.0f;
        if (fVar1 == 0.9f) { fVar12 = fVar16; if (mini) { fVar10 = 90.0f; fVar12 = fVar16; } }
        else if (fVar1 == 0.7f) { fVar16 = 60.0f; fVar15 = 120.0f; fVar10 = 90.0f; fVar12 = fVar16; }
        else if (fVar1 == 1.1f) { fVar10 = 150.0f; fVar16 = 120.0f; fVar15 = 195.0f; fVar14 = 2.0f; fVar12 = fVar16; if (mini) fVar10 = 90.0f; }
        else if (fVar1 == 1.3f) { fVar10 = 180.0f; fVar15 = 225.0f; fVar16 = 135.0f; fVar14 = 2.0f; fVar12 = fVar16; if (mini) fVar10 = 90.0f; }
        const float sx = S->cx, sy = S->cy, ox = O->cx, oy = O->cy;
        const float d = flip ? -1.0f : 1.0f;
        bool hit = false;
        if (!(fVar14 < std::fabs(ox - (sx + fVar10))) && !(fVar14 < std::fabs(oy - (d * 30.0f + sy)))) hit = true;
        else if (std::fabs(ox - (sx + fVar15)) <= fVar14 && std::fabs(oy - (sy - d * 30.0f)) <= fVar14) hit = true;
        else if (!(fVar14 < std::fabs(ox - (sx + fVar12))) && !(fVar14 < std::fabs(oy - (d * 60.0f + sy)))) hit = true;
        if (hit) {
            double dVar4 = (double)O->cx + P.f64(0x840);
            const double dVar11 = (double)P.f32(po::PosX);
            const double f14 = (double)fVar14;
            if (f14 < std::fabs(dVar4 - dVar11)) dVar4 = dVar4 <= dVar11 ? dVar11 - f14 : f14 + dVar11;
            setPosition(P, (float)dVar4, P.f32(po::PosY));
        }
    }
    setObj(c, po::SnappedTo, O);
    P.f64(0x840, (double)(P.f32(po::PosX) - O->cx));
}

// PlayerObject::preSlopeCollision 0x4ed660: a ramp's side edge and its flat face are walls; returns true when it handled the contact
inline bool preSlopeCollision(Ctx& c, float dt, GObj* o) {
    Mem& P = c.P;
    if (o->uid == P.i32(0x690)) return false;
    const float cx = o->rx, cy = o->ry, cw = o->rw, ch = o->rh;
    float px, py, pw, ph;
    playerRect(P, px, py, pw, ph);
    const int dir = o->slopeDir;
    bool bVar11 = false;
    float fVar14 = 0.0f;
    const bool bVar2 = dir == 2 || ((((unsigned)(dir - 3)) & 0xfffffffcu) == 0 && dir != 5);
    const bool bVar1 = dir < 7 && ((0x6au >> dir) & 1);
    float fVar15 = (!P.u8(po::Platformer) || (!P.u8(po::OnSlope) && !P.u8(po::WasOnSlope))) ? 0.0f : 5.0f;
    const float odx = o->cx - o->prevCx;
    float fVar12 = (float)((double)P.f32(0x9fc) * P.f64(0x7c0) * (double)dt);
    const float X = P.f32(po::PosX), Y = P.f32(po::PosY);
    auto strip = [&](float sx, int iVar3) {
        fVar14 = (float)iVar3;
        fVar15 = fVar14 + fVar15;
        const float fVar13 = ch - (fVar15 + fVar15);
        const float s[4] = {sx, fVar15 + cy, 1.0f, fVar13};
        bVar11 = true;
        if (rectsIntersect(px, py, pw, ph, s[0], s[1], s[2], s[3])) collidedWithObjectInternal(c, dt, o, s, false);
    };
    bool done = false;
    if (bVar2) {
        if (((P.u8(po::Leftward) == 0 || P.u8(po::Platformer)) || odx < fVar12) && X <= cx && cx != X) {
            strip(cx, (P.u8(po::Leftward) == 0 || P.u8(po::Platformer)) ? 0 : 1);
            done = true;
        }
    } else if ((P.u8(po::Leftward) != 0 || P.u8(po::Platformer)) || fVar12 < odx) {
        if (cx + cw < X) {
            strip((cw + cx) - 1.0f, (P.u8(po::Leftward) == 0 && !P.u8(po::Platformer)) ? 1 : 0);
            done = true;
        }
    }
    (void)done;
    float bx, by, bw, bh;
    const float ow = o->rw - (fVar14 + fVar14);
    bool take = false;
    if (bVar1) {
        bx = o->rx + fVar14; by = (o->ry + o->rh) - 1.0f; bw = ow; bh = 1.0f;
        take = (bh + by) <= Y;
    } else {
        bx = o->rx + fVar14; by = o->ry; bw = ow; bh = 1.0f;
        take = Y <= (bh + by);
    }
    if (slopeDbgOn(P)) std::fprintf(slopeDbgOut(), "slopedbg pre uid %d dir %d uphill %d bVar1 %d bVar2 %d X %.4f Y %.4f | player rect %.3f %.3f %.3f %.3f | ramp rect %.3f %.3f %.3f %.3f | strip %.3f %.3f %.3f %.3f take %d fVar14 %.1f odx %.4f\n", o->uid, dir, (int)o->slopeUphill, (int)bVar1, (int)bVar2, (double)X, (double)Y, (double)px, (double)py, (double)pw, (double)ph, (double)o->rx, (double)o->ry, (double)o->rw, (double)o->rh, (double)bx, (double)by, (double)bw, (double)bh, (int)take, (double)fVar14, (double)odx);
    if (take) {
        bVar11 = true;
        const float s[4] = {bx, by, bw, bh};
        if (rectsIntersect(px, py, pw, ph, s[0], s[1], s[2], s[3])) collidedWithObjectInternal(c, dt, o, s, false);
    } else if (!bVar11) {
        P.u8(po::NodeFlag671, 0);
        if (!P.u8(po::OnSlope)) {
            c.touched.push_back(o->uid);
            GObj* cf = objAt(c, po::Cf);
            bool set = true;
            if (cf) {
                const bool cfTop = cf->slopeUphill == false || slopeIsTop(cf->slopeDir);
                const bool oTop = o->slopeUphill == false || slopeIsTop(o->slopeDir);
                if (cfTop && oTop) {
                    if (o->cx <= cf->cx && cf->cx != o->cx) set = true;      // *pfVar5 <= *pfVar9 && *pfVar9 != *pfVar5 -> take o
                    else set = false;
                    if (set) { /* fall to the assignment */ }
                    else {
                        // the second test of the pair (L161-166): keep unless the stored one is a ceiling type and o is to its right
                        if (cf->slopeUphill == false || slopeIsTop(cf->slopeDir)) set = false;
                        else set = !(o->cx < cf->cx || o->cx == cf->cx);
                    }
                } else {
                    if (cf->slopeUphill == false || slopeIsTop(cf->slopeDir)) set = false;
                    else set = !(o->cx < cf->cx || o->cx == cf->cx);
                }
            }
            if (set) setObj(c, po::Cf, o);
        }
    }
    // L170-176
    float ox, oy, ow2, oh;
    ox = o->rx; oy = o->ry; ow2 = o->rw; oh = o->rh;
    const bool inter = rectsIntersect(ox, oy, ow2, oh, px, py, pw, ph);
    return bVar11 ? true : !inter;
}

// ----------------------------------------------------------------------------------------------------------------------------------------
// PlayerObject::collidedWithSlopeInternal 0x4edb90 for the upright frame of a non-platformer
inline void collidedWithSlopeInternal(Ctx& c, float dt, GObj* o, bool forced) {
    Mem& P = c.P;
    if (!forced && preSlopeCollision(c, dt, o)) { if (slopeDbgOn(P)) std::fprintf(slopeDbgOut(), "slopedbg main uid %d: preSlopeCollision handled it\n", o->uid); return; }
    if (slopeDbgOn(P)) std::fprintf(slopeDbgOut(), "slopedbg main uid %d: entered, WasOnSlope %d OnSlope %d forced %d\n", o->uid, (int)P.u8(po::WasOnSlope), (int)P.u8(po::OnSlope), (int)forced);
    const bool flip = P.u8(po::Upside) != 0;
    float px, py, pw, ph;           // local_128..: the player's rect
    playerRect(P, px, py, pw, ph);
    const float sx = o->rx, sy = o->ry, sw = o->rw, sh = o->rh;   // local_138..: the ramp's rect
    bool bVar20 = !o->slopeUphill;
    bool bVar23;
    if (P.u8(po::Platformer) == 0) {
        if (P.u8(po::Leftward) == 0) bVar23 = false;
        else { bVar23 = true; bVar20 = o->slopeUphill; }
    } else bVar23 = false;
    const int dir = o->slopeDir;
    const bool cVar4 = slopeIsTop(dir);
    bool bVar21 = false, bVar19 = false;
    const float odx = o->cx - o->prevCx, ody = o->cy - o->prevCy;
    (void)ody;
    if ((double)P.f32(0x9fc) * P.f64(0x7c0) * (double)dt < (double)odx) {
        if (!flip) { if (bVar20) bVar21 = true; } else bVar21 = !bVar20;
        bVar19 = true;
        bVar20 = !bVar20;
    }
    const uint8_t cVar6s = P.u8(po::Upside);
    if (cVar6s != P.u8(po::Sideways)) bVar20 = !bVar20;
    if (P.u8(po::WasOnSlope)) {
        const int iVar15 = flip ? -1 : 1;
        if (0.0f < (float)iVar15 * P.f32(0x9bc) && bVar20 && (P.u8(0x95b) != 0) == cVar4 && P.u8(0x95b) == cVar6s) return;
    }
    float fVar28 = sh / (float)((double)sw / ((double)P.f32(0x9fc) * P.f64(0x7c0)));
    const float ratio = fVar28;       // local_148
    if (P.u8(po::WasOnSlope)) {
        const int iVar15 = flip ? -1 : 1;
        if (0.0f < (float)iVar15 * P.f32(0x9bc) && (double)fVar28 < P.f64(0x940)) return;
    }
    {
        uint8_t bVar5 = bVar20 ? 1 : 0;
        if (P.u8(po::WasOnSlope)) bVar5 = (uint8_t)(bVar5 * 4);
        fVar28 = (float)bVar5;
    }
    // the ramp's centre now and before: d = prev - cur along the gravity axis
    float local_d4 = o->prevCy - o->cy;
    bool bVar17 = !flip ? (local_d4 > 0.0f) : (local_d4 < 0.0f);
    if (forced || bVar17) {
        local_d4 = std::fabs(local_d4);
        const float lim = P.u8(po::Platformer) ? 10.0f : 5.0f;
        if (lim * dt <= local_d4) local_d4 = lim * dt;
        fVar28 += local_d4;
    }
    float fVar29 = ph * 0.5f;
    float fVar24 = std::cos((float)P.f64(0x938));
    fVar29 = fVar29 / fVar24;
    const float X = P.f32(po::PosX), Y = P.f32(po::PosY);
    if (!flip) fVar24 = (Y - fVar29) - fVar28;
    else fVar24 = fVar29 + Y + fVar28;
    {
        const uint8_t cVar6 = P.u8(po::WasOnSlope);
        const bool inset = rectsIntersect(px, py, pw, ph, sx, sy + 1.0f, sw, sh - 2.0f);
        if (c.primeWords) {} else if (cVar6 == 0) { if (!inset) return; }   // primeWords (--shadowseedslope): the words of a ride taken a tick ago are wanted, not the contact test
        else if (!flip) { if (sy + sh < fVar24) return; }
        else { if (fVar24 < sy) return; }
    }
    // L222-: the surface under the player
    const float posX0 = X;
    float fVar29b = sy;                       // minY
    float fVar25 = sy + sh;                   // maxY
    const float ang = std::atan(sh / sw);     // local_118
    float fVar31 = (ph / std::cos(ang)) * 0.5f;
    fVar24 = 0.0f;
    bool bVar7;
    if (P.u8(po::WasOnSlope) == 0 || P.i32(0x690) == o->uid || (P.u8(0x95b) != 0) == cVar4) bVar7 = false;
    else { bVar7 = true; if (!P.u8(po::Platformer)) fVar24 = P.f32(0x9f8) * 20.0f; }
    const double dVar26 = slopeYPos(*o, X);
    float fVar30;
    bool bVar22s;                              // local_res20[0] low byte
    bool local_157;
    if (!cVar4) {
        fVar25 = (ph * 0.5f + fVar25) - fVar24;
        fVar24 = ((float)dVar26 + fVar31) - fVar24;
        bVar22s = fVar25 <= fVar24;
        if (bVar22s) fVar24 = fVar25;
        fVar30 = fVar24;
        if (fVar24 <= fVar29b) fVar30 = fVar29b;
        local_157 = fVar24 <= fVar29b;
    } else {
        fVar29b = (fVar29b - ph * 0.5f) + fVar24;
        fVar24 = ((float)dVar26 - fVar31) + fVar24;
        fVar30 = fVar24;
        if (fVar24 <= fVar29b) fVar30 = fVar29b;
        bVar22s = fVar24 <= fVar29b;
        local_157 = fVar25 <= fVar30;
        if (local_157) fVar30 = fVar25;
    }
    uint8_t local_154 = 0;
    bool bVar22 = false;
    bool bVar18 = false, bVar3 = false;
    const bool flyMode = P.u8(po::ModeShip) || P.u8(po::ModeUfo) || P.u8(po::ModeWave) || P.u8(po::ModeSwing);
    bool gotoKillCheck = false;
    const float posY0 = Y;       // local_13c
    if (flip == cVar4) {          // the ramp is the player's floor (A)
        uint8_t cVar6;
        if (bVar7 || !bVar20 || P.u8(po::OnSlope)) {
            cVar6 = 0;
            if (!bVar20) { cVar6 = forced ? 1 : 0; if (bVar17) cVar6 = 1; }
        } else {
            bool toE3 = false;
            if (P.u8(po::Boost) == 0) {
                if (P.u8(po::ModeShip)) { if (P.u8(0x98d)) toE3 = true; }
            } else {
                if (P.u8(po::ModeShip)) { if (P.u8(0x98d)) toE3 = true; }
                else if (!P.u8(po::ModeUfo) && !P.u8(po::ModeWave)) {
                    if (!P.u8(po::ModeSwing)) toE3 = true;
                    else { if (P.u8(po::ModeShip) && P.u8(0x98d)) toE3 = true; }
                }
            }
            if (toE3) { cVar6 = 0; if (!bVar20) { cVar6 = forced ? 1 : 0; if (bVar17) cVar6 = 1; } }
            else cVar6 = 1;
        }
        if (!flip) {
            if (posY0 < fVar30) bVar17 = true;
            else if (cVar6 != 0 && posY0 < fVar30 + fVar28) {
                if (P.u8(po::ModeUfo)) { bVar17 = 0.0 < P.f64(po::Vy); if (bVar17) bVar17 = false; else bVar17 = true; }
                else bVar17 = true;
            } else bVar17 = false;
        } else {
            if (posY0 <= fVar30) {
                if (cVar6 == 0 || posY0 <= fVar30 - fVar28) bVar17 = false;
                else if (P.u8(po::ModeUfo)) { bVar17 = P.f64(po::Vy) < 0.0; if (bVar17) bVar17 = false; else bVar17 = true; }
                else bVar17 = true;
            } else bVar17 = true;
        }
        if (P.u8(0x98d) == 0 || !flyMode || (bVar20 && !P.u8(po::Platformer))) { bVar18 = false; bVar3 = false; }
        else { bVar18 = true; bVar3 = !(P.u8(po::Platformer) && bVar7); }
        if (bVar17) gotoKillCheck = true;
    } else {                      // the underside (B)
        local_154 = 1;
        if (!bVar20) {
            if ((flyMode) && P.u8(0x98d) != 0) fVar28 = (float)(uint8_t)((P.u8(po::WasOnSlope) != 0) + 1);
        } else fVar28 = 0.0f;
        bool gate = !(bVar7 || ((flyMode && (bVar20 && !P.u8(po::Platformer))) || P.u8(0x9b8) != 0));
        // (the decompile's `(bVar7) || ((fly && bVar20 && !platformer) || forced-flag)`: the last term is [0x137] = isOnSlope)
        bool contact;
        if (!flip) {
            if (fVar30 < posY0) contact = true;
            else if (gate) { const float f = fVar30 - fVar28; contact = !(posY0 < f) && posY0 != f; }
            else contact = false;
        } else {
            if (fVar30 <= posY0) {
                if (gate) { const float f = fVar30 + fVar28; contact = !(f < posY0) && posY0 != f; }
                else contact = false;
            } else contact = true;
        }
        bVar17 = contact;
        if (P.u8(0x98d) == 0 && !P.u8(po::Platformer) && (flyMode || P.u8(po::ModeBall)) && (bVar20 && P.u8(po::WasOnSlope))) {
            bVar3 = true;
            if (flyMode) bVar22 = bVar20;
        } else {
            bVar3 = false;
            if (P.u8(0x98d) == 0 && flyMode) bVar22 = bVar20;
        }
        if (bVar17) {
            const bool cubeLike = !c.primeWords && !P.u8(po::ModeShip) && !P.u8(po::ModeUfo) && !P.u8(po::ModeWave) && !P.u8(po::ModeSwing) && !P.u8(po::ModeBall)
                                  && !P.u8(po::Platformer) && P.i32(0xb84) < 1;
            if (cubeLike) {
                const double t = P.f64(po::Time);
                if ((P.f64(0x7e0) != 0.0 && t - P.f64(0x7e0) < 0.1) || (P.f64(0x808) != 0.0 && t - P.f64(0x808) < 0.1)) {
                    setPosition(P, X, fVar30);
                    double d = P.f64(po::Vy);
                    if (!flip) { if (-2.0 <= d) d = -2.0; } else if (d <= 2.0) d = 2.0;
                    setYVelocity(P, d);
                    P.u8(po::G, 0);
                    P.u8(po::G2, 0);
                    return;
                }
                float yy = posY0;
                bool b;
                if (!flip) { yy -= 2.0f; b = yy < fVar30; } else { yy += 2.0f; b = fVar30 < yy; }
                if (b || yy == fVar30) { P.u8(po::G, 0); P.u8(po::G2, 0); return; }
                if (P.u8(0xc4c)) { bVar18 = false; gotoKillCheck = true; }
                else { if (P.u8(0x7ed)) { P.u8(0x98c, 1); return; } GDCOL_DIED(c); return; }
            } else { bVar18 = false; gotoKillCheck = true; }
        } else bVar18 = false;
    }
    if (c.primeWords) { bVar17 = true; gotoKillCheck = false; } if (gotoKillCheck) {
        // L463-474: a spiked ramp, or one met from the wrong side by a wave / a changed ramp, kills
        const bool hazard = o->slopeHazard;
        const bool B = (!bVar7 && (!P.u8(po::ModeWave) || 0 < P.i32(0xb80))) || P.u8(po::Platformer) || 0 < P.i32(0xb84);
        if (!((!hazard && B) || P.u8(0xc4c))) {
            if (P.u8(0x7ed)) { P.u8(0x98c, 1); return; }
            GDCOL_DIED(c);
            return;
        }
    }
    if (slopeDbgOn(P)) std::fprintf(slopeDbgOut(), "slopedbg main uid %d: flip %d top %d bVar20 %d bVar7 %d fVar24 %.4f fVar25 %.4f fVar29b %.4f fVar30 %.4f slopeY %.4f fVar28 %.4f posY0 %.4f contact %d kill %d bVar3 %d\n", o->uid, (int)flip, (int)cVar4, (int)bVar20, (int)bVar7, (double)fVar24, (double)fVar25, (double)fVar29b, (double)fVar30, dVar26, (double)fVar28, (double)posY0, (int)bVar17, (int)gotoKillCheck, (int)bVar3);
    // L485-: seat
    float fVar28b = fVar30;
    const uint8_t cVar6x = bVar22s ? 1 : 0;
    if (P.u8(po::WasOnSlope)) {
        const bool a = (P.f64(0x948) != (double)fVar30) && true;
        if (a || (P.u8(po::Platformer) && std::fabs(P.f64(0xb00)) < 0.1)) {
            if (ody != 0.0f && ((cVar6x && P.u8(0x958)) || (local_157 && P.u8(0x959))) && P.u8(0x9ec) == 0
                && (!P.u8(po::Platformer) || 0.1 < std::fabs(P.f64(0xb00))))
                bVar17 = false;
        } else bVar17 = false;
    }
    if (bVar20 && P.u8(po::ModeWave) && P.u8(0x98d) && cVar4 == flip) return;
    if (!bVar17) return;
    setObj(c, po::Cd0, o);
    P.f32(0x6a0, std::atan(sh / sw));
    P.put<int64_t>(po::Cf, P.get<int64_t>(po::Cd0));
    c.touched.push_back(o->uid);
    const float fAng = ang;
    P.f64(0x688, (double)(fVar31 - ph * 0.5f));
    P.u8(0x95b, cVar4 ? 1 : 0);
    P.f64(0x948, (double)fVar30);
    P.f64(0x950, (double)fVar28b);
    P.i32(0x690, o->uid);
    P.u8(0x694, bVar20 ? 1 : 0);
    const double dVar26a = P.f64(0x938);
    P.u8(0x958, cVar6x);
    P.u8(0x959, local_157 ? 1 : 0);
    int iVar15 = flip ? -1 : 1;
    if (!bVar20) iVar15 = -iVar15;
    int iVar8 = -iVar15;
    if (!bVar19) iVar8 = iVar15;
    iVar15 = -iVar8;
    if (!bVar23) iVar15 = iVar8;
    iVar8 = iVar15;
    if (P.u8(po::Sideways)) iVar8 = -iVar15;
    P.f64(0x938, (double)((float)iVar8 * fAng * 57.29578f * 0.017453292f));
    setPosition(P, X, fVar30);
    P.u8(po::OnSlope, 1);
    if (P.u8(po::WasOnSlope) == 0) { P.f64(0x598, P.f64(po::Time)); P.f64(0x568, P.f64(po::Vy)); }
    P.u8(po::OnSlopeUnder, local_154);
    if (!bVar3) {
        double dVar27 = P.f64(po::Vy);
        if (local_154 == 0) {
            const float fv = (float)dVar27;
            hitGround(c, o, false);
            if (!flip) { if (5.0f < fv) setYVelocity(P, (double)fv); } else if (fv < -5.0f) setYVelocity(P, (double)fv);
            updateCollide(c, 1, o);
            setObj(c, po::SlopeObj, o);
            const float a2 = std::atan(sh / sw);
            P.f32(0x5f8, a2);
            uint8_t cv = o->slopeUphill ? 1 : 0;
            P.u8(0x5fc, cv);
            if (P.u8(po::Sideways)) { cv = cv == 0; P.u8(0x5fc, cv); }
            if (cv != 0 && 1.0f <= a2) P.i32(0xb44, 3);
            // 0x4edb90 lines 630-646. A flipped player compares and divides the X components (local_114 = local_118; local_f4 = local_f8 -- GD reads the point's x where the upright case reads y),
            // so a slope that only moves in y never sets +0x630 for it (w_133224341_24310 t=24383: slopes 1727 / 1728 moving down, the game's +0x630 stays 0).
            if (o->cx != o->prevCx || o->cy != o->prevCy) {
                const float cur = flip ? o->cx : o->cy, last = flip ? o->prevCx : o->prevCy;
                const bool lt = flip ? (last < cur) : (cur < last), eq = cur == last;
                if (!lt && !eq) P.f64(0x630, (double)((cur - last) / dt));
            }
        } else {
            if (!flip) { if (0.0 <= dVar27) dVar27 = 0.0; } else if (dVar27 <= 0.0) dVar27 = 0.0;
            setYVelocity(P, dVar27);
            P.u8(po::G, 0);
            P.u8(po::G2, 0);
            updateCollide(c, 0, o);
        }
    }
    if (bVar22) {
        if (!flip) { if (-2.0 < P.f64(po::Vy)) setYVelocity(P, -2.0); }
        else if (P.f64(po::Vy) <= 2.0 && P.f64(po::Vy) != 2.0) setYVelocity(P, 2.0);
    }
    if (bVar18) {
        int iv;
        bool skip = false;
        if (!flip) { if (2.0 <= P.f64(po::Vy)) skip = true; iv = 1; }
        else { if (P.f64(po::Vy) <= -2.0) skip = true; iv = -1; }
        if (!skip) setYVelocity(P, (double)((float)iv + (float)iv));
    }
    P.f64(0x940, (double)ratio);
    const int iVs = flip ? -1 : 1;
    // The slope launch speed as 0x4eed9c..0x4eee55 computes it, read from the disassembly one instruction at a time: all single precision (the decompile shows 0.8 and 1.4 as doubles; they are the float constants).
    const float q = 0.800000011920929f / fAng;                       // divss xmm1, xmm13
    const float m = (1.100000023841858f < q) ? 1.100000023841858f : q;   // minss xmm0(1.1f), xmm1
    const float r14 = ratio * 1.399999976158142f;                    // mulss xmm2, [1.4f]
    float v = m * r14;                                                // mulss xmm0, xmm2
    if (bVar20 && !bVar21) v = v * -1.0f;                             // mulss xmm1, [-1.0f]
    const float gdLaunch = (float)iVs * v;                                  // cvtdq2ps; mulss
    P.f32(0x9bc, gdLaunch);
    if (P.u8(po::Platformer) && local_154 && P.u8(po::ModeUfo)) P.u8(0x98d, 0);
    const bool fb = P.u8(po::ModeShip) || P.u8(po::ModeUfo) || P.u8(po::ModeWave) || P.u8(po::ModeSwing) || P.u8(po::ModeBall);
    if (!fb) { if (c.slopeRotation) c.slopeRotation(c, dt); }
    else {
        P.f32(0x9bc, gdLaunch * 0.75f);
        if (!P.u8(po::ModeBall)) { if (c.slopeRotation) c.slopeRotation(c, dt); }
        else if (dVar26a != P.f64(0x938)) {
            float f28 = 1.0f / std::cos((float)P.f64(0x938));
            if (2.0f < f28) f28 = (f28 - 1.0f) * 0.7f + 1.0f;
            rotReset(P);
            runBallRotation(P, f28);
        }
    }
}


// ----------------------------------------------------------------------------------------------------------------------------------------
// the player's rect scaled about its centre (vtable +0x498(sx, sy)); the pass uses 0.3 for its "still inside" test
inline void playerRectScaled(Mem& P, float s, float& rx, float& ry, float& rw, float& rh) {
    // GameObject::getObjectRect(w, h) 0x294840 multiplies the hitbox words by the factors it is HANDED (+0x2fc * +0x488 * w, +0x48c * +0x300 * h); the cached rect is the same call with
    // the player's own scale (+0x394 / +0x398) as the factors. The small kill box is the call with 0.3, not 0.3 of the cached rect: a mini body's box is 9, not 5.4 (read in
    // collidedWithObjectInternal 0x4efd50, asm 0x4f186f..0x4f1884, by the static read of 2026-10-29).
    const auto [w, h] = plainPlayerRectSize(P, s, s);
    rx = P.f32(po::PosX) - w * 0.5f;
    ry = P.f32(po::PosY) - h * 0.5f;
    rw = w;
    rh = h;
}
inline void destroyBreakable(Ctx& c, GObj& o, bool platformerContact = false);
// PlayerObject::collidedWithObjectInternal 0x4efd50. A turned caller supplies object geometry rotated around the player.
// `rectArg` is null for the object's own rect, or x,y,w,h of the strip preSlopeCollision passes (then the object is not remembered: plVar28 = 0).
// Returns true where the decompile returns 1.
inline bool collidedWithObjectInternal(Ctx& c, float dt, GObj* Oarg, const float* rectArg, bool noOverlap) {
    Mem& P = c.P;
    const bool flip = P.u8(po::Upside) != 0;
    const GObj* O = rectArg ? nullptr : Oarg;                 // plVar28
    float pr[4];
    playerRect(P, pr[0], pr[1], pr[2], pr[3]);                 // local_1b0..: x, y, w, h
    float orc[4];                                              // local_198: the rect being tested
    if (rectArg) { orc[0] = rectArg[0]; orc[1] = rectArg[1]; orc[2] = rectArg[2]; orc[3] = rectArg[3]; }
    else { orc[0] = Oarg->rx; orc[1] = Oarg->ry; orc[2] = Oarg->rw; orc[3] = Oarg->rh; }
    const float ph = pr[3];                                    // local_1a4
    const bool flyM = P.u8(po::ModeShip) || P.u8(po::ModeUfo) || P.u8(po::ModeWave) || P.u8(po::ModeSwing);
    const bool flyBall = flyM || P.u8(po::ModeBall);
    float fVar30 = (!P.u8(po::Platformer) || P.u8(po::WasOnSlope) || P.u8(po::OnSlope)) ? 10.0f : 5.0f;
    double local_148 = (double)fVar30;
    if (0 < P.i32(0xaf8)) local_148 = 15.0;
    int iVar17 = flip ? -1 : 1;
    double dVar44 = (double)iVar17 * local_148;
    if (flyM && !P.u8(po::Platformer)) dVar44 = (double)((float)iVar17 * 6.0f);
    if (P.u8(po::WasOnSlope)) dVar44 = dVar44 + (double)iVar17 * P.f64(0x688);
    const bool passable = O ? O->passable : false;             // cVar27
    bool fastObj = false;                                      // local_158[0]
    bool bVar11 = false, bVar7 = false;
    float local_1a0 = 0.0f;
    // This block reads param_3, the real object, even when a strip rect was passed (preSlopeCollision gives the slope and a 1-px strip; plVar28 is then 0 for everything else): found at
    // w_133224341_24310 t=24312, where GD wrote +0x630 from a strip call on a moving slope.
    if (Oarg && 0.0f < dt) {
        const GObj* Om = Oarg;
        // A turned frame (+0x9cb up) reads the X pair: collidedWithObjectInternal 0x4efd50 L170-187 takes `fVar43 = lastPos.x, fVar30 = pos.x` when the
        // player is turned, and handleRotatedCollisionInternal gave the object's position and its +0x4d0 the same shift, so what is left is the world
        // motion along X (the frame's vertical), as postCollision's ground test above reads it. The y pair, read here before, is the motion across it.
        const bool turnedFrame = P.u8(po::Sideways) != 0;
        const float curY = turnedFrame ? Om->cx : Om->cy, prevY = turnedFrame ? Om->prevCx : Om->prevCy;
        if (Om->cx != Om->prevCx || Om->cy != Om->prevCy) {
            bool toVel = false;
            if (!flip) {
                if (curY <= prevY) { if (!(prevY < curY) && curY != prevY) bVar7 = true; }
                else toVel = true;
            } else {
                if (prevY <= curY) { if (!(curY < prevY) && curY != prevY) bVar7 = true; }
                else toVel = true;
            }
            if (toVel) {
                local_1a0 = (curY - prevY) / dt;
                fastObj = 5.0 < (double)std::fabs(local_1a0);
                dVar44 = dVar44 + (double)iVar17 * (double)std::fabs(local_1a0);
                P.f64(0x630, (double)local_1a0);
                bVar11 = true;
            }
        }
    }
    const bool bVar29 = !((P.i32(0xb84) < 1 && !P.u8(po::Platformer) && P.i32(0xb88) < 1)) && !passable;
    const float fy = P.f32(po::PosY);
    const float jumpDelta = P.u8(po::Sideways) ? P.f32(po::PosX) - P.f32(po::PrevX) : fy - P.f32(po::PrevY);   // 2.2091 0x4f0139..0x4f0161
    iVar17 = flip ? -1 : 1;
    const double dVar41 = (double)(fy - ph * 0.5f * (float)iVar17) + dVar44;      // the foot line now (+ margin)
    const double dVar36 = dVar41 - (double)jumpDelta;                              // ... and last tick
    iVar17 = flip ? 1 : -1;
    const double dVar44h = (double)(fy - (float)iVar17 * ph * 0.5f) - dVar44;      // the head line now (- margin)
    const double dVar33 = dVar44h - (double)jumpDelta;
    const float fMaxY = orc[1] + orc[3], fMinY = orc[1];
    const double dVar4 = (double)fMaxY;                                            // the object's top
    const double dVar42 = (double)fMinY;                                           // ... and bottom
    bool bVar12 = false;
    bool local_1c0 = true;
    const bool cVar24 = O && O->type == 0x15;
    const bool bVar8 = !(flyBall || P.u8(po::ModeRobot) || P.u8(po::ModeSpider));
    bool bVar6 = false, bVar9 = false, bVar10 = false;
    const GObj* Oc = O;
    bool platformerBreak = false;
    auto setPos = [&](float yy) { if (slopeDbgOn(P)) std::fprintf(slopeDbgOut(), "slopedbg setPos object uid %d type %d rect %.3f %.3f %.3f %.3f rectArg %d y %.4f -> %.4f\n", O ? O->uid : -1, O ? O->type : -1, (double)orc[0], (double)orc[1], (double)orc[2], (double)orc[3], rectArg ? 1 : 0, (double)P.f32(po::PosY), (double)yy); setPosition(P, P.f32(po::PosX), yy); };
    auto gflag = [&]() { return flip && flyM; };
    auto upd12d = [&](float v) {
        const double cur = P.f64(po::FloorBottomY);
        double nv = (double)v;
        if (cur != 0.0) { nv = cur; if (!flip) { if (cur <= (double)v) nv = (double)v; } else if ((double)v <= cur) nv = (double)v; }
        P.f64(po::FloorBottomY, nv);
    };
    auto upd300 = [&](float v) {
        const double cur = P.f64(po::CeilTopY);
        double nv = (double)v;
        if (cur != 0.0) { nv = cur; if (!flip) { if ((double)v <= cur) nv = (double)v; } else if (cur <= (double)v) nv = (double)v; }
        P.f64(po::CeilTopY, nv);
    };
    auto uidFloor = [&](int uid) { if (uid != 0 && uid != P.i32(po::CollisionBottomUid)) { P.i32(po::CollisionBottomUid, uid); c.botDict.insert(uid); } };
    auto uidCeil = [&](int uid) { if (uid != 0 && uid != P.i32(po::CollisionTopUid)) { P.i32(po::CollisionTopUid, uid); c.topDict.insert(uid); } };
    auto rememberObj = [&]() {
        if (Oc) {
            setObj(c, po::GroundObj, Oc);
            if (Oc->b51d) P.i32(0xaec, 2);
            if (Oc->b51e) P.i32(0xaf0, 2);
        }
    };
    auto tail = [&]() -> bool {          // L701-L741
        if (bVar8 && 0 < P.i32(0xb7c) && P.u8(0x7e8)) P.u8(0x98d, 0);
        if (!flyM || !bVar11) { if (bVar12) didHitHead(c); }
        else if (bVar12) { setYVelocity(P, P.f64(0x630)); didHitHead(c); }
        if (platformerBreak) destroyBreakable(c, *Oarg, true);   // 2.2091 0x4f1d93..0x4f1db0
        return true;
    };
    // the common "contact made" bookkeeping of the foot and the head sections (L588-L700, L792-L905)
    auto contact = [&](bool headBranch, bool b6) {
        if (bVar8) checkSnapJumpToObject(c, Oc);
        if (!bVar29 || b6) {
            const bool p3 = headBranch ? (!flip && flyM) : gflag();
            if (bVar12) hitGroundNoJump(c, p3); else hitGround(c, Oc, p3);
        } else P.f64(po::Vy, 0.0);
        const int uid = Oc ? Oc->uid : 0;
        if (!flip) {
            if (bVar12) { if (!cVar24 && !passable) { upd300(fMinY); uidCeil(uid); } }
            else { upd12d(fMaxY); uidFloor(uid); rememberObj(); P.f64(0x630, (double)local_1a0); }
        } else {
            if (!bVar12) { upd12d(fMinY); uidFloor(uid); rememberObj(); P.f64(0x630, (double)local_1a0); }
            else if (!cVar24 && !passable) { upd300(fMaxY); uidCeil(uid); }
        }
    };
    const bool waveSkip = P.u8(po::ModeWave) && P.i32(0xb80) < 1;
    bool result = false;
    bool resolved = false;      // a branch returned 1
    if (!waveSkip) {
        if (!(P.u8(po::OnSlope) == 0 && P.u8(po::WasOnSlope) == 0 && c.touched.empty())) {
            // L289-370: the ramps this tick has touched (the map at +0x6e8, by uid): is one of them over this solid and over the player (bVar10), with its surface
            // under the solid's top (bVar6) or over its bottom (bVar9)? Such a solid is a wall of the ramp, not a floor or a ceiling of its own.
            const bool cVar25 = P.u8(po::OnSlope) || P.u8(po::WasOnSlope) || O == nullptr;
            std::set<int> touchedSorted(c.touched.begin(), c.touched.end());
            const int64_t cd0 = P.get<int64_t>(po::Cd0);
            for (int uid : touchedSorted) {
                GObj* q = c.byUid(uid);
                if (!q) continue;
                bool proceed = true;
                if (!cVar25 && (int64_t)q->uid + 1 != cd0) {
                    const float ox = O->cx;
                    if (P.u8(po::Sideways)) {
                        // The turned ramp's rect for this x-range gate (GameObject::getObjectRect, vtable +0x4a0) keeps the ramp's UNTURNED width about its turned
                        // centre, and a solid exactly half a width off the ramp's centre is inside it (the max edge is met with `<=`). a custom level t=9,903..9,906: a cube
                        // in the turned frame meets the 1.5 px plates uid 15643 / 15654 as a head contact 0.1 .. 0.7 px deep, where GD makes no contact. The ramp uid
                        // 15644 (60 x 30, 30 px off the plates' row) is the only ramp in the list and the plates' x is exactly its centre + 30 on every tick. With the
                        // turned 30-wide rect the stack test (bVar10 && bVar9) never ran; with this one the four ticks come out as GD's (the window's first
                        // difference moves from 9,903 to the slope launch at 9,908).
                        const float bw0 = (q->bw > 0.0f && q->bh > 0.0f) ? q->bw : q->w0;
                        const float mx0 = q->cx - bw0 * 0.5f;
                        proceed = (mx0 < ox) && (ox <= mx0 + bw0);
                    } else
                        proceed = (q->rx < ox) && (ox < q->rx + q->rw);
                }
                if (proceed) {
                    // the touched ramps' rects of the foot section's wall test come from GameObject::getObjectRect(1.2, 1.1), which scales the UNROTATED size and only then swaps width and height for an object turned a quarter
        // (m_isRotatedSide, +0x390): a ramp turned 90 / 270 is 1.1 times as wide and 1.2 times as tall as its rect (a custom level t=5082, the ramp 2420)
        const bool side = std::fabs(std::fmod(std::fabs(q->rot), 180.0f) - 90.0f) < 0.5f;   // m_isRotatedSide: the factors swap with the sides
                    const float fw = side ? 1.1f : 1.2f, fh = side ? 1.2f : 1.1f;
                    const float qx = q->rx + q->rw * 0.5f - q->rw * fw * 0.5f, qw = q->rw * fw;
                    const float qy = q->ry + q->rh * 0.5f - q->rh * fh * 0.5f, qh = q->rh * fh;
                    if (rectsIntersect(orc[0], orc[1], orc[2], orc[3], qx, qy, qw, qh)
                        && rectsIntersect(pr[0], pr[1], pr[2], pr[3], qx, qy, qw, qh)) {
                        bVar10 = true;
                        const bool top = slopeIsTop(q->slopeDir);
                        float ex;
                        if (!q->slopeUphill) ex = top ? orc[0] : orc[0] + orc[2];
                        else ex = top ? orc[0] + orc[2] : orc[0];
                        const double dVar33x = slopeYPos(*q, ex);
                        const bool isCd0 = (int64_t)q->uid + 1 == cd0;
                        if (dVar4 <= dVar33x + 2.0 && (!top || isCd0)) bVar6 = true;
                        if (dVar33x - 2.0 <= dVar42 && (top || isCd0)) bVar9 = true;
                    }
                }
                if (bVar6 && bVar9) break;
            }
        }
        bool bVar5;
        if (!flip || (flyBall && !passable)) {
            bVar5 = true;
            if (flip) bVar12 = dVar4 <= dVar41 ? true : !(dVar36 < dVar4);
            else bVar12 = dVar42 < dVar41 ? !(dVar42 < dVar36) : true;
        } else {
            bVar5 = false;
            bVar12 = dVar4 <= dVar41 ? true : !(dVar36 < dVar4);
        }
        platformerBreak = bVar12 && cVar24 && P.u8(po::Platformer) && (flip ? jumpDelta < 0 : jumpDelta > 0) && !P.u8(0x996) && (noOverlap || rectsIntersect(orc[0], orc[1], orc[2], orc[3], pr[0] + 2.5f, pr[1], pr[2] - 5.0f, pr[3]));
        // ---- the foot section, L436-L741; break gate: 2.2091 0x4f0231..0x4f0255, 0x4f06a3..0x4f06ea ----
        if (slopeDbgOn(P)) std::fprintf(slopeDbgOut(), "slopedbg foot uid %d: dVar44 %.4f dVar41(foot now) %.4f dVar36(foot prev) %.4f top %.4f bottom %.4f flip %d bVar5 %d bVar29 %d bVar12 %d bVar11 %d fastObj %d vy %.4f Was %d On %d bVar10 %d bVar6 %d -> enter %d\n", O ? O->uid : -1, dVar44, dVar41, dVar36, dVar4, dVar42, (int)flip, (int)bVar5, (int)bVar29, (int)bVar12, (int)bVar11, (int)fastObj, P.f64(po::Vy), (int)P.u8(po::WasOnSlope), (int)P.u8(po::OnSlope), (int)bVar10, (int)bVar6, (int)((bVar5 || bVar29) && !(dVar41 < dVar4 && dVar36 < dVar4)));
        bool footDone = false;                       // reached "local_1c0 = false; goto LAB_0b32"
        if (bVar5 || bVar29) {
            double dVar39 = dVar36, dVar40 = dVar41;
            if (flip) { dVar39 = dVar33; dVar40 = dVar44h; }
            if (!(dVar40 < dVar4 && dVar39 < dVar4)) {
                if (!(flip && P.u8(po::ModeUfo) && cVar24 && !P.u8(po::Platformer))) {
                    dVar40 = P.f64(po::Vy);
                    if ((0.0 < dVar40 && P.u8(po::WasOnSlope) == 0 && !fastObj) || (bVar10 && bVar6)) {
                        if (!flip) {
                            if (!bVar12 && bVar11) {
                                setPos((float)((double)(ph * 0.5f) + dVar4));
                                hitGround(c, Oc, gflag());
                                upd12d(fMaxY);
                                uidFloor(Oc ? Oc->uid : 0);
                                rememberObj();
                                P.f64(0x630, (double)local_1a0);
                                setYVelocity(P, dVar40);
                                double nv = P.f64(po::Vy);
                                if (P.f64(po::Vy) <= P.f64(0x630)) nv = P.f64(0x630);
                                setYVelocity(P, nv);
                            }
                        } else if (bVar12 && bVar7) {
                            setPos((float)((double)(ph * 0.5f) + dVar4));
                            hitGroundNoJump(c);
                            if (!cVar24 && !passable) { upd300(fMaxY); uidCeil(Oc ? Oc->uid : 0); }
                            setYVelocity(P, (double)(float)dVar40);
                            double nv = P.f64(po::Vy);
                            if (nv <= P.f64(0x630) + 1.0) nv = P.f64(0x630) + 1.0;
                            setYVelocity(P, nv);
                            didHitHead(c); if (platformerBreak) destroyBreakable(c, *Oarg, true);   // 2.2091 0x4f0ac8..0x4f0ae5
                        }
                        footDone = true;
                    } else {
                        setPos((float)((double)(ph * 0.5f) + dVar4));
                        contact(false, bVar5);
                        result = tail();
                        resolved = true;
                    }
                } else {
                    float rx, ry, rw, rh;
                    playerRectScaled(P, 0.3f, rx, ry, rw, rh);
                    if (rectsIntersect(orc[0], orc[1], orc[2], orc[3], rx, ry, rw, rh)) { destroyBreakable(c, *Oarg); resolved = true; result = true; }   // 2.2091 0x4f0778 -> 0x4f1da1
                    else footDone = true;
                }
            }
        }
        if (footDone) local_1c0 = false;
        // ---- the head / underside section, L743-L1058 ----
        if (!resolved && local_1c0) {
            bool b6;
            bool skipHead = false;
            if (!flip && (!flyBall || passable)) {
                b6 = false;
                if (!bVar29) skipHead = true;
            } else b6 = true;
            if (!skipHead) {
                double hn = dVar44h, hp = dVar33;
                if (flip) { hn = dVar41; hp = dVar36; }
                const int uid16 = Oc ? Oc->uid : 0;
                bool go = false;
                if (hn <= dVar42 || hp <= dVar42) go = true;
                else if (!flip) { if (uid16 == P.i32(po::PrevTopUid)) go = true; }
                else if (uid16 == P.i32(po::PrevBottomUid)) go = true;
                if (go) {
                    bool brokeBlock = false;
                    if (!flip && P.u8(po::ModeUfo) && cVar24 && !P.u8(po::Platformer) && (hn <= dVar42 || hp <= dVar42 || uid16 == P.i32(po::PrevTopUid))) {
                        float rx, ry, rw, rh;
                        playerRectScaled(P, 0.3f, rx, ry, rw, rh);
                        if (rectsIntersect(orc[0], orc[1], orc[2], orc[3], rx, ry, rw, rh)) { destroyBreakable(c, *Oarg); resolved = true; result = true; brokeBlock = true; }   // 2.2091 0x4f0e50 -> 0x4f1da1
                        else local_1c0 = false;
                    }
                    if (!brokeBlock && !(local_1c0 == false && !flip && P.u8(po::ModeUfo) && cVar24)) {
                        const double vy = P.f64(po::Vy);
                        if (((0.0 <= vy) || P.u8(po::WasOnSlope) || fastObj) && (!bVar10 || !bVar9)) {
                            setPos((float)(dVar42 - (double)(ph * 0.5f)));
                            contact(true, b6);
                            result = tail();
                            resolved = true;
                        } else {
                            if (!flip) {
                                if (bVar12 && bVar7) {
                                    const double vy0 = P.f64(po::Vy);
                                    setPos((float)(dVar42 - (double)(ph * 0.5f)));
                                    hitGroundNoJump(c);
                                    if (!cVar24 && !passable) { upd300(fMinY); uidCeil(Oc ? Oc->uid : 0); }
                                    setYVelocity(P, vy0);
                                    double nv = P.f64(po::Vy);
                                    if (P.f64(0x630) - 1.0 <= nv) nv = P.f64(0x630) - 1.0;
                                    setYVelocity(P, nv);
                                    didHitHead(c); if (platformerBreak) destroyBreakable(c, *Oarg, true);   // 2.2091 0x4f10c0..0x4f10dd
                                }
                            } else if (!bVar12 && bVar11) {
                                const double vy0 = P.f64(po::Vy);
                                setPos((float)(dVar42 - (double)(ph * 0.5f)));
                                hitGround(c, Oc, false);
                                upd12d(fMinY); uidFloor(Oc ? Oc->uid : 0); rememberObj();
                                P.f64(0x630, (double)local_1a0);
                                setYVelocity(P, vy0);
                                double nv = P.f64(po::Vy);
                                if (P.f64(0x630) <= nv) nv = P.f64(0x630);
                                setYVelocity(P, nv);
                            }
                            local_1c0 = false;
                        }
                    }
                }
            }
        }
    }
    if (resolved) return result;
    // L1182-1228: the player is inside a solid
    if (local_1c0 && !P.u8(po::Platformer) && !passable) {
        float rx, ry, rw, rh;
        playerRectScaled(P, 0.3f, rx, ry, rw, rh);
        if (rectsIntersect(orc[0], orc[1], orc[2], orc[3], rx, ry, rw, rh)) {
            if (!bVar12 || P.f64(0x808) == 0.0 || 0.10000000149011612 <= P.f64(po::Time) - P.f64(0x808)) {
                if (P.u8(po::ModeSpider)) {
                    const float f30 = orc[0] + orc[2];
                    const float f31 = rx + rw;
                    // the spider's grace after a teleport: m_lastSpiderFlipTime is +0x828 on 22091 (+0x820 was the 2.2081 offset; gd_offsets.hpp kPlayerLastSpiderFlipTime, and 22091 collidedWithObjectInternal reads param_1[0x105] x 8)
                    if (f30 < f31 && P.f64(0x828) != 0.0 && P.f64(po::Time) - P.f64(0x828) < 0.03999999910593033) return true;
                }
                if (cVar24) { destroyBreakable(c, *Oarg); return true; }   // 2.2091 0x4f19bd -> 0x4f1da1, bypasses the +0x996 write
                if (P.u8(0x7ed) == 0) GDCOL_DIED(c); else if (P.u8(0xc4c) == 0) P.u8(0x98c, 1);
            } else {
                setPosition(P, P.f32(po::PosX), !flip ? (float)(dVar42 - (double)(ph * 0.5f)) : (float)((double)(ph * 0.5f) + dVar4));
                hitGround(c, O, true);
                P.u8(po::G2, 0);
            }
            return true;
        }
    }
    return false;
}


// FUN_140318cd0 (playerCircleCollision): an object with a radius (a saw) against the player's rect. `f1cf` is the byte at layer+0xdc8 -> +0x1cf.
//   0: hit when the circle's centre is inside the rect, or any of the rect's four corners is within the radius of the centre (a corner disc test:
//      a circle that crosses a rect's edge between two corners without covering one does not hit)
//   1: hit when the distance between the centres is within half the rect's width plus the radius
inline bool circleHit(Mem& P, const GObj& o, bool f1cf) {
    float rx, ry, rw, rh;
    playerRect(P, rx, ry, rw, rh);
    float R = o.radius;
    if (!(o.scaleX == 1.0f && o.scaleY == 1.0f)) { float m = o.scaleX; if (m <= o.scaleY) m = o.scaleY; R = m * o.radius; }
    if (!f1cf) {
        const float cx = o.cx, cy = o.cy;
        const float maxX = rx + rw, minX = rx, maxY = ry + rh, minY = ry;
        if (cx >= minX && cx <= maxX && cy >= minY && cy <= maxY) return true;
        const float cxs[4] = {maxX, maxX, minX, minX}, cys[4] = {maxY, minY, minY, maxY};
        for (int k = 0; k < 4; ++k) {
            const float dx = cxs[k] - cx, dy = cys[k] - cy;
            const float d = std::sqrt(dx * dx + dy * dy);
            if (!(R <= std::fabs(d))) return true;
        }
        return false;
    }
    const float dx = P.f32(po::PosX) - o.cx, dy = P.f32(po::PosY) - o.cy;
    return std::sqrt(dx * dx + dy * dy) <= rw * 0.5f + R;
}

// ----------------------------------------------------------------------------------------------------------------------------------------
// OBB2D (FUN_14007b480 builds it, FUN_14007bb70 is overlaps1Way): the test a rotated hazard gets after its box. Float arithmetic as the game's CCPoint operators do it.
struct Obb {
    float cx[4], cy[4];          // the corners (+0x140)
    float ax[2], ay[2];          // the axes (+0x180), scaled to 1 / length^2
    double org[2];               // corner 0 on each axis (+0x1a0)
};
inline Obb makeObb(float px, float py, float w, float h, float ang) {
    Obb b;
    const float s = std::sin(ang), co = std::cos(ang);
    const float xx = co * (w * 0.5f), xy = s * (w * 0.5f);        // the half-width vector
    const float yx = -s * (h * 0.5f), yy = co * (h * 0.5f);       // the half-height vector
    b.cx[0] = (px - xx) - yx; b.cy[0] = (py - xy) - yy;
    b.cx[1] = (px + xx) - yx; b.cy[1] = (py + xy) - yy;
    b.cx[2] = (px + xx) + yx; b.cy[2] = (py + xy) + yy;
    b.cx[3] = (px - xx) + yx; b.cy[3] = (py - xy) + yy;
    b.ax[0] = b.cx[1] - b.cx[0]; b.ay[0] = b.cy[1] - b.cy[0];
    b.ax[1] = b.cx[3] - b.cx[0]; b.ay[1] = b.cy[3] - b.cy[0];
    for (int a = 0; a < 2; ++a) {
        const float invLen2 = 1.0f / (b.ay[a] * b.ay[a] + b.ax[a] * b.ax[a]);   // OBB2D::calculateWithCenter 0x7b69f..0x7b710: divide once, then multiply each component.
        b.ax[a] *= invLen2; b.ay[a] *= invLen2;
        b.org[a] = (double)(b.ax[a] * b.cx[0] + b.ay[a] * b.cy[0]);
    }
    return b;
}
inline bool obbOverlaps1Way(const Obb& t, const Obb& o) {
    for (int a = 0; a < 2; ++a) {
        double lo = 0, hi = 0;
        for (int k = 0; k < 4; ++k) {
            const double d = (double)(t.ay[a] * o.cy[k] + t.ax[a] * o.cx[k]);
            if (k == 0) lo = hi = d; else { if (d < lo) lo = d; if (d > hi) hi = d; }
        }
        if (t.org[a] + 1.0 < lo || hi < t.org[a]) return false;
    }
    return true;
}
inline bool obbOverlap(const Obb& a, const Obb& b) { return obbOverlaps1Way(a, b) && obbOverlaps1Way(b, a); }
// The box of a rotated object: the dump carries the rect of its rotation (W, H) and the unrotated size (w0, h0); the box's own size is solved from
// W = w|cos| + h|sin|, H = w|sin| + h|cos| (near 45 degrees the system is singular and the unrotated size stands in).
inline bool objRotatedBox(const GObj& o) { return o.oriented && !(std::fmod(std::fabs(o.rot), 90.0f) == 0.0f); }
// An oriented box from its four corners (GD's getOrientedBox order, the one makeObb produces).
inline Obb obbFromCorners(const float* x, const float* y) {
    Obb b;
    for (int k = 0; k < 4; ++k) { b.cx[k] = x[k]; b.cy[k] = y[k]; }
    b.ax[0] = b.cx[1] - b.cx[0]; b.ay[0] = b.cy[1] - b.cy[0];
    b.ax[1] = b.cx[3] - b.cx[0]; b.ay[1] = b.cy[3] - b.cy[0];
    for (int a = 0; a < 2; ++a) {
        const float invLen2 = 1.0f / (b.ay[a] * b.ay[a] + b.ax[a] * b.ax[a]);   // same native normalization as the generated box above
        b.ax[a] *= invLen2; b.ay[a] *= invLen2;
        b.org[a] = (double)(b.ax[a] * b.cx[0] + b.ay[a] * b.cy[0]);
    }
    return b;
}
inline Obb objObb(const GObj& o) {
    if (o.hasGc) {   // GD's own corners: the box need not stand on the rect's centre (a hitbox off the sprite's pivot), and its size is not solved from anything
        float x[4], y[4];
        const float dx = o.cx - o.gcx, dy = o.cy - o.gcy; const bool asCaptured = dx == 0.0f && dy == 0.0f && o.gscale == 1.0f;
        for (int k = 0; k < 4; ++k) { x[k] = asCaptured ? o.gc[2 * k] : o.gcx + (o.gc[2 * k] - o.gcx) * o.gscale + dx; y[k] = asCaptured ? o.gc[2 * k + 1] : o.gcy + (o.gc[2 * k + 1] - o.gcy) * o.gscale + dy; }   // OBB2D::overlaps1Way 0x7bb8f..0x7bbf7 reads the stored corners without recentering.
        return obbFromCorners(x, y);
    }
    const float ang = -o.rot * 0.017453292f;
    const float cs = std::fabs(std::cos(ang)), sn = std::fabs(std::sin(ang));
    const float det = cs * cs - sn * sn;
    float bw = o.w0, bh = o.h0;
    if (o.bw > 0.0f && o.bh > 0.0f) { bw = o.bw; bh = o.bh; }
    else if (std::fabs(det) > 0.002f) { bw = (o.rw * cs - o.rh * sn) / det; bh = (o.rh * cs - o.rw * sn) / det; }
    return makeObb(o.cx, o.cy, bw, bh, ang);
}
inline Obb playerObbAt(Mem& P, bool turned) {
    float qx, qy, qw, qh;
    if (P.u8(0x368)) {   // the oriented box (vtable +0x588) does not rebuild the cached rect (+0x358, dirty byte +0x368; it has its own, +0x369): the size is the words' as they stand, the cache is left for the next getObjectRect2. w_86407629_37350 t=37423: a ball portal and a size portal, both turned, in one pass
        qw = P.f32(0x2fc) * P.f32(0x488) * P.f32(0x394);
        qh = P.f32(0x300) * P.f32(0x48c) * P.f32(0x398);
        qx = P.f32(po::PosX) - qw * 0.5f;
        qy = P.f32(po::PosY) - qh * 0.5f;
    } else {
        playerRect(P, qx, qy, qw, qh);   // The AABB already swapped its sides; the OBB (0x29e598..0x29e607) is turned by angle instead.
        if (P.u8(0x390)) { std::swap(qw, qh); qx = P.f32(po::PosX) - qw * 0.5f; qy = P.f32(po::PosY) - qh * 0.5f; }
    }
    return makeObb(qx + qw * 0.5f, qy + qh * 0.5f, qw, qh, turned ? -P.f32(0x38) * 0.017453292f : 0.0f);   // PlayerObject::getOrientedBox: the node's rotation turns it
}
inline Obb playerObb(Mem& P) { return playerObbAt(P, true); }
// KEYOBS=1 (env, read once; diagnosis): the counts of what the search's key leaves out -- the `rotobs:` and `boost2obs:` lines cli.hpp prints with --phaseprof's (here, search_dp.hpp and
// progress.hpp). Off, nothing of them runs: under --phaseprof alone they cost a sort of every group's children (lv20, horizon 2500: the dedupe bucket 24.9 -> 32.1 s), and every survey run
// carries --phaseprof.
inline const bool g_keyObs = std::getenv("KEYOBS") != nullptr;
// KEYOBS (observation only; cli.hpp prints the `rotobs:` line): the questions the player's own angle (+0x38) answers. The angle is not in the search's key (search_key.hpp) though a
// seeded body takes it from the State, so two states the dedupe folds may answer these differently. Per site (0 a portal / pad / orb, 1 a hazard, 2 the shadow's "seen already" gate):
// the oriented-box questions asked, the ones answered "no overlap", and the ones whose answer is not the one the same box gives unturned (a lower bound of the answers the angle decided).
// ...and an upper bound of them: the questions some angle of the player's box could answer either way -- the object's box is farther from the player's centre than the circle inside the
// player's box (nearer, every angle overlaps) and no farther than the circle around it (farther, none does).
struct RotAskObs { std::atomic<long long> asks[3] = {}, miss[3] = {}, turnedOnly[3] = {}, open[3] = {}; };
inline RotAskObs g_rotAsk;
inline bool rotAskOverlap(int site, Mem& P, const GObj& o) {
    const Obb ob = objObb(o);
    const Obb pb = playerObb(P);
    const bool hit = obbOverlap(pb, ob);
    if (g_keyObs) {
        g_rotAsk.asks[site].fetch_add(1, std::memory_order_relaxed);
        if (!hit) g_rotAsk.miss[site].fetch_add(1, std::memory_order_relaxed);
        if (obbOverlap(playerObbAt(P, false), ob) != hit) g_rotAsk.turnedOnly[site].fetch_add(1, std::memory_order_relaxed);
        const double px = 0.5 * ((double)pb.cx[0] + (double)pb.cx[2]), py = 0.5 * ((double)pb.cy[0] + (double)pb.cy[2]);   // the player's centre
        const double e0x = (double)ob.cx[1] - ob.cx[0], e0y = (double)ob.cy[1] - ob.cy[0], e1x = (double)ob.cx[3] - ob.cx[0], e1y = (double)ob.cy[3] - ob.cy[0];
        const double l0 = e0x * e0x + e0y * e0y, l1 = e1x * e1x + e1y * e1y;
        const double u = l0 > 0.0 ? std::min(1.0, std::max(0.0, ((px - ob.cx[0]) * e0x + (py - ob.cy[0]) * e0y) / l0)) : 0.0;
        const double v = l1 > 0.0 ? std::min(1.0, std::max(0.0, ((px - ob.cx[0]) * e1x + (py - ob.cy[0]) * e1y) / l1)) : 0.0;
        const double d = std::hypot(px - ((double)ob.cx[0] + u * e0x + v * e1x), py - ((double)ob.cy[0] + u * e0y + v * e1y));   // from the centre to the object's box (0 inside it)
        const double w = std::hypot((double)pb.cx[1] - pb.cx[0], (double)pb.cy[1] - pb.cy[0]), h = std::hypot((double)pb.cx[3] - pb.cx[0], (double)pb.cy[3] - pb.cy[0]);
        if (d >= 0.5 * std::min(w, h) && d <= 0.5 * std::hypot(w, h)) g_rotAsk.open[site].fetch_add(1, std::memory_order_relaxed);
    }
    return hit;
}
// ----------------------------------------------------------------------------------------------------------------------------------------
// Portals and pads. What each does to the player's bytes is read from PlayerObject::flipGravity 0x4f8440, propellPlayer 0x4fdc90, bumpPlayer 0x4fdae0 and
// GJBaseGameLayer::bumpPlayer 0x31f080; the sprite / streak / event calls are left out (their words are masked by the test).
inline void unsup(Ctx& c, int key) { ++c.unsupported; ++c.unsupportedBy[key]; }

// PlayerObject::runNormalRotation 0x4eb4d0 (the rotation words only; the sprite action is not in the image)
inline void runNormalRotation(Mem& P, bool force, float f) {
    if (!force) {
        if (P.u8(po::ModeShip) || P.u8(po::ModeUfo) || P.u8(po::ModeWave) || P.u8(po::ModeSwing) || P.u8(po::ModeRobot) || P.u8(po::ModeSpider) || P.u8(0x9ec)) return;
        if (P.u8(0xb78) && !P.u8(0xb09) && !P.u8(0xb08) && !P.u8(0xb61) && !P.u8(0xb62)) return;
    }
    const float fVar4 = P.f32(0x9f8) == 1.0f ? 0.43333334f : 0.33333334f;
    P.u8(0x730, 1);
    const int iVar3 = P.u8(po::Upside) ? -0xb4 : 0xb4;
    const int iVar1 = P.u8(po::Leftward) ? -1 : 1;
    const int iVar2 = P.u8(po::Sideways) ? -1 : 1;
    P.f32(0x728, ((float)(iVar3 * iVar1) * (float)iVar2 * P.f32(0xb8c) * f) / fVar4);
}

// PlayerObject::runBallRotation 0x4eb600
inline void runBallRotation(Mem& P, float f) {
    if (P.u8(0x9ec) || P.u8(0xb78)) return;
    P.u8(0x670, f != 1.0f);
    P.f32(0x72c, f);
    const float fVar4 = P.f32(0x9f8) == 1.0f ? 1.0f : 0.8f;
    const float spd = P.f32(0x9fc);
    float fVar5 = 1.0f;
    if (spd == 0.7f) fVar5 = 1.2405638f;
    else if (spd == 1.1f) fVar5 = 0.80424345f;
    else if (spd == 1.3f) fVar5 = 0.6657693f;
    else if (spd == 1.6f) fVar5 = 0.5409375f;
    const int iVar3 = P.u8(po::Upside) ? -0x78 : 0x78;
    const int iVar2 = P.u8(po::Leftward) ? -1 : 1;
    fVar5 = (float)(iVar3 * iVar2) / (fVar4 * 0.2f * fVar5);
    P.f32(0x728, fVar5);
    if (P.u8(po::Sideways)) P.f32(0x728, fVar5 * -1.0f);
    P.u8(0x730, 1);
}

// PlayerObject::runBallRotation2 0x4eb730
inline void runBallRotation2(Mem& P) {
    if (P.u8(0x9ec)) return;
    const float fVar4 = P.f32(0x9f8) == 1.0f ? 1.0f : 0.8f;
    const float spd = P.f32(0x9fc);
    float fVar5 = 1.0f;
    if (spd == 0.7f) fVar5 = 1.2405638f;
    else if (spd == 1.1f) fVar5 = 0.80424345f;
    else if (spd == 1.3f) fVar5 = 0.6657693f;
    else if (spd == 1.6f) fVar5 = 0.5409375f;
    const int iVar3 = P.u8(po::Upside) ? 0x154 : -0x154;
    P.u8(0x731, 1);
    const int iVar2 = P.u8(po::Leftward) ? -1 : 1;
    P.f32(0x728, (float)(((double)(float)(iVar3 * iVar2) * (double)P.f32(0xb8c)) / ((double)fVar4 * 0.8 * (double)fVar5)));
}
// FUN_1404fe4a0: the streak is resumed (with the "hide trail" game variable and levelFlipping both false)
inline void streakResume(Mem& P, const Layer* L = nullptr) {
    if (L && levelFlipping(P, *L)) return;   // FUN_1404fe4a0 starts with !levelFlipping() (0x4f97c0): while the level turns the streak is not resumed (w_lv13_9210 t=9454: a UFO portal four steps after a mirror portal)
    if (!P.u8(0x733)) P.u8(0x980, 1);
}

// PlayerObject::flipGravity 0x4f8440
inline void playerFlipGravity
(Ctx& c, uint8_t up, bool p3) {
    Mem& P = c.P;
    (void)p3;
    if (P.u8(po::Upside) == up) return;
    P.u8(0x5a0, 0);                                     // FUN_1404fec10
    P.u8(po::Upside, up);
    P.f64(0x808, P.f64(po::Time));                      // param_1[0x101] = param_1[0x155]
    P.f64(po::FloorBottomY, 0.0);
    P.f64(po::CeilTopY, 0.0);
    P.u8(0xa31, 0);
    if (P.u8(po::WasOnSlope) || P.u8(po::OnSlope)) P.u8(po::Slope694, P.u8(po::Slope694) == 0 ? 1 : 0);
    c.topDict.clear();
    c.botDict.clear();
    P.put<int64_t>(0x5e0, -1);                          // m_lastCollision uids
    P.put<int64_t>(0x5d0, -1);
    P.put<int64_t>(0x5d8, -1);
    if (P.u8(0x7e9) == 0) P.f64(po::Vy, P.f64(po::Vy) * 0.5);   // the velocity is halved, unrounded
    // updatePlayerScale / updatePlayerArt: sprite. updatePlayerScale's setScaleX / setScaleY mark the oriented rect dirty (+0x369) and not the cached rect (+0x368): a spider's air press that turns the
    // gravity and finds no surface to teleport to ends with +0x369 = 1 and +0x368 as it was (btraw.csv, pushButton rows of a custom level; the same byte setPosition sets together with +0x368)
    P.u8(0x369, 1);
    if (!P.u8(po::ModeBall) && !P.u8(po::ModeWave) && P.u8(0x7e9) == 0) { P.u8(0x7eb, 1); streakResume(P, &c.L); }
    P.put<uint64_t>(0xa34, P.get<uint64_t>(0xa08));
    P.u8(po::G, 0);
    if (P.u8(po::ModeBall)) { rotReset(P); runBallRotation2(P); return; }
    if (P.u8(po::ModeSwing)) unsup(c, 1100);            // FUN_1404f9080
}

// PlayerObject::propellPlayer 0x4fdc90
inline void propellPlayer(Ctx& c, float coef, bool noEffects, int type) {
    Mem& P = c.P;
    (void)noEffects; (void)type;
    P.u8(po::Boost, 1);
    P.u8(po::G2, 0);
    P.u8(po::G, 0);
    P.u8(0x7eb, 1);
    P.put<uint16_t>(po::OnSlope, 0);
    const float f5 = P.f32(0x9f8) != 1.0f ? 0.8f : 1.0f;
    const int dir = P.u8(po::Upside) ? -1 : 1;
    setYVelocity(P, (double)((float)dir * coef * 16.0f * f5));
    if (P.u8(po::ModeBall) || P.u8(po::ModeSpider) || P.u8(po::ModeSwing)) P.f64(po::Vy, P.f64(po::Vy) * 0.6000000238418579);
    if (P.u8(0xa32) == 0 && P.u8(0x9ec) == 0) {
        rotReset(P);
        if (!P.u8(po::ModeBall)) runNormalRotation(P, false, 1.0f); else runBallRotation(P, 1.0f);
    }
    streakResume(P, &c.L);
    P.f32(0xa34, P.f32(po::PosX));
    P.f32(0xa38, P.f32(po::PosY));
}

// PlayerObject::togglePlayerScale 0x4fecb0: type 0x11 (to normal size, param_2 = 0) and 0x12 (to mini, 1). The node scale words and the sprites are left to the test's mask.
inline void togglePlayerScale(Ctx& c, bool mini) {
    Mem& P = c.P;
    const float cur = P.f32(0x9f8);
    if (!mini) {
        if (cur == 1.0f) return;
        if (P.u8(0xb78)) P.i32(0xaf8, 2);
        P.f32(0x9f8, 1.0f);
    } else {
        if (cur != 1.0f) return;
        P.f32(0x9f8, 0.6f);
    }
    P.f32(0x394, P.f32(0x9f8));
    P.f32(0x398, P.f32(0x9f8));
    if (!(P.u8(0x7ec) && P.u8(0x7e9) == 0)) {           // a CCScaleTo with an elastic ease animates the node's scale; otherwise setScaleX / setScaleY at once
        P.f32(0x40, P.f32(0x9f8));
        P.f32(0x44, P.f32(0x9f8));
    }
    if (P.u8(po::ModeBall) && P.u8(0x730) && !P.u8(0xa32) && !P.u8(0x9ec)) { rotReset(P); runBallRotation(P, 1.0f); }
    P.u8(0x5a0, 0);                                     // FUN_1404fec10
}
// GameObject::isFacingDown 0x29e820 / isFacingLeft 0x29e8e0
inline bool objFacingDown(const GObj& o) {
    const int r = (int)o.rot;
    bool b = false;
    if (r != (r / 90) * 90) {
        if ((unsigned)(r - 0x5b) < 0xb3u || (unsigned)(r + 0x10d) < 0xb3u) b = true;
        return o.flipY ? !b : b;
    }
    const int a = r < 0 ? -r : r;
    return o.flipY ? (a != 180) : (a == 180);
}
inline bool objFacingLeft(const GObj& o) {
    const int r = (int)o.rot;
    bool b = false;
    if (r != (r / 90) * 90) {
        if ((unsigned)(r - 0x5b) < 0xb3u || (unsigned)(r + 0x10d) < 0xb3u) b = true;
        return o.flipY ? !b : b;
    }
    const int v = r >= 0 ? r : r + 360;
    return (v == 270 && !o.flipY) || (v == 90 && o.flipY);
}

// (moved here from buttons.hpp: the pads' bumpPlayer / gravity pad call reversePlayer too)
inline void setLeftward(Mem& P, uint8_t nw) {
    const uint8_t old = P.u8(0x9ca);
    P.u8(0x9ca, nw);
    if (old != nw) P.i32(0x9d0, 2);
    if (old != nw && P.u8(po::ModeWave) && P.u8(0x980)) P.u8(0x5a0, 0);   // FUN_1404fec10
    if (P.u8(po::ModeBall) && P.u8(0xa32) == 0 && P.u8(0x9ec) == 0) {
        P.put<uint16_t>(0x730, 0); P.u8(0x670, 0); P.put<uint32_t>(0x728, 0);
        runBallRotation(P, 1.0f);
    }
}
// PlayerObject::reversePlayer 0x4f6740 (a reverse orb / pad, ringJump line 83): not on a platformer, the pending x offset +0x9d8 grows by twice the orb's distance from the player's
// displayed position (x + the offset) when +0x562 is set, then the direction flips. Measured on a_136107503_12121 t=12006: +0x9d8 -47.432 -> 117.2701, orb at 13635, player 13600.064.
inline void reversePlayerPort(Ctx& c, GObj* ring) {
    Mem& P = c.P;
    if (P.u8(0xb78)) return;
    if (ring && P.u8(0x562)) {
        const float pend = (float)P.f64(0x9d8);
        // GD Win 2.2091: reversePlayer calls getRealPosition at RVA 0x4f67bd, not the rect centre.
        const float dx = (float)ring->realX() - (P.f32(po::PosX) + pend);
        const float dy = (float)ring->realY() - (P.f32(po::PosY) + pend);
        const float d = P.u8(po::Sideways) ? dy : dx;
        P.f64(0x9d8, (double)(d + d) + P.f64(0x9d8));
    }
    setLeftward(P, P.u8(0x9ca) == 0 ? 1 : 0);
}

// the gravity pad (type 10) of collisionCheckObjects
inline void gravityPad(Ctx& c, GObj* o) {
    Mem& P = c.P;
    uint8_t up = (P.u8(po::Sideways) == 0 ? objFacingDown(*o) : objFacingLeft(*o)) ? 1 : 0;
    up ^= 1;
    if (P.u8(po::Upside) == up) return;
    if (o->reverse) reversePlayerPort(c, o);   // reversePlayer (collisionCheckObjects 0x31ccda)
    P.f32(0xa08, o->cx); P.f32(0xa0c, o->cy - 10.0f);
    setObj(c, 0xa88, o);
    propellPlayer(c, 0.800000011920929f, false, 10);
    playerFlipGravity(c, up, true);
    dualFlipSite(c, up ^ 1, true);                     // GJBaseGameLayer::flipGravity 0x319e20: the partner (counted as not ported without --shadowdual)
    P.u8(0x7e8, 1);
}
// GJBaseGameLayer::bumpPlayer 0x31f080 (the pads of type 8, 9, 0x22; 0x2c is the spider's)
inline void padBump(Ctx& c, GObj* o) {
    Mem& P = c.P;
    const int type = o->type;
    P.f32(0xa08, o->cx);
    P.f32(0xa0c, o->cy - 10.0f);
    uint32_t bits = 0x3f800000;
    float coef;
    if (type == 9) {
        if (P.u8(po::ModeShip)) bits = 0x3eb33333;
        else if (P.u8(po::ModeUfo)) bits = 0x3ecccccd;
        else if (P.u8(po::ModeBall)) bits = 0x3f333333;
        else if (P.u8(po::ModeSpider)) bits = 0x3f333333;
        else bits = 0x3f266666;
    } else if (type == 0x22) {
        const bool big = P.f32(0x9f8) >= 1.0f;
        if (P.u8(po::ModeShip)) bits = big ? 0x3f2147ae : 0x3f733333;
        else if (P.u8(po::ModeUfo)) bits = big ? 0x3f19999a : 0x3f7ae148;
        else bits = 0x3fa00000;
    }
    std::memcpy(&coef, &bits, 4);
    setObj(c, 0xa88, o);
    if (o->reverse) reversePlayerPort(c, o);   // GJBaseGameLayer::bumpPlayer 0x31f080: reversePlayer(player, pad) before PlayerObject::bumpPlayer
    // PlayerObject::bumpPlayer 0x4fdae0
    if (P.u8(0xb78) || !P.u8(0xbb8)) P.u8(0x9a4, 1);
    propellPlayer(c, coef, false, type);
    P.u8(0x95a, 0);
    if (type == 0x22) P.u8(0x95a, 1);                   // + the point at 0xa34 := DAT_14090b428 (a constant)
    // (0x160 vtable call for the plain cube: sprite)
}

// Spider pad (0x2c), GD Win 2.2091: GJBaseGameLayer::bumpPlayer tests pad+0x704 at RVA 0x31f1de and calls reversePlayer (0x4f6740) at 0x31f1ed, before PlayerObject::bumpPlayer (0x4fdae0) enters the spider search (0x4f25f0).
inline void padSpider(Ctx& c, GObj* o) {
    Mem& P = c.P;
    P.f32(0xa08, o->cx); P.f32(0xa0c, o->cy - 10.0f);
    setObj(c, 0xa88, o);
    if (o->reverse) reversePlayerPort(c, o);
    if (P.u8(0xb78) || !P.u8(0xbb8)) P.u8(0x9a4, 1);
    const bool facing = P.u8(po::Sideways) ? objFacingLeft(*o) : objFacingDown(*o);
    if (facing != (P.u8(po::Upside) != 0)) playerFlipGravity(c, P.u8(po::Upside) ? 0 : 1, true);
    if (c.spiderJump) c.spiderJump(c); else unsup(c, 2005);
}

// The push of a force block (FUN_140662b00 / calculateForceToTarget): a direction (the block's rotation, or towards the block when the force is relative) times a magnitude (the block's force,
// or a lerp of its min / max by how deep the player is in its radius). Read from the decompile, whose float arguments are lost: checked against +0xb94 / +0xb98 of the windows that have one.
inline void forceBlockPush(Ctx& c, const GObj& o, float& vx, float& vy) {
    Mem& P = c.P;
    float r = o.radius;
    if (!(o.radius > 0.0f)) r = std::max(o.rw, o.rh) * 0.5f;
    else if (!(o.scaleX == 1.0f && o.scaleY == 1.0f)) r = std::max(o.scaleX, o.scaleY) * r;
    if (!(r > 0.0f)) r = 1.0f;
    const float px = P.f32(po::PosX), py = P.f32(po::PosY);
    float ang;
    if (!o.fbRelative) ang = ((90.0f - o.rot) + (o.flipY ? 180.0f : 0.0f)) * 0.017453292f;   // vtable +0x500 is GameObject's vertical flip
    else ang = (float)((double)std::atan2(o.cy - py, o.cx - px) + (o.flipY ? 0.0 : 3.141592653589793));
    float mag = o.fbForce;
    if (o.fbRange) {
        float t = 0.0f;
        if (!o.fbRelative) {
            const float dx = o.cx - px, dy = o.cy - py;
            t = ((std::sin(ang) * dy + std::cos(ang) * dx + r) / r) * 0.5f;
        } else {
            const float dist = std::sqrt((o.cx - px) * (o.cx - px) + (o.cy - py) * (o.cy - py));
            if (dist < r) t = 1.0f - dist / r;
        }
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        mag = o.fbMin + (o.fbMax - o.fbMin) * t;
    }
    vx = std::cos(ang) * mag;
    vy = std::sin(ang) * mag;
}

// PlayerObject::redirectPlayerForce (22091 0x4fe0a0, called by teleportPlayer when the portal's redirect flag is set): the body's velocity (+0xb00 x, +0x9a8 y; floats from the doubles) turned by the
// closest-direction difference to `ang`, times `rmod`, held at `rmax` when above it or lifted to `rmin` when below it (a still body goes along `ang`); written back with the jump flag +0x95a.
inline void redirectPlayerForce(Ctx& c, float ang, float rmod, float rmin, float rmax) {
    Mem& P = c.P;
    float x = (float)P.f64(0xb00), y = (float)P.f64(po::Vy);
    const float t = ang * 0.017453292f;
    const float half = 3.1415927f;
    float a = t - std::atan2(y, x);
    float d = a;                                           // GJBaseGameLayer::convertToClosestDirection 0x3373e0
    if (a < -half) d = std::ceil(std::floor(std::fabs(a) / half) * 0.5f) * (half + half) + a;
    else if (half < a) d = a - std::ceil(std::floor(std::fabs(a) / half) * 0.5f) * (half + half);
    if (d != 0.0f) {
        const float cs = std::cos(d), sn = std::sin(d);
        const float nx = cs * x - sn * y;
        const float ny = cs * y + sn * x;
        x = nx; y = ny;
    }
    x *= rmod; y *= rmod;
    const float len = std::sqrt(y * y + x * x);
    if (rmax > 0.0f && len > rmax) { const float s = rmax / len; x *= s; y *= s; }
    else if (rmin > 0.0f && len < rmin) {
        if (len != 0.0f) { const float s = rmin / len; x *= s; y *= s; }
        else { y = std::sin(t) * rmin; x = std::cos(t) * rmin; }
    }
    if (P.u8(0x9cb)) std::swap(x, y);
    P.u8(0x95a, 1);
    P.f64(po::Vy, (double)y);
    if (P.u8(0xb78)) { P.u8(0xb9c, 1); P.f64(0xb00, (double)x); }
}

// GJBaseGameLayer::teleportPlayer 0x315f20, the part that is the player's bytes: the exit (the linked half's position; an id-747 portal puts the body at its own x and the portal's
// target y), the saved offset (target minus how far the body is from the portal), the keeps, the gravity mode, the grace words. A force/velocity the portal sets is counted.
// --tpexitmove in the shadow's pass (step.hpp sets the hook to tpExitShift; selfshadow.hpp sets the tick at the head of each step, player 2's pass of the same step reads it too):
// how far the Moves that carry a linked exit have taken it. GJBaseGameLayer::teleportPlayer sends the body to the exit object's place at the call; the dump holds its place at load.
// [M] a custom level uid 191 (exit uid 204, moved +1695 in y by an x-crossing Move at tick 1036): GD's player 2 is at y 4365 in the row of tick 1095, the dump's exit 2670 (the shadow's pass used it).
inline double (*g_tpExitShiftHook)(int uid, long long t, bool yAxis) = nullptr;
inline double (*g_tpExitTurnHook)(int uid, long long t) = nullptr;
inline thread_local long long g_tpExitTick = -1;
inline void setDashBodyRotation(Mem& P);

inline void redirectDash(Ctx& c, float degrees) {
    // GD Win 2.2091: teleportPlayer 0x3167cf..0x31695a rotates the existing float vector without rescaling it.
    Mem& P = c.P;
    float x = (float)P.f64(0x570), y = (float)P.f64(0x578);
    float delta = degrees * 0.017453292f - std::atan2(y, x);
    // RVA 0x3373e0 folds the angular difference before sin/cos.
    constexpr float pi = 3.1415927410125732f;
    if (std::fabs(delta) > pi) {
        const float turns = std::ceil(std::floor(std::fabs(delta) / pi) * 0.5f);
        delta -= std::copysign(turns * (pi + pi), delta);
    }
    if (delta != 0.0f) {
        const float sn = std::sin(delta), cs = std::cos(delta);
        const float nx = cs * x - sn * y;
        y = sn * x + cs * y;
        x = nx;
    }
    const bool side = P.u8(po::Sideways) != 0;
    const int wrapped = ((int)(-(std::atan2(y, x) * 57.29578f)) - (side ? 90 : 0)) % 360;
    P.f64(0x580, -(wrapped < 0 ? wrapped + 360 : wrapped));
    if (side) std::swap(x, y);
    P.f64(0x570, x); P.f64(0x578, y);
    if (x < 0.0f) setLeftward(P, !side);
    else if (x > 0.0f) setLeftward(P, side);
    setDashBodyRotation(P);
    if (c.restartDashSpin) c.restartDashSpin(c);
}

inline void teleportPortal(Ctx& c, GObj* o) {
    Mem& P = c.P;
    P.u8(0x560, 1);
    const bool dashRedirect = (o->tpFm & kTpRedirectDash) != 0;
    const bool dashUsesEntry = (o->tpFm & kTpDashUsesEntryAngle) != 0;
    // At RVA 0x3165cd the resolved pointer, not the target coordinates, distinguishes a linked exit.
    const bool hasTarget = dashRedirect ? !dashUsesEntry : (o->objectId == 747 || o->tpEx != 0.0f || o->tpEy != 0.0f);
    // GD Win 2.2091: teleportPlayer's missing-target branch (RVA 0x3161d9) bypasses contact and position writes, but still applies gravity and force.
    if (hasTarget) {
        const float px = P.f32(po::PosX), py = P.f32(po::PosY);
        P.f32(0xa08, o->cx); P.f32(0xa0c, o->cy);
        setObj(c, 0xa88, o);
        P.f32(0xc44, 0.0f);
        float tx, ty;
        if (o->objectId == 747) { tx = px; ty = o->tpY; ty += o->cy - o->cy0; } else { tx = o->tpEx; ty = o->tpEy; }
        if (o->objectId != 747 && g_tpExitShiftHook && g_tpExitTick >= 0) {
            tx += (float)g_tpExitShiftHook(o->uid, g_tpExitTick, false); ty += (float)g_tpExitShiftHook(o->uid, g_tpExitTick, true);
        }
        if (o->tpSave) {
            // GD Win 2.2091: teleportPlayer reads the entry's live getRealPosition at RVA 0x316157, then subtracts its offset at 0x316176.
            // A translation moves the box centre and the real position by the same amount.
            tx -= (o->tpRX + (o->cx - o->cx0)) - px;
            ty -= (o->tpRY + (o->cy - o->cy0)) - py;
        }
        if (o->tpKeepX) tx = px;
        if (o->tpKeepY) ty = py;
        c.tpLog.push_back({o->uid, px, tx});
        setPosition(P, tx, ty);
        if (o->objectId == 3022) { P.f32(0xa00, tx); P.f32(0xa04, ty); }   // a Teleport trigger: the previous-position floats read the new position (measured: w_143308401_3030 t=3176; a portal's leave them)
        P.u8(0x369, 1);
    }
    if (o->tpGrav == 1) playerFlipGravity(c, 0, true);
    else if (o->tpGrav == 2) playerFlipGravity(c, 1, true);
    else if (o->tpGrav == 3) playerFlipGravity(c, P.u8(po::Upside) ? 0 : 1, true);
    if (hasTarget) {   // RVA 0x3165cd skips these writes when no exit was resolved.
        P.u8(0xa14, 0);
        P.u8(0x5a0, 0);   // FUN_1404fec10 (the streak restarts at the new position)
    }
    const bool redirectsDash = dashRedirect && P.u8(0x9ec);
    float forceAngle = o->tpAng;
    if (dashUsesEntry) {
        // Without an exit, teleportPlayer instead reads the entry's live rotation at 0x316583.
        forceAngle = (o->flipX ? 180.0f : 0.0f) - o->rot;
    } else if (((o->tpFm & 5) || redirectsDash) && g_tpExitTurnHook && g_tpExitTick >= 0) {
        // GD Win 2.2091: teleportPlayer subtracts the exit's live rotation at RVAs 0x3162db..0x3162dd.
        const double turn = g_tpExitTurnHook(o->uid, g_tpExitTick);
        if (turn != 0.0) forceAngle = (float)((double)forceAngle - turn);
    }
    if (o->tpFm & 4) redirectPlayerForce(c, forceAngle, o->tpRmod, o->tpRmin, o->tpRmax);
    else if (o->tpFm & 1) {   // the static force (teleportPlayer 0x315f20 after the streak call: +0x759 set); +0x75c is the strength, +0xef adds to the velocity instead of setting it
        const bool add = (o->tpFm & 2) != 0;
        if (o->tpF == 0.0f && !add) { P.u8(0x95a, 0); P.f64(po::Vy, 0.0); if (P.u8(0xb78)) { P.f64(0xb00, 0.0); P.u8(0xb9c, 0); } }
        else {
            const float a = forceAngle * 0.017453292f;
            const float px = std::cos(a), py = std::sin(a);                         // ccpForAngle
            const float len = std::sqrt(py * py + px * px);
            if (0.0f < len) {
                const float s = o->tpF / len;
                float x = px * s, y = py * s;
                if (P.u8(0x9cb)) std::swap(x, y);
                P.u8(0x95a, 1);
                double vy = (double)y; if (add) vy += P.f64(po::Vy);
                P.f64(po::Vy, vy);
                if (P.u8(0xb78)) { double vx = (double)x; if (add) vx += P.f64(0xb00); P.f64(0xb00, vx); P.u8(0xb9c, 1); }
            }
        }
    }
    if (redirectsDash) redirectDash(c, forceAngle);
    if (c.L.dual) unsup(c, 1101);
}

// ----------------------------------------------------------------------------------------------------------------------------------------
// The mode portals. PlayerObject::toggleFlyMode 0x4f8790 / toggleBirdMode 0x4f8ad0 / toggleDartMode 0x4f9230 / toggleRollMode 0x4f9840, switchedToMode 0x4fa150,
// resetPlayerIcon 0x4f95b0. Robot, spider and swing are not ported (they count as unsupported).
enum : int { kShip = 0x9c1, kUfo = 0x9c2, kBall = 0x9c3, kWave = 0x9c4, kRobot = 0x9c5, kSpider = 0x9c6, kSwing = 0x9cc };

inline void hitboxPending(Mem& P) {      // a mode change rebuilds the cached rect at once (a size portal leaves it for the next setPosition)
    P.f32(0x360, P.f32(0x2fc) * P.f32(0x9f8));
    P.f32(0x364, P.f32(0x300) * P.f32(0x9f8));
}

// `rebuild` false: leaving the spider leaves the cached rect as it was (no setPosition marks it dirty in that pass; measured: the cache keeps the spider's 27 x 27 after a cube / robot / ball portal)
inline void resetPlayerIcon(Mem& P, bool rebuild = true) {
    P.f32(0x2fc, 30.0f); P.f32(0x300, 30.0f); P.f32(0xab8, 30.0f);
    if (P.u8(0xa32) == 0 && P.u8(0x9ec) == 0) {
        rotReset(P);
        if (!P.u8(po::ModeBall)) runNormalRotation(P, false, 1.0f); else runBallRotation(P, 1.0f);
    }
    if (P.u8(0x980)) P.u8(0x980, 0);
    if (rebuild) hitboxPending(P);
}

// PlayerObject::setRotation 0x501e70 on the image (CCNode keeps the angle twice; a change marks both rect caches dirty, sets +0x350 and decides the 90-degree flag +0x390)
inline void setNodeRotation(Mem& P, float r) {
    if (P.f32(0x38) != r || P.f32(0x3c) != r) {
        P.put<uint16_t>(0x368, 0x101);
        P.u8(0x350, 1);
        P.f32(0x38, r);
        P.f32(0x3c, r);
        P.u8(0x390, (r == 90.0f || r == -90.0f || r == 270.0f || r == -270.0f) ? 1 : 0);
    }
}
inline void toggleMode(Ctx& c, int flag, bool on);
// switchedToMode: every other mode is switched off
inline void switchedToMode(Ctx& c, int keep) {
    for (int f : {kShip, kUfo, kBall, kWave, kRobot, kSpider, kSwing})
        if (f != keep) toggleMode(c, f, false);
}

inline void toggleMode(Ctx& c, int flag, bool on) {
    Mem& P = c.P;
    if ((P.u8(flag) != 0) == on) return;
    P.f64(0x7e0, P.f64(po::Time));
    P.u8(flag, on);
    if (flag == kRobot || flag == kSpider) {            // toggleRobotMode 0x4f99d0 / toggleSpiderMode 0x4f9d80: no velocity or ground change
        if (!on) { if (flag == kSpider) resetPlayerIcon(P, false); }
        else {
            switchedToMode(c, flag);
            if (flag == kSpider) { P.f32(0xab8, 27.0f); P.f32(0x2fc, 27.0f); P.f32(0x300, 27.0f); }   // (the cached rect is not rebuilt in this pass: measured, robot -> spider)
            P.f64(0x838, 1.5);
            rotReset(P);
            setNodeRotation(P, 0.0f);                       // vtable +0x160 with 0 (toggleRobotMode line 85, toggleSpiderMode line 89)
            if (P.u8(0x9ec)) {           // --shadowdashrot: the branch ends with 0x4f43c0 (GJBaseGameLayer::checkRepellPlayer in the bindings: it returns unless +0x9ec), whose part that turns the player puts the dash angle back
                float r = (float)((double)(P.u8(0x9cb) ? 90 : 0) - P.f64(0x580));
                if (P.u8(0x9ca)) r += 180.0f;
                setNodeRotation(P, r);
            }
        }
        rotReset(P);
        return;
    }
    if (on) switchedToMode(c, flag);
    if (flag == kBall) {
        if (!on) setNodeRotation(P, P.u8(po::Upside) ? 180.0f : 0.0f);   // toggleRollMode 0x4f9840 lines 36-44: leaving the ball calls vtable +0x160 (setRotation, argument lost in the decompile) after updatePlayerBirdFrame; the asm at 0x4f995e..0x4f997c reads +0x9c7 (neg al; sbb ecx, ecx; and ecx, 0xb4): 180 for an upside-down player, else 0; w_149725478_4510 t=4696 (ball -> cube portal: the game's rotation 0, the shadow's 952)
        rotReset(P);
        return;
    }
    rotReset(P);
    P.f64(po::Vy, P.f64(po::Vy) * 0.5);
    setNodeRotation(P, 0.0f);                           // vtable +0x160 (setRotation) with 0, in toggleFlyMode / toggleBirdMode / toggleDartMode / toggleSwingMode
    P.u8(po::G2, 0);
    P.u8(po::G, 0);
    P.u8(0x7eb, 0);
    if (flag == kSwing && on) unsup(c, 1201);           // checkRepellPlayer (the layer), the swing fire sprites
    if (!on) {
        resetPlayerIcon(P);
        if (flag == kSwing) setNodeRotation(P, P.u8(po::Upside) ? 180.0f : 0.0f);   // toggleSwingMode 0x4f8dd0: the off branch (0x4f9010) ends with setRotation(upside ? 180 : 0) (0x4f9026..0x4f9044); w_146917834_15070 t=15127 (swing -> cube portal, upside down: the game's 180)
    } else {
        if (flag == kWave) {
            const bool fromSpiderCache = P.f32(0x360) == 27.0f;   // the spider's 27 x 27 cache, left as it was when the spider switched off (resetPlayerIcon rebuild = false)
            P.f32(0xab8, 20.0f); P.f32(0x2fc, 10.0f); P.f32(0x300, 10.0f);
            if (!fromSpiderCache) hitboxPending(P);          // o_lv21_17810 t=17816 (spider -> wave): the cache still reads 27 after the pass and 10 after the next
            P.u8(0x5a0, 0);                             // FUN_1404fec10
        }
        streakResume(P, &c.L);
        P.u8(0x7ee, 0);
        if (flag == kShip) P.u8(0x7ef, 0);
    }
}

// The body angle set by PlayerObject::updateDashArt at 0x4f442a..0x4f4470 (not the spinning child's angle).
inline void setDashBodyRotation(Mem& P) {
    float rotation = (float)((P.u8(po::Sideways) ? 90.0 : 0.0) - P.f64(0x580));
    if (P.u8(po::Leftward)) rotation += 180.0f;
    setNodeRotation(P, rotation);
}
// the portals of type 5, 6, 0x10, 0x13, 0x1a (ship, cube, ball, UFO, wave)
inline void modePortal(Ctx& c, GObj* o) {
    Mem& P = c.P;
    P.f32(0xa08, o->cx); P.f32(0xa0c, o->cy);
    setObj(c, 0xa88, o);
    if (!dualWillSwitch(c, o->type) && c.L.dual) unsup(c, 1102);                        // FUN_14031a210 flips the second player's gravity to match
    switch (o->type) {
    case 5: switchedToMode(c, kShip); toggleMode(c, kShip, true); break;
    case 0x13: switchedToMode(c, kUfo); toggleMode(c, kUfo, true); break;
    case 0x1a: switchedToMode(c, kWave); toggleMode(c, kWave, true); break;
    case 0x10: switchedToMode(c, kBall); toggleMode(c, kBall, true); break;
    case 6:
        for (int f : {kShip, kUfo, kBall, kWave, kRobot, kSpider, kSwing}) toggleMode(c, f, false);
        // GD Win 2.2091: collisionCheckObjects calls updateDashArt at RVA 0x31c2c7 even if already a cube.
        if (P.u8(0x9ec)) {
            setDashBodyRotation(P);
            if (c.restartDashSpin) c.restartDashSpin(c);
        }
        break;
    case 0x1b: switchedToMode(c, kRobot); toggleMode(c, kRobot, true); break;
    case 0x21: switchedToMode(c, kSpider); toggleMode(c, kSpider, true); break;
    case 0x29: switchedToMode(c, kSwing); toggleMode(c, kSwing, true); break;
    }
}
// PlayerObject::ringJump 0x4f6f50's writes on the player's bytes (notes: pushbutton-state-table 3.3 / 4.3) once its gate has let the ring through. The velocity and the position are the
// next update's (it writes them); the flip of a gravity ring goes through flipGravity. A spider orb is taken to succeed (H cleared).
inline void ringJumpWrites(Ctx& c, int ty) {
    Mem& P = c.P;
    const bool hcl = P.u8(po::ModeBall) || P.u8(po::ModeSwing) || P.u8(po::ModeSpider);
    auto flip = [&]() { playerFlipGravity(c, P.u8(po::Upside) ? 0 : 1, true); };
    switch (ty) {
    case 0xb: case 0xc: case 0x23: case 0xd: case 0x1d:           // yellow, pink, red, blue, green
        P.u8(0x98e, 0);
        P.u8(po::Boost, 1);
        P.u8(po::G2, 0);
        P.u8(po::G, 0);
        if (hcl) P.u8(0x98d, 0);
        if (ty == 0xd || ty == 0x1d) flip();
        break;
    case 0x20:                                                      // drop
        P.u8(0x98e, 0);
        if (P.u8(po::ModeBall) || P.u8(po::ModeSwing)) P.u8(0x98d, 0);
        break;
    case 0x25: case 0x26:                                           // dash: no press register; 0x26 turns gravity
        if (ty == 0x26) flip();
        break;
    case 0x2b:                                                      // spider orb
        P.u8(0x98d, 0);
        break;
    case 0x24:                                                      // spawn orb
        if (c.p22) P.u8(0x98e, 0);
        break;
    case 0x2e:                                                      // teleport orb
        P.u8(0x98e, 0);
        break;
    default: return;
    }
    P.u8(0x848, 1);
    P.u8(ty == 0x24 ? 0x994 : ty == 0x2e ? 0x995 : 0x993, 1);       // the ring's slot (A / B / C)
    if (ty != 0x24 && ty != 0x2e) P.u8(0x7e8, 1);                   // class A: the press fired a ring
}
// ----------------------------------------------------------------------------------------------------------------------------------------
// ----------------------------------------------------------------------------------------------------------------------------------------
// The turned frame (the player''s Sideways byte, +0x9cb): PlayerObject::handleRotatedCollisionInternal 0x4ed4c0 turns the object (and the slopes the pass has registered, Ctx::touched) 90 degrees about the player
// (FUN_1404ef700), runs the upright frame''s collidedWithObjectInternal / collidedWithSlopeInternal, carries the displacement the player got back by the inverse turn, and turns the objects back
// (unrotateGameplayObject 0x4ef9d0). The player and his velocities are never turned.

// GameObject::determineSlopeDirection 0x299320: +0x444 from the flips and the rotation (a rotation that is not a quarter turn keeps it), +0x440 raised for the directions 0, 3, 6 and 7
inline void determineSlopeDirection(GObj& o) {
    const float r = (float)((int)o.rot % 360);
    const bool a180 = std::fabs(r) == 180.0f, a90 = (r == 90.0f || r == -270.0f), a270 = (r == -90.0f || r == 270.0f);
    const bool r0 = r == 0.0f;
    o.slopeUphill = false;
    int d = -1;
    if (!o.flipX && !o.flipY) d = r0 ? 0 : a180 ? 3 : a90 ? 4 : a270 ? 5 : -1;
    else if (!o.flipX && o.flipY) d = r0 ? 1 : a180 ? 2 : a90 ? 7 : a270 ? 6 : -1;
    else if (o.flipX && o.flipY) d = r0 ? 3 : a180 ? 0 : a90 ? 5 : a270 ? 4 : -1;
    else d = r0 ? 2 : a180 ? 1 : a90 ? 6 : a270 ? 7 : -1;
    if (d < 0) return;
    o.slopeDir = d;
    o.slopeUphill = (d == 0 || d == 3 || d == 6 || d == 7);
}

// the box''s own size (the unrotated hitbox): from the object''s fields, else solved from the rect of its rotation (objObb''s rule)
inline void ownBoxOf(const GObj& o, float& bw, float& bh) {
    bw = o.w0; bh = o.h0;
    if (o.bw > 0.0f && o.bh > 0.0f) { bw = o.bw; bh = o.bh; return; }
    const float ang = -o.rot * 0.017453292f;
    const float cs = std::fabs(std::cos(ang)), sn = std::fabs(std::sin(ang));
    const float det = cs * cs - sn * sn;
    if (std::fabs(det) > 0.002f) { bw = (o.rw * cs - o.rh * sn) / det; bh = (o.rh * cs - o.rw * sn) / det; }
}

// FUN_1404ef700: one object turned +90 degrees about the player''s position (floats, sinf / cosf of the float pi/2: cos is -4.37e-8, not 0), its rotation lowered by 90
inline void turnObjectAboutPlayer(GObj& o, float px, float py) {
    const float sn = std::sin(1.5707964f), cs = std::cos(1.5707964f);
    float bw, bh;
    ownBoxOf(o, bw, bh);
    auto turn = [&](float x, float y, float& nx, float& ny) {
        const float dx = x - px;
        nx = (cs * dx - sn * (y - py)) + px;
        ny = cs * (y - py) + sn * dx + py;
    };
    float nx, ny;
    turn(o.cx, o.cy, nx, ny);
    const float ddx = nx - o.cx, ddy = ny - o.cy;
    o.prevCx += ddx; o.prevCy += ddy;           // +0x4d0: the previous position takes the same shift (the motion is not turned)
    o.cx = nx; o.cy = ny;
    if (o.hasGc) {
        for (int k = 0; k < 4; ++k) { float qx, qy; turn(o.gc[2 * k], o.gc[2 * k + 1], qx, qy); o.gc[2 * k] = qx; o.gc[2 * k + 1] = qy; }
        float qx, qy; turn(o.gcx, o.gcy, qx, qy); o.gcx = qx; o.gcy = qy;
    }
    o.rot -= 90.0f;
    float W, H;
    const float rm = std::fmod(std::fabs(o.rot), 180.0f);
    if (rm == 0.0f) { W = bw; H = bh; }
    else if (rm == 90.0f) { W = bh; H = bw; }
    else { const float a = o.rot * 0.017453292f; const float cc = std::fabs(std::cos(a)), ss = std::fabs(std::sin(a)); W = bw * cc + bh * ss; H = bw * ss + bh * cc; }
    o.rw = W; o.rh = H;
    o.rx = o.cx - W * 0.5f; o.ry = o.cy - H * 0.5f;
    if (o.type == 0x19) determineSlopeDirection(o);
}

inline bool handleRotatedCollisionInternal(Ctx& c, float dt, GObj* O, const float* rectArg, bool noOverlap, bool forced, bool slope) {
    Mem& P = c.P;
    const float x0 = P.f32(po::PosX), y0 = P.f32(po::PosY);
    std::vector<std::pair<GObj*, GObj>> saved;
    auto turnOne = [&](GObj* o) {
        for (const auto& s : saved) if (s.first == o) return;
        saved.push_back({o, *o});
        turnObjectAboutPlayer(*o, x0, y0);
    };
    turnOne(O);
    for (int u : c.touched) if (GObj* t = c.byUid(u)) turnOne(t);
    bool r = false;
    if (!slope) r = collidedWithObjectInternal(c, dt, O, rectArg, noOverlap);
    else collidedWithSlopeInternal(c, dt, O, forced);
    const float x1 = P.f32(po::PosX), y1 = P.f32(po::PosY);
    setPosition(P, (y1 - y0) + x0, y0 - (x1 - x0));
    for (const auto& s : saved) {                 // unrotateGameplayObject: the geometry as it was; what the pass marked on the object stays
        GObj& o = *s.first; const GObj& g = s.second;
        o.cx = g.cx; o.cy = g.cy; o.prevCx = g.prevCx; o.prevCy = g.prevCy; o.rx = g.rx; o.ry = g.ry; o.rw = g.rw; o.rh = g.rh; o.rot = g.rot;
        o.slopeDir = g.slopeDir; o.slopeUphill = g.slopeUphill; o.gcx = g.gcx; o.gcy = g.gcy;
        for (int k = 0; k < 8; ++k) o.gc[k] = g.gc[k];
    }
    return r;
}
// GJBaseGameLayer::destroyObject (2.2091 0x31d460) marks +0x3d2 at 0x31d536; collisionCheckObjects skips it (0x31bdc8).
// It emits DestroyBlock at 0x31d568..0x31d575. Platformer head contacts first set +0x996, cleared at update end (0x4e817c).
// Do not unfile the object or mark it inactive: staticObjectsInRect still tests its box.
inline void destroyBreakable(Ctx& c, GObj& o, bool platformerContact) {
    if (platformerContact) c.P.u8(0x996, 1);
    o.activated = true;
    if (c.actLog) c.actLog->push_back(&o);
    if (c.gameEvents) c.gameEvents->push_back({gdev::gdEventOfType(o.type), o.uid, o.type});
}
// Does the pass's answer for this object depend on whether the contact is the first one (`fresh`)? Only the types whose case in collisionCheckObjects reads it
// (portals, pads, mirrors, size / swing / dual portals, speed portals: selfshadow.hpp takes the speed from a fresh 0x14 fired entry) do. The Special objects (0x28), the
// touch triggers and coins (0x14 other than a speed portal, 0x2d, 0x16) and the rings take no once-gate decision of the shadow's: whether the seeding rule or
// the shadow's own list holds them changes nothing the pass writes, so a reverse pass over one is no reason to hand the tick back (Ctx::everGuess).
inline bool freshMatters(const GObj& o) {
    switch (o.type) {
    case 0x28: case 0x2d: case 0x16: return false;
    case 0xb: case 0xc: case 0xd: case 0x1d: case 0x20: case 0x23: case 0x24: case 0x25: case 0x26: case 0x2b: case 0x2e: return false;
    case 0x14: return o.objectId == 200 || o.objectId == 201 || o.objectId == 202 || o.objectId == 203 || o.objectId == 1334;
    default: return true;
    }
}
inline bool collidedWithObject(Ctx& c, float dt, GObj* O, const float* rectArg, bool noOverlap) {
    if (c.P.u8(po::Sideways)) return handleRotatedCollisionInternal(c, dt, O, rectArg, noOverlap, false, false);
    return collidedWithObjectInternal(c, dt, O, rectArg, noOverlap);
}
inline void collidedWithSlope(Ctx& c, float dt, GObj* o, bool forced) {
    if (c.P.u8(po::Sideways)) { handleRotatedCollisionInternal(c, dt, o, nullptr, false, forced, true); return; }
    const float bx = c.P.f32(po::PosX), by = c.P.f32(po::PosY);
    collidedWithSlopeInternal(c, dt, o, forced);
}
// GJBaseGameLayer::collisionCheckObjects 0x31bce0: the objects in the order the caller lists them. Solids and hazards are queued for the second pass;
// everything else is answered at once. Ramps, gravity portals and pads are ported; the rest are counted.
inline void collisionCheckObjects(Ctx& c, float dt, const std::vector<GObj*>& cand, std::vector<GObj*>& solids, std::vector<GObj*>& hazards) {
    Mem& P = c.P;
    float px, py, pw, ph;
    playerRect(P, px, py, pw, ph);
    // GJBaseGameLayer::checkCollisions calls collisionCheckObjects once per section cell (then once for the flat Extended Collision list) and each call reads the player's rect afresh at its head
    // (22091 collisionCheckObjects 0x31bce0: getObjectRect, the four bounds, before the object loop; inside the loop only the slope, teleport portal and teleport orb cases read it again). The candidates are in
    // call order, so the rect is read again where the next object belongs to another call: an object that moved the body (a spider orb's teleport, ringJump 0x2b) is met by the next cell's objects where the body now stands.
    int64_t callKey = INT64_MIN; bool rectStale = false;   // rectStale: an object passed the overlap tests since the rect was last read (see the loop's first line)
    const bool byCall = c.L.scaleX > 0.0f;
    auto callKeyOf = [&](const GObj& q) -> int64_t {
        if (q.extCol) return -1;
        auto cellOf = [](double v, float s) -> int { if (!(v > 0.0)) return 0; return (int)(v < 1e7 ? (double)s * v : (double)(s * 1e7f)); };
        return ((int64_t)cellOf(q.realX(), c.L.scaleX) << 32) ^ (int64_t)(uint32_t)cellOf(q.realY(), c.L.dynHeight ? 0.01f : 0.0f);
    };
    for (GObj* o : cand) {
        // The rect is a function of the player's bytes, and in this loop only an object that passes the overlap tests below writes them (or the rect's locals). So the read at a call boundary gives the same four numbers unless such an
        // object was met since the last read: the boundary is looked for only then, from the call of the object that was met (callKey). (The key of every candidate, two float-to-cell conversions an object, was 15% of lv22's tick and 30% of
        // Grief's, whose pass sees 960 objects.) (These four lines stand where the old four stood: GDCOL_DIED prints this file's line numbers into the trace.)
        if (rectStale && callKeyOf(*o) != callKey) { playerRect(P, px, py, pw, ph); rectStale = false; }
        if (o->type == 7 || o->inactive || o->activated) continue;
        if (o->type == 0 || o->type == 0x15) { solids.push_back(o); continue; }
        if (o->type == 2 || o->type == 0x2f) { hazards.push_back(o); continue; }
        if (o->type == 0x27) continue;
        c.curUid = o->uid;
        float rx = o->rx, ry = o->ry, rw = o->rw, rh = o->rh;
        if (o->type == 0x19) { rx = o->cx - o->rw; ry = o->cy - o->rh; rw = o->rw * 2.0f; rh = o->rh * 2.0f; }     // getObjectRect(2, 2)
        if (o->radius <= 0.0f) {
            if (!(rx <= px + pw && px <= rx + rw && ry <= py + ph && py <= ry + rh)) continue;
        } else if (!circleHit(P, *o, c.L.f1cf)) continue;
        if (o->type != 0x19 && objRotatedBox(*o) && o->radius <= 0.0f) {      // the portal / pad / orb is a rotated box: the oriented boxes must overlap as well (both ways)
            if (!rotAskOverlap(0, P, *o)) continue;
        }
        if (byCall && !rectStale) { callKey = callKeyOf(*o); rectStale = true; }   if (o->type == 0x19) {
            collidedWithSlope(c, dt, o, false);
            playerRect(P, px, py, pw, ph);
        } else {
            if (o->type == 10 && P.u8(po::Upside) == (((P.u8(po::Sideways) == 0 ? objFacingDown(*o) : objFacingLeft(*o)) ? 1 : 0) ^ 1)) continue;   // --shadowpadgate (constants.hpp): the gravity pad that would not flip does not touch the gate
            // The once-per-contact gate (FUN_14031ef50), as a rising edge: it fires when the object was not overlapped on the last tick.
            const bool seenBefore = (o->multiAct ? (c.gateIn && c.gateIn->count(o->uid)) : (c.everIn && (c.everIn->count(o->uid) || (c.everBase && c.everBase->count(o->uid)) || (c.everRule && c.everRule(c.everRuleCtx, o->uid))))) != 0;
            const bool fresh = !seenBefore;
            if (seenBefore && !o->multiAct && freshMatters(*o) && (P.u8(po::Leftward) || P.u8(po::Sideways)) && !(c.everIn && c.everIn->count(o->uid)) && !(c.everBase && c.everBase->count(o->uid))) c.everGuess = true;
            c.everNow.insert(o->uid);
            c.fired.push_back({o->type + (fresh ? 1000 : 0), o->uid});
            c.gateNow.insert(o->uid);
            switch (o->type) {
            case 3:
            case 4:
                if (!fresh) break;
                setObj(c, 0xa88, o);
                P.f32(0xa08, o->cx); P.f32(0xa0c, o->cy);
                { const uint8_t up0 = P.u8(po::Upside); playerFlipGravity(c, o->type == 3 ? 1 : 0, false);
                dualFlipSite(c, up0, false); }           // the second player's flip (GJBaseGameLayer::flipGravity 0x319e20)
                break;
            case 8: case 9: case 0x22:
                if (fresh) { padBump(c, o); gdev::notePadEvents(c.gameEvents, o->uid, o->type); }
                break;
            case 10:
                if (fresh) { gravityPad(c, o); gdev::notePadEvents(c.gameEvents, o->uid, o->type); }
                break;
            case 0x2c:   // GD Win 2.2091: 0x31cc82 resumes the loop without refreshing the rect; SpiderTeleport at 0x4fdb8d precedes the pad events at 0x31f23a / 0x31f24d.
                if (fresh) { padSpider(c, o); gdev::notePadEvents(c.gameEvents, o->uid, o->type); }
                break;
            case 0x2a:                                // the gravity toggle portal: flips whatever the gravity is (collisionCheckObjects case 0x2a; the layer's own flip and the animation are not the player's bytes)
                if (!fresh) break;
                if (c.L.dual && !c.dualH) unsup(c, 1101);
                P.f32(0xa08, o->cx); P.f32(0xa0c, o->cy);
                setObj(c, 0xa88, o);
                { const uint8_t up0 = P.u8(po::Upside); playerFlipGravity(c, up0 ? 0 : 1, true); dualFlipSite(c, up0, true, false); }
                break;
            case 5: case 6: case 0x10: case 0x13: case 0x1a: case 0x1b: case 0x21:
                if (fresh) modePortal(c, o);
                break;
            case 0x1c:                                // the teleport portal (case 0x1c, gated)
                // collisionCheckObjects jumps to LAB_14031d282 after teleportPlayer, which asks the player for its rect again (vtable +0x4a0, getObjectRect) and takes its four edges: the objects after the
                // portal in the same pass are tested against the teleported body. a custom level t=6872, a mini robot: the teleport portal uid 165 puts it at y 826 and the gravity portal uid 1963 (bottom 793.1,
                // 1.6 px above the body's top before the move) fires in the same pass in GD; a rect taken once at entry fired it a tick late.
                if (fresh) { teleportPortal(c, o); playerRect(P, px, py, pw, ph); }
                break;
            case 0x29:                                // the swing portal (collisionCheckObjects case 0x29; the layer's swing flag +0x317e stops it)
                if (fresh && !c.L.f317e) modePortal(c, o);
                break;
            case 0x14: case 0x2d: case 0x16:
                if (c.rotApply && o->type == 0x14 && o->objectId == 2900 && o->touchTrig) playerRotateGameplay(c, *o);   // a touch Rotate Gameplay turns the player in this pass; the other touch triggers and the coins act on the layer and on the object, not on the player's bytes (playerTouchedTrigger / pickupItem)
                break;
            case 0xe: case 0xf:                       // mirror portals: the layer's flip; the player only records the portal
                if (fresh) { P.f32(0xa08, o->cx); P.f32(0xa0c, o->cy); setObj(c, 0xa88, o); }
                break;
            case 0x11: case 0x12:
                if (!fresh) break;
                P.f32(0xa08, o->cx); P.f32(0xa0c, o->cy);
                setObj(c, 0xa88, o);
                togglePlayerScale(c, o->type == 0x12);
                break;
            case 0xb: case 0xc: case 0xd: case 0x1d: case 0x20: case 0x23: case 0x24: case 0x25: case 0x26: case 0x2b: case 0x2e:
                // playerTouchedRing: a registration (the ring array +0xa40 and the map +0xa48 are not in the image); the jump itself is ringJump
                c.ringsTouched.push_back(o->uid);
                // ringJump 0x4f6f50 returns at once unless +0x992 (a press is armed) and +0x991 are up and the player is not dashing (+0x9ec) or dead (+0x9c8);
                // what it does past that point (the press side's H / S / slot registers) is not ported.
                // (playerTouchedRing 0x31f5a0 calls ringJump only for a body that is not a ship / UFO / wave / swing and a ring whose [+0x740] claim is 0: a claiming ring waits for the press, pushButton's)
                if (!P.u8(po::ModeShip) && !P.u8(po::ModeUfo) && !P.u8(po::ModeWave) && !P.u8(po::ModeSwing) && !o->claim
                    && !P.u8(0x9c8) && P.u8(0x992) && P.u8(0x991) && !P.u8(0x9ec)) {
                    if (c.ringJump) c.ringJump(c, o);
                    else if (!c.hsKnown) unsup(c, o->type);
                    else {
                        // ringJump's own gate: the ring is not one this press has used, and the class's slot (A for a ring, B for a spawn orb) is down
                        const int slot = o->type == 0x24 ? 0x994 : 0x993;
                        if ((!c.usedRings || !c.usedRings->count(o->uid)) && !P.u8((size_t)slot)) {
                            if (c.usedRings) c.usedRings->insert(o->uid);
                            noteFired(c.fireOrder, o->uid); gdev::noteOrbEvents(c.gameEvents, o->uid, o->type);
                            ringJumpWrites(c, o->type);
                            if (o->type == 0x24 || o->type == 0x25 || o->type == 0x26 || o->type == 0x2b) unsup(c, o->type);   // (the trigger group, the dash, the spider's jump are not ported)
                        }
                    }
                }
                if (o->type == 0x2e) playerRect(P, px, py, pw, ph);   // GD Win 2.2091: RVA 0x31cde1 jumps to the rect refresh at 0x31d282 after touching a teleport orb.
                break;
            case 0x28:                                // GameObjectType::Special (collisionCheckObjects case 0x28, no once-gate): by object id, a grace counter the update counts down
                switch (o->objectId) {
                case 0x743: P.i32(0xb84, 2); break;
                case 0x6db: P.i32(0xb80, 2); break;   // the wave's
                case 0x715: P.i32(0xb7c, 2); break;   // the robot's
                case 0xb32: P.i32(0xb88, 2); break;
                case 0x725:                                           // the dash end: stopDashing, then H := 0 (collisionCheckObjects case 0x28, id 0x725)
                    if (P.u8(0x9ec)) { if (c.stopDash) { c.stopDash(c); P.u8(0x98d, 0); } else unsup(c, 4001); }
                    break;
                case 0x815: case 0xe3d: {                             // a force block: +0xb90 := 2, and its push is added to +0xb94 (once per force id and step)
                    P.i32(0xb90, 2);
                    if (o->fbId > 0) { if (c.forceSeen.count(o->fbId)) break; c.forceSeen.insert(o->fbId); P.put<uint64_t>(0xba8, (uint64_t)c.forceSeen.size()); }   // (+0xba8 = the map's size)
                    float vx, vy;
                    forceBlockPush(c, *o, vx, vy);
                    P.f32(0xb94, P.f32(0xb94) + vx);
                    P.f32(0xb98, P.f32(0xb98) + vy);
                    const float len2 = P.f32(0xb98) * P.f32(0xb98) + P.f32(0xb94) * P.f32(0xb94);
                    if (9999.0f < std::sqrt(len2)) { const float k = 9999.0f / std::sqrt(len2); P.f32(0xb94, P.f32(0xb94) * k); P.f32(0xb98, P.f32(0xb98) * k); unsup(c, 4002); }
                    break;
                }
                default: break;
                }
                break;
            case 23:                                  // dual portal: the second body is born from the first one as it stands now (earlier objects of the pass have acted)
                if (fresh) { c.bornFired = true; c.bornY = P.f32(po::PosY); c.bornVy = P.f64(po::Vy); P.u8(0xaa1, 1); if (!dualPortalHook(c, true)) unsup(c, o->type); }   // toggleDualMode (0x31de96): both players' +0xaa1 := the layer's dual flag (+0x422), here just set; hwwatch=0xaa0 on s_spider_dn_116791791 t=10494
                break;
            case 24:                                  // dual portal off: toggleDualMode(false) copies the layer's dual flag (0) into both players' +0xaa1 (the same store as the dual-on portal; read, not measured)
                if (fresh) { P.u8(0xaa1, 0); if (!dualPortalHook(c, false)) unsup(c, o->type); }
                break;
            default:
                if (fresh) unsup(c, o->type);       // portals, triggers, coins: their effects are not ported
                break;
            }
        }
    }
}

// GJBaseGameLayer::checkCollisions 0x31aae0. `cand` = the candidate objects in the order the grid lists them. Returns 1 where the decompile does.
inline int checkCollisions(Ctx& c, float dt, bool ignoreDamage, const std::vector<GObj*>& cand) {
    Mem& P = c.P;
    // +0x6f8 = the number of distinct uids in c.touched. Counted on a per-thread scratch copy (sort + unique) instead of a std::set built and
    // freed on every pass (gd-05 11-13): the same count, no node allocations.
    struct MapSize { Ctx& c; ~MapSize() {
        uint64_t n = 0;
        if (!c.touched.empty()) {
            static thread_local std::vector<int> u;
            u.assign(c.touched.begin(), c.touched.end());
            std::sort(u.begin(), u.end());
            n = (uint64_t)(std::unique(u.begin(), u.end()) - u.begin());
        }
        c.P.put<uint64_t>(0x6f8, n);
    } } mapSize{c};
    const Layer& L = c.L;
    const uint8_t lVar9 = P.u8(0x560);
    P.u8(0x560, 0);
    P.u8(0x848, 0);
    P.f64(po::CeilTopY, 0.0);
    P.f64(po::FloorBottomY, 0.0);
    P.f64(po::LeftX, 0.0);
    P.f64(po::RightX, 0.0);
    P.u8(po::WasOnSlope, P.u8(po::OnSlope));
    P.u8(po::OnSlope, 0);
    P.u8(0xb40, P.u8(po::G2));
    if (P.u8(po::G2) && !P.u8(0xb61) && !P.u8(0xb62) && 0 < P.i32(0xb44)) { P.i32(0xb44, 0); P.f64(0xb48, P.f64(po::Time)); }
    if (P.u8(po::NodeFlag671) == 0) P.u8(po::NodeFlag671, 1); else P.put<int64_t>(po::Cf, 0);
    c.touched.clear();
    c.topDict.clear();
    c.botDict.clear();
    if (P.get<int64_t>(po::Cf)) c.touched.push_back((int)P.get<int64_t>(po::Cf) - 1);
    if (P.get<int64_t>(po::Cd0)) c.touched.push_back((int)P.get<int64_t>(po::Cd0) - 1);
    const float fVar18 = P.f32(0xab8);
    float fVar22 = 0.0f;
    if (P.f32(0x9f8) != 1.0f) fVar22 = (1.0f - P.f32(0x9f8)) * fVar18 * 0.5f;
    const float fVar25 = fVar18 * 0.5f;
    const float fVar23 = (fVar25 + 90.0f) - fVar22;
    const bool flip = P.u8(po::Upside) != 0;
    const bool boundedModes = P.u8(po::ModeShip) || P.u8(po::ModeUfo) || P.u8(po::ModeWave) || P.u8(po::ModeSwing) || P.u8(po::ModeBall) || P.u8(po::ModeSpider) || L.dual;
    bool bVar3 = !L.freeMode && boundedModes;
    const uint8_t killPrev = P.u8(0xc40);
    P.u8(0xc40, 0);
    uint8_t cVar13 = 0, cVar19 = 0;
    const float x0 = P.f32(po::PosX);
    if (!L.f317e) {
        if (P.u8(po::Leftward) && x0 < -30.0f) { cVar13 = 1; cVar19 = 1; }
    } else if (x0 < -30.0f) {
        P.f64(0xb00, 0.0);
        setPosition(P, -30.0f, P.f32(po::PosY));
    }
    float y = P.f32(po::PosY);
    if (fVar23 <= y || (bVar3 && boundedModes)) {
        if (fVar22 + L.ceilWord < y) cVar13 = 1;
    } else if (!flip) {
        cVar13 = cVar19;
        if (P.u8(po::Boost) == 0) {
            setPosition(P, P.f32(po::PosX), fVar23);
            hitGround(c, nullptr, false);
            updateCollide(c, 1, nullptr);
        }
    } else {
        if (P.f64(0x808) != 0.0 && P.f64(po::Time) - P.f64(0x808) < 0.10000000149011612) {
            setPosition(P, P.f32(po::PosX), fVar23);
            hitGround(c, nullptr, true);
            updateCollide(c, 0, nullptr);
            P.u8(po::G2, 0);
            return 0;
        }
        cVar13 = 1;
    }
    if (bVar3 && lVar9 == 0) {
        // the flight band (L168-313)
        const float ceilY = L.maxPortalY, floorY = L.minPortalY;
        const float fVar26 = ceilY - L.w764;
        const uint8_t cVar4 = (P.u8(po::ModeShip) && P.u8(0x98d)) ? 1 : 0;
        const float cTop = (ceilY - fVar25) + fVar22;
        const float cBot = (fVar25 + floorY) - fVar22;
        y = P.f32(po::PosY);
        const bool cubeFam = !P.u8(po::ModeShip) && !P.u8(po::ModeUfo) && !P.u8(po::ModeWave) && !P.u8(po::ModeSwing) && !P.u8(po::ModeBall)
                             && !P.u8(po::ModeSpider) && P.i32(0xb84) < 1;
        const double now = P.f64(po::Time);
        auto recentFlip = [&]() { return (P.f64(0x808) != 0.0 && now - P.f64(0x808) < 0.2) || (P.f64(0x7e0) != 0.0 && now - P.f64(0x7e0) < 0.2); };
        if (y <= cTop) {
            if (y <= cBot && cBot != y) {
                if (!P.u8(po::WasOnSlope) || cBot - fVar18 * 0.5f <= y) {
                    bool bVar2 = false, b3 = false;
                    float vyKeep = (float)P.f64(po::Vy);
                    float add = 0.0f;
                    bool kill = false;
                    if (cubeFam && flip) {
                        b3 = true;
                        if (!recentFlip()) kill = true;
                    } else {
                        b3 = false;
                        if (flip || ((P.u8(po::ModeShip) || P.u8(po::ModeWave)) && P.u8(0x98e)) || fVar26 == 0.0f) bVar2 = false; else bVar2 = true;
                        if ((P.u8(po::ModeShip) || P.u8(po::ModeWave)) && P.u8(0x98e) && L.speedA != L.speedB) add = 1.0f;
                    }
                    if (kill) cVar13 = 1;
                    else {
                        setPosition(P, P.f32(po::PosX), add + cBot);
                        if (b3) { P.f64(po::Vy, 0.0); P.u8(po::G2, 0); P.u8(po::G, 0); }
                        else if ((!P.u8(po::ModeShip) && !P.u8(po::ModeSwing)) || (P.f64(po::Vy) <= 0.0 && P.f64(po::Vy) != 0.0)) hitGround(c, nullptr, flip);
                        updateCollide(c, !flip ? 1 : 0, nullptr);
                        if ((bVar2 && (double)vyKeep < P.f64(po::Vy)) || (flip && cVar4 && fVar26 < 0.0f)) setYVelocity(P, (double)vyKeep);
                        P.u8(0x9b9, 0);
                    }
                } else cVar13 = 1;
            }
        } else {
            if (!P.u8(po::WasOnSlope) || y <= fVar25 + ceilY) {
                bool bVar2 = false, b3 = false;
                float vyKeep = (float)P.f64(po::Vy);
                bool kill = false;
                if (cubeFam && !flip) {
                    b3 = true;
                    if (!recentFlip()) kill = true;
                } else {
                    b3 = false;
                    if (!flip || ((P.u8(po::ModeShip) || P.u8(po::ModeWave)) && P.u8(0x98e)) || fVar26 == 0.0f) bVar2 = false; else bVar2 = true;
                }
                if (kill) cVar13 = 1;
                else {
                    setPosition(P, P.f32(po::PosX), cTop);
                    if (b3) { P.f64(po::Vy, 0.0); P.u8(po::G2, 0); P.u8(po::G, 0); }
                    else if (0.0 < P.f64(po::Vy)) hitGround(c, nullptr, !flip);
                    updateCollide(c, flip ? 1 : 0, nullptr);
                    if ((bVar2 && P.f64(po::Vy) <= (double)vyKeep && (double)vyKeep != P.f64(po::Vy)) || (!flip && cVar4 && 0.0f < fVar26)) {
                        double d = (double)vyKeep, ip = (double)(int)vyKeep;
                        if (d != ip) d = std::round((d - ip) * 1000.0) / 1000.0 + ip;
                        P.f64(po::Vy, d);
                    }
                    P.u8(0x9b9, 0);
                }
            } else cVar13 = 1;
        }
    }
    // L317-325: a level-bounds kill needs two ticks in a row
    if (killPrev != 0 && cVar13 != 0) {
        { c.died = !ignoreDamage; c.why = __LINE__; }
        return 1;
    }
    P.u8(0xc40, cVar13);
    // The pass's two lists, kept per thread and per nesting depth instead of built and freed on every pass (an allocation each, every
    // stepped child of the search: gd-05 11-13, lv5 1-thread profile). Cleared before use, so the lists the pass sees are the same.
    struct ColBufs { std::vector<GObj*> solids, hazards; };
    static thread_local std::vector<std::unique_ptr<ColBufs>> t_colBufs;
    static thread_local size_t t_colDepth = 0;
    if (t_colBufs.size() <= t_colDepth) t_colBufs.emplace_back(new ColBufs);
    ColBufs& colBufs = *t_colBufs[t_colDepth++];
    struct ColDepthGuard { ~ColDepthGuard() { --t_colDepth; } } colDepthGuard;
    std::vector<GObj*>& solids = colBufs.solids;
    std::vector<GObj*>& hazards = colBufs.hazards;
    solids.clear(); hazards.clear();
    collisionCheckObjects(c, dt, cand, solids, hazards);
    // the solids, last queued first
    {
        // GJBaseGameLayer::checkCollisions 0x31aae0, the loop before each solid's overlap test: while the player stands on something (+0x608 and +0x660 set), a solid that is in a group and has moved down since the last tick
        // (up, for a flipped player) is tested with its rect grown by dt * 5 in the direction it left; if the player's rect meets that, the solid is registered as +0x600 (postCollision then counts it as touching and lands
        // the player on the ground object that followed it) and the registration stops for this pass. Found 2026-10-29 at the robot landing t=7239 (wtcap/s_robot_dn_111832753).
        bool preReg = P.get<int64_t>(po::GroundObj) != 0 && P.u8(0x660) != 0;
        for (size_t k = 0; k < solids.size(); ++k) {
            GObj* o = solids[solids.size() - 1 - k];
            if (o->inactive) continue;
            c.curUid = o->uid;
            float px, py, pw, ph;
            playerRect(P, px, py, pw, ph);
            if (preReg && o->groupCount > 0) {
                bool hit = false;
                if (o->cx != o->prevCx || o->cy != o->prevCy) {
                    // GD Win 2.2091: RVAs 0x31b8f7 / 0x31b93a read gravity after the portal and pad pass.
                    const bool solidFlip = P.u8(po::Upside) != 0;
                    const bool down = !solidFlip ? (o->cy < o->prevCy) : (o->prevCy < o->cy);
                    if (down) {
                        float gy = o->ry, gh = o->rh + dt * 5.0f;
                        if (solidFlip) gy -= dt * 5.0f;
                        if (rectsIntersect(px, py, pw, ph, o->rx, gy, o->rw, gh)) { setObj(c, po::Collided, o); hit = true; }
                    }
                }
                preReg = !hit;
            }
            if (slopeDbgOn(P)) std::fprintf(slopeDbgOut(), "slopedbg solid uid %d type %d rect %.3f %.3f %.3f %.3f (cx %.3f cy %.3f prev %.3f %.3f) player %.3f %.3f %.3f %.3f overlap %d\n", o->uid, o->type, (double)o->rx, (double)o->ry, (double)o->rw, (double)o->rh, (double)o->cx, (double)o->cy, (double)o->prevCx, (double)o->prevCy, (double)px, (double)py, (double)pw, (double)ph, (int)(o->rx <= px + pw && px <= o->rx + o->rw && o->ry <= py + ph && py <= o->ry + o->rh));
            if (o->rx <= px + pw && px <= o->rx + o->rw && o->ry <= py + ph && py <= o->ry + o->rh)
                collidedWithObject(c, dt, o, nullptr, false);
            if (c.died) return 1;
        }
    }
    // the hazards, first queued first, against the rect the solids left
    {
        float px, py, pw, ph;
        playerRect(P, px, py, pw, ph);                 // (the pass asks the game for the rect again here: line 514)
        P.u8(0x369, 0);                                // ... and for the oriented box (vtable +0x588, line 520), which clears the second dirty byte
        for (GObj* o : hazards) {
            if (o->inactive) continue;
            bool hit;
            if (o->radius <= 0.0f) hit = o->rx <= px + pw && px <= o->rx + o->rw && o->ry <= py + ph && py <= o->ry + o->rh;
            else hit = circleHit(P, *o, c.L.f1cf);
            if (hit) {
                // (a circle -- a saw -- is not asked: its dump W / H is the rect of its rotation, so the box solved from it is the square INSCRIBED in the circle, half 17.7 for a 24 radius,
                // and a mini wave 3 px inside the circle was let through; GD kills it: lv20 uid2641 id918 rot 61 at (3947.7, 109.25), wave at (3929.92, 130.81), t=3028, the old model too)
                if (objRotatedBox(*o) && !(o->radius > 0.0f)) {          // a rotated box: the game asks the oriented boxes as well, both ways
                    if (!rotAskOverlap(1, P, *o)) continue;
                }
                { c.died = !ignoreDamage; c.why = __LINE__; c.whyUid = o->uid; }
                return 1;
            }
        }
    }
    postCollision(c, dt, L.betweenSteps);
    return c.died ? 1 : 0;
}

// ----------------------------------------------------------------------------------------------------------------------------------------
// Dual (--shadowdual): the layer's calls in the pass and the buttons that reach the OTHER player. selfshadow_dual.hpp fills a DualHooks and hangs it on the
// Ctx of each phase (Ctx::dualH); without one, each site counts what it would have done as not ported, as before.
struct DualHooks {
    void* self = nullptr;
    void (*flip)(void* self, Ctx& c, uint8_t newUp, bool p3) = nullptr;   // GJBaseGameLayer::flipGravity 0x319e20, the half after this player's own flip
    void (*willSwitch)(void* self, Ctx& c, int type) = nullptr;            // FUN_14031a210 (playerWillSwitchMode), its gravity part
    void (*portal)(void* self, Ctx& c, bool on) = nullptr;                 // GJBaseGameLayer::toggleDualMode 0x31de10
};
// A site that went through GJBaseGameLayer::flipGravity (the gravity portals 3 / 4, the gravity pad, the toggle portal 0x2a): `up0` is the player's gravity before
// its own flip. The game calls the partner only when this player's gravity changed (the function's own gate, `0x9c7 != param_3`).
inline void dualFlipSite(Ctx& c, uint8_t up0, bool p3, bool count) {
    if (!c.dualH) { if (count && c.L.dual) unsup(c, 1101); return; }
    if (up0 != c.P.u8(po::Upside)) c.dualH->flip(c.dualH->self, c, c.P.u8(po::Upside), p3);
}
// PlayerObject::ringJump's flips go through the same layer call (six sites, 0x4f7316..0x4f797b); without a hook nothing was counted there, and nothing is.
inline void dualRingFlip(Ctx& c, uint8_t up0) {
    if (c.dualH && up0 != c.P.u8(po::Upside)) c.dualH->flip(c.dualH->self, c, c.P.u8(po::Upside), true);
}
// true = handled (the hook decided, whatever it did)
inline bool dualWillSwitch(Ctx& c, int type) {
    if (!c.dualH) return false;
    c.dualH->willSwitch(c.dualH->self, c, type);
    return true;
}
inline bool dualPortalHook(Ctx& c, bool on) {
    if (!c.dualH) return false;
    c.dualH->portal(c.dualH->self, c, on);
    return true;
}

// --shadowrotapply: a touch Rotate Gameplay (2900) the pass overlaps, in the object loop (collisionCheckObjects case 0x14), so that the solids the loop queues for the second pass already see
// the new directions. GJBaseGameLayer::rotateGameplay 0x31f980 hands the trigger's mvdir / gnddir / editvel / vmodx / vmody / ovrvel / dontslide to the player's rotateGameplay (22091: FUN_1404f7fa0
// at 0x4f7fa0; Broma's name for 0x35ac20 is a vector insert), ported here from its decompile. NOT ported: the sprite and streak calls (updatePlayerArt, placeStreakPoint, the wave's streak restart,
// the particle reset), the layer's own half (the swarm channel +0x33c and its reverse map, the vtable +0x518 call with the trigger-to-player offset, +0x75c) -- those are the model's queue's
// bookkeeping, not the body's bytes -- and the ball arm (+0x9c3 set: counted as not ported). The point +0xa34 is set to what DAT_14090b428 holds, which is a static the file does not
// initialise (zero here; nothing in a cube's physics reads it). A trigger in rotSpent (the uids the caller knows have fired) or already turned on in this pass does nothing; with no rotSpent given
// the pass cannot tell a first touch from a later one and turns nothing.
inline void playerRotateGameplay(Ctx& c, const GObj& o) {
    Mem& P = c.P;
    if (!c.rotSpent) return;
    for (int u : c.rotFired) if (u == o.uid) return;
    for (int u : *c.rotSpent) if (u == o.uid) return;
    c.rotFired.push_back(o.uid);
    const bool plat = P.u8(po::Platformer) != 0;
    const double vy0 = P.f64(po::Vy);
    const double spd = plat ? P.f64(0xb00) : (double)P.f32(0x9fc) * P.f64(0x7c0);
    const uint8_t up0 = P.u8(po::Upside);
    const uint8_t sw0 = P.u8(po::Sideways);
    const uint8_t sw1 = (unsigned)(o.rotMv - 3) < 2u ? 1 : 0;
    P.u8(po::Sideways, sw1);
    playerFlipGravity(c, (o.rotMv == 1 || o.rotMv == 4) ? 1 : 0, true);
    const uint8_t lf0 = P.u8(po::Leftward);
    setLeftward(P, plat ? lf0 : (uint8_t)((o.rotGnd == 3 || o.rotGnd == 2) ? 1 : 0));
    if (sw1 == sw0 && up0 == P.u8(po::Upside) && lf0 == P.u8(po::Leftward)) return;
    if (sw1 != sw0) {
        c.topDict.clear(); c.botDict.clear();             // +0x5b0 / +0x5b8 (the dictionaries at +0x5c0 / +0x5c8 are not tracked)
        P.put<int64_t>(0x5d0, -1); P.put<int64_t>(0x5d8, -1); P.put<int64_t>(0x5e0, -1);
        const float spF = (float)spd, vyF = (float)vy0;
        float nx = vyF, ny = spF;                         // the axes swap: the old vy is the new travel speed, the old travel speed the new vy
        if (o.rotEdit) {
            if (!o.rotOvr) { nx = vyF * o.rotVmx; ny = spF * o.rotVmy; }
            else { nx = o.rotVmx; ny = o.rotVmy; }
        }
        P.u8(0x95a, 1);
        P.f64(po::Vy, (double)ny);
        if (plat) { P.u8(0xb9c, 1); P.f64(0xb00, (double)nx); }
        P.u8(po::Boost, 1);
        P.u8(po::G2, 0);
        P.put<uint64_t>(0xa34, 0);
        if (o.rotDont) { P.u8(0x95a, 0); P.u8(0xb9c, 0); }
        const bool plain = !P.u8(po::ModeShip) && !P.u8(po::ModeUfo) && !P.u8(po::ModeWave) && !P.u8(po::ModeSwing) && !P.u8(po::ModeBall) && !P.u8(po::ModeRobot) && !P.u8(po::ModeSpider)
                           && !P.u8(0x730) && !P.u8(0xa32);
        if (plain && P.u8(0x9ec) == 0) {
            P.put<uint16_t>(0x730, 0); P.u8(0x670, 0); P.put<uint32_t>(0x728, 0);
            runNormalRotation(P, false, 1.0f);
        }
    }
    if (P.u8(0x9ec)) {
        // GD Win 2.2091: rotateGameplay swaps 0x570/0x578 at RVA 0x4f8337..0x4f8354, rounding old 0x570 through float.
        if (sw1 != sw0) {
            const float travel = (float)P.f64(0x570);
            P.f64(0x570, P.f64(0x578));
            P.f64(0x578, (double)travel);
        }
        // RVA 0x4f8397..0x4f840b mirrors the dash angle on a direction change, normalizes it, then restarts 0x4f43c0.
        const bool left = P.u8(po::Leftward) != 0;
        double angle = P.f64(0x580);
        if (lf0 != (uint8_t)left) {
            if (sw1 != sw0) angle += 180.0;
            angle = (angle > 0.0 ? 180.0 : -180.0) - angle;
        }
        if (angle > 180.0) angle -= 360.0;
        else if (angle < -180.0) angle += 360.0;
        P.f64(0x580, angle);
        setDashBodyRotation(P);
        if (c.restartDashSpin) c.restartDashSpin(c);
    }
    if (P.u8(po::ModeBall)) unsup(c, 1102);   // the ball-specific behavior is still not ported
}

}  // namespace gdcol
}  // namespace dp
