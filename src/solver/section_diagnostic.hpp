#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace p1::solver {

// Observations only: no pointers or restorable game state are retained here.
struct SectionDiagnosticPlayer {
    bool present = false, dead = false, grounded = false, flipped = false;
    bool holding = false, jumpBuffered = false, wasJumpBuffered = false;
    bool stateRingJump = false;
    int stateJumpBuffered = 0;
    int mode = -1, snapped = -1, collided = -1;
    double x = 0, y = 0, vy = 0, previousX = 0, previousY = 0, size = 0;
    double lastX = 0, lastY = 0, slopeCorrection = 0;
    bool onSlope = false, wasOnSlope = false;
    double playerSpeed = 0, speedMultiplier = 0, gravity = 0;
    double reverseSpeed = 0, reverseAcceleration = 0, dashX = 0, dashY = 0;
    double lastLandTime = 0;
    bool dashing = false, touchedPad = false, justPlacedStreak = false;
    bool goingLeft = false, sideways = false;
    std::array<int, 4> collisionCounts{};
    std::array<uint64_t, 4> collisionKeyHashes{};
    std::array<std::vector<intptr_t>, 4> collisionKeys;
    size_t touchingRingsCount = 0;
    uint64_t touchingRingsHash = 1469598103934665603ull;
    std::vector<int> touchingRings;
    int slope = -1;
    double spiderLow = 0, spiderHigh = 0;
    int followCursor = 0;
    size_t followHeights = 0;
    uint64_t followHash = 1469598103934665603ull;
};

struct SectionDiagnosticObject {
    int uid = -1, id = -1;
    double x = 0, y = 0, rotation = 0, scaleX = 0, scaleY = 0;
    double rectX = 0, rectY = 0, rectW = 0, rectH = 0;
    double positionX = 0, positionY = 0, lastX = 0, lastY = 0;
    int moveTick = 0;
    bool rectDirty = false, disabled = false, activated1 = false, activated2 = false;
};

struct SectionDiagnosticGeometryDifference {
    const char* field = nullptr;
    int uid = -1;
};

// Compare shared bounded probes by UID; a missing probe is not evidence of a missing object.
inline SectionDiagnosticGeometryDifference sectionDiagnosticGeometryDifference(
    const std::vector<SectionDiagnosticObject>& fast,
    const std::vector<SectionDiagnosticObject>& replay, double tolerance) {
    for (const auto& p : fast) for (const auto& q : replay) {
        if (p.uid != q.uid) continue;
        if (std::fabs(p.x - q.x) > tolerance || std::fabs(p.y - q.y) > tolerance
            || std::fabs(p.rotation - q.rotation) > tolerance
            || std::fabs(p.scaleX - q.scaleX) > tolerance
            || std::fabs(p.scaleY - q.scaleY) > tolerance) return {"object.transform", p.uid};
        if (std::fabs(p.positionX - q.positionX) > tolerance
            || std::fabs(p.positionY - q.positionY) > tolerance) return {"object.ledger", p.uid};
        if (std::fabs(p.lastX - q.lastX) > tolerance || std::fabs(p.lastY - q.lastY) > tolerance
            || p.moveTick != q.moveTick) return {"object.move_history", p.uid};
        if (std::fabs(p.rectX - q.rectX) > tolerance || std::fabs(p.rectY - q.rectY) > tolerance
            || std::fabs(p.rectW - q.rectW) > tolerance || std::fabs(p.rectH - q.rectH) > tolerance
            || p.rectDirty != q.rectDirty) return {"object.rect_cache", p.uid};
        if (p.disabled != q.disabled || p.activated1 != q.activated1 || p.activated2 != q.activated2)
            return {"object.activation", p.uid};
    }
    return {};
}

// Bounded queue contents explain input differences without retaining game-owned containers.
struct SectionDiagnosticButton {
    int button = 0, step = 0;
    bool push = false, player2 = false;
    double timestamp = 0;
};

// Ordered command observations exclude allocator addresses and struct padding.
struct SectionDiagnosticMotion {
    int uid = 0, type = 0, group = 0, center = 0, trigger = 0, control = 0;
    uint32_t flags = 0;
    uint64_t remapHash = 1469598103934665603ull;
    std::array<double, 18> values{};
};

// The native reader uses an active prefix, not the entire allocated bucket.
struct SectionDiagnosticCandidate {
    int uid = -1, type = -1, index = -1;
    unsigned flags = 0;
};

