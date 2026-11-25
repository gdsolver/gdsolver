#pragma once
// The event a world is told about (restructure draft sections 2.2 / 3.4, P4): one record per call that changes a world, in the form the trigger shadow applies it.
// A world's identity is the digest of the events it has been given -- not of the player history that decided them (a press edge, a box overlap): two branches that were
// given the same events are in the same world. Producers (the functions that turn the player's state into events) live elsewhere: TouchDecider / TapDecider / the orb rule /
// the x crossings; the consumer is TrigShadow (addTouchEvent, addItemEvent, addGroupToggleEvent) and TrigLive.
#include <cstdint>

namespace dp {
namespace gdtrig {

enum EventKind : uint8_t {
    kEvCross = 0,    // an x-crossing trigger fired: step, uid
    kEvTouch = 1,    // a Touch-Triggered trigger was called: step, uid
    kEvTap = 2,      // a Touch trigger (1595, touch = 0 spawn = 0) toggled its group: step, group, on
    kEvOrb = 3,      // a Toggle Orb was clicked: step, uid (late: the push is in the button phase, after the collision pass)
    kEvPickup = 4,   // an item was taken: step, uid (the pickup), group = the item id
    kEvCount = 5,    // a Count / Instant Count entry fired: step, uid
};

struct Event {
    uint8_t kind = kEvCross;
    int64_t step = 0;     // the shadow step that applies it (game tick of the call + 1)
    int32_t uid = -1;     // the trigger or object, -1 when the event names a group
    int32_t group = -1;   // the group (Tap) or the item id (Pickup), else -1
    bool on = false;      // Tap: the group goes on (true) or off (false)
    bool late = false;    // applied after the collision pass (an orb's push)
};

inline uint64_t eventHash(const Event& e) {
    auto mix = [](uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); return h * 0xff51afd7ed558ccdULL; };
    uint64_t h = mix(0x45564e54ULL, e.kind);
    h = mix(h, (uint64_t)e.step);
    h = mix(h, (uint64_t)(uint32_t)e.uid);
    h = mix(h, (uint64_t)(uint32_t)e.group);
    h = mix(h, (uint64_t)((e.on ? 1 : 0) | (e.late ? 2 : 0)));
    return h;
}

}  // namespace gdtrig
}  // namespace dp