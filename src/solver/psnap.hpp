#pragma once
// ============================================================================
// Raw-byte snapshot of the player state (`psnap`)
//
// Why it is needed: the cost of the section solver is set by the BRANCHING
// ITSELF. The current branch primitive is GD's practice-mode checkpoint,
// measured at 0.95 MB per checkpoint / 2.02ms per restore (one physics tick is
// 0.007ms, so one branch = the equivalent of 300 physics ticks). A checkpoint
// holds the whole level (19,685 objects in lv20), hence the price.
//
// By contrast, PlayerObject is 3,144 bytes. If the geometry does not move
// within the section, the player should be all that branching needs, and if
// that holds it gets 1-2 orders of magnitude cheaper.
//
// This is not a new idea: `warp.hpp` from the beam era did the same thing and
// confirmed a bit-exact match at all 9 orb-activation boundaries in lv11
// (commit c1925e1). Orb boundaries are the only known section where the
// checkpoint mechanism breaks, so matching there is strong evidence. It was
// later deleted along with the beam solver in 91f92c9 (not a rejection of the
// method). This is just its core, brought back.
//
// ---------------------------------------------------------------------------
// [2026-08-07 settled] In a section where the geometry does not move, it works
// as a branch primitive.
//
//   Inside lv20 wave2 (target 5000, depth 274, 24,557 expansions):
//     checkpoint and psnap agree on ALL of depth / restores / cps / foundY
//     Splice the solution and plain replay → dy=0.000 dvy=0.000
//     75.0 s → 12.5 s (6x)
//
//   lv20 ship section (231 grouped objects inside the window):
//     Splits at depth 106. Branches the checkpoint counts as dead are missed
//     by psnap (dead 4 vs 2) → because psnap does NOT restore GameObject
//     positions. Unusable when there are moving objects
//
// So usability is decided by two gates (`secsnap=2` does this automatically):
//   1. `snapSweep`  … checks restore equivalence over 8 input patterns × drift
//   2. `movingObjectsInSection` … runs the section once and counts whether
//      anything ACTUALLY moves. The sweep alone is not enough (the window is
//      short and touches no moving object, so it passes the ship section)
//
// What was hunted down on the way (each showed up as "fast but lying"):
//   - heap corruption: raw bytes copy pointers along → typed whitelist
//   - CCNode position/rotation are not in the member table → carried via
//     the public API
//   - GJGameState was misclassified as an enum (because the name ends in
//     …State). Its contents are in-progress moves/rotations/portals/object
//     physics. Carried by C++ assignment
//   - dying stops the level → during search destroyPlayer is swallowed and
//     only the flag is picked up (`g_noKill` / `g_died`). Re-wakes 256 → 3
//
// ---------------------------------------------------------------------------
// [Below is the record from before it was settled. Kept as history]
//
// The speed was there. On the 864-tick wave section of lv20: 11.9 s (the
// checkpoint takes 11.9 min, 60x), 1.4µs per restore (checkpoint 2,020µs),
// 3KB per state (checkpoint 0.95MB).
// BUT the solution it produced does not reproduce in plain replay (dies at
// x=5,856). Fast, but it lies.
//
// The 3 hunted down so far, and the 1 that still remained:
//  1. Heap corruption (0xC0000374) — raw bytes copy pointers along. Restoring
//     many snapshots taken at different times alternately lets stale pointers
//     in. → Changed to a whitelist on the copy side (`mask()`). Only scalars
//     are copied so not a single pointer moves. Solved
//  2. CCNode position/rotation were missing from the member table and dropped
//     → carried explicitly via the public API. Solved (while they were dropped,
//     drift=1 split by exactly one tick's worth, ±1.95px)
//  3. After dying the level enters an "attempt over" state, and no matter how
//     much the player is restored afterwards the physics does not advance. The
//     snapshot restores m_isDead=false, so the search side believes it is
//     alive and the frontier spins forever at the same x (at depth 50 it sat
//     frozen at x=4563.7 for 1,000 layers). → When a branch dies, re-wake the
//     level with a checkpoint. Solved
//  4. UNRESOLVED: the solution still does not reproduce.
//
// [Summary as of 2026-08-07] Three suspects eliminated, still does not reproduce.
//
//   Player fields          innocent — widening the typed whitelist 756→1,268B
//                          left the output unchanged
//   Level objects          innocent — after restore, all 200 stable members
//                          match (`secworld`)
//   Game-layer scalars     PART OF THE CULPRIT — restored by the checkpoint but
//                          not by psnap. Identified 5 (m_extraDelta /
//                          m_gameState / m_blending / m_areaObjectsUpdated /
//                          m_audioPaused) and added them to the restore. The
//                          world diff went 5 → 0 / 341 stable members
//
// Still, the search's solution does not reproduce in plain replay (dies at
// x≈5,858). What remains is the OPAQUE side — 261 container objects on the game
// layer alone (m_sections, the group dictionaries, the effect manager, the
// trigger ledgers). A byte whitelist cannot reach them in principle. What
// CheckpointObject spends 0.95MB holding turns out, ultimately, to be this.
//
// ---- Below are rejected readings. Kept as history ----
// The real cause of 4: NOT on the player side. (The "whitelist is too narrow"
// below was rejected as well)
//
// Built the typed member table and widened the whitelist from 756 to 1,268
// bytes (424 scalars, including 49 doubles and 11 CCPoints), but the solution
// the search produces did NOT change by a single byte (foundY=233.147, dies at
// the same x=5,856.7 in plain replay). So what is missing is not a player field.
//
// What remains is the world side. But the one-off checks on the world ((a)(d)(e))
// pass. The search walks the tree with 103,911 expansions and 2,294 re-wakes,
// which resembles none of them. The conclusion at this point: a player-only
// snapshot cannot create the branches of a tree search. What CheckpointObject
// spends 0.95MB on is apparently needed in a form that one-off checks do not
// reveal.
//
// [Below are rejected readings. Kept as history]
//
// All 5 kinds of verification pass:
//   (a) restore the same one many times + shift the world by 10,000 ticks → bit match
//   (b) a chain with capture->restore inserted every tick → bit match
//   (c) 8 input patterns (including flipping every tick) → bit match
//   (d) rewind (restore to the state k ticks earlier, k=1..64) → bit match
//       ← the hypothesis was rejected
//   (e) re-wake (restore only the world to the section start) → bit match
// Still the search's solution does not reproduce. Because all 5 only measure in
// a situation where "the live object already holds nearly the right values",
// a mix-up in a field outside the whitelist never surfaces. The search
// restores far-away states, so it surfaces there.
//
// The whitelist is currently 756 / 3,144 bytes (24%). That is because size-8
// members are dropped across the board, and among them are REAL PHYSICAL
// QUANTITIES such as double and CCPoint. Telling them from pointers needs TYPE
// INFORMATION. The member table `src/po_members.inc` only holds name, offset
// and size. The generator `scripts/gen_member_table.py` was deleted in 24c256f.
// The next move is to rebuild it, typed, from the bindings (.bro).
// With types, "copy an 8-byte member unless it is a pointer" becomes writable
// and the whitelist widens at once.
//
// Default is off (`secsnap=0`). If used, always cross-check the resulting
// solution with a plain replay.
// ---------------------------------------------------------------------------
// ============================================================================

