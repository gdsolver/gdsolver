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
        // Public API first, scalar copy after. GameObject::setPosition sets the
        // dirty flag, so it has to go through the API, but as a side effect it
        // also rewrites ledgers such as m_positionX. Overwrite with the copied
        // values so that "the value at capture time" remains rather than "the
        // result of going through the API".
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
    gd::unordered_set<int> touchedRings;
    gd::unordered_set<int> ringRelatedSet;
    gd::map<int, bool> jumpPadRelated;
    gd::unordered_map<int, GameObject*> potentialSlopeMap;
    // References to "what am I touching right now". Pointer-typed, so not in
    // the whitelist. Objects are NOT re-created during a section, so carrying
    // the pointers does not leave them stale (if there were a path that
    // re-creates them, they must not be carried). cfg `secsnapobj=1`.
    GameObject* objectSnappedTo = nullptr;
    GameObject* collidedObject = nullptr;
};

// cfg `secsnapobj=1`: also carry the two pointers above
inline bool g_snapCollideObj = false;

inline void captureTouch(PlayerObject* p, Touch& out) {
    if (!p) return;
    out.touchedRings = p->m_touchedRings;
    out.ringRelatedSet = p->m_ringRelatedSet;
    out.jumpPadRelated = p->m_jumpPadRelated;
    out.potentialSlopeMap = p->m_potentialSlopeMap;
    out.objectSnappedTo = p->m_objectSnappedTo;
    out.collidedObject = p->m_collidedObject;
}

inline void restoreTouch(PlayerObject* p, const Touch& in) {
    if (!p) return;
    p->m_touchedRings = in.touchedRings;
    p->m_ringRelatedSet = in.ringRelatedSet;
    p->m_jumpPadRelated = in.jumpPadRelated;
    p->m_potentialSlopeMap = in.potentialSlopeMap;
    if (g_snapCollideObj) {
        p->m_objectSnappedTo = in.objectSnappedTo;
        p->m_collidedObject = in.collidedObject;
    }
}

inline void captureState(GJBaseGameLayer* l, GJGameState& out) {
    if (l) out = l->m_gameState;
}

inline void restoreState(GJBaseGameLayer* l, const GJGameState& in) {
    if (l) l->m_gameState = in;
}

inline void captureEM(GJBaseGameLayer* l, gd::vector<PulseEffectAction>& out) {
    if (auto* em = l ? l->m_effectManager : nullptr)
        out = em->m_pulseEffectVector;
    else
        out.clear();
}

inline void restoreEM(GJBaseGameLayer* l,
                      const gd::vector<PulseEffectAction>& in) {
    if (auto* em = l ? l->m_effectManager : nullptr)
        em->m_pulseEffectVector = in;
}

inline void capture(PlayerObject* p, GJBaseGameLayer* l,
                    std::vector<uint8_t>& out) {
    observe(p);
    out.resize(sizeof(PlayerObject) + kExtra + layerBytes());
    std::memcpy(out.data(), (const void*)p, sizeof(PlayerObject));
    const float ex[3] = {p->getPositionX(), p->getPositionY(), p->getRotation()};
    std::memcpy(out.data() + sizeof(PlayerObject), ex, kExtra);
    size_t o = sizeof(PlayerObject) + kExtra;
    for (auto& m : layerRestoreList()) {
        std::memcpy(out.data() + o, (const uint8_t*)l + m.off, m.size);
        o += m.size;
    }
}

// Injection of the raw snapshot (the old warp's `injectBytes` itself).
// CALL AT A FRAME BOUNDARY: called in the middle of a substep, the remaining
// substeps run on top of the injected state, create a transition impossible in
// one substep, and diverge immediately.
// Writes back only the whitelisted range. NOT A SINGLE POINTER MOVES, so
// alternately restoring snapshots taken at different times lets no stale
// pointer in.
inline void restore(PlayerObject* p, GJBaseGameLayer* l, const uint8_t* bytes) {
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
    size_t o = sizeof(PlayerObject) + kExtra;
    for (auto& m : layerRestoreList()) {
        std::memcpy((uint8_t*)l + m.off, bytes + o, m.size);
        o += m.size;
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