struct SectionDiagnosticCollisionList {
    int player = 0, x = -1, y = -1, active = 0;
    bool present = false, countKnown = false, sortKnown = false, needsSort = false;
    size_t storage = 0, read = 0;
    uint64_t orderHash = 1469598103934665603ull, filterHash = 1469598103934665603ull;
    std::vector<SectionDiagnosticCandidate> candidates;
};

inline constexpr size_t kSectionDiagnosticCandidateLimit = 16;

// Hash every active entry in its stored order; keep only a bounded printable prefix.
template <class Objects>
SectionDiagnosticCollisionList sectionDiagnosticCollisionList(const Objects* objects, int active) {
    SectionDiagnosticCollisionList out;
    out.present = objects != nullptr; out.active = active;
    if (!objects) return out;
    out.storage = objects->size();
    out.read = active > 0 ? std::min((size_t)active, out.storage) : 0;
    for (size_t i = 0; i < out.read; ++i) {
        auto* o = (*objects)[i];
        SectionDiagnosticCandidate v;
        if (o) {
            v.uid = o->m_uniqueID; v.type = (int)o->m_objectType;
            v.index = o->m_innerSectionIndex;
            v.flags = (unsigned)o->m_isGroupDisabled | ((unsigned)o->m_isDisabled << 1);
        }
        out.orderHash ^= (uint32_t)v.uid; out.orderHash *= 1099511628211ull;
        out.filterHash ^= (uint32_t)v.type; out.filterHash *= 1099511628211ull;
        out.filterHash ^= v.flags; out.filterHash *= 1099511628211ull;
        if (out.candidates.size() < kSectionDiagnosticCandidateLimit) out.candidates.push_back(v);
    }
    return out;
}

// Explain list differences without treating them as proof of a physics-output failure.
inline const char* sectionDiagnosticCollisionListDifference(const SectionDiagnosticCollisionList& a,
                                                            const SectionDiagnosticCollisionList& b) {
    if (a.player != b.player || a.x != b.x || a.y != b.y) return "collision.query_window";
    if (a.present != b.present || a.countKnown != b.countKnown || a.storage != b.storage
        || a.active != b.active || a.read != b.read) return "collision.counts";
    if (a.sortKnown != b.sortKnown || a.needsSort != b.needsSort) return "collision.sort_flag";
    if (a.orderHash != b.orderHash) return "collision.candidate_order";
    if (a.filterHash != b.filterHash) return "collision.candidate_flags";
    return nullptr;
}

// Actual solid calls are recorded separately from broad-phase bucket membership.
struct SectionDiagnosticCollisionContact {
    int player = 0, uid = -1, type = -1;
    unsigned flags = 0;
    bool skip = false, result = false, finished = false, rectDirty = false;
    double dt = 0, x = 0, y = 0, vy = 0, afterX = 0, afterY = 0, afterVy = 0;
    std::array<double, 4> rect{};
    double lastX = 0, lastY = 0, slopeCorrection = 0;
    bool onSlope = false, wasOnSlope = false;
};

// Hash the measured command fields, not pointer caches regenerated on the next tick.
inline uint64_t sectionDiagnosticMotionHash(const SectionDiagnosticMotion& motion) {
    uint64_t h = motion.remapHash;
    for (int v : {motion.uid, motion.type, motion.group, motion.center, motion.trigger, motion.control}) {
        h ^= (uint32_t)v; h *= 1099511628211ull;
    }
    h ^= motion.flags; h *= 1099511628211ull;
    for (double v : motion.values) {
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        h ^= bits; h *= 1099511628211ull;
    }
    return h;
}