namespace psnap {

struct Mem { const char* name; size_t off; size_t size; };

// Member table of PlayerObject / GameObject. `src/po_members.inc` is generated
// by `scripts/gen_member_table.py` FROM THE TYPES IN THE BINDINGS (.bro).
// SCALAR = only PODs that cannot contain a pointer. Regenerate when the
// bindings are updated.
//
// Do NOT classify by size. When 8-byte members were discarded across the
// board, real physical quantities such as `double m_yVelocityBeforeSlope` were
// dropped too (all 49 doubles), the whitelist shrank to 756/3,144 bytes and the
// search's solutions stopped reproducing.
// Native slope resolution writes +0x680; collision reads it when m_wasOnSlope is set.
// save/loadPlayerCheckpoint (2.2081 0x3a1710/0x3a21b0) carries this double as well.
#ifdef GEODE_IS_WINDOWS
static_assert(offsetof(PlayerObject, unk_584) == 0x680);
#endif
static_assert(std::is_same_v<decltype(PlayerObject::unk_584), double>);

inline const std::vector<Mem>& scalars() {
    static std::vector<Mem> tbl = [] {
        std::vector<Mem> m;
#define PO_SCALAR(n) m.push_back({#n, offsetof(PlayerObject, n), sizeof(PlayerObject::n)});
#define GO_SCALAR(n) m.push_back({#n, offsetof(GameObject, n), sizeof(GameObject::n)});
#define PO_OPAQUE(n)
#define GO_OPAQUE(n)
#define GB_SCALAR(n)
#define GB_OPAQUE(n)
#define EM_SCALAR(n)
#define EM_OPAQUE(n)
#include "../po_members.inc"
#undef PO_SCALAR
#undef GO_SCALAR
#undef PO_OPAQUE
#undef GO_OPAQUE
#undef GB_SCALAR
#undef GB_OPAQUE
#undef EM_SCALAR
#undef EM_OPAQUE
        std::sort(m.begin(), m.end(),
                  [](const Mem& a, const Mem& b) { return a.off < b.off; });
        return m;
    }();
    return tbl;
}

// Members that must not be injected. The first half are visual effects, the
// second half are ones that OWN THEIR OWN HEAP.
//
// A raw-byte copy swaps in the containers' internal pointers as well, so the
// restored object becomes "a container pointing at another instance's
// buckets". The symptom in lv11 is that ORBS DO NOT FIRE (measured: with
// restore, 1 activation in 48,900 attempts; without, 400 in 8,400 attempts).
// These are not injected; the live values are left as they are.
inline const char* const PRESERVE[] = {
    "m_dashFireSprite", "m_particleSystems", "m_ghostTrail",
    "m_iconSprite", "m_iconSpriteSecondary", "m_iconSpriteWhitener",
    "m_vehicleSprite", "m_vehicleSpriteSecondary", "m_vehicleSpriteWhitener",
    "m_dashSpritesContainer", "m_regularTrail", "m_waveTrail",
    "m_robotSprite", "m_spiderSprite", "m_maybeSpriteRelated",
    "m_playerGroundParticles", "m_trailingParticles", "m_shipClickParticles",
    "m_vehicleGroundParticles", "m_ufoClickParticles", "m_robotBurstParticles",
    "m_dashParticles", "m_swingBurstParticles1", "m_swingBurstParticles2",
    "m_landParticles0", "m_landParticles1",
    "m_touchedRings", "m_ringRelatedSet", "m_touchingRings",
    "m_rotateObjectsRelated", "m_potentialSlopeMap",
    "m_playerFollowFloats", "m_jumpPadRelated", "m_holdingButtons",
    "m_currentRobotAnimation",
    "m_collisionLogTop", "m_collisionLogBottom",
    "m_collisionLogLeft", "m_collisionLogRight",
    "m_unk958",
};

// The PRESERVE above is the exclusion list from the beam era. NOT USED NOW
// (classification is by type now, so neither containers nor pointers ever
// enter the whitelist in the first place). Kept as history: it is the measured
// record of which members own their own heap.
inline size_t scalarCount() { return scalars().size(); }

// ---------------------------------------------------------------------------
// Whitelist on the copy side (2026-08-07. Countermeasure after the exclusion-
// list approach failed with heap corruption)
//
// Copying the raw bytes wholesale copies THE POINTERS ALONG. If one snapshot is
// restored right after it was taken, those pointers are still alive, but the
// search restores many taken at different times alternately, so it puts values
// pointing at freed buffers back into the live object.
//
// So the default is "do not copy", and ONLY MEMBERS CONFIRMED TO BE SCALARS
// are copied:
//   - bytes not listed in the member table (po_members.inc) (padding etc.)
//     are not copied
//   - size > 8 (containers, strings, structs) is not copied
//   - the CCObject header (first 0x20, vtable and refcount) is not copied
//   - names in the old PRESERVE continue not to be copied
//   - size-8 members are EXCLUDED FOREVER ONCE THEY EVER HELD A POINTER-LIKE
//     VALUE (monotone, since it is a union. Does not miss types that are null
//     at init and become a pointer later)
// ---------------------------------------------------------------------------
// cfg `secpos=1`: leave m_position after restore at the snapshot's value
inline bool g_keepSnapPos = false;
// Narrow the restored byte range to [g_maskLo, g_maskHi) (cfg `secmasklo`/`secmaskhi`).
// A bisection tool used together with the stacking check (`secoverlay`).
// Halving the non-identity range repeatedly names the member that breaks things.
inline size_t g_maskLo = 0;
inline size_t g_maskHi = (size_t)-1;
// The reverse narrowing (cfg `secmaskexlo`/`secmaskexhi`): do not copy ONLY
// this range. For pulling a suspect found by bisection out of the psnap path
// and seeing whether the split heals.
inline size_t g_maskExLo = 0;
inline size_t g_maskExHi = 0;
// cfg `secskipextras=1`: skip writing back CCNode position/rotation and m_position
inline bool g_skipExtras = false;

inline bool looksPointer(uint64_t v) {
    // Win64 user-mode space. 8-byte aligned. Excludes 0 and small integers.
    return v >= 0x10000ull && v < 0x7FFFFFFFFFFFull && (v & 7) == 0;
}

// The range to copy. ONLY MEMBERS WHOSE TYPE IS SCALAR. The classification is
// closed at generation time, so it is not narrowed at runtime by looking at
// values (if the mask changes mid-run, the meaning of a restore differs between
// the first and second half — measured: doing that made solutions stop
// reproducing).
inline std::vector<uint8_t>& mask() {
    static std::vector<uint8_t> m = [] {
        std::vector<uint8_t> v(sizeof(PlayerObject), 0);
        for (auto& mem : scalars()) {
            if (mem.off < 0x20) continue;                   // CCObject header
            if (mem.off + mem.size > sizeof(PlayerObject)) continue;
            std::fill(v.begin() + (ptrdiff_t)mem.off,
                      v.begin() + (ptrdiff_t)(mem.off + mem.size), (uint8_t)1);
        }
        return v;
    }();
    return m;
}

// If a size-8 member held a pointer-like value, never copy it again from then
// on. Called on every capture (it only narrows monotonically, never loosens
// midway).
//
// This is dangerous if it happens during a search: a field the first-half
// snapshots copied is no longer copied in the second half = the meaning of a
// restore changes midway. It does not show in short tests, which stabilise
// early. The number of narrowings during a search is counted and reported.
// Since the whitelist now only holds members of 4 bytes or less, value-based
// exclusion is no longer needed. Kept only as a sentinel: count whenever a
// pointer-like 8 bytes appears inside the copied range (if it becomes non-zero,
// the way the whitelist is built is broken).
inline int g_shrinks = 0;
inline void observe(PlayerObject* p) {
    auto& m = mask();
    for (size_t o = 0x20; o + 8 <= m.size(); o += 8) {
        bool all = true;
        for (size_t j = 0; j < 8; ++j) if (!m[o + j]) { all = false; break; }
        if (!all) continue;
        uint64_t v;
        std::memcpy(&v, (const uint8_t*)p + o, 8);
        // COUNT ONLY. Do not touch the mask. Two adjacent 4-byte scalars can
        // happen to look like a pointer-like 8 bytes, and going in to erase
        // them drops legitimate physics fields mid-run (measured maskShrinks=6).
        if (looksPointer(v)) ++g_shrinks;
    }
}

// ---------------------------------------------------------------------------
// Fingerprint of the world side (cfg `secworld=N`)
//
// Every diagnosis so far took the form "look at the player and imagine the
// world". Three hypotheses were raised in turn and rejected in turn, without
// ever LOOKING AT THE WORLD DIRECTLY. Fold the SCALAR members of GameObject
// (GO_SCALAR in the typed table) plus position over all objects, and report
// WHAT DIFFERS AND HOW MANY between after-checkpoint-restore and
// after-psnap-restore.
// ---------------------------------------------------------------------------
inline const std::vector<Mem>& goScalars() {
    static std::vector<Mem> tbl = [] {
        std::vector<Mem> m;
#define PO_SCALAR(n)
#define PO_OPAQUE(n)
#define GO_SCALAR(n) m.push_back({#n, offsetof(GameObject, n), sizeof(GameObject::n)});
#define GO_OPAQUE(n)
#define GB_SCALAR(n)
#define GB_OPAQUE(n)
#define EM_SCALAR(n)
#define EM_OPAQUE(n)
#include "../po_members.inc"
#undef PO_SCALAR
#undef PO_OPAQUE
#undef GO_SCALAR
#undef GO_OPAQUE
#undef GB_SCALAR
#undef GB_OPAQUE
#undef EM_SCALAR
#undef EM_OPAQUE
        return m;
    }();
    return tbl;
}

// State OUTSIDE m_objects. The last suspect left once both the player and the
// level objects had been cleared (timers, counters, camera, group ledgers).
inline const std::vector<Mem>& gbScalars() {
    static std::vector<Mem> tbl = [] {
        std::vector<Mem> m;
#define PO_SCALAR(n)
#define PO_OPAQUE(n)
#define GO_SCALAR(n)
#define GO_OPAQUE(n)
#define GB_SCALAR(n) m.push_back({#n, offsetof(GJBaseGameLayer, n), sizeof(GJBaseGameLayer::n)});
#define GB_OPAQUE(n)
#define EM_SCALAR(n)
#define EM_OPAQUE(n)
#include "../po_members.inc"
#undef PO_SCALAR
#undef PO_OPAQUE
#undef GO_SCALAR
#undef GO_OPAQUE
#undef GB_SCALAR
#undef GB_OPAQUE
#undef EM_SCALAR
#undef EM_OPAQUE
        return m;
    }();
    return tbl;
}

inline uint64_t objSig(GameObject* o) {
    uint64_t h = 1469598103934665603ull;                    // FNV-1a
    auto mix = [&h](const void* p, size_t n) {
        const uint8_t* b = (const uint8_t*)p;
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    const float pos[3] = {o->getPositionX(), o->getPositionY(),
                          o->getRotation()};
    mix(pos, sizeof(pos));
    for (auto& m : goScalars()) mix((const uint8_t*)o + m.off, m.size);
    return h;
}

inline size_t maskedBytes() {
    size_t n = 0;
    for (uint8_t b : mask()) n += b;
    return n;
}

// ---------------------------------------------------------------------------
// Snapshot of moving objects (`wsnap`. 2026-08-07, user proposal)
//
// psnap lied in moving sections because it does NOT restore GameObject
// positions (lv20 ship section, `cp dead=4 / psnap dead=2` at d=106 — branches
// that should hit a moving floor passed through). So "do the same thing for
// moving objects as for the player".
//
// Restoring all 19,685 would cost the same as a checkpoint, so narrow it to
// ONLY THE OBJECTS THAT ACTUALLY MOVE. The set is decided once at the section
// start and held as pointers from then on:
//   - objects that ACTUALLY MOVED when the section was run once (scanning the
//     whole level)
//   - GROUPED objects inside the section's x window. Objects that move only
//     after being touched do not move on a plain run, so "what moved" alone is
//     not enough ([[gd-touch-triggers]])
//
// The cost per node is set by the size of the set. In a section with a large
// set, falling back to the checkpoint is cheaper, so the caller cuts off at a
// limit (`secmaxmov`).
// ---------------------------------------------------------------------------
struct ObjXf { float x, y, rotX, rotY, scaleX, scaleY; uint8_t visible; };

// Do NOT copy the section indices. GJBaseGameLayer keeps objects bucketed by x
// (`m_sections`), and collision looks up those buckets. The bucket arrays
// themselves are OPAQUE, so the whitelist does not restore them. Copying only
// the indices would give "index says the bucket at capture time / the object
// actually sits in the live bucket", so after restoring the position make
// `updateObjectSection` RE-BUCKET it.
inline bool isSectionIndex(const char* n) {
    return std::strcmp(n, "m_someOtherIndex") == 0
        || std::strcmp(n, "m_innerSectionIndex") == 0
        || std::strcmp(n, "m_outerSectionIndex") == 0
        || std::strcmp(n, "m_middleSectionIndex") == 0;
}

inline const std::vector<Mem>& goWorldList() {
    static std::vector<Mem> v = [] {
        std::vector<Mem> r;
        for (auto& m : goScalars()) if (!isSectionIndex(m.name)) r.push_back(m);
        return r;
    }();
    return v;
}

inline size_t goStride() {
    static size_t n = [] {
        size_t s = sizeof(ObjXf);
        for (auto& m : goWorldList()) s += m.size;
        return s;
    }();
    return n;
}

inline void captureWorld(const std::vector<GameObject*>& set,
                         std::vector<uint8_t>& out) {
    out.resize(set.size() * goStride());
    uint8_t* d = out.data();
    for (auto* o : set) {
        const ObjXf xf{o->getPositionX(), o->getPositionY(),
                       o->getRotationX(), o->getRotationY(),
                       o->getScaleX(), o->getScaleY(),
                       (uint8_t)(o->isVisible() ? 1 : 0)};
        std::memcpy(d, &xf, sizeof(xf)); d += sizeof(xf);
        for (auto& m : goWorldList()) {
            std::memcpy(d, (const uint8_t*)o + m.off, m.size); d += m.size;
        }
    }
}

inline void restoreWorld(GJBaseGameLayer* l,
                         const std::vector<GameObject*>& set,
                         const std::vector<uint8_t>& in) {
    if (in.size() != set.size() * goStride()) return;
    const uint8_t* s = in.data();
    for (auto* o : set) {
        ObjXf xf;
        std::memcpy(&xf, s, sizeof(xf)); s += sizeof(xf);
        // Public API first, scalar copy after. In 2.2081 setPosition changes the
        // drawing node and its children, not the physics ledger or rect flags.
        // Rotation/scale setters also affect caches; put their saved values back.
        o->setPosition({xf.x, xf.y});
        o->setRotationX(xf.rotX);
        o->setRotationY(xf.rotY);
        o->setScaleX(xf.scaleX);
        o->setScaleY(xf.scaleY);
        o->setVisible(xf.visible != 0);
        for (auto& m : goWorldList()) {
            std::memcpy((uint8_t*)o + m.off, s, m.size); s += m.size;
        }
        // Re-bucket. Restoring the position alone does NOT put it into collision.
        if (l) l->updateObjectSection(o);
    }
}

// The member table only covers PlayerObject / GameObject, so CCNode POSITION
// AND ROTATION ARE NOT IN THE WHITELIST. Carrying them explicitly via the
// public API is more robust than guessing names and offsetof-ing (appended at
// the end of the byte string).
// The first whitelist version dropped these, and drift=1 split by exactly one
// tick's worth (±1.95px).
constexpr size_t kExtra = 3 * sizeof(float);   // x, y, rotation

// Game-layer things that DIVERGE UNLESS RESTORED (measured with cfg `secworld`).
// After both the player and the level objects had been cleared, this is what
// was left. In lv20 wave2 the stable members that "are restored by the
// checkpoint but not by psnap" were exactly these 5 (out of 341 stable members).
//   m_extraDelta        carried-over time of the fixed step. THE PRIME
//                       SUSPECT — decides how many substeps one update runs
//   m_gameState / m_blending / m_areaObjectsUpdated / m_audioPaused
// Conversely m_attempts / m_randomSeed / m_timePlayed / m_tickIndex /
// m_timestamp VARY EVEN UNDER CHECKPOINT RESTORE, so they are left alone
// (ledgers, not physics).
// m_gameState is not put in here — it is a struct, not a scalar, so
// `captureState`/`restoreState` carry it separately by C++ assignment.
inline const char* const LAYER_RESTORE[] = {
    "m_blending", "m_areaObjectsUpdated", "m_extraDelta", "m_audioPaused",
};

// The collision window (m_left/right/bottom/topSectionIndex) MUST NOT be put in.
// GJBaseGameLayer keeps objects divided into x/y sections, and these 4 form the
// scan range. Restoring them was tried: the lv20 ship split did not move by a
// single bit, while the positive control (`secwinshift=±1`) DIED INSTANTLY WITH
// 0xC0000005. So this is not "a ledger where stale values linger" but AN
// INVARIANT IN ONE-TO-ONE CORRESPONDENCE WITH THE LIVE ARRAYS, not something
// to write from outside.

inline const std::vector<Mem>& layerRestoreList() {
    static std::vector<Mem> v = [] {
        std::vector<Mem> r;
        for (auto& m : gbScalars())
            for (auto* n : LAYER_RESTORE)
                if (std::strcmp(m.name, n) == 0) { r.push_back(m); break; }
        return r;
    }();
    return v;
}

inline size_t layerBytes() {
    size_t n = 0;
    for (auto& m : layerRestoreList()) n += m.size;
    return n;
}

// ---------------------------------------------------------------------------
// The effect manager's queues (2026-08-07, pinned down along the line of the
// user's proposal)
//
// Rather than widening the 261 by guesswork, narrow down from WHAT GD ITSELF
// SAVES IN CheckpointObject. CheckpointObject holds an `EffectManagerState`
// wholesale, whose contents are the trigger queues and the group commands in
// progress. Counting the elements that differ after a psnap restore, this was
// the first to show up.
//
// Carried by C++ assignment, not memcpy. They are containers, so a byte copy
// is out of the question (that was the cause of the first heap corruption).
// The type is known, so a plain assignment works.
// THE PRIME SUSPECT: `GJBaseGameLayer::m_gameState` (type GJGameState).
// What CheckpointObject holds BY VALUE as `GJGameState m_gameState;`.
// 29 of its 160 members are containers/pointers, and the contents are physics
// itself:
//   m_moveEffectInstances / m_rotateEffectInstances   moves/rotations in progress
//   m_dynamicMoveActions / m_dynamicRotateActions     dynamic move commands
//   m_advanceFollowInstances                          follow
//   m_gameObjectPhysics                               per-object physics
//   m_lastActivatedPortal1/2                          most recently passed portals
//   m_activatedObjectIDs / m_tweenActions / m_stateObjects
//
// Carried by C++ assignment (memcpy strictly forbidden. There are 29
// containers). Since GD itself does the same thing, it is guaranteed to be
// copyable.
//
// History: the typed table's enum classification picked up names ending in
// `...State` and misclassified GJGameState, A STRUCT, as enum = scalar. So for
// a long time it was memcpy'd wholesale, copying the pointers of 29
// containers. Fixing it by matching against the class-name list dropped it to
// OPAQUE, and it is picked up again here.
// The player-side "already touched" ledgers. The types are known, so they can
// be carried by C++ assignment.
//
// These are OPAQUE, so the whitelist does not restore them and they KEEP
// ACCUMULATING FOR THE WHOLE SEARCH. On the checkpoint path resetLevel wipes
// them clean every time, so that is where it diverges. The "orbs stop firing"
// symptom left in the beam-era records belongs to this family (with restore,
// 1 activation in 48,900 attempts; without, 400 in 8,400 attempts).
struct Touch {
    solver::PlayerFollowState<PlayerObject> follow;
    // Only the local collision columns, not every object in the section horizon.
    std::vector<solver::CollisionObjectState<GameObject>> collisionObjects;
    // pushButton walks this array in first-contact order; retain entries across branch clears.
    std::vector<geode::Ref<cocos2d::CCObject>> touchingRings;
    gd::unordered_set<int> touchedRings;
    gd::unordered_set<int> ringRelatedSet;
    gd::map<int, bool> jumpPadRelated;
    gd::unordered_map<int, GameObject*> potentialSlopeMap;
    // postCollision reads these entries, not just the scalar last-collision IDs.
    // Retain values while another branch clears the player's original dictionaries.
    std::vector<std::pair<intptr_t, geode::Ref<cocos2d::CCObject>>> collisionLogs[4];
    // References to "what am I touching right now". Pointer-typed, so not in
    // the whitelist. Objects are NOT re-created during a section, so carrying
    // the pointers does not leave them stale (if there were a path that
    // re-creates them, they must not be carried). cfg `secsnapobj=1`.
    GameObject* objectSnappedTo = nullptr;
    GameObject* collidedObject = nullptr;
    // Player 2's, when the snapshot had one (see partner below).
    std::shared_ptr<Touch> second;
    decltype(std::declval<GJBaseGameLayer>().m_queuedButtons) queuedButtons;
};

// cfg `secsnapobj=1`: also carry the two pointers above
inline bool g_snapCollideObj = false;

// THE SECOND PLAYER travels with the first. Every snapshot below used to be of player 1 alone, so
// a search through a dual section restored p1 and left p2 wherever the last branch had taken it:
// branch after branch p2 ran on ahead, and every branch "died" when p2 did. Measured on official
// lv16 with coins (cfg dpsecauto, a rung window from 800 to 5,600 ticks before its dual ball
// section): every window, wherever it began, was exhausted within ~100 ticks of the dual portal,
// the branches killed by p2 150-2,200 px ahead of p1 (`killer: who=p2`, obj=NULL or an object
// p1 had not reached), while the plan the rung was fired from flew on past that point.
// Captured whenever the layer has a player 2 (GD makes one with every level); outside a dual
// section GD does not move it, and putting back what was taken changes nothing.
inline PlayerObject* partner(PlayerObject* p, GJBaseGameLayer* l) {
    return (l && p && p == l->m_player1 && l->m_player2 && l->m_player2 != p) ? l->m_player2
                                                                            : nullptr;
}

// Defined below with the mirror-transition position handling.
inline cocos2d::CCPoint physPosition(PlayerObject* p, GJBaseGameLayer* l);

// Layout of the collision reader and its once-per-tick move guard in GD 2.2081.
#ifdef GEODE_IS_WINDOWS
static_assert(offsetof(GJBaseGameLayer, m_nonEffectObjects) == 0x35b0);
static_assert(offsetof(GJBaseGameLayer, m_calcNonEffectObjects) == 0x35e0);
static_assert(offsetof(GJBaseGameLayer, m_sectionXFactor) == 0x36a0);
static_assert(offsetof(GameObject, m_objectRect) == 0x358);
static_assert(offsetof(GameObject, m_unk4C4) == 0x4dc);
static_assert(offsetof(GameObject, m_scaleX) == 0x488);
// processPlayerFollowActions (0x22ded0), save/loadPlayerCheckpoint (0x3a1710/0x3a21b0).
static_assert(offsetof(PlayerObject, m_followRelated) == 0xabc);
static_assert(offsetof(PlayerObject, m_playerFollowFloats) == 0xac0);
#endif

inline long long g_collisionCaptures = 0, g_collisionObjects = 0;
inline size_t g_collisionMax = 0;

// Capture the collision reader's local columns for both players, without calculating rects.
inline void captureCollisionEnvironment(GJBaseGameLayer* l, Touch& out) {
    out.collisionObjects.clear();
    if (!l) return;
    std::vector<GameObject*> objects;
    for (auto* p : {l->m_player1, l->m_player2}) {
        if (!p) continue;
        const double x = std::clamp((double)physPosition(p, l).x, 0.0, 10000000.0);
        const int column = (int)(x * l->m_sectionXFactor);
        solver::appendCollisionColumns(l->m_nonEffectObjects, column, objects);
    }
    // checkCollisions also scans this unpartitioned list (2.2081 +0x35e0).
    const int n = std::min((int)l->m_calcNonEffectObjects.size(), l->m_calcNonEffectObjectsSize);
    for (int i = 0; i < n; ++i)
        if (auto* o = l->m_calcNonEffectObjects[(size_t)i]) objects.push_back(o);
    solver::uniqueCollisionObjects(objects);
    out.collisionObjects.reserve(objects.size());
    for (auto* o : objects) out.collisionObjects.push_back(solver::captureCollisionObject(o));
    ++g_collisionCaptures;
    g_collisionObjects += (long long)objects.size();
    g_collisionMax = std::max(g_collisionMax, objects.size());
}

// Restore object ledgers/geometry while preserving the live bucket owners and indices.
inline void restoreCollisionEnvironment(GJBaseGameLayer* l, const Touch& in) {
    if (!l) return;
    for (const auto& state : in.collisionObjects)
        solver::restoreCollisionObject(state, [l](GameObject* o) { l->updateObjectSection(o); });
}

// Capture the typed contact containers without copying their owning pointers.
inline void captureTouchOne(PlayerObject* p, Touch& out) {
    solver::capturePlayerFollow(p, out.follow);
    out.touchingRings.clear();
    if (auto* rings = p->m_touchingRings) {
        out.touchingRings.reserve(rings->count());
        for (unsigned i = 0; i < rings->count(); ++i)
            out.touchingRings.emplace_back(rings->objectAtIndex(i));
    }
    out.touchedRings = p->m_touchedRings;
    out.ringRelatedSet = p->m_ringRelatedSet;
    out.jumpPadRelated = p->m_jumpPadRelated;
    out.potentialSlopeMap = p->m_potentialSlopeMap;
    out.objectSnappedTo = p->m_objectSnappedTo;
    out.collidedObject = p->m_collidedObject;
    cocos2d::CCDictionary* logs[] = {p->m_collisionLogTop, p->m_collisionLogBottom,
                                    p->m_collisionLogLeft, p->m_collisionLogRight};
    for (size_t i = 0; i < 4; ++i) {
        out.collisionLogs[i].clear();
        if (!logs[i]) continue;
        for (auto [key, value] : geode::cocos::CCDictionaryExt<intptr_t>(logs[i]))
            out.collisionLogs[i].emplace_back(key, value);
    }
}

// Restore entries into the player's existing dictionaries, including empty snapshots.
inline void restoreTouchOne(PlayerObject* p, const Touch& in) {
    solver::restorePlayerFollow(p, in.follow);
    if (auto* rings = p->m_touchingRings) {
        rings->removeAllObjects();
        for (const auto& value : in.touchingRings) rings->addObject(value.data());
    }
    p->m_touchedRings = in.touchedRings;
    p->m_ringRelatedSet = in.ringRelatedSet;
    p->m_jumpPadRelated = in.jumpPadRelated;
    p->m_potentialSlopeMap = in.potentialSlopeMap;
    cocos2d::CCDictionary* logs[] = {p->m_collisionLogTop, p->m_collisionLogBottom,
                                    p->m_collisionLogLeft, p->m_collisionLogRight};
    for (size_t i = 0; i < 4; ++i) {
        if (!logs[i]) continue;
        logs[i]->removeAllObjects();
        for (const auto& [key, value] : in.collisionLogs[i])
            logs[i]->setObject(value.data(), key);
    }
    if (g_snapCollideObj) {
        p->m_objectSnappedTo = in.objectSnappedTo;
        p->m_collidedObject = in.collidedObject;
    }
}

inline void captureTouch(PlayerObject* p, Touch& out, GJBaseGameLayer* l = nullptr) {
    if (!p) return;
    if (l) out.queuedButtons = l->m_queuedButtons;
    captureCollisionEnvironment(l, out);
    captureTouchOne(p, out);
    out.second.reset();
    if (auto* p2 = partner(p, l)) {
        out.second = std::make_shared<Touch>();
        captureTouchOne(p2, *out.second);
    }
}

inline void restoreTouch(PlayerObject* p, const Touch& in, GJBaseGameLayer* l = nullptr) {
    if (!p) return;
    if (l) l->m_queuedButtons = in.queuedButtons;
    restoreTouchOne(p, in);
    if (auto* p2 = partner(p, l); p2 && in.second) restoreTouchOne(p2, *in.second);
}

// A GJGameState copied the plain way rebuilds every ordered map in it node by node: MSVC's
// std::map copy-assignment clears the destination and allocates a node per element. A section
// search copies the state in both directions many times a layer -- a snapshot per child, a restore
// per expansion -- and between two states of one search those maps differ in a few entries at
// most. Sampled on a heavy custom level's search: GJGameState's assignment and destruction were
// ~16% of the main thread, the two maps keyed by (event, id) about half of that, with the heap
// under them on top.
//
// So the ordered maps are brought level with the source in place: walked in key order together,
// what the destination lacks inserted where it goes, what it has extra erased, a value that
// differs assigned. The result holds exactly the source's keys and values in the source's order,
// which is all anything reads of an ordered map; only the nodes are not new. Everything else is
// the plain member-wise assignment, done with the maps moved out of both sides for its duration
// (a swap: O(1), and it puts the very same nodes back).
#ifdef GEODE_IS_WINDOWS
template <class M>
inline void syncOrderedMap(M& dst, const M& src) {
    auto d = dst.begin();
    auto s = src.begin();
    const auto less = dst.key_comp();
    while (s != src.end()) {
        if (d == dst.end() || less(s->first, d->first)) {
            d = std::next(dst.emplace_hint(d, *s));
            ++s;
        } else if (less(d->first, s->first)) {
            d = dst.erase(d);
        } else {
            d->second = s->second;   // a vector keeps its capacity; a scalar is a scalar
            ++d;
            ++s;
        }
    }
    while (d != dst.end()) d = dst.erase(d);
}

inline void assignState(GJGameState& dst, GJGameState& src) {
    if (&dst == &src) return;
    // Empty maps to park the four in while the rest is assigned (held across calls: an MSVC map
    // allocates its head node on construction).
    static decltype(dst.m_activatedObjectIDs) d0, s0;
    static decltype(dst.m_unkMapPairGJGameEventIntVectorEventTriggerInstance) d1, s1;
    static decltype(dst.m_unkMapPairGJGameEventIntInt) d2, s2;
    static decltype(dst.m_proximityVolumeRelated) d3, s3;
    d0.swap(dst.m_activatedObjectIDs);
    s0.swap(src.m_activatedObjectIDs);
    d1.swap(dst.m_unkMapPairGJGameEventIntVectorEventTriggerInstance);
    s1.swap(src.m_unkMapPairGJGameEventIntVectorEventTriggerInstance);
    d2.swap(dst.m_unkMapPairGJGameEventIntInt);
    s2.swap(src.m_unkMapPairGJGameEventIntInt);
    d3.swap(dst.m_proximityVolumeRelated);
    s3.swap(src.m_proximityVolumeRelated);
    dst = src;   // every other member; the four maps are empty on both sides here
    dst.m_activatedObjectIDs.swap(d0);
    src.m_activatedObjectIDs.swap(s0);
    dst.m_unkMapPairGJGameEventIntVectorEventTriggerInstance.swap(d1);
    src.m_unkMapPairGJGameEventIntVectorEventTriggerInstance.swap(s1);
    dst.m_unkMapPairGJGameEventIntInt.swap(d2);
    src.m_unkMapPairGJGameEventIntInt.swap(s2);
    dst.m_proximityVolumeRelated.swap(d3);
    src.m_proximityVolumeRelated.swap(s3);
    syncOrderedMap(dst.m_activatedObjectIDs, src.m_activatedObjectIDs);
    syncOrderedMap(dst.m_unkMapPairGJGameEventIntVectorEventTriggerInstance,
                   src.m_unkMapPairGJGameEventIntVectorEventTriggerInstance);
    syncOrderedMap(dst.m_unkMapPairGJGameEventIntInt, src.m_unkMapPairGJGameEventIntInt);
    syncOrderedMap(dst.m_proximityVolumeRelated, src.m_proximityVolumeRelated);
}
#else
// Android: GD's maps are Geode's GNU STL copies, whose swap does not compile against the NDK's
// libc++ (and whose iterators std::next does not accept). The plain member-wise assignment
// gives the same keys and values in the same order, only with new nodes and more time.
inline void assignState(GJGameState& dst, GJGameState& src) {
    if (&dst == &src) return;
    dst = src;
}
#endif

inline void captureState(GJBaseGameLayer* l, GJGameState& out) {
    if (l) assignState(out, l->m_gameState);
}

inline void restoreState(GJBaseGameLayer* l, GJGameState& in) {
    if (l) assignState(l->m_gameState, in);
}

// OBJECT-SIDE ACTIVATION: what GD's own checkpoint carries and the player snapshot did not.
// PlayLayer::saveActiveSaveObjects (win 0x3b89f0) walks the vector at PlayLayer+0x3808 and,
// for each object whose hasBeenActivated() (vtable +0x568) is true, saves the bytes at +0x5b4
// and +0x5b5 -- EnhancedGameObject::m_activatedByPlayer1/2 -- as a SavedActiveObjectState. A
// checkpoint restore resets every object and puts those back. psnap resets nothing, so once
// one branch had taken a portal, pad or ring, every branch expanded after it found that object
// already used. Measured on a custom level from a head at t=23,928: under psnap the plan's own
// rollout missed the gravity portal GD takes at t=23,971, which the checkpoint path reproduces,
// and a window the checkpoint path solves (t=24,328, depth 454) came back EXHAUSTED at depth
// 403; carrying the flags, psnap solves it to the same state as the checkpoint path.
// NOT ONLY GD's LIST. The objects that list holds survive a checkpoint restore; everything
// else is simply reset by resetLevel -- rings among them (a respawn gives the orbs back). psnap
// resets neither, so it carries every EnhancedGameObject in reach (the class that declares the
// two flags).
// Only the objects within the section's reach are carried: a node costs a byte per object in
// reach, not the whole level. cfg `secsnapact=0` turns it off for A/B.
// NOT THE DECORATIONS. A decoration (type 7) is never collided with, so its two flags stay
// clear through any search and carrying them carries nothing -- but their zero bytes did go into
// the signature below, and the signature orders the cap's buckets. A level solved on its slice
// (mod/level_slice.hpp), which drops decorations, therefore capped different branches than the
// level itself from the same nodes: on a custom level with 13,797 ungrouped decorations dropped,
// the search from t=24,680 split from the level's own at layer 164 and came back EXHAUSTED at
// depth 322 where the level's was SOLVED, the plain game being identical on the two on every
// tick of the route. The decorations in reach are kept aside only to check that none of them is
// ever activated: their flags are read on every capture and every restore and OR-ed together, so
// a flag that was set and cleared again between two ends of a search is still seen (`decoEver`
// on the secsnapact line; `decoUsed` is the same count at the end of the search alone).
inline bool g_snapAct = true;
inline std::vector<EnhancedGameObject*> g_actObjs;
inline std::vector<EnhancedGameObject*> g_actDecos;
inline std::vector<uint8_t> g_decoSeen;   // per g_actDecos: either flag ever seen set
// What the carry costs and what it carried, for the `secsnapact:` line at the end of a
// search: calls, time spent in them, and which objects were ever seen activated by each player
// in a captured node (the P2 column is the dual sections' half).
inline long long g_actCaptures = 0, g_actRestores = 0;
inline double g_actCapUs = 0.0, g_actRestUs = 0.0;
inline std::vector<uint8_t> g_actSeen;

inline void buildActWindow(GJBaseGameLayer* l, double x0, double x1) {
    g_actObjs.clear();
    g_actDecos.clear();
    g_actSeen.clear();
    g_actCaptures = g_actRestores = 0;
    g_actCapUs = g_actRestUs = 0.0;
    if (!g_snapAct || !l || !l->m_objects) return;
    for (unsigned i = 0; i < l->m_objects->count(); ++i) {
        auto* o = static_cast<GameObject*>(l->m_objects->objectAtIndex(i));
        if (!o) continue;
        const double x = o->getPositionX();
        if (x < x0 || x > x1) continue;
        auto* e = typeinfo_cast<EnhancedGameObject*>(o);
        if (!e) continue;
        (o->m_objectType == GameObjectType::Decoration ? g_actDecos : g_actObjs).push_back(e);
    }
    g_actSeen.assign(g_actObjs.size(), 0);
    g_decoSeen.assign(g_actDecos.size(), 0);
}

// OR the decorations' flags into g_decoSeen (called on every capture and restore).
inline void noteDecos() {
    for (size_t i = 0; i < g_actDecos.size(); ++i) {
        const auto* e = g_actDecos[i];
        if (e->m_activatedByPlayer1 || e->m_activatedByPlayer2) g_decoSeen[i] = 1;
    }
}

// The decorations in reach that carry either flag now. Anything but 0 at the end of a search
// means a decoration can be activated after all, and leaving them out of the carry is wrong.
inline int actDecosUsed() {
    int n = 0;
    for (const auto* e : g_actDecos)
        n += (e->m_activatedByPlayer1 || e->m_activatedByPlayer2) ? 1 : 0;
    return n;
}

// ...and the ones seen with a flag at any capture or restore of the search.
inline int actDecosEver() {
    noteDecos();
    int n = 0;
    for (uint8_t s : g_decoSeen) n += s;
    return n;
}

// The activation bytes AND their 64-bit signature, in one pass. The signature is what makes two
// nodes with the same player state but a different set of used objects two different states
// (secsolve's dedupe key and the cap's buckets, cfg `secactkey`): the future of "ring A already
// taken" is not the future of "ring A still there", and a key without it kept whichever came
// first. FNV-1a over the bytes in window order, so WHICH object differs
// changes the signature, not only how many.
inline uint64_t actSignature(const std::vector<uint8_t>& v) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : v) { h ^= b; h *= 1099511628211ull; }
    return h;
}

