#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace p1::solver {

// Carry collision geometry, not the CCNode/OBB owners or the live section indices.
template <class Object>
struct CollisionObjectState {
    using Point = decltype(std::declval<Object>().m_lastPosition);
    using Rect = decltype(std::declval<Object>().m_objectRect);
    using Size = decltype(std::declval<Object>().m_lastSize);
    using Box = std::remove_pointer_t<decltype(std::declval<Object>().m_orientedBox)>;
    struct Oriented {
        decltype(std::declval<Box>().m_corners) corners;
        decltype(std::declval<Box>().m_positions) positions;
        decltype(std::declval<Box>().m_edges) edges;
        decltype(std::declval<Box>().m_projections) projections;
        decltype(std::declval<Box>().m_center) center;
    };
    Object* object = nullptr;
    std::array<float, 6> transform{};
    double positionX = 0, positionY = 0;
    Point lastPosition{}, boxOffset{}, customBoxOffset{};
    Size lastSize{};
    Rect textureRect{}, objectRect{};
    float customScaleX = 1, customScaleY = 1;
    float physicsScaleX = 1, physicsScaleY = 1;
    float unmodifiedX = 0, unmodifiedY = 0;
    std::array<float, 6> offsets{};
    std::array<int, 4> updateTicks{};
    int enabledGroupsCounter = 0;
    bool groupDisabled = false, groupDisabledTemp = false, disabled = false;
    bool dirty = false, posDirty = false, unmodifiedPosDirty = false;
    bool rectDirty = false, orientedDirty = false, boxOffsetCalculated = false;
    std::optional<Oriented> oriented;
};

// Read caches directly: a getter would calculate geometry and change the captured node.
template <class Object>
CollisionObjectState<Object> captureCollisionObject(Object* o) {
    CollisionObjectState<Object> s;
    s.object = o;
    s.transform = {o->getPositionX(), o->getPositionY(), o->getRotationX(),
                   o->getRotationY(), o->getScaleX(), o->getScaleY()};
    s.positionX = o->m_positionX; s.positionY = o->m_positionY;
    s.lastPosition = o->m_lastPosition;
    s.lastSize = o->m_lastSize;
    s.boxOffset = o->m_boxOffset; s.customBoxOffset = o->m_customBoxOffset;
    s.textureRect = o->m_textureRect; s.objectRect = o->m_objectRect;
    s.customScaleX = o->m_customScaleX; s.customScaleY = o->m_customScaleY;
    s.physicsScaleX = o->m_scaleX; s.physicsScaleY = o->m_scaleY;
    s.unmodifiedX = o->m_unmodifiedPositionX; s.unmodifiedY = o->m_unmodifiedPositionY;
    s.offsets = {o->m_positionXOffset, o->m_positionYOffset, o->m_rotationXOffset,
                 o->m_rotationYOffset, o->m_scaleXOffset, o->m_scaleYOffset};
    s.updateTicks = {o->m_unk4C0, o->m_unk4C4, o->m_unk4C8, o->m_unk4CC};
    s.enabledGroupsCounter = o->m_enabledGroupsCounter;
    s.groupDisabled = o->m_isGroupDisabled; s.groupDisabledTemp = o->m_isGroupDisabledTemp;
    s.disabled = o->m_isDisabled;
    s.dirty = o->m_isDirty; s.posDirty = o->m_isObjectPosDirty;
    s.unmodifiedPosDirty = o->m_isUnmodifiedPosDirty;
    s.rectDirty = o->m_isObjectRectDirty; s.orientedDirty = o->m_isOrientedBoxDirty;
    s.boxOffsetCalculated = o->m_boxOffsetCalculated;
    if (const auto* box = o->m_orientedBox)
        s.oriented = typename CollisionObjectState<Object>::Oriented{
            box->m_corners, box->m_positions, box->m_edges, box->m_projections, box->m_center};
    return s;
}