// Order-independent entry hashes allow unordered maps/sets to be compared by value.
inline uint64_t sectionDiagnosticEntryHash(uint64_t value) {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

struct SectionDiagnosticState {
    std::array<SectionDiagnosticPlayer, 2> players;
    std::vector<SectionDiagnosticObject> objects;
    bool deathCall = false, dual = false, anticheat = false;
    int caller = 0, killer = -1;
    int held = 0, feed = 0;
    size_t queuedButtons = 0;
    std::vector<SectionDiagnosticButton> buttons;
    double extraDelta = 0;
    double pendingSpeed = 0;
    bool pendingSpeedNoEffects = false;
    size_t moves = 0, rotations = 0, activated = 0;
    size_t dynamicMoves = 0, dynamicRotations = 0;
    uint64_t activatedHash = 1469598103934665603ull;
    size_t motionCount = 0, completedMoveCount = 0, followingCount = 0;
    size_t triggeredIDCount = 0;
    uint64_t triggeredIDHash = 0;
    size_t disabledGroupCount = 0;
    uint64_t disabledGroupHash = 0;
    std::vector<int> disabledGroups;
    uint64_t motionHash = 1469598103934665603ull, completedMoveHash = 0, followingHash = 0;
    std::vector<SectionDiagnosticMotion> motionCommands;
    bool collisionEnvironmentObserved = false;
    std::vector<SectionDiagnosticCollisionList> collisionLists;
};

// A bounded call trace shares the node's existing replay, never another physics pass.
struct SectionDiagnosticStage {
    const char* name = "";
    bool duringStep = false;
    int actor = 0, argument = 0, flags = 0, result = -1;
    double dt = 0;
    SectionDiagnosticState state;
};

struct SectionDiagnosticStep {
    int depth = 0, input = 0;
    SectionDiagnosticState before, after;
    std::vector<SectionDiagnosticStage> stages;
    size_t stagesDropped = 0;
    std::vector<SectionDiagnosticCollisionList> collisionBatches;
    std::vector<SectionDiagnosticCollisionContact> collisionContacts;
    size_t collisionBatchesDropped = 0, collisionContactsDropped = 0;
};

inline constexpr size_t kSectionDiagnosticStageLimit = 48;
inline constexpr size_t kSectionDiagnosticBatchLimit = 32, kSectionDiagnosticContactLimit = 64;

// Compare actual batch inputs independently of earlier unrelated input-buffer differences.
inline const char* sectionDiagnosticBatchesDifference(const SectionDiagnosticStep& a,
                                                      const SectionDiagnosticStep& b, size_t& index) {
    const size_t common = std::min(a.collisionBatches.size(), b.collisionBatches.size());
    for (index = 0; index < common; ++index)
        if (const auto* field = sectionDiagnosticCollisionListDifference(a.collisionBatches[index],
                                                                        b.collisionBatches[index])) return field;
    if (a.collisionBatches.size() != b.collisionBatches.size()) return "collision.batch_count";
    if (a.collisionBatchesDropped || b.collisionBatchesDropped) return "collision.trace_incomplete";
    return nullptr;
}

// Find the first differing solid call, retaining entry caches separately from resolution output.
inline const char* sectionDiagnosticContactsDifference(const SectionDiagnosticStep& a,
                                                       const SectionDiagnosticStep& b, size_t& index,
                                                       double tolerance) {
    const size_t common = std::min(a.collisionContacts.size(), b.collisionContacts.size());
    for (index = 0; index < common; ++index) {
        const auto& x = a.collisionContacts[index];
        const auto& y = b.collisionContacts[index];
        if (x.player != y.player || x.uid != y.uid || x.type != y.type) return "collision.solid_order";
        if (x.flags != y.flags || x.skip != y.skip || std::fabs(x.dt - y.dt) > 1e-9)
            return "collision.solid_arguments";
        if (x.onSlope != y.onSlope || x.wasOnSlope != y.wasOnSlope
            || std::fabs(x.slopeCorrection - y.slopeCorrection) > tolerance)
            return "collision.slope_state";
        if (std::fabs(x.lastX - y.lastX) > tolerance || std::fabs(x.lastY - y.lastY) > tolerance)
            return "collision.sweep_history";
        if (x.rectDirty != y.rectDirty) return "collision.solid_cache";
        for (size_t i = 0; i < x.rect.size(); ++i)
            if (std::fabs(x.rect[i] - y.rect[i]) > tolerance) return "collision.solid_cache";
        if (std::fabs(x.x - y.x) > tolerance || std::fabs(x.y - y.y) > tolerance
            || std::fabs(x.vy - y.vy) > tolerance) return "collision.solid_entry";
        if (!x.finished || !y.finished) return "collision.trace_incomplete";
        if (x.result != y.result || std::fabs(x.afterX - y.afterX) > tolerance
            || std::fabs(x.afterY - y.afterY) > tolerance || std::fabs(x.afterVy - y.afterVy) > tolerance)
            return "collision.solid_output";
    }
    if (a.collisionContacts.size() != b.collisionContacts.size()) return "collision.solid_count";
    if (a.collisionContactsDropped || b.collisionContactsDropped) return "collision.trace_incomplete";
    return nullptr;
}

// Keep storage bounded even if GD repeatedly calls a function within one update.
inline bool sectionDiagnosticStageRoom(SectionDiagnosticStep& step) {
    if (step.stages.size() < kSectionDiagnosticStageLimit) return true;
    ++step.stagesDropped;
    return false;
}

// Find a gameplay-output difference; supporting world observations do not claim causation.
inline const char* sectionDiagnosticDifference(const SectionDiagnosticState& fast,
                                               const SectionDiagnosticState& replay,
                                               double tolerance) {
    if (fast.deathCall != replay.deathCall) return "death_call";
    if (fast.dual != replay.dual) return "dual";
    for (size_t i = 0; i < fast.players.size(); ++i) {
        const auto& a = fast.players[i];
        const auto& b = replay.players[i];
        if (a.present != b.present) return i ? "p2.present" : "p1.present";
        if (!a.present) continue;
        if (a.dead != b.dead) return i ? "p2.dead" : "p1.dead";
        if (std::fabs(a.x - b.x) > tolerance) return i ? "p2.x" : "p1.x";
        if (std::fabs(a.y - b.y) > tolerance) return i ? "p2.y" : "p1.y";
        if (std::fabs(a.vy - b.vy) > tolerance) return i ? "p2.vy" : "p1.vy";
        if (a.mode != b.mode) return i ? "p2.mode" : "p1.mode";
        if (a.flipped != b.flipped) return i ? "p2.flip" : "p1.flip";
        if (a.grounded != b.grounded) return i ? "p2.grounded" : "p1.grounded";
        if (std::fabs(a.size - b.size) > tolerance) return i ? "p2.size" : "p1.size";
    }
    return nullptr;
}

struct SectionDiagnosticStageDifference {
    const char* field = nullptr;
    size_t fast = 0, replay = 0;
};

// Match only physics calls; fast-only restore/rearm calls remain visible but are not misaligned.
inline SectionDiagnosticStageDifference sectionDiagnosticStagesDifference(
    const SectionDiagnosticStep& fast, const SectionDiagnosticStep& replay,
    double tolerance, bool supporting) {
    size_t a = 0, b = 0;
    while (true) {
        while (a < fast.stages.size() && !fast.stages[a].duringStep) ++a;
        while (b < replay.stages.size() && !replay.stages[b].duringStep) ++b;
        if (a == fast.stages.size() || b == replay.stages.size()) break;
        const auto& x = fast.stages[a];
        const auto& y = replay.stages[b];
        if (std::strcmp(x.name, y.name) != 0 || x.actor != y.actor)
            return {"call_sequence", a, b};
        if (x.argument != y.argument || x.flags != y.flags || x.result != y.result
            || std::fabs(x.dt - y.dt) > 1e-9) return {"call_arguments", a, b};
        if (!supporting) {
            if (const auto* field = sectionDiagnosticDifference(x.state, y.state, tolerance))
                return {field, a, b};
        }
        if (supporting) {
            if (x.state.pendingSpeed != y.state.pendingSpeed
                || x.state.pendingSpeedNoEffects != y.state.pendingSpeedNoEffects)
                return {"pending_speed", a, b};
            if (x.state.triggeredIDCount != y.state.triggeredIDCount
                || x.state.triggeredIDHash != y.state.triggeredIDHash)
                return {"em.triggered_ids", a, b};
            if (x.state.collisionEnvironmentObserved && y.state.collisionEnvironmentObserved) {
                if (x.state.collisionLists.size() != y.state.collisionLists.size())
                    return {"collision.bucket_count", a, b};
                for (size_t i = 0; i < x.state.collisionLists.size(); ++i)
                    if (const auto* field = sectionDiagnosticCollisionListDifference(
                            x.state.collisionLists[i], y.state.collisionLists[i])) return {field, a, b};
            }
            if (x.state.disabledGroupCount != y.state.disabledGroupCount
                || x.state.disabledGroupHash != y.state.disabledGroupHash)
                return {"em.disabled_groups", a, b};
            if (x.state.motionCount != y.state.motionCount || x.state.motionHash != y.state.motionHash)
                return {"em.motion_commands", a, b};
            if (x.state.completedMoveCount != y.state.completedMoveCount
                || x.state.completedMoveHash != y.state.completedMoveHash)
                return {"em.completed_moves", a, b};
            if (x.state.followingCount != y.state.followingCount
                || x.state.followingHash != y.state.followingHash)
                return {"em.following", a, b};
            for (size_t i = 0; i < 2; ++i) {
                const auto& p = x.state.players[i];
                const auto& q = y.state.players[i];
                if (p.playerSpeed != q.playerSpeed || p.speedMultiplier != q.speedMultiplier
                    || p.gravity != q.gravity)
                    return {i ? "p2.speed_state" : "p1.speed_state", a, b};
                if (p.reverseSpeed != q.reverseSpeed || p.reverseAcceleration != q.reverseAcceleration
                    || p.goingLeft != q.goingLeft || p.sideways != q.sideways)
                    return {i ? "p2.wave_motion" : "p1.wave_motion", a, b};
                if (p.dashing != q.dashing || p.dashX != q.dashX || p.dashY != q.dashY
                    || p.lastLandTime != q.lastLandTime || p.touchedPad != q.touchedPad
                    || p.justPlacedStreak != q.justPlacedStreak)
                    return {i ? "p2.release_state" : "p1.release_state", a, b};
                if (p.onSlope != q.onSlope || p.wasOnSlope != q.wasOnSlope
                    || std::fabs(p.slopeCorrection - q.slopeCorrection) > tolerance)
                    return {i ? "p2.slope_state" : "p1.slope_state", a, b};
                if (std::fabs(p.lastX - q.lastX) > tolerance || std::fabs(p.lastY - q.lastY) > tolerance)
                    return {i ? "p2.sweep_history" : "p1.sweep_history", a, b};
                if (p.followCursor != q.followCursor || p.followHeights != q.followHeights
                    || p.followHash != q.followHash)
                    return {i ? "p2.follow_history" : "p1.follow_history", a, b};
                if (p.holding != q.holding || p.jumpBuffered != q.jumpBuffered
                    || p.wasJumpBuffered != q.wasJumpBuffered
                    || p.stateJumpBuffered != q.stateJumpBuffered || p.stateRingJump != q.stateRingJump)
                    return {i ? "p2.input" : "p1.input", a, b};
                if (p.touchingRingsCount != q.touchingRingsCount
                    || p.touchingRingsHash != q.touchingRingsHash)
                    return {i ? "p2.touching_rings" : "p1.touching_rings", a, b};
                if (p.snapped != q.snapped || p.collided != q.collided || p.slope != q.slope
                    || p.collisionCounts != q.collisionCounts
                    || p.collisionKeyHashes != q.collisionKeyHashes
                    || std::fabs(p.spiderLow - q.spiderLow) > tolerance
                    || std::fabs(p.spiderHigh - q.spiderHigh) > tolerance)
                    return {i ? "p2.contacts" : "p1.contacts", a, b};
            }
            if (x.state.queuedButtons != y.state.queuedButtons
                || x.state.buttons.size() != y.state.buttons.size()) return {"button_queue", a, b};
            for (size_t i = 0; i < x.state.buttons.size(); ++i) {
                const auto& p = x.state.buttons[i];
                const auto& q = y.state.buttons[i];
                if (p.button != q.button || p.step != q.step || p.push != q.push
                    || p.player2 != q.player2 || p.timestamp != q.timestamp)
                    return {"button_queue", a, b};
            }
            for (const auto& p : x.state.objects) {
                for (const auto& q : y.state.objects) {
                    if (p.uid != q.uid) continue;
                    if (std::fabs(p.x - q.x) > tolerance || std::fabs(p.y - q.y) > tolerance
                        || std::fabs(p.rotation - q.rotation) > tolerance
                        || std::fabs(p.scaleX - q.scaleX) > tolerance
                        || std::fabs(p.scaleY - q.scaleY) > tolerance
                        || std::fabs(p.rectX - q.rectX) > tolerance
                        || std::fabs(p.rectY - q.rectY) > tolerance
                        || std::fabs(p.rectW - q.rectW) > tolerance
                        || std::fabs(p.rectH - q.rectH) > tolerance
                        || std::fabs(p.positionX - q.positionX) > tolerance
                        || std::fabs(p.positionY - q.positionY) > tolerance
                        || std::fabs(p.lastX - q.lastX) > tolerance
                        || std::fabs(p.lastY - q.lastY) > tolerance || p.moveTick != q.moveTick
                        || p.rectDirty != q.rectDirty || p.disabled != q.disabled
                        || p.activated1 != q.activated1 || p.activated2 != q.activated2)
                        return {"contact_geometry", a, b};
                }
            }
        }
        ++a; ++b;
    }
    if (fast.stagesDropped || replay.stagesDropped) return {"trace_incomplete", a, b};
    if (a != fast.stages.size() || b != replay.stages.size()) return {"call_count", a, b};
    return {};
}

} // namespace p1::solver