inline uint64_t captureActSig(std::vector<uint8_t>& out) {
    const auto t0 = std::chrono::steady_clock::now();
    out.resize(g_actObjs.size());
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < g_actObjs.size(); ++i) {
        const auto* e = g_actObjs[i];
        out[i] = (uint8_t)((e->m_activatedByPlayer1 ? 1 : 0) | (e->m_activatedByPlayer2 ? 2 : 0));
        g_actSeen[i] |= out[i];
        h ^= out[i];
        h *= 1099511628211ull;
    }
    noteDecos();
    ++g_actCaptures;
    g_actCapUs += std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - t0).count();
    return h;
}

inline void captureAct(std::vector<uint8_t>& out) { (void)captureActSig(out); }

// cfg `secactkey=0`: keep carrying the flags but leave them out of the key and the buckets.
inline bool g_actKey = true;
inline constexpr uint64_t kActKeyMul = 0xC2B2AE3D27D4EB4Full;

// Self-check at the head of a search: two activation vectors that differ in exactly one
// object must give two signatures and two keys, and differing at the first or at the last
// object must not coincide either. Returns 1 when all hold, 0 when not, -1 with nothing in
// reach to test.
inline int actKeySelfCheck(const std::vector<uint8_t>& base, long long playerKey) {
    if (base.empty()) return -1;
    std::vector<uint8_t> a = base, b = base;
    a.front() ^= 1;
    b.back() ^= 2;
    const uint64_t s0 = actSignature(base), sa = actSignature(a), sb = actSignature(b);
    const long long k0 = playerKey ^ (long long)(s0 * kActKeyMul);
    const long long ka = playerKey ^ (long long)(sa * kActKeyMul);
    const long long kb = playerKey ^ (long long)(sb * kActKeyMul);
    const bool ok = s0 != sa && s0 != sb && (base.size() == 1 || sa != sb)
                    && k0 != ka && k0 != kb;
    return ok ? 1 : 0;
}

