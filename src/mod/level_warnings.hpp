#pragma once
// What the play menu warns about before a bot run, read from the level string alone: the menu opens
// before the level is loaded, so nothing here may need a GameObject.
//
// Two kinds, two colours in the menu:
//   * UNMODELLED (red): the level holds something the model cannot express at all, so it plans
//     blind there and the section solver is the only way through -- a solve may well fail. Each
//     bit is a shape the dp loader reports as NOT modelled or refuses outright, a feature no part
//     of dp reads, or a level that does not behave the same on every attempt.
//   * NEWER (yellow, custom levels only): an object added in 2.0 or later that is not a recolour of
//     an older one. The model is measured against those one mechanic at a time and much of it is
//     not done, so such a level takes longer and may not solve.
// Neither refuses anything; a warning that shows a little too often costs nothing.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "object_types.gen.hpp"

namespace levelwarn {

enum : uint32_t {
    // RETIRED (never set): a teleport portal (2902) or a teleport trigger (3022) whose exit is away
    // from it in x. dp moves x to an exit ahead (--tpportalx, --tpsideways) and since 2026-10-13 to
    // one behind as well (--tpback, which reads the crossing triggers at the farthest x reached, as
    // GD's crossing queue does: checkSpawnObjects 0x21a8f0). The bit keeps its place so the others do.
    kSidewaysTeleport = 1u << 0,
    // RETIRED (never set): a teleport orb (3027) with a static force (key 345) or a redirected one (key
    // 347). The orb goes through the same teleportPlayer as a portal (PlayerObject::ringJump, type 46),
    // and the shadow step (--shadowstep=all, the body of every run) sets its velocity as GD does: on the
    // calib_tporbforce rig (static force along a marker's or an orb's direction, added, 0, redirected,
    // ignoreY, forced gravity, every mode, mini) the shadow's vy / y / x match GD's per tick on 18 of 20
    // units, the other two (ship, ufo) off by the one-tick lag their no-force units already have. The old
    // step's loader line "sets a velocity - NOT modelled" is the old model's. A portal's force or keep
    // met in a turned frame or by a second player is not measured. The bit keeps its place so the others do.
    kOrbPush = 1u << 1,
    // A 2902, a teleport orb (3027) or a teleport trigger (3022) whose target group holds more than
    // one object: teleportPlayer (0x20fdb0) draws one of them from the seed at 0x6c2ef8. The export
    // takes the first; the model has no draw.
    kTeleportSeveralExits = 1u << 2,
    // A teleport trigger (3022) dp leaves out: one that keeps the player's offset from it
    // (m_saveOffset, key 351), which the export does not carry, or one set off by a spawn (key 62)
    // that no chain reaches from an x crossing or a touch (spawnChainReach, through delayed and ordered
    // Spawns: the trigger shadow runs them) and whose group some object names as a target
    // (targetedGroups: a group nothing names is never fired). The trigger is modelled
    // otherwise -- by its x (dp --teleporttrig), by its own touch (--tptouch), from a chain's root
    // (--tptrigspawn (removed), --tptouch) -- as the teleport orb (3027) is (--teleportorb) apart from the
    // shapes above; a second player or a turned frame meeting either is not measured, which the
    // yellow note and dp's load lines cover.
    kTeleportTrigger = 1u << 3,
    // A gravity trigger (2066) dp leaves out: one for player 2 only (key 200) that a touch or a spawn
    // sets off, in a level that has a second player at all (a dual portal, 286, or two-player mode; one on
    // the x-crossing queue is the shadow's second body's, --shadowp2grav), or one set off by a spawn that
    // no chain reaches from an x crossing or a touch (spawnChainReach; a delayed or ordered chain from an
    // x crossing counts too where dp does not refuse its delay: spawnChainReachDelayed) and whose group some object
    // does name as a target (targetedGroups; a group nothing names is never fired). dp models the trigger by its
    // x (--gravtrig), by its own touch (--gravtouch) and from a chain's root (--gravspawn (removed),
    // --gravchain (removed), --gravtouch).
    kGravityTrigger = 1u << 4,
    // A level the dp loader refuses for its gravity portals ("gravity portals: N (limit 128, ...)"
    // in level_loader.hpp), the gravity toggle portal (2926) counted with the two kinds of gravity
    // portal as the loader counts it. The portal latch holds kGravPortalBits (128) of them; past that the
    // loader shares bits -- portal j takes the bit of portal j-128 in x order -- unless an object
    // carries a reversal or some such pair is closer than kGpShareGap (120 px). Most levels over
    // 128 share fine, so this is set only when the sharing would fail: see gravityPortalsRefused.
    kGravityPortals = 1u << 5,
    // The level itself plays differently from one attempt to the next: an Item Compare or Item Edit
    // (3620, 3619) that reads the attempt count (item type 5, keys 476/477; getItemValue 0x2341c0
    // reads it from the layer), or an item Item Persistence (3641) keeps across attempts, sets off
    // triggers that reach something the player touches or the player itself (attemptReachesPlay). The
    // loop records the moving geometry on one attempt and flies its plans on later ones, and a
    // solution replayed as a session's first attempt meets a different level. A level whose count
    // only shows -- a counter label, a message, a colour -- does not set it (Findings::attemptShown).
    kAttemptDependent = 1u << 6,
    kPlatformer = 1u << 7,
    // Two-player mode (header kA10): the second player has a button of its own. dp plans it as bit 1 of
    // the input mask, each body searched against the other's plan (docs/TWO_PLAYER.md); a level where one
    // player's press switches the other's world is not solved yet. The play menu takes this bit out of the red note and shows a
    // yellow one instead (hooks_playmenu.cpp, LevelFacts::twoPlayer).
    kTwoPlayer = 1u << 8,
    // A Follow Player Y (1814) with a delay (key 91) -- it follows the player's y that long ago,
    // from a ring of past heights a state cannot carry -- or one set off by touch or by a spawn
    // (keys 11 / 62) rather than by its x: dp cli.hpp's followy block says "NOT modelled: delayed"
    // and "touch- or spawn-triggered".
    kFollowNotRun = 1u << 9,
    // More Follow Player Y runs at once than a state holds -- "NOT modelled: one more than a state's
    // slots" in the same block. See followSlotsShort.
    kFollowSlots = 1u << 10,
};

// The phrase each bit is shown as on the menu's one line, in bit order: short, since two of them
// share the line. kUnmodelledNames below says exactly which objects, for the log.
inline const char* const kUnmodelledShort[] = {
    "teleports that move you back",
    "teleport orbs that push you",
    "teleports with several exits",
    "some teleport triggers",
    "some gravity triggers",
    "too many gravity portals",
    "triggers that count attempts",
    "platformer mode",
    "two-player mode",
    "some follow triggers",
    "more than two follow triggers at once",
};

// Each bit in full, in bit order, for the log line the menu writes.
inline const char* const kUnmodelledNames[] = {
    "teleports that move you back",
    "teleport orbs that push you",
    "teleports with several exits",
    "teleport triggers keeping an offset or set off by an item, a collision, a touch or an event no pad or orb raises",
    "gravity triggers for player 2 or set off by an item, a delayed spawn or an event no pad or orb raises",
    "too many gravity portals",
    "triggers that count attempts",
    "platformer mode",
    "two-player mode",
    "delayed, touch- or spawn-started follow triggers",
    "more than two follow triggers at once",
};

// Objects added in 2.0 or later that are not a recolour of an older one. Derived, not listed by
// hand, in two steps.
// Which objects are new mechanics: an ID from 286 on (the IDs of pre-1.8 levels, game version 7
// and 10 on the servers, stop at 285) counts when the game gives it a type no pre-1.8 object has
// (dual, slope, wave, robot, teleport, the green, black, red, custom, dash, teleport and spider
// rings and pads, collision and force blocks, swing and gravity-toggle portals, the area effects),
// read off the objects of a few hundred levels as loaded by the game, or when it is a trigger
// whose editor icon (ObjectToolbox::init) names a gameplay effect -- move, rotate, toggle, spawn,
// follow, the item and count triggers, collision, camera, time, gravity, teleport, end. Colour,
// pulse, alpha, shake, animation, particles, shaders, background, sound, UI and BPM triggers do not
// count, nor do solids, hazards (the animated monsters included), decoration and coins, nor a new
// ID of an old type such as the 4x speed portal.
// Which of those came in 2.0 or later: an object's version is the lowest game version of a server
// level that holds it (game version 18 = 1.8, 19 = 1.9, 20 = 2.0, ...), over 525 levels downloaded
// from the servers. 43 came out as 1.8 or 1.9 -- the dual and slope objects and the rings and pads
// of their kind, IDs 286-367 and 483-493, and 1.9's wave and its companions, IDs 651-674 and 728 --
// and are left out. In a survey of 300 custom levels solved to the end, 206 of the 215 levels with
// nothing newer than 1.9 were solved, and they diverged from the game about a fifth as often per
// tick as the levels with 2.0 objects. 371 and 372 sit among 1.8's IDs but were found only in 2.1
// levels, and are kept. The 15 found in none of the 525 are kept too: all are numbered among
// 2.2's.
// Each comes with that version (20 = 2.0, 21 = 2.1, 22 = 2.2), dated the same way over 682 server
// levels; the two found in none of them (1931, 3655) are given 22. The menu's note names the newest
// version a level holds. A level's version is the last one it was saved in, so the date is the
// latest the object can have come in: one may read newer than it is, never older. Sorted by ID.
struct NewerObject {
    int id;
    int version;
};
inline constexpr NewerObject kNewer[] = {
    {371, 21}, {372, 21}, {709, 21}, {745, 20}, {747, 20}, {901, 20}, {1022, 20}, {1049, 20},
    {1268, 20}, {1330, 21}, {1331, 21}, {1332, 21}, {1333, 21}, {1338, 21}, {1339, 21}, {1341, 21},
    {1342, 21}, {1344, 21}, {1345, 21}, {1346, 21}, {1347, 21}, {1594, 21}, {1595, 21}, {1611, 21},
    {1616, 21}, {1704, 21}, {1717, 21}, {1718, 21}, {1723, 21}, {1724, 21}, {1743, 21}, {1744, 21},
    {1745, 22}, {1746, 21}, {1747, 21}, {1748, 21}, {1749, 21}, {1750, 21}, {1751, 21}, {1755, 21},
    {1811, 21}, {1812, 21}, {1813, 21}, {1814, 21}, {1815, 21}, {1816, 21}, {1817, 21}, {1829, 21},
    {1859, 21}, {1906, 21}, {1907, 21}, {1912, 22}, {1913, 22}, {1914, 22}, {1916, 22}, {1917, 22},
    {1931, 22}, {1932, 22}, {1933, 22}, {1935, 22}, {2015, 22}, {2062, 22}, {2066, 22}, {2067, 22},
    {2068, 22}, {2069, 22}, {2866, 22}, {2899, 22}, {2900, 22}, {2901, 22}, {2902, 22}, {2925, 22},
    {2926, 22}, {3004, 22}, {3005, 22}, {3006, 22}, {3007, 22}, {3008, 22}, {3011, 22}, {3012, 22},
    {3013, 22}, {3016, 22}, {3017, 22}, {3018, 22}, {3019, 22}, {3022, 22}, {3023, 22}, {3024, 22},
    {3027, 22}, {3033, 22}, {3600, 22}, {3604, 22}, {3607, 22}, {3609, 22}, {3614, 22}, {3615, 22},
    {3617, 22}, {3618, 22}, {3619, 22}, {3620, 22}, {3640, 22}, {3641, 22}, {3643, 22}, {3645, 22},
    {3655, 22}, {3660, 22}, {3661, 22}};

inline const NewerObject* findNewer(int id) {
    const auto* it = std::lower_bound(std::begin(kNewer), std::end(kNewer), id,
                                      [](const NewerObject& o, int v) { return o.id < v; });
    return (it != std::end(kNewer) && it->id == id) ? it : nullptr;
}

inline bool isNewer(int id) { return findNewer(id) != nullptr; }

struct Findings {
    uint32_t unmodelled = 0;   // the bits above
    bool newer = false;        // an object in kNewer
    int newest = 0;            // the newest version among them (20 = 2.0, 21 = 2.1, 22 = 2.2), or 0
    std::unordered_map<int, int> newerCount;   // how many of each kNewer object, for the log
    // Reads the attempt count or keeps items across attempts, but nothing it sets off reaches play
    // (attemptReachesPlay), so kAttemptDependent is not set. For the log.
    bool attemptShown = false;
    // With kAttemptDependent set: do the items Item Persistence keeps reach play on their own (attemptReachesPlay with the
    // attempt count left out)? The attempt pin (cfg attemptauto, level_entry.hpp) answers the count and leaves the kept items alone,
    // so a level where only the count reaches play is one fixed level while the bot drives it; the play menu shows it as a
    // yellow note, not a red one. Unset when kAttemptDependent is.
    bool attemptKeeps = false;
};

// The key's value in one object's "k,v,k,v" run, or empty.
inline std::string_view field(std::string_view kv, std::string_view key) {
    size_t p = 0;
    while (p < kv.size()) {
        const size_t c1 = kv.find(',', p);
        if (c1 == std::string_view::npos) break;
        const size_t c2 = kv.find(',', c1 + 1);
        const size_t e = (c2 == std::string_view::npos) ? kv.size() : c2;
        if (kv.substr(p, c1 - p) == key) return kv.substr(c1 + 1, e - c1 - 1);
        if (c2 == std::string_view::npos) break;
        p = c2 + 1;
    }
    return {};
}

inline bool isOn(std::string_view v) { return !v.empty() && v != "0"; }
inline int toInt(std::string_view v) { return v.empty() ? 0 : std::atoi(std::string(v).c_str()); }
inline double toNum(std::string_view v) { return v.empty() ? 0.0 : std::atof(std::string(v).c_str()); }

// Calls f(object) for every object of a decompressed level string (the header before the first
// ';' is skipped).
template <class F>
inline void forEachObject(std::string_view all, F&& f) {
    size_t pos = all.find(';');
    while (pos != std::string_view::npos) {
        const size_t end = all.find(';', pos + 1);
        const auto obj = all.substr(pos + 1, end == std::string_view::npos
                                                 ? std::string_view::npos : end - pos - 1);
        if (!obj.empty()) f(obj);
        pos = end;
    }
}

// A gravity portal as the dp loader sees it: its x and the half width of its box.
struct GravPortal {
    double x;
    double hw;
};

// The half width of a gravity portal's box as getObjectRect gives it to the export the loader
// reads: the 25x75 hitbox (m_width, m_height) -- 23.75x75.5 for the gravity toggle portal (2926),
// as every one of its rows in the export reads -- scaled by key 32 (both axes) or 128 / 129
// (x / y) and turned by key 6, which makes it the bound of the turned box, |w cos| + |h sin|. Keys
// 131 / 132 turn the two axes apart; then the larger |cos| and the larger |sin| of the angles
// given are taken, which is never narrower than the box. Two keys for one axis (never seen) take
// the larger scale. Checked against the export on 6,595 portals of a few hundred levels: x as
// exported, and the half width within 0.01 px except on one portal whose axes are turned 29.75
// and 29.76 degrees, where it comes out 0.013 px wider.
inline double gravPortalHalfWidth(std::string_view obj, int id) {
    auto scale = [&](std::string_view axisKey) {
        double s = 0.0;
        bool given = false;
        for (std::string_view k : {std::string_view("32"), axisKey}) {
            const auto v = field(obj, k);
            if (v.empty()) continue;
            s = std::max(s, std::fabs(toNum(v)));
            given = true;
        }
        return given ? s : 1.0;
    };
    const bool toggle = id == 2926;
    const double w = (toggle ? 23.75 : 25.0) * scale("128"), h = (toggle ? 75.5 : 75.0) * scale("129");
    double c = 0.0, s = 0.0;
    bool turned = false;
    for (std::string_view k : {"6", "131", "132"}) {
        const auto v = field(obj, k);
        if (v.empty()) continue;
        const double a = toNum(v) * 3.14159265358979 / 180.0;
        c = std::max(c, std::fabs(std::cos(a)));
        s = std::max(s, std::fabs(std::sin(a)));
        turned = true;
    }
    if (!turned) c = 1.0;
    return 0.5 * (w * c + h * s);
}

// Would the dp loader refuse these gravity portals? level_loader.hpp numbers the portals it holds
// in cx order, and past kGravPortalBits (128) it refuses the level when any object carries a
// reversal, or else when for some j the box of portal j starts less than kGpShareGap (120 px)
// after the box of portal j - 128 ends. `reversal`: an object with key 117 (m_isReverse -- a ring
// or pad that turns the player round).
//
// From the string the answer may come out too eager, never too shy:
//   * The loader may hold fewer portals than the string. The load that refuses a session is the
//     one at its start, which has no recording and holds them all, but the solver's later loads
//     route a portal that moving geometry carries to the dynamic set, which the numbering does
//     not walk. Fewer portals only spread the pairs, so a test on all of them is the strict one.
//   * The export writes x to six significant digits, and cx order leaves ties to the sort. So a
//     pair is not "128 apart in the string's order" but any two portals with at least 129
//     portals, both included, between their x widened by kSlack -- every pair the loader can form
//     is one, whatever it holds and however it orders ties -- and a pair is too close within kSlack
//     of kGpShareGap. Two px covers the rounding for any x below 1,000,000. Where portals stand
//     under kSlack apart, the window also admits a partner a portal or two short of the 128th.
//   * Any object with key 117 counts as a reversal, where the loader reads it only off what it
//     collides with (rings, pads, portals, solids, hazards). It has only been seen on rings and
//     pads.
inline bool gravityPortalsRefused(std::vector<GravPortal> g, bool reversal) {
    constexpr size_t kBits = 128;       // dp: kGravPortalBits
    constexpr double kGap = 120.0;      // dp: kGpShareGap
    constexpr double kSlack = 2.0;
    if (g.size() <= kBits) return false;
    if (reversal) return true;
    std::sort(g.begin(), g.end(), [](const GravPortal& a, const GravPortal& b) { return a.x < b.x; });
    const size_t n = g.size();
    auto firstAt = [&](double x) {   // the first portal whose x is at least `x`
        return (size_t)(std::lower_bound(g.begin(), g.end(), x,
                                         [](const GravPortal& p, double v) { return p.x < v; })
                        - g.begin());
    };
    // hiMin[k]: the leftmost box start among portals k.. in x order.
    std::vector<double> hiMin(n + 1, std::numeric_limits<double>::infinity());
    for (size_t k = n; k-- > 0;) hiMin[k] = std::min(hiMin[k + 1], g[k].x - g[k].hw);
    for (const GravPortal& a : g) {
        // The window starts at the first portal within kSlack of a, and a partner must lie at or
        // beyond the 129th portal from there. Both only move right as a does.
        const size_t lo = firstAt(a.x - kSlack);
        if (lo + kBits >= n) break;
        const size_t from = firstAt(g[lo + kBits].x - kSlack);
        if (hiMin[from] - (a.x + a.hw) < kGap + kSlack) return true;
    }
    return false;
}

// The per-tick x advance of a speed portal (dp speed.hpp dxForSpeedId); anything else is the normal
// speed's.
inline double speedDx(int id) {
    switch (id) {
        case 200: return 251.16 / 240.0;
        case 202: return 1.6142578;
        case 203: return 468.0 / 240.0;
        case 1334: return 576.0 / 240.0;
        default: return 1.29825044;
    }
}

// Would the dp loader leave a Follow Player Y out for want of a slot? Its followy block (cli.hpp)
// takes, in x order, each 1814 with a target group that is not touch- or spawn-triggered, not
// delayed and not at speed 0, and gives it one of kFollowYSlots (2) slots free by its x; a run
// holds its slot until x = its x + duration x 240 x the level's fastest per-tick advance, and one
// that finds none is NOT modelled. From the string the answer may come out too eager, never too
// shy: the fastest advance here also counts the start speed (the loader's does not), a missing
// duration never ends. A 1814 whose group holds nothing the model collides with, which the loader
// drops without taking a slot, is left out by the caller (scan: groupsWithBody).
struct FollowRun {
    double x;
    double dur;   // seconds
};
inline bool followSlotsShort(std::vector<FollowRun> runs, double dxMax) {
    constexpr int kSlots = 2;   // dp: kFollowYSlots
    std::sort(runs.begin(), runs.end(),
              [](const FollowRun& a, const FollowRun& b) { return a.x < b.x; });
    double freeAt[kSlots];
    for (double& v : freeAt) v = -std::numeric_limits<double>::infinity();
    for (const FollowRun& r : runs) {
        int slot = -1;
        for (int s = 0; s < kSlots; ++s)
            if (freeAt[s] < r.x && (slot < 0 || freeAt[s] < freeAt[slot])) slot = s;
        if (slot < 0) return true;
        freeAt[slot] = r.x + r.dur * 240.0 * dxMax;
    }
    return false;
}

// The triggers that change only what is seen, never what is touched, by their editor icon in
// ObjectToolbox::init (2.2081): the enter effects (edit_ee*), colour (edit_eTintCol01Btn), start
// position, ghost trail, pulse, alpha, shake, animate, player hide and show, background effect on and
// off, zoom, camera guide, song, gradient, the shaders (edit_eSh_*), middle-ground setup, background /
// ground / middle-ground change, the fade and tint of the area and enter triggers, SFX, edit SFX, edit
// song, background and middle-ground speed, spawn particle, UI settings, BPM and link visible. Sorted. dp keeps the same list
// (dp/src/dp/triggers.hpp, actLookOnlyId); tools/check_look_only_tables.py fails when they differ, so change both together.
inline constexpr int kLookOnly[] = {
    22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 55, 56, 57, 58, 59, 105, 744, 899, 900, 915,
    1006, 1007, 1520, 1585, 1612, 1613, 1818, 1819, 1913, 1915, 1934, 2016, 2903, 2904, 2905, 2907,
    2909, 2910, 2911, 2912, 2913, 2914, 2915, 2916, 2917, 2919, 2920, 2921, 2922, 2923, 2924, 2999,
    3009, 3010, 3014, 3015, 3020, 3021, 3029, 3030, 3031, 3602, 3603, 3605, 3606, 3608, 3612, 3613,
    3642, 3662};

// Calls f(n) for each '.'-separated number of a list such as key 57's groups; `step` 2 takes every
// other one from the first (a "group.chance.group.chance" list).
template <class F>
inline void forEachListed(std::string_view list, F&& f, int step = 1) {
    size_t p = 0;
    for (int i = 0; !list.empty() && p <= list.size(); ++i) {
        const size_t d = list.find('.', p);
        if (i % step == 0)
            f(toInt(list.substr(p, d == std::string_view::npos ? std::string_view::npos : d - p)));
        if (d == std::string_view::npos) break;
        p = d + 1;
    }
}

// Does what makes a level differ by attempt reach play, or only what is shown? Followed from the
// string alone and eagerly -- when in doubt, it reaches play.
//   * Where it starts: the attempt count, and an item a 3641 keeps -- 491 (persistent) on and 492
//     (target all) off: item 80, or timer 80 with 494 -- when something writes that item at all
//     (activatePersistentItemTrigger 0x234a40 copies the live value into the kept table; with
//     persistent off it takes the item out, and target all only resets or empties the table).
//   * A 3620, 3619, 1611, 1811, 3614 or 3615 that reads such an item fires differently by attempt,
//     and an item a 3619, 1817 or 3614 so fired writes is one more. A counter label (1615) only shows
//     it. A slot's mode (keys 476 / 477 / 478) names an item (1) or a timer (2); without one it is
//     taken as either, and points and the level time (3, 4) start over each attempt.
//   * What such a trigger sets off: the spawn-triggered (key 62) triggers of a group it spawns (1268,
//     3620's true and false groups, 1912, 2068 and 3607's lists, ...); every trigger of a 1611 / 1811
//     target (a count toggles its group, and toggling on spawns it); and the triggers inside a group
//     it moves or toggles, whose own firing then moves with them.
//   * Play is reached when such a move, rotate, scale, follow, toggle, stop, touch or count acts on a
//     group holding anything but triggers and decoration (objtypes, or No Touch, key 121), or when
//     such a trigger outside kLookOnly acts by itself -- a teleport, a gravity change, an end, the
//     camera, an area move, or anything this does not know.
//   `sources` says what may start a taint: bit 0 the attempt count, bit 1 the items a 3641 keeps (both by default;
//   2 asks whether the kept items reach play with the count answered by the attempt pin).
inline bool attemptReachesPlay(std::string_view all, int sources = 3) {
    struct Obj {
        std::string_view kv;
        int id;
    };
    std::vector<Obj> objs;
    std::unordered_map<int, std::vector<int>> members;   // group -> objects in it
    forEachObject(all, [&](std::string_view obj) {
        const auto id = field(obj, "1");
        if (id.empty()) return;
        const int j = (int)objs.size();
        objs.push_back({obj, toInt(id)});
        forEachListed(field(obj, "57"), [&](int g) { members[g].push_back(j); });
    });
    // Speed portals are triggers to the export (type 20) but are touched like any object.
    auto isTrigger = [](int id) {
        return objtypes::isTrigger(id) && id != 200 && id != 201 && id != 202 && id != 203 && id != 1334;
    };
    auto lookOnly = [](int id) { return std::binary_search(std::begin(kLookOnly), std::end(kLookOnly), id); };
    auto key = [](int kind, int n) { return ((uint64_t)(uint32_t)kind << 32) | (uint32_t)n; };
    constexpr int kItem = 1, kTimer = 2, kAttempts = 5;
    auto slot = [&](int n, std::string_view mode, auto&& f) {
        if (n <= 0) return;
        if (mode == "1") f(key(kItem, n));
        else if (mode == "2") f(key(kTimer, n));
        else if (mode.empty() || mode == "0") { f(key(kItem, n)); f(key(kTimer, n)); }
    };
    auto reads = [&](const Obj& o, auto&& f) {
        const int n = toInt(field(o.kv, "80"));
        switch (o.id) {
            case 3619: case 3620:
                for (const auto& [k, m] : {std::pair{"80", "476"}, std::pair{"95", "477"}}) {
                    const auto mode = field(o.kv, m);
                    if (mode == "5") f(key(kAttempts, 0));
                    else slot(toInt(field(o.kv, k)), mode, f);
                }
                break;
            case 1611: case 1811: case 1615: if (n > 0) f(key(kItem, n)); break;
            case 3614: case 3615: if (n > 0) f(key(kTimer, n)); break;
            default: break;
        }
    };
    auto writes = [&](const Obj& o, auto&& f) {
        const int n = toInt(field(o.kv, "80"));
        switch (o.id) {
            case 3619: slot(toInt(field(o.kv, "51")), field(o.kv, "478"), f); break;
            case 1817: if (n > 0) f(key(kItem, n)); break;
            case 3614: if (n > 0) f(key(kTimer, n)); break;
            default: break;
        }
    };

    // What can hold a value at all: the item triggers' targets, and any object with an item ID (80)
    // that is not a trigger, a collision block (1816, whose 80 is its block ID) or a counter label.
    std::unordered_set<uint64_t> written, tainted;
    std::vector<int> readers;
    for (int j = 0; j < (int)objs.size(); ++j) {
        const Obj& o = objs[j];
        writes(o, [&](uint64_t k) { written.insert(k); });
        const int n = toInt(field(o.kv, "80"));
        if (n > 0 && !isTrigger(o.id) && o.id != 1816 && o.id != 1615) written.insert(key(kItem, n));
        switch (o.id) {
            case 3619: case 3620: case 1611: case 1811: case 3614: case 3615: case 1615:
                readers.push_back(j);
                break;
            default: break;
        }
    }
    if (sources & 1) tainted.insert(key(kAttempts, 0));
    for (const Obj& o : objs) {
        if (o.id != 3641 || !(sources & 2)) continue;
        const int n = toInt(field(o.kv, "80"));
        if (n <= 0 || !isOn(field(o.kv, "491")) || isOn(field(o.kv, "492"))) continue;
        const uint64_t k = key(isOn(field(o.kv, "494")) ? kTimer : kItem, n);
        if (written.count(k)) tainted.insert(k);
    }

    std::vector<char> marked(objs.size(), 0);
    std::vector<int> work;
    auto mark = [&](int j) {
        if (!marked[j]) {
            marked[j] = 1;
            work.push_back(j);
        }
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (int j : readers) {
            if (marked[j]) continue;
            bool hit = false;
            reads(objs[j], [&](uint64_t k) { hit = hit || tainted.count(k) != 0; });
            if (hit) mark(j);
        }
        while (!work.empty()) {
            const int j = work.back();
            work.pop_back();
            changed = true;
            const Obj& o = objs[j];
            writes(o, [&](uint64_t k) { tainted.insert(k); });
            if (o.id == 1615) continue;
            if (o.id == 3641) {
                const int n = toInt(field(o.kv, "80"));
                if (n > 0 && isOn(field(o.kv, "491")))
                    tainted.insert(key(isOn(field(o.kv, "494")) ? kTimer : kItem, n));
                continue;
            }
            if (lookOnly(o.id)) continue;
            const bool count = o.id == 1611 || o.id == 1811;
            auto spawn = [&](int g) {
                if (g <= 0) return;
                const auto it = members.find(g);
                if (it == members.end()) return;
                for (int m : it->second)
                    if (isTrigger(objs[m].id) && (count || isOn(field(objs[m].kv, "62")))) mark(m);
            };
            bool spawner = true;
            switch (o.id) {
                case 1268: case 3614: case 3615: case 3604: case 1812: case 1815: case 1611: case 1811:
                    spawn(toInt(field(o.kv, "51")));
                    break;
                case 3620: case 1912: case 3609:
                    spawn(toInt(field(o.kv, "51")));
                    spawn(toInt(field(o.kv, "71")));
                    break;
                case 2068: forEachListed(field(o.kv, "152"), spawn, 2); break;
                case 3607: forEachListed(field(o.kv, "435"), spawn, 2); break;
                case 3619: case 1817: break;
                default: spawner = false; break;
            }
            bool acts = count;
            switch (o.id) {
                case 901: case 1346: case 2067: case 1347: case 1814: case 3016: case 3033: case 1049:
                case 1616: case 1595:
                    acts = true;
                    break;
                default: break;
            }
            if (acts) {
                const auto it = members.find(toInt(field(o.kv, "51")));
                if (it == members.end()) continue;
                for (int m : it->second) {
                    const Obj& in = objs[m];
                    if (isTrigger(in.id)) mark(m);
                    else if (!isOn(field(in.kv, "121")) && !objtypes::isDecoration(in.id)) return true;
                }
                continue;
            }
            if (!spawner) return true;
        }
    }
    return false;
}

// The objects (by index in the level string) a spawn chain with no delay reaches from a root dp
// follows (--tptrigspawn (removed), --gravchain (removed), --tptouch, --gravtouch: triggers.hpp spawnFiresOf): a Spawn
// (1268) or a Toggle that turns its group on (1049, key 56), not itself spawn-triggered (key 62),
// fires its target group (key 51), and a spawn-triggered one of the same kinds in that group goes
// on. A Spawn with a delay (key 63) or an ordered one (key 441) ends the walk: the old model takes a
// delayed chain only from an x crossing and only where the speed holds, which this does not ask.
// `throughDelay` (the teleport triggers' walk): the trigger shadow (dp trigshadow.hpp, on by default,
// cfg `trigshadow`) runs a delayed or ordered Spawn on its own timeline and hands the Teleport it fires
// to the player (shadow_step.hpp, TrigLive::tpFiresAt), so such a Spawn does not end the walk. Measured
// on a custom level: the ordered chain from the crossing Spawn uid 2354 (delay 10.254 s) fires the
// Teleport trigger uid 2355, GD puts the cube at (28770, 135) at t=20,738, and the 402-tick replay window
// from t=20,601 with the cfg's trigshadow arguments is empty. The gravity triggers keep the strict walk.
inline std::vector<char> spawnChainReach(std::string_view all, bool throughDelay = false) {
    std::vector<std::string_view> objs;
    std::unordered_map<int, std::vector<int>> members;   // group -> objects in it
    forEachObject(all, [&](std::string_view obj) {
        const int j = (int)objs.size();
        objs.push_back(obj);
        forEachListed(field(obj, "57"), [&](int g) { members[g].push_back(j); });
    });
    auto hop = [&](std::string_view o) {   // the group a no-delay spawner fires, or 0
        const int id = toInt(field(o, "1"));
        const bool spawner = id == 1268 || (id == 1049 && isOn(field(o, "56")));
        if (!spawner) return 0;
        if (id == 1268 && !throughDelay && (toNum(field(o, "63")) != 0.0 || isOn(field(o, "441")))) return 0;
        return std::max(0, toInt(field(o, "51")));
    };
    // ...and an Event trigger (3604) on the crossing queue whose events are all a pad's or an orb's (dp
    // --eventtrig (removed) takes those off the state's own pads and orbs: PadActivated / OrbActivated and YellowOrb 34 ..
    // SpiderPad 49), with no extra ID (447 / 525)
    auto eventRoot = [&](std::string_view o) {
        if (toInt(field(o, "1")) != 3604 || isOn(field(o, "11"))) return 0;
        if (toInt(field(o, "447")) != 0 || toInt(field(o, "525")) != 0) return 0;
        bool any = false, all = true;
        forEachListed(field(o, "430"), [&](int e) {
            any = true;
            all = all && (e == 8 || e == 9 || (e >= 34 && e <= 49));
        });
        return (any && all) ? std::max(0, toInt(field(o, "51"))) : 0;
    };
    std::vector<char> reach(objs.size(), 0);
    std::vector<int> work;
    std::unordered_set<int> seen;
    for (const auto o : objs)
        if (!isOn(field(o, "62"))) {
            if (const int g = hop(o); g > 0 && seen.insert(g).second) work.push_back(g);
            if (const int g = eventRoot(o); g > 0 && seen.insert(g).second) work.push_back(g);
        }
    while (!work.empty()) {
        const int g = work.back();
        work.pop_back();
        const auto it = members.find(g);
        if (it == members.end()) continue;
        for (const int m : it->second) {
            reach[m] = 1;
            if (isOn(field(objs[m], "62")))
                if (const int g2 = hop(objs[m]); g2 > 0 && seen.insert(g2).second) work.push_back(g2);
        }
    }
    return reach;
}

// The objects a DELAYED or ORDERED chain from an x crossing reaches where dp places them (the gravity triggers' walk: --gravspawn (removed)
// with --gravchain (removed), cli.hpp spawnFireX). The walk is triggers.hpp spawnFiresFromRoots from the roots that are not touch-triggered
// (a touch's or an Event's root with a delay is left out by the loader): a group is fired `delayS` behind its root -- each
// Spawn's own (key 63), and an ordered Spawn's (key 441) (x - xFirst) / 311.58 s behind the group's first spawn-triggered trigger
// -- with the loader's own bound of 4096 steps per root. An object it reaches by a delay counts only where dp does not refuse
// the fire: a speed portal, a time warp or a teleport trigger on the crossing queue, or a teleport portal between the root and
// the x the delay carries the fire to (the loader asks the speed in force at the root; here the fastest, 576 px/s, and a margin of
// 60 px for the portal's width and the player's: a window never narrower, so a level dp would refuse keeps its warning).
inline std::vector<char> spawnChainReachDelayed(std::string_view all) {
    struct Ob {
        int id = 0, target = 0;
        double x = 0.0, delay = 0.0;
        bool spawned = false, ordered = false, touch = false, spawner = false, trig = false;
    };
    std::vector<Ob> ob;
    std::vector<double> stops;   // x of every object that makes dp refuse a delayed fire across it
    std::unordered_map<int, std::vector<int>> members;
    forEachObject(all, [&](std::string_view obj) {
        const int j = (int)ob.size();
        Ob o;
        o.id = toInt(field(obj, "1"));
        o.x = toNum(field(obj, "2"));
        o.spawned = isOn(field(obj, "62"));
        o.touch = isOn(field(obj, "11"));
        o.target = std::max(0, toInt(field(obj, "51")));
        o.trig = objtypes::isTrigger(o.id);
        o.spawner = o.id == 1268 || (o.id == 1049 && isOn(field(obj, "56")));
        if (o.id == 1268) { o.delay = std::max(0.0, toNum(field(obj, "63"))); o.ordered = isOn(field(obj, "441")); }
        switch (o.id) {
            case 200: case 201: case 202: case 203: case 1334: case 747: stops.push_back(o.x); break;
            case 1935: if (!o.spawned && !o.touch) stops.push_back(o.x); break;
            case 3022: if (!o.spawned && !o.touch && !isOn(field(obj, "352"))) stops.push_back(o.x); break;   // key 352: ignoreX
            default: break;
        }
        ob.push_back(o);
        forEachListed(field(obj, "57"), [&](int g) { members[g].push_back(j); });
    });
    auto refused = [&](double rootX, double delayS) {
        const double lag = std::max(0.0, std::ceil(delayS * 240.0 - 1e-9) - 1.0);
        const double to = rootX + lag * (576.0 / 240.0) + 60.0;
        for (const double s : stops) if (s > rootX - 60.0 && s <= to) return true;
        return false;
    };
    std::vector<char> reach(ob.size(), 0);
    struct Item { int group; double delayS; bool ordered; };
    std::vector<Item> stack;
    for (const Ob& root : ob) {
        if (root.spawned || root.touch || !root.spawner || root.target <= 0) continue;
        stack.assign(1, Item{root.target, root.delay, root.ordered});
        int guard = 0;
        while (!stack.empty() && guard++ < 4096) {
            const Item it = stack.back();
            stack.pop_back();
            const auto gi = members.find(it.group);
            if (gi == members.end()) continue;
            double xFirst = 1e18;
            if (it.ordered)
                for (const int m : gi->second)
                    if (ob[(size_t)m].spawned && ob[(size_t)m].trig) xFirst = std::min(xFirst, ob[(size_t)m].x);
            for (const int m : gi->second) {
                const Ob& o = ob[(size_t)m];
                const double d = it.delayS + ((it.ordered && xFirst < 1e17) ? std::max(0.0, (o.x - xFirst) / 311.5801086425781) : 0.0);
                if (!reach[(size_t)m] && (d <= 0.0 || !refused(root.x, d))) reach[(size_t)m] = 1;
                if (o.spawned && !o.touch && o.spawner && o.target > 0) stack.push_back({o.target, d + o.delay, o.ordered});
            }
        }
    }
    return reach;
}

// Every group number some object names as a target: a trigger's target (51) and centre group (71), an
// Advanced Random's (152) and a Sequence's (435) lists, a Spawn's remap pairs (442, either side of a
// pair). Over-inclusive on purpose (a list's chances and counts are taken too): it answers "can anything
// fire a spawn-triggered object of this group?", and a group named nowhere cannot be fired.
// An object that never spawns what it names is left out: an Alpha (1007), a Pulse (1006) and a Stop (1616) only change or end an effect on their target (dp trigshadow.hpp: no
// spawnGroup on their path; GJBaseGameLayer::toggleGroupTriggered 0x32b7f0 is the one place a switch spawns, and only when the Toggle is set to activate: the same test as spawnChainReach's
// `spawner`, key 56).
inline std::unordered_set<int> targetedGroups(std::string_view all) {
    std::unordered_set<int> t;
    forEachObject(all, [&](std::string_view obj) {
        const int oid = toInt(field(obj, "1"));
        if (oid == 1006 || oid == 1007 || oid == 1616 || (oid == 1049 && !isOn(field(obj, "56")))) return;
        for (const char* key : {"51", "71", "152", "435", "442"})
            forEachListed(field(obj, key), [&](int g) { if (g > 0) t.insert(g); });
    });
    return t;
}

// The groups (key 57; a follow trigger's key 51 names one) that hold an object with a body. A trigger
// does not count, nor does a No Touch copy (key 121) or an ID the game builds as decoration. An object of
// an ID the table does not know counts, as everywhere in this file. One pass over the level: ask it
// once and look the groups up, not once per group.
inline std::unordered_set<int> groupsWithBody(std::string_view all) {
    std::unordered_set<int> has;
    forEachObject(all, [&](std::string_view obj) {
        const int id = toInt(field(obj, "1"));
        if (isOn(field(obj, "121")) || objtypes::isDecoration(id)
            || id == 1816   // a Collision Block (export type 39) is in none of the loader's collision buckets: dp says "its group holds nothing the model collides with" (a custom level: groups 151, 152, 173 = a 1816 and a decoration)
            || (objtypes::isTrigger(id) && id != 200 && id != 201 && id != 202 && id != 203 && id != 1334))   // a speed portal has a body
            return;
        forEachListed(field(obj, "57"), [&](int g) { has.insert(g); });
    });
    return has;
}

inline Findings scan(std::string_view all, bool platformer) {
    Findings r;
    if (platformer) r.unmodelled |= kPlatformer;
    const std::string_view head = all.substr(0, all.find(';'));
    if (field(head, "kA10") == "1") r.unmodelled |= kTwoPlayer;
    struct Portal { double x; int group; bool keepX; bool spawnedOnly = false; std::vector<int> own; };   // spawnedOnly / own: a spawn-triggered 3022 and its groups (never fired when no object names one)
    std::vector<Portal> portals;
    std::vector<GravPortal> gravity;
    bool reversal = false;
    std::vector<FollowRun> follows;
    // the runs dp would give a slot, with their target group: a run whose group holds no body is
    // dropped by the loader without taking one, so followSlotsShort is asked of those that do
    std::vector<std::pair<int, FollowRun>> followCand;
    std::vector<int> unrunFollowGroups;   // target groups of the follow triggers dp does not run
    bool attempts = false;
    // spawned teleport / gravity triggers (by object index), asked of spawnChainReach after the pass;
    // a gravity trigger for player 2, which counts only where there is a second player
    std::vector<int> spawnedTp, spawnedGrav;
    std::vector<std::vector<int>> spawnedGravGroups;   // key 57 of each spawnedGrav entry, for the "nothing names its group" test
    std::vector<std::vector<int>> spawnedTpGroups;     // ...and of each spawnedTp entry
    bool gravP2 = false, dualPortal = false;
    int idx = -1;
    double dxMax = speedDx(201);
    {
        static constexpr int kStartSpeed[] = {201, 200, 202, 203, 1334};   // header kA4 0..4
        const int s = toInt(field(head, "kA4"));
        if (s >= 0 && s < 5) dxMax = std::max(dxMax, speedDx(kStartSpeed[s]));
    }
    forEachObject(all, [&](std::string_view obj) {
        ++idx;
        const int id = toInt(field(obj, "1"));
        if (id == 286) dualPortal = true;
        if (const auto* n = findNewer(id)) {
            r.newer = true;
            r.newest = std::max(r.newest, n->version);
            ++r.newerCount[id];
        }
        // A No Touch object (key 121) is a decoration to the game (GameObjectType 7 on every one
        // in the export): a No Touch portal is not a portal and a No Touch ring reverses nothing.
        if (!reversal && isOn(field(obj, "117")) && !isOn(field(obj, "121"))) reversal = true;
        switch (id) {
            // the gravity toggle portal (2926) takes a bit too: the loader counts it with the two
            // gravity portals (dp --gravtoggle, on by default)
            case 10: case 11: case 2926:
                if (!isOn(field(obj, "121")))
                    gravity.push_back({toNum(field(obj, "2")), gravPortalHalfWidth(obj, id)});
                break;
            case 747: case 2902:
                // a force (345 / 347) and ignoreY (353) are modelled (dp --tppush, --tpkeepy (removed))
                if (isOn(field(obj, "121"))) break;   // No Touch: a decoration to the game, it teleports nobody
                if (id == 2902) {
                    const int g = toInt(field(obj, "51"));
                    if (g > 0) portals.push_back({toNum(field(obj, "2")), g, isOn(field(obj, "352"))});
                }
                break;
            case 3027: {
                // dp models the orb (--teleportorb) and, in the shadow step, its force (kOrbPush, retired);
                // it has no draw for a group of several; it moves x to the exit as the game does, so a
                // sideways orb is modelled (the keepX below only keeps it out of that test)
                if (isOn(field(obj, "121"))) break;   // No Touch: a decoration to the game, nobody touches it
                const int g = toInt(field(obj, "51"));
                if (g > 0) portals.push_back({toNum(field(obj, "2")), g, true});
                break;
            }
            case 3022: {
                // dp moves it as the game does when its x (--teleporttrig) or its touch (--tptouch)
                // sets it off, or a chain's root does (spawnChainReach, after the pass); one with a
                // target goes through the exits test like a 2902 (sideways, several exits)
                if (isOn(field(obj, "351"))) r.unmodelled |= kTeleportTrigger;
                else if (isOn(field(obj, "62")) && !isOn(field(obj, "11"))) {
                    spawnedTp.push_back(idx);
                    std::vector<int> gs;
                    forEachListed(field(obj, "57"), [&](int g) { gs.push_back(g); });
                    spawnedTpGroups.push_back(std::move(gs));
                }
                const int g = toInt(field(obj, "51"));
                if (g > 0) {
                    Portal p{toNum(field(obj, "2")), g, isOn(field(obj, "352"))};
                    if (isOn(field(obj, "62")) && !isOn(field(obj, "11"))) {
                        p.spawnedOnly = true;
                        forEachListed(field(obj, "57"), [&](int og) { p.own.push_back(og); });
                    }
                    portals.push_back(std::move(p));
                }
                break;
            }
            case 200: case 201: case 202: case 203: case 1334:
                dxMax = std::max(dxMax, speedDx(id));
                break;
            case 1814:
                if (toInt(field(obj, "51")) <= 0) break;
                if (isOn(field(obj, "91")) || isOn(field(obj, "11")) || isOn(field(obj, "62"))) {
                    // not modelled -- but that only matters if its group holds something with a body: dp loads a
                    // group of decoration alone as "nothing the model collides with" whatever the trigger. Asked
                    // after the pass (groupsWithBody).
                    unrunFollowGroups.push_back(toInt(field(obj, "51")));
                } else {
                    const auto speed = field(obj, "90");
                    const auto dur = field(obj, "10");
                    if (speed.empty() || toNum(speed) > 0.0)
                        followCand.push_back({toInt(field(obj, "51")),
                                              {toNum(field(obj, "2")),
                                               dur.empty() ? std::numeric_limits<double>::infinity()
                                                           : toNum(dur)}});
                }
                break;
            case 2066:
                // read off the string (keys 11 / 62 touch / spawn, 200 player 2; checked against the
                // export's touch / spawn / gpl columns on 457 triggers: no disagreement). dp leaves out
                // one for player 2 (gpl bit 1) and takes a touched one (--gravtouch); a spawned one is
                // asked of spawnChainReach after the pass
                // for player 2: one on the x-crossing queue is in the loader's second-body list
                // (--shadowp2grav, in the shadow's second body); one a touch or a spawn sets off is not
                if (isOn(field(obj, "200"))) {
                    if (isOn(field(obj, "11")) || isOn(field(obj, "62"))) gravP2 = true;
                } else if (isOn(field(obj, "62")) && !isOn(field(obj, "11"))) {
                    spawnedGrav.push_back(idx);
                    std::vector<int> gs;
                    forEachListed(field(obj, "57"), [&](int g) { gs.push_back(g); });
                    spawnedGravGroups.push_back(std::move(gs));
                }
                break;
            // what changes by attempt; whether it reaches play is asked after the pass
            case 3641: attempts = true; break;
            case 3619: case 3620:
                if (field(obj, "476") == "5" || field(obj, "477") == "5") attempts = true;
                break;
            default: break;
        }
    });
    if (attempts) {
        if (attemptReachesPlay(all)) {
            r.unmodelled |= kAttemptDependent;
            r.attemptKeeps = attemptReachesPlay(all, 2);
        } else r.attemptShown = true;
    }
    if (!spawnedTp.empty() || !spawnedGrav.empty()) {
        const std::vector<char> reach = spawnChainReach(all);
        // an unreached one whose groups no object names as a target is never set off (in the game
        // either): nothing to leave out
        std::unordered_set<int> named = targetedGroups(all);
        if (!spawnedTp.empty()) {
            // the teleport triggers' walk goes through a delayed or an ordered Spawn (the trigger shadow runs them)
            const std::vector<char> reachTp = spawnChainReach(all, true);
            for (size_t k = 0; k < spawnedTp.size(); ++k) {
                if (reachTp[(size_t)spawnedTp[k]]) continue;
                bool fired = false;
                for (const int g : spawnedTpGroups[k]) fired = fired || named.count(g) != 0;
                if (fired) r.unmodelled |= kTeleportTrigger;
            }
        }
        // a gravity trigger a delayed or ordered chain reaches from an x crossing is placed by dp as well (--gravchain (removed)), unless the delay
        // crosses a speed change, a warp or a teleport (spawnChainReachDelayed)
        std::vector<char> reachD;
        for (size_t k = 0; k < spawnedGrav.size(); ++k) {
            if (reach[(size_t)spawnedGrav[k]]) continue;
            if (reachD.empty()) reachD = spawnChainReachDelayed(all);
            if (reachD[(size_t)spawnedGrav[k]]) continue;
            bool fired = false;
            for (const int g : spawnedGravGroups[k]) fired = fired || named.count(g) != 0;
            if (fired) r.unmodelled |= kGravityTrigger;
        }
    }
    // a trigger for player 2 moves nobody the solver plans without a second player
    if (gravP2 && (dualPortal || (r.unmodelled & kTwoPlayer))) r.unmodelled |= kGravityTrigger;
    if (gravityPortalsRefused(std::move(gravity), reversal)) r.unmodelled |= kGravityPortals;
    // [2026-10-23] Every run used to take a slot, whatever its group held ("a 1814 whose group holds
    // nothing the model collides with ... takes one here"), so a custom level whose eight runs all
    // target decoration or nothing, two of them 999 s long, came out as "more than two at once" while
    // the loader drops all eight. The same body test as kFollowNotRun, from one pass over the level.
    if (!followCand.empty() || !unrunFollowGroups.empty()) {
        const std::unordered_set<int> bodyGroups = groupsWithBody(all);
        for (const auto& c : followCand)
            if (bodyGroups.count(c.first)) follows.push_back(c.second);
        if (followSlotsShort(std::move(follows), dxMax)) r.unmodelled |= kFollowSlots;
        for (const int g : unrunFollowGroups)
            if (bodyGroups.count(g)) {
                r.unmodelled |= kFollowNotRun;
                break;
            }
    }
    if (portals.empty()) return r;
    // The exits: every object in a portal's target group (key 57, '.'-separated).
    // teleportPlayer (0x315f20) draws the exit at random from the C runtime's generator when the group holds several objects (dp takes the
    // first; the shadow's draw, SelfShadow::doTeleport, has no seed or exit table behind it). The draw decides nothing when every object
    // stands on one spot with one turn (x, y, key 6 rotation, keys 4 / 5 flips: the force's angle reads the exit's turn) and nothing moves
    // it: a Move, Rotate, Follow, ... (any trigger but a Spawn, Toggle, Teleport, Event or touch Toggle) that names one of the groups the
    // object is in. A teleport trigger set off by a spawn that no object names (nobody can fire it) draws nothing either.
    struct Exits { int n = 0; bool same = true, moves = false; std::string sig; };
    std::unordered_map<int, Exits> exits;
    for (const auto& p : portals) exits.emplace(p.group, Exits{});
    std::unordered_set<int> movedGroups;
    forEachObject(all, [&](std::string_view obj) {
        const int id = toInt(field(obj, "1"));
        if (!objtypes::isTrigger(id) || id == 1268 || id == 1049 || id == 3022 || id == 3604 || id == 1595) return;
        for (const char* key : {"51", "71"})
            forEachListed(field(obj, key), [&](int g) { if (g > 0) movedGroups.insert(g); });
    });
    forEachObject(all, [&](std::string_view obj) {
        const auto groups = field(obj, "57");
        if (groups.empty()) return;
        std::string sig;
        bool movesIt = false;
        for (std::string_view part : {field(obj, "2"), field(obj, "3"), field(obj, "6"), field(obj, "4"), field(obj, "5")}) { sig += part; sig += '|'; }
        forEachListed(groups, [&](int g) { movesIt = movesIt || movedGroups.count(g) != 0; });
        forEachListed(groups, [&](int g) {
            const auto it = exits.find(g);
            if (it == exits.end()) return;
            Exits& e = it->second;
            if (e.n++ == 0) e.sig = sig;
            else if (e.sig != sig) e.same = false;
            e.moves = e.moves || movesIt;
        });
    });
    std::unordered_set<int> named;
    for (const auto& p : portals) {
        const Exits& e = exits[p.group];
        if (e.n <= 1) continue;
        if (e.same && !e.moves) continue;   // one exit wherever the draw falls
        if (p.spawnedOnly) {
            if (named.empty()) named = targetedGroups(all);
            bool fired = false;
            for (const int g : p.own) fired = fired || named.count(g) != 0;
            if (!fired) continue;
        }
        r.unmodelled |= kTeleportSeveralExits;
    }
    return r;
}

// Every bit set, "; "-separated, or "none" -- for the log line the menu writes.
inline std::string describeAll(uint32_t bits) {
    std::string s;
    for (size_t i = 0; i < sizeof(kUnmodelledNames) / sizeof(kUnmodelledNames[0]); ++i)
        if (bits & (1u << i)) s += (s.empty() ? "" : "; ") + std::string(kUnmodelledNames[i]);
    return s.empty() ? "none" : s;
}

// "2.2" for a Findings::newest of 22, or "" for none.
inline std::string versionName(int v) {
    return v > 0 ? std::to_string(v / 10) + "." + std::to_string(v % 10) : std::string();
}

// "901 x12, 1346 x3, ..." -- the kNewer objects found, most numerous first, at most `most` of
// them -- or "none". For the log line the menu writes.
inline std::string newerSummary(const Findings& f, size_t most = 12) {
    std::vector<std::pair<int, int>> v(f.newerCount.begin(), f.newerCount.end());
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    std::string s;
    for (size_t i = 0; i < v.size() && i < most; ++i)
        s += (i ? ", " : "") + std::to_string(v[i].first) + " x" + std::to_string(v[i].second);
    if (v.size() > most) s += ", +" + std::to_string(v.size() - most) + " more kinds";
    return s.empty() ? "none" : s;
}

// "a", "a and b", or "a, b and N more" for the bits set, in bit order, in their short phrases: the
// menu gives it one line.
inline std::string describe(uint32_t bits) {
    static_assert(sizeof(kUnmodelledShort) == sizeof(kUnmodelledNames), "one short phrase per bit");
    std::vector<const char*> names;
    for (size_t i = 0; i < sizeof(kUnmodelledShort) / sizeof(kUnmodelledShort[0]); ++i)
        if (bits & (1u << i)) names.push_back(kUnmodelledShort[i]);
    std::string s;
    const size_t shown = names.size() > 2 ? 2 : names.size();
    for (size_t i = 0; i < shown; ++i) {
        if (i) s += (i + 1 == names.size()) ? " and " : ", ";
        s += names[i];
    }
    if (names.size() > shown) s += " and " + std::to_string(names.size() - shown) + " more";
    return s;
}

}  // namespace levelwarn