// Re-bucket through the game, then replace caches dirtied by transform/section setters.
template <class Object, class Rebucket>
void restoreCollisionObject(const CollisionObjectState<Object>& s, Rebucket rebucket) {
    auto* o = s.object;
    const auto& t = s.transform;
    if (o->getPositionX() != t[0] || o->getPositionY() != t[1])
        o->setPosition(typename CollisionObjectState<Object>::Point{t[0], t[1]});
    if (o->getRotationX() != t[2]) o->setRotationX(t[2]);
    if (o->getRotationY() != t[3]) o->setRotationY(t[3]);
    if (o->getScaleX() != t[4]) o->setScaleX(t[4]);
    if (o->getScaleY() != t[5]) o->setScaleY(t[5]);
    // updateObjectSection reads these doubles, not the drawing node's position (2.2081).
    o->m_positionX = s.positionX; o->m_positionY = s.positionY;
    o->m_customScaleX = s.customScaleX; o->m_customScaleY = s.customScaleY;
    o->m_scaleX = s.physicsScaleX; o->m_scaleY = s.physicsScaleY;
    o->m_unmodifiedPositionX = s.unmodifiedX; o->m_unmodifiedPositionY = s.unmodifiedY;
    o->m_positionXOffset = s.offsets[0]; o->m_positionYOffset = s.offsets[1];
    o->m_rotationXOffset = s.offsets[2]; o->m_rotationYOffset = s.offsets[3];
    o->m_scaleXOffset = s.offsets[4]; o->m_scaleYOffset = s.offsets[5];
    // processMoveActions skips last-position/dirty updates when +0x4dc equals this tick.
    o->m_unk4C0 = s.updateTicks[0]; o->m_unk4C4 = s.updateTicks[1];
    o->m_unk4C8 = s.updateTicks[2]; o->m_unk4CC = s.updateTicks[3];
    rebucket(o);
    // Group changes ran before this local restore; keep the counter and its filters together.
    o->m_enabledGroupsCounter = s.enabledGroupsCounter;
    o->m_isGroupDisabled = s.groupDisabled; o->m_isGroupDisabledTemp = s.groupDisabledTemp;
    o->m_isDisabled = s.disabled;
    o->m_lastPosition = s.lastPosition;
    o->m_lastSize = s.lastSize;
    o->m_boxOffset = s.boxOffset; o->m_customBoxOffset = s.customBoxOffset;
    o->m_boxOffsetCalculated = s.boxOffsetCalculated;
    o->m_textureRect = s.textureRect; o->m_objectRect = s.objectRect;
    o->m_isDirty = s.dirty; o->m_isObjectPosDirty = s.posDirty;
    o->m_isUnmodifiedPosDirty = s.unmodifiedPosDirty;
    o->m_isObjectRectDirty = s.rectDirty; o->m_isOrientedBoxDirty = s.orientedDirty;
    if (s.oriented && o->m_orientedBox) {
        auto* box = o->m_orientedBox;
        box->m_corners = s.oriented->corners; box->m_positions = s.oriented->positions;
        box->m_edges = s.oriented->edges; box->m_projections = s.oriented->projections;
        box->m_center = s.oriented->center;
    } else if (o->m_orientedBox || s.oriented) {
        // A branch may lazily create an OBB. Keep its owner, but do not reuse that branch's box.
        o->m_isObjectRectDirty = true;
        o->m_isOrientedBoxDirty = true;
    }
}

// GD checks x +/- 1 sections. Carry one more column for the next step; all y rows
// are needed because a spider can change height without traversing the intervening rows.
template <class Sections, class Object>
void appendCollisionColumns(const Sections& sections, int center, std::vector<Object*>& out) {
    const int first = std::max(0, center - 2);
    const int last = std::min((int)sections.size() - 1, center + 2);
    for (int x = first; x <= last; ++x) {
        const auto* column = sections[(size_t)x];
        if (!column) continue;
        for (const auto* row : *column) {
            if (!row) continue;
            for (auto* o : *row) if (o) out.push_back(o);
        }
    }
}

// Merge player windows and the unpartitioned collision list without truncating contacts.
template <class Object>
void uniqueCollisionObjects(std::vector<Object*>& out) {
    std::sort(out.begin(), out.end(), std::less<Object*>{});
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

} // namespace p1::solver