inline void restoreAct(const std::vector<uint8_t>& in) {
    if (in.size() != g_actObjs.size()) return;
    const auto t0 = std::chrono::steady_clock::now();
    noteDecos();   // what the step before this restore left on them
    for (size_t i = 0; i < g_actObjs.size(); ++i) {
        g_actObjs[i]->m_activatedByPlayer1 = (in[i] & 1) != 0;
        g_actObjs[i]->m_activatedByPlayer2 = (in[i] & 2) != 0;
    }
    ++g_actRestores;
    g_actRestUs += std::chrono::duration<double, std::micro>(
        std::chrono::steady_clock::now() - t0).count();
}

// Of the objects a node has used (any bit set in `want`), how many the live game has lost (a
// bit set in `want` and clear on the object now). Reads only: the `secorigin:` line counts it
// where a cross-check replay starts, to show whether its origin's world is the node's.
inline std::pair<long long, long long> originUsedLost(const std::vector<uint8_t>& want) {
    if (want.size() != g_actObjs.size()) return {0, 0};
    long long used = 0, lost = 0;
    for (size_t i = 0; i < g_actObjs.size(); ++i) {
        if (!want[i]) continue;
        ++used;
        const uint8_t now = (uint8_t)((g_actObjs[i]->m_activatedByPlayer1 ? 1 : 0)
                                      | (g_actObjs[i]->m_activatedByPlayer2 ? 2 : 0));
        if (want[i] & ~now) ++lost;
    }
    return {used, lost};
}

