#pragma once
// GJGameEvent (Geode Enums.hpp) raised by the objects the player touches, as a pure function of the GameObjectType.
// [R] 22091 FUN_14033b210 (base+0x33b210), called by PlayerObject::ringJump (a ring that has fired), GJBaseGameLayer::bumpPlayer (a pad that bumped) and FUN_14031a210:
// the table below is its switch, entry for entry (a type it does not list gives 0 = GJGameEvent::None).
// What the callers raise with it ([R], r2091 notes event_orb_producer_design_gd03_1119.md):
//   ringJump (after its gate: a ring the press has not used, the slot down, ...): gameEventTriggered(OrbActivated = 8, material 0, the player's id), then (gdEventOfType(ring type), 0, the player's id)
//   bumpPlayer (a pad):                                                           gameEventTriggered(gdEventOfType(pad type), 0, 0), then (PadActivated = 9, 0, 0)
// gameEventTriggered 0x33ac80 spawns the group of each Event trigger (id 3604) listening for the event, once a step for each (event, material * 10000 + player) key, and calls itself again with the player 0.
#include <cstdint>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dp {
namespace gdev {

inline constexpr int kOrbActivated = 8;
inline constexpr int kPadActivated = 9;
inline constexpr int kGravityInverted = 10;
inline constexpr int kGravityRestored = 11;
inline constexpr int kSpiderTeleport = 18;

inline int gdEventOfType(int objType) {
    switch (objType) {
    case 3: return 0x32;
    case 4: return 0x33;
    case 5: return 0x1b;
    case 6: return 0x1a;
    case 8: return 0x2d;
    case 9: return 0x2e;
    case 10: return 0x30;
    case 0xb: return 0x22;
    case 0xc: return 0x23;
    case 0xd: return 0x25;
    case 0xe: return 0x35;
    case 0xf: return 0x36;
    case 0x10: return 0x1c;
    case 0x11: return 0x37;
    case 0x12: return 0x38;
    case 0x13: return 0x1d;
    case 0x15: return 0x3d;
    case 0x16: case 0x1f: return 0x3e;
    case 0x17: return 0x39;
    case 0x18: return 0x3a;
    case 0x1a: return 0x1e;
    case 0x1b: return 0x1f;
    case 0x1c: return 0x3b;
    case 0x1d: return 0x26;
    case 0x1e: return 0x3f;
    case 0x20: return 0x27;
    case 0x21: return 0x20;
    case 0x22: return 0x2f;
    case 0x23: return 0x24;
    case 0x24: return 0x28;
    case 0x25: return 0x29;
    case 0x26: return 0x2a;
    case 0x29: return 0x21;
    case 0x2a: return 0x34;
    case 0x2b: return 0x2b;
    case 0x2c: return 0x31;
    case 0x2e: return 0x2c;
    default: return 0;
    }
}

// ---- the world's event record (State::evgate) --------------------------------------------------------------------------------------------------------------------------------------------------
// The game events a body raised that some Event trigger of the level listens for, along its lineage: State::evgate is the digest of the chain (0 = none), and g_evChain keeps each link (digest -> the digest
// before it, the step, the events of that step in the order they were raised), so a timeline that must be told the events a state has raised can walk the chain from the digest it was last told. Nothing is
// recorded where the level has no listener (evgate stays 0 and the chain empty). The chain is a level's side table: it outlives a call (a ladder resume walks it again) and is emptied by the level's reset.
struct EvTok { int ev = 0, uid = -1, type = -1; };

inline void noteOrbEvents(std::vector<EvTok>* out, int uid, int type) {
    if (!out) return;
    out->push_back({kOrbActivated, uid, type});
    out->push_back({gdEventOfType(type), uid, type});
}
inline void notePadEvents(std::vector<EvTok>* out, int uid, int type) {
    if (!out) return;
    out->push_back({gdEventOfType(type), uid, type});
    out->push_back({kPadActivated, uid, type});
}
// The objects only an Event trigger's chain (3604 -> its target group -> a spawn-triggered Move / Toggle -> their target) acts on, with no touch box and no crossing trigger of their own (triggers.hpp
// loadSpawnReachedMovers): dynamics.hpp applyTriggersImpl hands them the shadow's on / off as it does an object an autonomous trigger controls. An object such a chain switches stood on for the model, which never visited it.
inline std::unordered_set<int> g_supplyEventUids;
inline uint64_t g_evListened[2] = {0, 0};   // the GJGameEvents some Event trigger (3604) of the level listens for (bit e, e < 128): set by the loader, emptied by the reset
inline bool evListened(int e) { return e >= 0 && e < 128 && ((g_evListened[e >> 6] >> (e & 63)) & 1ull); }
// From which progress x on an event can be HEARD: a listener exists only after its Event trigger has run (activateEventTrigger 0x33ada0 registers it in the listener map, layer +0x588), and gameEventTriggered
// (0x33ac80) looks a raised event up in that map: an event raised before the registration finds no entry and nothing happens. An Event trigger nothing but the player's crossing can run (not a spawn trigger, not a touch
// trigger, and in no group another trigger targets) runs no earlier than the player's x reaches its own: g_evFromX[e] is the least such x over the triggers that listen for e (-inf: one that something else can run, so
// no event is early for it; +inf: no listener). The crossing is the trigger shadow's own (trigshadow.hpp checkSpawnObjects, the game's: a crossing trigger fires in the first step whose player x -- read once when
// the loop starts -- is not below the trigger's x, the travel direction forward and on the main channel), so the threshold is the trigger's x exactly; the pass reads the x after its step, which is never below that one.
inline double g_evFromX[128];   // set by the loader (evFromReset), lowered per listening trigger
inline void evFromReset() { for (double& v : g_evFromX) v = std::numeric_limits<double>::infinity(); }
inline void evFromLower(int e, double x) { if (e >= 0 && e < 128 && x < g_evFromX[e]) g_evFromX[e] = x; }
// an event raised at progress x that some Event trigger could have registered a listener for by then
inline bool evHeardAt(int e, double x) { return evListened(e) && x >= g_evFromX[e]; }   // (the pass' x after the step: never below the x the crossing loop read)
inline bool evAnyListened() { return (g_evListened[0] | g_evListened[1]) != 0; }
struct EvNode { uint64_t prev = 0; long long step = 0; std::vector<int> events; };
inline std::unordered_map<uint64_t, EvNode> g_evChain;
inline std::mutex g_evChainMu;
inline uint64_t evMix(uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); h *= 0xff51afd7ed558ccdULL; return h ^ (h >> 31); }
// the digest after `events` were raised at `step` on top of `prev` (State::evKey takes this without the link)
inline uint64_t evDigest(uint64_t prev, long long step, const std::vector<int>& events) {
    uint64_t h = evMix(evMix(0x4556474154ULL, prev), (uint64_t)step);
    for (const int e : events) h = evMix(h, (uint64_t)(uint32_t)e);
    if (h == 0) h = 1;
    return h;
}
// the digest after `events` were raised at `step` on top of `prev`; the link is recorded
inline uint64_t evChainAdd(uint64_t prev, long long step, const std::vector<int>& events) {
    const uint64_t h = evDigest(prev, step, events);
    std::lock_guard<std::mutex> lk(g_evChainMu);
    EvNode& n = g_evChain[h];
    n.prev = prev; n.step = step; n.events = events;
    return h;
}
// A Tap's press (Touch trigger 1595, touch = 0 / spawn = 0) rides the same chain as a game event: its token is kTapTok + 4 * (the Tap's index in gdtrig::g_togTable.taps) + kind (1 = a press, 2 = a release), above every
// GJGameEvent (< 128). It is recorded only on a level where the shadow runs the Taps' chains (g_tapFed: a Tap whose group holds a trigger; set by TrigLive when the base shadow is built, emptied by the reset), only from the
// x the Tap listens from (the Tap's own cx, as g_evFromX is for an event) and only until the Stop that ends it (gdtrig::TogTap::cx / stopCx, from the level's triggers). A press the Tap does not hear leaves no trace.
inline constexpr int kTapTok = 1 << 12;
inline bool g_tapFed = false;
// What the KEY sees of the presses (State::tapN / tapWin, search_key.hpp tapKeyBits): g_tapWinTicks = how long a press's effect keeps running over the Tap's chain, from the level's triggers (the longest Move duration /
// Spawn delay along the chain, in the shadow's steps, plus kTapLag: the press reaches the timeline at the head of the step after its pass, and a command's first step does not advance it); a press older than that
// changes the world only by being counted. At most 31 (the window is a 32-bit mask), 8 chain Taps (State::tapN), 255 steps of age after a count (State::tapAge): these are the CAPACITY of the fields, not a rule about the
// game -- a level over one is not run by the shadow (TrigLive) and is handled as it was before the Taps' chains were run; a wider field lifts the limit. The chain Taps' slots (TogTap::slot) index State::tapN.
inline constexpr int kTapLag = 2;
inline int g_tapWinTicks = 0;
inline uint32_t g_tapWinMask = 0;
// Where the chains' effects are: the x span of every object a chain trigger moves, widened by the trigger's own offset. The key reads the effect window (State::tapWin) only while the player is within kTapNearPx of one of
// them: far from everything a press moves, the edges' exact ticks cannot reach the player's collisions, and states that pressed at other ticks are one world (counted by tapN / tapAge all the same). kTapNearPx is a
// margin g_tapNearPx, from the level: the cube's reach on both sides (2 * kCubeHalf, the moved object's own half-size counted as one cube) plus how far the player can travel while a press is still in the window (g_tapWinTicks steps at
// the fastest speed portal, 576 px/s = 2.4 px per 240 Hz step). gd-01's world audit (Grief, 3000 layers) is the check of this reading: mixed keys must stay 0.
// (Now a box per object, in x AND y: each member's position widened by the trigger's offset and by the member's own reach (half size or radius + the cube; decorations and triggers are left out), and at the query by how far the
// player can travel in the window: g_tapNearPx / g_tapNearPy.) The vertical bound is 30 (world vy) * 0.225 px per step, the largest speed a pad / orb / portal gives.
struct TapNearBox { double xlo, xhi, ylo, yhi; };
// The call's objrects has the sscx / sscy columns (GameObject::m_startScaleX / Y), so the trigger timeline knows the scale each dumped object stands at (trigshadow_live.hpp osx0): a turned hazard's box is
// then the dumped box times the timeline's ratio since the dump (shadow_step.hpp), and its angle is the rows' (dynamics.hpp turnedBox). False for an older objrects: those stay as they were.
inline bool g_objrectsStartScale = false;
inline std::vector<TapNearBox> g_tapNear;
inline double g_tapNearPx = 0, g_tapNearPy = 0;
inline constexpr double kMaxStepPx = 576.0 / 240.0;
inline constexpr double kMaxVyStepPx = 30.0 * 0.225;
// Past x per Tap slot (a narrow Tap, whose presses move objects by the count): once the player is beyond the farthest x any object the Tap moves can reach (its x, what every Move on its groups can add, the member's reach, and how far the player travels in the effect window), the count cannot change what the player meets, and the key leaves it out (search_key.hpp tapKeyBits). +inf = the count is always read.
inline double g_tapPastX[8] = {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
inline bool tapNearXY(double x, double y) { for (const auto& b : g_tapNear) if (x >= b.xlo - g_tapNearPx && x <= b.xhi + g_tapNearPx && y >= b.ylo - g_tapNearPy && y <= b.yhi + g_tapNearPy) return true; return false; }
inline bool isTapTok(int e) { return e >= kTapTok; }
inline bool evChainOn() { return evAnyListened() || g_tapFed; }   // the level has something State::evgate records: a listened game event or a Tap whose chain the shadow runs
// Can the player's x go back on this level (a teleport portal whose exit lies behind it, a Reverse 1917 or Rotate Gameplay 2900 trigger)? Decided where the live shadow reads the level (trigshadow_live.hpp); true until it has.
inline bool g_levelMayGoBack = true;
inline void evChainReset() { std::lock_guard<std::mutex> lk(g_evChainMu); g_evChain.clear(); g_evListened[0] = g_evListened[1] = 0; evFromReset(); g_tapFed = false; g_tapWinTicks = 0; g_tapWinMask = 0; g_levelMayGoBack = true; for (double& p : g_tapPastX) p = std::numeric_limits<double>::infinity(); }

}  // namespace gdev
}  // namespace dp