// Objects ever seen activated by player 1 / player 2 in a captured node.
inline int actSeenCount(uint8_t bit) {
    int n = 0;
    for (uint8_t s : g_actSeen) n += (s & bit) ? 1 : 0;
    return n;
}

// cfg `secsnapem` (on by default since 2026-10; 0 = off): the effect manager in full, as GD's own
// checkpoint keeps it (GJEffectManager::saveToState 0x263e80 / loadFromState 0x2644a0, into an
// EffectManagerState), not only its pulse queue. The group commands in progress live there -- among
// them the ones a Keyframe Animation trigger (id 3033) runs, created by createKeyframeCommand --
// and the snapshot carried none of them. Measured on MOAI's ladder window at t=9,628: the step just
// after the player crossed a 3033 at x=13,995 (target group 736) put decoration uid 47313, a member
// of that group, at (NaN, NaN).
inline bool g_snapEM = true;
// cfg `secsnapemfrom=<tick>` (a probe, 0 = every search): only a search whose head is at this tick or
// later carries the manager in full, so a run can reach a late window the way it always does and be
// compared there. g_snapEMActive is set at each search's head (hooks_gamelayer).
inline long long g_snapEMFrom = 0;
inline bool g_snapEMActive = false;

struct EMSnap {
    gd::vector<PulseEffectAction> pulses;
    std::shared_ptr<EffectManagerState> full;   // cfg secsnapem only
    decltype(std::declval<GJEffectManager>().m_unkMap460) disabledGroups;
};
using EMState = EMSnap;
inline size_t g_emCommandsMax = 0, g_emCompletedMax = 0;

// Use GD's native state inventory, with group membership kept for pre-restore toggles.
inline void captureEM(GJBaseGameLayer* l, EMSnap& out) {
    auto* em = l ? l->m_effectManager : nullptr;
    if (!em) {
        out.pulses.clear();
        out.full.reset();
        out.disabledGroups.clear();
        return;
    }
    out.pulses = em->m_pulseEffectVector;
    out.disabledGroups = em->m_unkMap460;
    g_emCommandsMax = std::max(g_emCommandsMax, em->m_unkVector560.size());
    g_emCompletedMax = std::max(g_emCompletedMax, em->m_unkMap578.size());
    if (g_snapEMActive) {
        out.full = std::make_shared<EffectManagerState>();   // fresh: saveToState may append
        em->saveToState(*out.full);
    } else {
        out.full.reset();
    }
}

// Toggle actual members before restoring their saved geometry and counters.
inline void restoreGroups(GJBaseGameLayer* l, const EMSnap& in) {
    if (!l || !l->m_effectManager || !g_snapEMActive) return;
    const auto& live = l->m_effectManager->m_unkMap460;
    std::vector<std::pair<int, bool>> changes;
    for (int group : live)
        if (in.disabledGroups.find(group) == in.disabledGroups.end())
            changes.emplace_back(group, true);
    for (int group : in.disabledGroups)
        if (live.find(group) == live.end()) changes.emplace_back(group, false);
    std::sort(changes.begin(), changes.end());
    for (const auto& [group, enabled] : changes) l->toggleGroup(group, enabled);
}

// A node is restored once per branch and again by the cross-check, so the state is loaded from a copy
// (whether loadFromState takes the containers out of what it is given has not been looked at).
inline void restoreEM(GJBaseGameLayer* l, const EMSnap& in) {
    auto* em = l ? l->m_effectManager : nullptr;
    if (!em) return;
    restoreGroups(l, in);
    if (g_snapEMActive && in.full) {
        EffectManagerState copy = *in.full;
        em->loadFromState(copy);
    }
    em->m_pulseEffectVector = in.pulses;
}

// Where GD's physics will start the player from on the next update. Not always the node's
// position: while a mirror transition runs (layer+0x41c strictly between 0 and 1),
// GJBaseGameLayer::update (0x237850 in 2.2081) ends by storing the position in m_position
// (player+0xa90, unless player+0xa2a is set) and then moving the NODE to its screen-flipped place
// for drawing, x + t * (winWidth / zoom - 2 * (x - cameraX)); the next update starts by putting the
// node back from m_position. A section search steps with the whole update (secStep), so a node read
// between two steps is the drawing position. Captured as the player's place, it went into physics
// on restore: on a custom level (2026-09-26), from the tick the player touches the mirror portal at x=13,095,
// psnap's x ran 0.7, 1.7, 2.7 ... px a tick ahead of GD's own run and every window died short of
// the wall. Outside a transition the two are the same point, so nothing else moves.
static_assert(offsetof(PlayerObject, m_position) == gdoff::kPlayerPosition,
              "PlayerObject::m_position moved");
inline cocos2d::CCPoint physPosition(PlayerObject* p, GJBaseGameLayer* l) {
    if (p && l) {
        const auto* lb = reinterpret_cast<const uint8_t*>(l);
        const float t = *reinterpret_cast<const float*>(lb + gdoff::kLayerLevelFlipping);
        const bool stored = reinterpret_cast<const uint8_t*>(p)[gdoff::kPlayerLocked] == 0;
        if (t > 0.f && t < 1.f && stored) return p->m_position;
    }
    return p ? p->getPosition() : cocos2d::CCPoint{};
}

// One player's bytes and its node extras (x, y, rotation), at dst.
inline void capturePlayer(PlayerObject* p, uint8_t* dst, GJBaseGameLayer* l = nullptr) {
    std::memcpy(dst, (const void*)p, sizeof(PlayerObject));
    const cocos2d::CCPoint at = physPosition(p, l);
    const float ex[3] = {at.x, at.y, p->getRotation()};
    std::memcpy(dst + sizeof(PlayerObject), ex, kExtra);
}

// Layout: player 1, its extras, the layer members, then player 2 and its extras when the layer
// has one (partner). restore() reads the same layout back under the same condition.
// captureRaw leaves out the pointer sentinel (observe), whose count a snapshot taken outside the
// search's own nodes must not move.
inline void captureRaw(PlayerObject* p, GJBaseGameLayer* l, std::vector<uint8_t>& out) {
    PlayerObject* p2 = partner(p, l);
    const size_t one = sizeof(PlayerObject) + kExtra;
    out.resize(one + layerBytes() + (p2 ? one : 0));
    capturePlayer(p, out.data(), l);
    size_t o = one;
    for (auto& m : layerRestoreList()) {
        std::memcpy(out.data() + o, (const uint8_t*)l + m.off, m.size);
        o += m.size;
    }
    if (p2) capturePlayer(p2, out.data() + o, l);
}

inline void capture(PlayerObject* p, GJBaseGameLayer* l,
                    std::vector<uint8_t>& out) {
    observe(p);
    if (PlayerObject* p2 = partner(p, l)) observe(p2);
    captureRaw(p, l, out);
}

// ---- cfg seccpcheck (print only): what a section search's checkpoint load leaves ----
// The members a load was measured to leave as the branch before had them (secsolve::g_cpPlayer),
// read from three snapshots of one point in capture()'s layout: the point's own (origin), the
// player right after the load (what seccpplayer=0 searches on), and after the write-back.
inline bool g_cpCheck = false;
inline const char* const kCpCheckP1[] = {"m_stateRingJump"};
inline const char* const kCpCheckP2[] = {"m_padRingRelated", "m_stateJumpBuffered", "m_wasRobotJump",
                                         "m_blackOrbRelated", "m_yVelocityBeforeSlope",
                                         "m_slopeStartTime"};

inline const Mem* scalarNamed(const char* n) {
    for (const auto& m : scalars())
        if (std::strcmp(m.name, n) == 0) return &m;
    return nullptr;
}

inline std::string cpValue(const uint8_t* p, size_t n) {
    char b[40];
    if (n == 1) snprintf(b, sizeof(b), "%d", (int)p[0]);
    else if (n == 4) { float f; std::memcpy(&f, p, 4); snprintf(b, sizeof(b), "%.6g", (double)f); }
    else if (n == 8) { double d; std::memcpy(&d, p, 8); snprintf(b, sizeof(b), "%.9g", d); }
    else snprintf(b, sizeof(b), "(%zu bytes)", n);
    return b;
}

// One player's named members: "name=origin/load/after" each, and the FNV-1a hash of the members'
// bytes per snapshot. `off` = where that player starts in the layout; false when a snapshot has no
// such player.
inline bool cpPlayerPart(const std::vector<uint8_t>* s[3], size_t off, const char* const* names,
                         size_t n, std::string& out, uint32_t h[3]) {
    for (int k = 0; k < 3; ++k)
        if (s[k]->size() < off + sizeof(PlayerObject)) return false;
    for (int k = 0; k < 3; ++k) h[k] = 2166136261u;
    for (size_t i = 0; i < n; ++i) {
        const Mem* m = scalarNamed(names[i]);
        if (!m) { out += std::string(" ") + names[i] + "=?"; continue; }
        out += std::string(" ") + names[i] + "=";
        for (int k = 0; k < 3; ++k) {
            const uint8_t* p = s[k]->data() + off + m->off;
            for (size_t j = 0; j < m->size; ++j) { h[k] ^= p[j]; h[k] *= 16777619u; }
            out += (k ? "/" : "") + cpValue(p, m->size);
        }
    }
    return true;
}

inline std::string cpCheckLine(const char* site, long long at, const std::vector<uint8_t>& origin,
                               const std::vector<uint8_t>& load, const std::vector<uint8_t>& after,
                               bool writeBack) {
    const std::vector<uint8_t>* s[3] = {&origin, &load, &after};
    std::string p1, p2;
    uint32_t h1[3] = {0, 0, 0}, h2[3] = {0, 0, 0};
    cpPlayerPart(s, 0, kCpCheckP1, sizeof(kCpCheckP1) / sizeof(kCpCheckP1[0]), p1, h1);
    const bool has2 = cpPlayerPart(s, sizeof(PlayerObject) + kExtra + layerBytes(), kCpCheckP2,
                                   sizeof(kCpCheckP2) / sizeof(kCpCheckP2[0]), p2, h2);
    char b[200];
    snprintf(b, sizeof(b), " | hash origin/load/after p1=%08x/%08x/%08x p2=%08x/%08x/%08x | "
             "load=origin:%s after=origin:%s", h1[0], h1[1], h1[2], h2[0], h2[1], h2[2],
             (h1[1] == h1[0] && (!has2 || h2[1] == h2[0])) ? "yes" : "no",
             (h1[2] == h1[0] && (!has2 || h2[2] == h2[0])) ? "yes" : "no");
    return std::string("seccp: site=") + site + " at=" + std::to_string(at) + " writeback="
           + (writeBack ? "1" : "0") + " p1[" + p1 + " ] p2[" + (has2 ? p2 : " none") + " ]" + b;
}

// Injection of the raw snapshot (the old warp's `injectBytes` itself).
// CALL AT A FRAME BOUNDARY: called in the middle of a substep, the remaining
// substeps run on top of the injected state, create a transition impossible in
// one substep, and diverge immediately.
// Writes back only the whitelisted range. NOT A SINGLE POINTER MOVES, so
// alternately restoring snapshots taken at different times lets no stale
// pointer in.
// One player's whitelisted bytes and extras back from src (see capturePlayer).
// Raises g_psnapPlayerWritten (config.hpp): the mode flags now hold the snapshot's while the
// mode sprites stay as they were (repair.hpp tidyPlayerModes).
inline void restorePlayer(PlayerObject* p, const uint8_t* bytes) {
    g_psnapPlayerWritten = true;
    const auto& m = mask();
    uint8_t* dst = (uint8_t*)p;
    size_t i = 0;
    const size_t n = m.size();
    while (i < n) {
        if (!m[i] || i < g_maskLo || i >= g_maskHi
            || (i >= g_maskExLo && i < g_maskExHi)) { ++i; continue; }
        size_t j = i;
        while (j < n && m[j] && j < g_maskHi
               && !(j >= g_maskExLo && j < g_maskExHi)) ++j;
        std::memcpy(dst + i, bytes + i, j - i);
        i = j;
    }
    if (!g_skipExtras) {
        float ex[3];
        std::memcpy(ex, bytes + sizeof(PlayerObject), kExtra);
        p->setPosition({ex[0], ex[1]});
        p->setRotation(ex[2]);
    }
}

inline void restore(PlayerObject* p, GJBaseGameLayer* l, const uint8_t* bytes) {
    restorePlayer(p, bytes);
    size_t o = sizeof(PlayerObject) + kExtra;
    for (auto& m : layerRestoreList()) {
        std::memcpy((uint8_t*)l + m.off, bytes + o, m.size);
        o += m.size;
    }
    PlayerObject* p2 = partner(p, l);
    if (p2) {
        restorePlayer(p2, bytes + o);
        if (!g_keepSnapPos && !g_skipExtras) p2->m_position = p2->getPosition();
    }
    // In fast mode m_position is "the value at the start of the update batch"
    // and is 0-3 substeps stale. GD re-adopts it as the physics position in the
    // next batch, so write the new value back.
    //
    // BUT writing it back erases "the previous position". GD's collision looks
    // at the sweep from m_position → current position, so making them equal
    // gives a segment of length 0, and a trajectory that dips in and comes back
    // within that tick passes straight through. cfg `secpos=1` uses the
    // snapshot's value as-is (for isolating the cause).
    if (!g_keepSnapPos && !g_skipExtras) p->m_position = p->getPosition();
}

}  // namespace psnap
