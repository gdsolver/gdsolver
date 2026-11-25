#pragma once
// The trigger shadow: GD's x-crossing triggers and the commands they make, run as a world of their own beside a trajectory. Read from the 22091 decompile
// (notes/trigger-state-table-2091-1028.md): GJBaseGameLayer::update 0x3406d0 (one step = FUN_140369ae0 steps the commands -> processMoveActions writes the groups' motion into the objects ->
// the player's update -> checkCollisions -> checkSpawnObjects 0x322080 fires the triggers the player's x has reached), GroupCommandObject2::step 0x361db0, FUN_140361e80 (the eased share),
// FUN_1403264c0 (a Move's command), GJBaseGameLayer::toggleGroup 0x32b860, GameToolbox::getEasedValue 0x73230.
//
// Closed form: the only thing taken from the game is the player's x after each step (`step(t, px)`); the world -- the trigger queue's index, the commands in flight, the groups' bits and the
// objects' counters, every object's position, rotation and on/off -- is the shadow's own and is carried from one step to the next. Self-contained on purpose (std only): tools/trig_replay.cpp
// compiles it alone and holds it against wtrace.csv (cfg wtrace, the live objects' state at full precision) before anything in the model depends on it.
//
// What a step is, in the dump's clock (tick t = the state at the end of the game's step t):
//   1. every command made in an earlier step takes its step: a new command's first step only clears its +0x1b0 flag (elapsed stays 0, but the eased share is still taken), so a Move made in step c
//      moves from step c+2 (f(0) = 0) and a Toggle, which makes no command, acts in step c itself;
//   2. checkSpawnObjects walks the channel's array from its saved index, in (int(x), uid) order, and fires each trigger while the player's x has reached its x -- it stops at the first one
//      that has not been reached, so the ones behind it wait (trigfire.csv against the dump's x: all but 1 of 3,411 calls).
// Not here (the objects they act on are `uncovered`): Area Move / Rotate / Scale, camera-locked and player-locked Moves, Follow, keyframes, Spawn / Touch / Count chains, a Rotate whose centre moves.
#include "dp/shared_const.hpp"
#include "dp/guard_hit.hpp"
#include <memory>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dp {
namespace gdtrig {
// --trigshadowrandlog FILE (observation only, default off): every draw a Random / Advanced Random makes in the REPLAY of a parentless timeline (the steps along the game's own x track, trigshadow_live.hpp replayInto) is appended to FILE as
// `step,uid,id,r15,group,rng,candidates` (r15 = the 15-bit number the draw used, group = the group it spawned, rng = the LCG state after the draw). Read against the game's own record, it says whether the shadow's branches are the game's.
inline std::string g_randLogFile;
inline bool g_randLogReplay = false;
inline std::atomic<long long> g_vertWaitsY{0};   // crossings of a vertical channel that waited for a world y the step did not have (checkSpawnObjects; trigshadow_live.hpp supplyPrint says it)

// GameToolbox::getEasedValue 0x73230, in float as the game computes it
inline float eased(float x, int kind, float rate) {
    if (kind == 0) return x;
    const float r = rate > 0.0f ? rate : 2.0f;
    const float pi = 3.1415927f;
    auto bounce = [](float t) -> float {
        if (t < 0.36363637f) return t * 7.5625f * t;
        if (t < 0.72727275f) return (t - 0.54545456f) * 7.5625f * (t - 0.54545456f) + 0.75f;
        if (t < 0.90909094f) return (t - 0.8181818f) * 7.5625f * (t - 0.8181818f) + 0.9375f;
        return (t - 0.95454544f) * 7.5625f * (t - 0.95454544f) + 0.984375f;
    };
    switch (kind) {
    case 1: { const float y = x + x; return y >= 1.0f ? 1.0f - std::pow(2.0f - y, r) * 0.5f : std::pow(y, r) * 0.5f; }
    case 2: return std::pow(x, r);
    case 3: return std::pow(x, 1.0f / r);
    case 4: {
        if (x == 0.0f || x == 1.0f) return x;
        const float y = (x + x) - 1.0f;
        const float s = std::sin(((y - r * 0.25f) * pi * 2.0f) / r);
        return y < 0.0f ? std::pow(2.0f, y * 10.0f) * -0.5f * s : std::pow(2.0f, y * -10.0f) * s * 0.5f + 1.0f;
    }
    case 5: {
        if (x == 0.0f || x == 1.0f) return x;
        const float s = std::sin((((x - 1.0f) - r * 0.25f) * pi * 2.0f) / r);
        return s * -std::pow(2.0f, (x - 1.0f) * 10.0f);
    }
    case 6: {
        if (x == 0.0f || x == 1.0f) return x;
        const float s = std::sin(((x - r * 0.25f) * pi * 2.0f) / r);
        return s * std::pow(2.0f, x * -10.0f) + 1.0f;
    }
    case 7: return x >= 0.5f ? bounce((x + x) - 1.0f) * 0.5f + 0.5f : (1.0f - bounce(1.0f - (x + x))) * 0.5f;
    case 8: return 1.0f - bounce(1.0f - x);
    case 9: return bounce(x);
    case 10: { const float y = (x + x) - 1.0f; return (x + x) >= 1.0f ? (2.0f - std::pow(2.0f, y * -10.0f)) * 0.5f : std::pow(2.0f, y * 10.0f) * 0.5f; }
    case 11: return x == 0.0f ? 0.0f : std::pow(2.0f, (x - 1.0f) * 10.0f) - 0.001f;
    case 12: return x == 1.0f ? 1.0f : 1.0f - std::pow(2.0f, x * -10.0f);
    case 13: return (std::cos(x * pi) - 1.0f) * -0.5f;
    case 14: return 1.0f - std::cos(x * 1.5707964f);
    case 15: return std::sin(x * 1.5707964f);
    case 16: { const float y = x + x; if (y >= 1.0f) { const float z = y - 2.0f; return (z * 3.5949094f + 2.5949094f) * z * z * 0.5f + 1.0f; } return y * y * (y * 3.5949094f - 2.5949094f) * 0.5f; }
    case 17: return x * x * (x * 2.70158f - 1.70158f);
    case 18: { const float y = x - 1.0f; return (y * 2.70158f + 1.70158f) * y * y + 1.0f; }
    default: return x;
    }
}

// A trigger as triggers.txt has it (only the columns the shadow reads)
struct TrigDef {
    int uid = 0, id = 0, target = 0, center = 0, chan = 0, touch = 0, spawn = 0, togon = -1, ease = 0, t360 = 0;
    int actgrpCol = -1;   // a touch 1594's own "Activate Group" column (triggers.txt actgrp): 1 switches its target group on (and spawns it), 0 off; -1 = not read
    bool spawnOnly = false;   // a touch 1594 with m_isSpawnOnly (triggers.txt sponly = 1): its press SPAWNS the target group and switches nothing (triggers.hpp TrigRow::sponly); its Activate Group column is not what it does
    double cx = 0, cy = 0, dur = 0, ox = 0, oy = 0, erate = 2.0, deg = 0;
    bool lockPlayer = false, lockCam = false;   // a Move locked to the player / the camera (the camera ones are run only with camLockOk)
    bool lockCX = false, lockCY = false;        // ...which axis of the camera (m_lockToCameraX / Y)
    bool camLockOk = false;                     // the loader's option camLock: a Move locked to the camera is run (its group moves by the camera centre's displacement of the step before)
    bool lockPX = false, lockPY = false;        // ...which axis of the player (m_lockToPlayerX / Y)
    double modX = 1.0, modY = 1.0;              // the lock's multiplier (m_moveModX / Y; 0 means 1)
    bool lockrot = false;
    double tw = 0;   // a Time Warp trigger's factor (EffectGameObject::m_timeWarpTimeMod, objrects.txt `tw`)
    bool areaStopOk = false;   // an Area Stop (3024) and the dump has the Areas' effect ids
    bool areaOk = false;   // an Area Move (3006) whose areas.txt row is one the shadow runs (see AreaDef)
    bool editOk = false;   // an Edit Area (3011..3015) whose areaedit.txt row changes only fields of an Area Move the shadow keeps (see AreaEdit)
    double sdelay = 0;     // a Spawn trigger's delay in seconds (m_spawnDelay)
    bool sord = false, remap = false, silent = false;   // a spawn-order index (not read), a group remap, m_isSilent: a Spawn the shadow runs has none of them
    bool sordd = false;    // the Spawn is ordered: the triggers of its group are spawned one after the other, by their x (FUN_140322560)
    // an Item Compare (3620, ItemTriggerGameObject; trigext.txt): the shadow runs it when every value it reads is one the shadow has (itemCmp)
    bool itemCmp = false; int i1mode = 0, i2mode = 0, item1 = 0, item2 = 0, res1 = 0, res2 = 0, res3 = 0, rnd1 = 0, rnd2 = 0, sgn1 = 0, sgn2 = 0; float mod1 = 0, mod2 = 0, tol = 0;
    // T57 (the item values the shadow keeps): an Item Edit (3619, GJBaseGameLayer::activateItemEditTrigger 0x33cf40: the same fields as an Item Compare, `target` is the item written -- NOT a group --, edTmode its kind), a Pickup trigger
    // (1817, CountTriggerGameObject::triggerObject 0x65c2b0 case 0x719: item pkItem, count pkCount, mode pkMode 0 add / 1 multiply / 2 divide, override pkOverride, factor pkFactor) and an Item Persist (3641,
    // activatePersistentItemTrigger 0x33d730: flags enable / all / reset / timer) the shadow runs: every value they read is one the shadow has (trigshadow_load.hpp itemUnknown)
    bool edOk = false, pkOk = false, psOk = false; int edTmode = 1, pkItem = 0, pkCount = 0, pkMode = 0; bool pkOverride = false; float pkFactor = 0; bool psEnable = false, psAll = false, psReset = false;
    int mvaObj = 0; bool mvaCol = false;   // a Move-To: the uid of the object the game takes for the group it measures from (trigext.txt `mva`; -1 none), and whether the dump has the column
    bool tpPlayer = false, tpCol = false;  // a Move-To whose destination is a player (m_targetPlayer1 / 2, trigext.txt), and whether the dump has the column
    int ctrObj = 0; bool ctrCol = false;   // the uid of the object the game took for the centre group (triggers.txt `ctrobj`: -1 none, 0 no centre group), and whether the dump has the column
    double dirDist = 0.0;   // m_directionModeDistance (key 396): a Move with a distance is in the game's Direction mode, a displacement the shadow does not compute (kindRuns: not run)
    bool mvtgt = false, dynmode = false; int mvaxis = 0, tmodctr = 0;   // a Move in the target mode (m_useMoveTarget), the dynamic one, which axis (m_moveTargetMode: 1 x, 2 y), the centre-modifier group (m_targetModCenterID)
    std::vector<std::pair<int, int>> remapTo;   // a Spawn's group remap (triggers.txt `remap`, source:source:target:flag; entries by ';'): a trigger it spawns acts on the target group in place of the source one
    std::string raw;   // a trigger that moves the player (Teleport 3022): its objrects.txt columns, as `name=value` text, for whoever reads the event
    double gmod = 1.0; int gpl = 0;   // a Gravity trigger (2066): the value it sets (objrects.txt gmod) and the player it is for (gpl)
    int optp1 = 0, optp2 = 0, optudg = 0;   // an Options trigger (2899): disable (1) / enable (-1) player 1's / 2's controls, unlink dual gravity (objrects.txt)
    int stopMode = 0;      // a Stop (1616): 0 stop, 1 pause, 2 resume (TriggerControlGameObject +0x760; stopdump.txt, absent = 0)
    bool stopCtrl = false; // ...and it works on a control ID instead of a group (+0x69c): not run
    bool multi = true;     // m_isMultiTriggered (+0x690, multi.txt): a spawn-triggered trigger without it fires once only (GJBaseGameLayer::spawnObject's gate); true where the dump has no row
    int ord = 0;           // a trigger's order value (the group array's sort key together with the channel)
    // a Collision trigger (1815; trigext.txt item1 / item2 / actgrp, a collision-block list from collblocks.txt): blocks A and B, whether it activates (toggles on and spawns) or deactivates its group `target`.
    // collOk: the dump has the block ids and the shadow's collision model is on (see TrigShadow::collisionModel)
    int collA = 0, collB = 0; bool collActivate = false, collOk = false;
    bool remapOk = false;   // a Spawn with a group remap that the shadow's own chain runs, handing the keys down the call (TrigShadow::curRemap_)
    // a Sequence trigger (3607, SequenceTriggerGameObject::triggerObject 0x655c50; the string's keys 435 / 436 / 437 / 438 / 439 / 505, tools: dp/src/dp/trigshadow_load.hpp parseSequences): each call spawns the next group of its list
    // (group, count: the group is spawned for `count` calls), `seqMode` (+0x7dc) says what happens after the last (0 stop, 1 loop, 2 stay on the last), `seqMinInt` (+0x7d8) is the least time between two calls that count,
    // `seqReset` (+0x7e4) the time without a call after which the count goes back (`seqResetMode` +0x7e0: 0 to the start, else by the number of whole periods), `seqUnique` (+0x7ec) a count per remap key
    bool seqOk = false, seqUnique = false; std::vector<std::pair<int, int>> seqTargets; int seqMode = 0, seqResetMode = 0; double seqMinInt = 0, seqReset = 0;
    // a Random (1912, EffectGameObject::triggerObject case 0x778: one draw of the trigger LCG, `chance` (key 10, +0x5bc) < r / 32767 * 100 spawns the centre group (+0x5cc), else the target group (+0x5c8)) or an Advanced Random
    // (2068, RandTriggerGameObject::triggerObject 0x4b41e0: one draw, the first entry of seqTargets (group, weight) whose running weight reaches roundf(r / 32767 * the weights' sum) is spawned): run when randOk (TrigShadow::rng_)
    bool randOk = false; double randChance = 0.0;
    // a Transform trigger (2067) that only scales (GJBaseGameLayer::triggerTransformCommand 0x327220; the string's keys 150 / 151 scale, 153 / 154 divide, 152 only move, 156 relative scale; parseTransforms in trigshadow_load.hpp):
    // each step the target group's offset from the centre object is multiplied by (the scale reached this step) / (the one of the step before), where the scale is (S - 1) * eased share + 1
    // a Count (1611, 0x65c2b0: registers an entry on an item) or an Instant Count (1811: compares the item's number at once): the item, the count it waits for / compares with, the Instant Count's mode (0 equal, 1 more than, 2 less than),
    // whether the group is switched on (and spawned) or off, and the Count's multi-activate (key 104, from the level string; false = the entry goes after it has fired)
    bool cntOk = false, cntAct = false, cntMulti = false; int cntItem = 0, cntCount = 0, cntMode = 0;
    bool sclOk = false; double sclX = 1.0, sclY = 1.0;   // the scale already divided (key 153 / 154 set: 1 / S; a scale of 0 is 1)
    bool sclRel = false;   // Relative Scale (key 156 / property 577): at the fire the scale is divided by the centre object's own scale ratio (triggerTransformCommand 0x327220, `(S - 1) / ratio + 1` per axis)
    // an Event trigger (3604, EventLinkTrigger; GJBaseGameLayer::activateEventTrigger 0x33ada0 [R]; triggers.txt events / evextra): when it fires it registers, for each event of `evList`, a listener under the key
    // (event, evKey = extra ID 1 * 10000 + extra ID 2); a game event raised with that key (TrigShadow::raiseEvent, gameEventTriggered 0x33ac80) spawns the target group. Run when the list is read and the extra IDs are 0 / 0
    // (a listener with an extra ID filters on a material / player the shadow is not told), and only in a timeline that is fed its events (TrigShadow::setEventsFed)
    bool evOk = false; std::vector<int> evList; int evKey = 0;
    int rgSwarm = 0, rgSwch = -1, rgDir = 0; bool rgChanOnly = false;   // a Rotate Gameplay trigger (2900): it switches the active channel (m_changeChannel) to rgSwch (rotgameplay.txt) and, unless it is channel-only, turns the player to the direction rgDir (objrects gnddir: 1 up, 2 down, 3 left, 4 right)
};

// An Area Move as areas.txt has it (the columns the shadow reads). Run only for a shape whose every term is a constant: the horizontal or vertical direction (dirType 1 / 2) in the x / y mode,
// no variation (lenV, offV, moveXV, moveYV all 0), one ease for both sides (eOut 0), the centre a player (special -1), an object (special 0 + centre uid) or a point of the screen
// (special -3 .. -11, the camera: see Cam; NOT run yet, see below), no `relative`.
// Screen-centred Areas: stepAreas can read a Cam (camL = camx / zoom, camW = window width / zoom from the dump), but the centre it gives is not the game's: on c136796102 (zoom 1.1) the game's
// centre is ~9.8 px further right than camL + W/2 with W = 320 * 2490 / 1401, i.e. the game's area code reads a camera position (+0x410) that dump.camx / zoom does not reproduce
// (a window width of ~590.1 fits one Area). Until the harness hands the shadow that camera position, such Areas stay uncovered (tools/trig_replay.cpp does not arm them).
struct AreaDef {
    int uid = 0, target = 0, special = -1, center = 0, dirType = 1, eInType = 0, eInBuf = -1;
    int index = 0;   // the trigger's target-group index (+0x698): >= 1 = works on parents, its arrays in areaArrays_ / areaDicts_ (areatargets.txt)
    bool inbound = false, twoDir = false;
    double len = 0, off = 0, modFront = 1, modBack = 1, deadzone = 0, moveX = 0, moveY = 0, eInRate = 2;
    bool angleMode = false;   // the Move is a distance at an angle (xy = 0, angle 0: straight up) instead of an x / y offset (processAreaMoveGroupAction's other branch)
    double moveDist = 0;
    // a separate ease-out (EnterEffectObject +0x7c5, areas.txt `eOut`): getEasedAreaValue 0x330eb0 takes the ease-in or the ease-out curve by the side the object is on, latched per object while it is in the
    // transit zone (see TrigShadow::areaMoveOf). eOutType / eOutRate: +0x79c / +0x7a0; eOutBuf: +0x7a4 (-1 the value itself, -2 the curve, else the table base)
    bool eOutOn = false;
    int eOutType = 0, eOutBuf = -1;
    double eOutRate = 2;
    // an Area Scale (3008, processAreaTransformGroupAction 0x331e60): not a displacement but a size factor (S - 1) * (1 - eased) + 1 per axis, S = scaleX / scaleY (areas.txt; only the constant ones are run)
    bool scaleKind = false; double scaleX = 0, scaleY = 0;
    // direction type 0 (getAreaObjectValue 0x330cc0: the value is the distance from the centre point + (off, offY), the side multipliers are not used) and, in the angle shape, `relative`: the move is along the line from the
    // centre to the object (processAreaMoveGroupAction 0x333120), `relFade` = the distance under which it fades out (areas.txt relFade, +0x78c, held at 0 or more)
    double offY = 0, relFade = 0; bool relative = false;
    double angle = 0;   // the angle shape's direction in degrees (m_moveAngle): 0 down, 180 up, 90 / -90 the sides (areaMoveDisp)
    // the variation terms (areas.txt lenV, offV, offYV, moveXV, moveYV, moveDistV, angleV): each is multiplied by an entry of the layer's table V at (the object's m_varianceIndex + k), see TrigShadow::vAt. All 0 = a constant Area
    double lenV = 0, offV = 0, offYV = 0, moveXV = 0, moveYV = 0, moveDistV = 0, angleV = 0;
    bool anyVar() const { return lenV != 0 || offV != 0 || offYV != 0 || moveXV != 0 || moveYV != 0 || moveDistV != 0 || angleV != 0; }
};

// An Edit Area trigger (3011..3015, GJBaseGameLayer::triggerAreaEffect 0x32fe50 -> EnterEffectInstance::loadTransitions 0x209ae0) as areaedit.txt has it: the fields it changes (the instance's slot index of
// FUN_14020aa40 -> the new value; 0 length, 2 offset, 6 dead zone, 11 move x, 13 move y), taken from the current value over `dur` seconds with the trigger's ease-in (type, rate, buffer).
// useEffect: it finds the Area by effect id (the first one in force with that id, every one for an id of 0 or less) instead of by the Area triggers in the group `target`.
struct AreaEdit {
    int uid = 0, target = 0, type = 0, buf = -1;
    bool useEffect = false;
    float dur = 0, rate = 2;
    std::vector<std::pair<int, float>> fields;   // ascending slot index
};

// The camera as the Area actions read it: the view's left / bottom edge in object space and its size (window size / zoom). Set by the caller before each step; the areas read the previous step's.
struct Cam {
    double l = 0, b = 0, w = 0, h = 0;
    bool ok = false;
    bool okY = false;   // b is the game's (a recorded camera, or the camera model under a follow Static past its tween); the camera model has no Y otherwise and leaves b at 0
};

struct ObjState {
    int uid = 0;
    double x = 0, y = 0, rot = 0;   // position and rotation as the game holds them
    int counter = 0;                // +0x4c0, the disable counter
    bool areaXLock = false, areaKidSkip = false, areaAlways = false;   // +0x2c8 (an x displacement is dropped), +0x520 (a child is not moved), +0x305 (the section range is skipped), from areatargets.txt
    bool noOwnRot = false;          // m_canRotateFree (+0x36c) is false: a Rotate trigger turns it round its centre but leaves its own rotation alone (FUN_140334450 adds the angle only where +0x36c is set)
    int secX = 0;                   // its section along x at load (the game's +0x278): the Areas look only at the sections near their centre
    long long areaStamp = 0;        // the Area step this object was last reset in (resetAreaObjectValues' once-per-step gate)
    float asx = 1.0f, asy = 1.0f;   // the Area Scale factors in force this step (1 = none); the object's size is its own size times these
    float osx = 1.0f, osy = 1.0f;   // its OWN scale as a ratio to the scale it was loaded with (the game's +0x488 / +0x3dc and +0x48c / +0x3e0): a Transform that scales takes it along (scaleGroup), and a Relative Scale Transform reads the centre object's
    float ax = 0, ay = 0;           // the Area Move offset already inside x / y (+0x2a0 / +0x2a4), taken off again before the next frame's areas are added
    int lastTrig = -1, lastTrigId = 0;   // the last trigger (uid, object id) that acted on it (a report, not a state)
    long long rank = -1;                 // its place in the level's object order (see addObject)
    bool blindArea = false;              // `blind` was set by a screen Area of the stateless shape alone (TrigShadow::setBlindArea): it comes off at the next step whose camera is certain
    bool blind = false;                  // a Move locked to the camera stepped it with a camera that was not certain (its displacement was taken as 0), or a Rotate / Scale turned it about a centre object that is blind: its place is no longer the game's, for good (sticky; the supply leaves it on the model's placement)
    bool on() const { return counter >= 0; }
    IntSpan groups;   // a view into TrigShadow::groupStore_ (shared_const.hpp: IntSpan, and its lifetime)
};

class TrigShadow {
public:
    CowPages<ObjState> objs;   // read with objs[i], written with objs.mut(i): a copy of the shadow shares the pages no step has written since (shared_const.hpp)
    std::shared_ptr<std::deque<std::vector<int>>> groupStore_ = std::make_shared<std::deque<std::vector<int>>>();   // the lists the objects' `groups` views point into: shared by every copy of the shadow, appended to only by addObject
    SharedConst<std::unordered_map<int, size_t>> byUid;
    SharedConst<std::unordered_map<int, std::vector<size_t>>> members;   // group -> object indices, in the file's order
    std::unordered_map<int, bool> groupBit;                  // a group's switch (absent = on)
    SharedConst<std::set<int>> covered;                                   // groups only the shadow's triggers act on
    long long fired = 0, queueWaits = 0;
    // the player-visibility trigger (1612 hide / 1613 show) is an x-crossing object like the others: its place in the queue is real, and what it sets is the player's hidden byte (+0x733 by toggleVisibility)
    bool controlsDisabled[2] = {false, false};   // from the Options triggers (player 1's byte +0xa33)
    struct Event { long long step; int uid, id; std::string raw; };
    std::vector<std::pair<long long, int>> spawnReach;   // (step, trigger uid) of every spawn-triggered trigger the shadow does NOT run that its spawn chain reached (a report; the camera triggers' fire ticks)
    std::vector<std::pair<long long, int>> spawnFires;   // (step, trigger uid) of every spawn-triggered trigger the shadow's own spawn chain fired
    std::vector<Event> events;   // fired triggers that act on the player (Teleport), in firing order
    struct GravChange { long long step; int player; double value; int uid; };
    std::vector<GravChange> gravChanges;
    struct CtrlChange { long long step; int player; bool disabled; };
    std::vector<CtrlChange> ctrlChanges;
    bool playerHidden = false;
    std::vector<std::pair<long long, bool>> hiddenChanges;   // (step it fired in, hidden afterwards)

    // ---- loading -------------------------------------------------------------------------------------------------------------------------------
    // `rank` = the object's place in the level's own object order (objgroups.txt's line): the first member of a group (tryGetObject, a Rotate's centre) is the lowest rank, whatever order the objects were added in
    void addObject(int uid, double x, double y, double rot, int counter, const std::vector<int>& groups, long long rank = -1) {
        ObjState o;
        o.secX = std::max(0, (int)((float)x * 0.01f));   // a negative x is in section 0
        o.uid = uid; o.x = x; o.y = y; o.rot = rot; o.counter = counter; o.rank = rank;
        if (!groups.empty()) { groupStore_->push_back(groups); o.groups = IntSpan(groupStore_->back()); }
        byUid.mut()[uid] = objs.size();
        for (int g : groups) members.mut()[g].push_back(objs.size());
        objs.push_back(o);
    }
    void setGroupBit(int g, bool on) { groupBit[g] = on; }
    // (dptest) an item's number written as a Pickup would write it, and an item put in / taken out of the persistent set as an Item Persist would
    void setItemForTest(int item, int n) { updateCountForItem(item, n); }
    void setPersistForTest(int item, bool on) { if (on) persistItems_.insert(item); else persistItems_.erase(item); }
    // +0x36c is written at the end of the object's set-up (RVA 0x29d9a1 in 2.209b): the object type (+0x3a0) is not 0 / 21 / 25, else the byte at +0x4c6 (a flag not exported; read as 0). Type 0 = solid, 21 / 25 = the other two types that do not turn.
    void setObjectType(int uid, int type) {
        const auto it = byUid.find(uid);
        if (it != byUid.end()) objs.mut(it->second).noOwnRot = (type == 0 || type == 21 || type == 25);
    }
    // a Random (1912) the loader found in the level string: its chance (key 10) and the order to run it (before finish())
    void markRandom(int uid, double chance) { for (TrigDef& t : trigs_.mut()) if (t.uid == uid && t.id == 1912) { t.randOk = true; t.randChance = chance; } }
    void addTrigger(const TrigDef& t) { trigs_.push_back(t); if (!t.remapTo.empty()) anyRemap_ = true; }
    // (the live path, before finish()) the touch Toggle Blocks (1594) the caller's own touch table keeps as boxes -- one that switches its group on when the button is down inside it, or off -- as the Events it hands over
    // fire them: uid -> 1 (on) / 0 (off). Only these are run (togon is what kindRuns asks for); a 1594 the caller does not keep as a box is never given an Event, so running it would leave its group where it started.
    // A value 2 = a box the caller keeps whose way the caller does not know (the activators / --boxoff (removed) tests left it out because its group holds a chain they do not follow): the trigger's own Activate Group column
    // decides, and the shadow runs the chain (toggleGroupTriggered switches the group on and spawns it, as GD's 1594 does).
    void setToggleBoxes(const std::map<int, int>& boxes) {
        for (TrigDef& t : trigs_.mut()) if (t.id == 1594 && t.touch) {
            const auto it = boxes.find(t.uid);
            if (it == boxes.end()) continue;
            if (it->second == 2) { if (t.spawnOnly) t.togon = 1; else if (t.actgrpCol >= 0) t.togon = t.actgrpCol; }   // (a spawn-only box runs: the handler spawns its group, no switch)
            else t.togon = it->second;
        }
    }

    // whether the shadow runs this trigger. A channelled trigger (chan != 0) is on another array, fired only while its channel is the active one (the 2900-series triggers switch it): not run here, so its groups are not vouched for
    static bool runs(const TrigDef& t) { return runsOld(t); }
    static bool runsOld(const TrigDef& t) {
        const bool plainMove = t.id == 901 && !t.touch && !t.spawn && !t.lockPlayer && (!t.lockCam || t.camLockOk) && t.center == 0 && t.chan == 0;
        const bool plainToggle = t.id == 1049 && !t.touch && !t.spawn && t.chan == 0;
        const bool timewarp = (t.id == 1935 || t.id == 1612 || t.id == 1613) && !t.touch && !t.spawn;   // 1612 / 1613: hide / show the player (no group)
        const bool rotate = t.id == 1346 && !t.touch && !t.spawn && !t.lockCam && !t.lockPlayer && t.chan == 0;
        const bool areaMove = (t.id == 3006 || t.id == 3008) && t.areaOk && !t.touch && !t.spawn && t.chan == 0;
        return plainMove || plainToggle || rotate || timewarp || areaMove;
    }
    // the kinds the shadow knows how to run, whether the trigger is crossed by the player (spawn = 0) or spawned by a Spawn trigger (spawn = 1); touch-triggered ones are never run
    static bool kindRuns(const TrigDef& t, bool touchFed = false, bool recordComplete = false, bool chanModel = false, bool evFed = false) {
        // the game's record (trigfire.csv) is made at EffectGameObject::triggerObject: a Spawn (1268) and a Stop (1616) have their own override and are never in it (0 of 571,155 rows), so a touch-triggered one has no event to be fired from
        // (a record made with the hooks of the overriding classes -- a `via` column -- holds them: recordComplete)
        if ((t.touch && (!touchFed || (!recordComplete && (t.id == 1268 || t.id == 1616)))) || (t.chan != 0 && !t.spawn && !chanModel)) return false;   // a channel only gates the crossing array: spawnObject 0x3227a0 has no channel test, and the record fires spawn-triggered channelled triggers (Stop 71702 of c145493833)
        switch (t.id) {
            case 901: return (!t.lockCam || t.camLockOk) && t.dirDist == 0.0 && (t.center == 0 || (t.mvtgt && !t.dynmode));   // a centre group on a Move is the Move-To's destination (moveToOf); the dynamic one follows the objects every step: not run
            case 1049: return true;
            case 1594: return t.touch && t.togon >= 0;   // a Pickup (collectible) trigger: on a touch of its own object it switches its target group on / off (togon = its Activate Group column)
            case 1346: return !t.lockCam && !t.lockPlayer;
            case 1935: case 1612: case 1613: case 2899: case 2900: case 2066: case 3022: case 1616: return true;
            case 3006: case 3008: return t.areaOk;
            case 3011: case 3012: case 3013: case 3014: case 3015: return t.editOk;
            case 3024: return t.areaStopOk;
            case 1815: return t.collOk;    // a Collision trigger: registered when it fires, checked every step (stepCollisions)
            case 3620: return t.itemCmp;   // trigger objects whose Item Compare reads a value the shadow does not have are left to the record (not vouched for)   // needs the Areas' effect ids (areafx.txt)
            case 1268: return (!t.remap || t.remapOk) && !t.sord && !t.silent;
            case 3607: return t.seqOk;
            case 3604: return evFed && t.evOk;
            case 1912: case 2068: return t.randOk;
            case 2067: return t.sclOk;
            case 1611: case 1811: return t.cntOk;
            case 3619: return t.edOk;
            case 1817: return t.pkOk;
            case 3641: return t.psOk;
        }
        return false;
    }
    // whether the shadow runs this trigger (valid after finish()): a crossed one of a known kind, or a spawn-triggered one whose every activator is itself run
    bool modelled(int uid) const { return modelled_.count(uid) > 0; }
    // --trigshadowtable (observation only): what finish() decided about one trigger
    bool kindRunsNow(const TrigDef& t) const { return kindRuns(t, touchFed_, recordComplete_, chanModel_, evFed_); }
    bool touchWasFed() const { return touchFed_; }
    bool modelledT(const TrigDef& t) const { return modelled_.count(t.uid) > 0; }
    // a touch-triggered trigger fires in the collision pass when the player's box meets its own; the shadow does not know the player's box, so it is told: the game's record of when each touch
    // trigger was called (trigfire.csv, cfg trigfire=1: the step is the game's tick, the shadow's step is one more). With it the touch-triggered triggers of a known kind are run.
    void setTouchEvents(const std::map<long long, std::vector<int>>& byStep) { touchAt_ = byStep; touchFed_ = true; }
    // the item counters (GJEffectManager +0x330): the pickups' collection is fed from outside (addItemEvent: the shadow's step, the item, the change), as the touch calls are
    void addItemEvent(long long step, int item, int delta) { itemAt_[step].push_back({item, delta}); }
    long long itemsFed = 0;   // the counter changes applied (a report)
    int itemCountOf(int item) const { const auto it = itemCount_.find(item < 0 ? 0 : (item > 9999 ? 9999 : item)); return it == itemCount_.end() ? 0 : it->second; }
    void addTouchEvent(long long step, int uid, bool late = false) { touchAt_[step].push_back(uid); touchFed_ = true; if (late) lateAt_.insert({step, uid}); }   // late: the call was made in the button phase (a Toggle Orb's push), after the collision pass   // one call at a time, for a caller that decides the touches while it steps (trig_replay --selftouch)
    // A touch call learned AFTER the step it belongs to has run (the live search sees a call in the state the pass produced, one layer later): the fire() that fireTouch(t) makes inside step t, made at once, with
    // the step's own tick and x still in curTick_ / curPx_. The command it starts then has its first (zero) step in the next step, as it has when the call is fed in time.
    void fireTouchLate(int uid) {
        if (!touchFed_) return;
        const auto ti = trigByUid_.find(uid);
        if (ti == trigByUid_.end()) return;
        const TrigDef x = trigs_[ti->second];
        if (!x.touch || !modelled_.count(x.uid)) return;
        const size_t had = pendLen_;
        fire(x);
        // The call belongs to step curTick_, whose delayed spawns have already counted down (updateSpawns runs after the crossed triggers fired, spawnPhase 2). A Spawn this call queued would have counted
        // that step's dt in the game (a touch trigger fires before the step's updateSpawnTriggers), so it takes it now: without it an ordered spawn's delay ran one step long (c127327304 Spawn 608: Move 610
        // started one step late, the touch-fired group 5 orders its Moves by x / 311.58 s).
        if (spawnPhase == 2 && pendLen_ > had) updateSpawns(had);
    }
    // the game events (GJGameEvent: a ring's OrbActivated / its type's event, a pad's PadActivated ..., dp/src/dp/game_event.hpp) are handed over, as the touch calls are: setEventsFed() before finish() makes the Event triggers
    // (3604) run, addGameEvent(step, event, material, player) files one under the shadow's step (a step the timeline has not run), raiseEvent() raises one at once, after the step it belongs to has run (the live search
    // learns a pass's events one layer later; the spawn it makes is the one fireTouchLate makes for a touch call). Without setEventsFed an Event trigger is not run: its groups are not vouched for.
    void setEventsFed() { evFed_ = true; }
    bool eventsFed() const { return evFed_; }
    void addGameEvent(long long step, int ev, int material = 0, int player = 0) { evAt_[step].push_back({ev, material, player}); }
    // gameEventTriggered 0x33ac80 [R]: the listeners of (event, material * 10000 + player), once a step for each key, each spawning its group; a player other than 0 then calls itself with the player 0
    void raiseEvent(int ev, int material = 0, int player = 0) {
        if (!evFed_) return;
        const int key = material * 10000 + player;
        if (evDone_.insert({ev, key}).second) {
            const size_t n = evListen_.size();   // (a listener a spawn registers waits for the next call)
            for (size_t i = 0; i < n; ++i) {
                if (evListen_[i].ev != ev || evListen_[i].key != key) continue;
                const EvListener L = evListen_[i];
                const int keepSp = curSpawnUid_; const RemapKeys keepR = curRemap_;
                curSpawnUid_ = L.trigUid; curRemap_ = L.remap;
                spawnGroup(L.target, false, 0.0);
                curRemap_ = keepR; curSpawnUid_ = keepSp;
                ++eventsRaised;
            }
        }
        if (player != 0) raiseEvent(ev, material, 0);
    }
    // an event learned AFTER the step it belongs to has run (the live search sees a pass's events in the next layer): raised at once, with the step's own tick and x still in curTick_ / curPx_; a Spawn it queued counts that step's dt now,
    // as fireTouchLate's does (the step's delayed spawns have counted down already: spawnPhase 2)
    void raiseEventLate(int ev, int material = 0, int player = 0) {
        const size_t had = pendLen_;
        raiseEvent(ev, material, player);
        if (spawnPhase == 2 && pendLen_ > had) updateSpawns(had);
    }
    void endEventStep() { evDone_.clear(); }   // a caller that raises the events of several steps one after another (TrigLive::feedEvents): each step's (event, key) stamps are its own
    long long eventsRaised = 0;   // the listener calls made (a report)
    void setMultiMap(const std::unordered_map<int, bool>& m) { multiMap_ = m; }
    void setAreaEffectIds(const std::unordered_map<int, int>& m) { areaFx_ = m; }
    // every Edit Area row of areaedit.txt (also the ones the shadow does not run): uid -> (target, useEffect); useEffect = `target` is an Area effect id (EnterEffectInstance), not a group
    void addAreaEditInfo(int uid, int target, bool useEffect) { areaEditInfo_.mut()[uid] = {target, useEffect}; }
    const std::pair<int, bool>* editInfoOf(const TrigDef& t) const { if (t.id < 3011 || t.id > 3015) return nullptr; const auto it = areaEditInfo_.find(t.uid); return it == areaEditInfo_.end() ? nullptr : &it->second; }
    void setAttempt(int a) { attemptNo_ = a; }   // the attempt number an Item Compare's mode 5 reads (layer +0x3164)
    void setCrossRef(const std::map<long long, double>& m) { crossRef_ = m; }   // step -> the player's x the crossing loop of that step started with (trigfire.csv: the px of its first row)   // multi.txt: uid -> m_isMultiTriggered
    bool recordComplete() const { return recordComplete_; }
    void setRecordComplete(bool c) { recordComplete_ = c; }   // the game's record holds the triggers whose class overrides triggerObject (Spawn, Stop, ...): trigfire.csv with a `via` column
    // the spawn-triggered triggers (spawn = 1) the shadow does not decide itself are fired from the game's record (trigfire.csv rows with spawnTriggered = 1): mode 1 = those whose activators it cannot run
    // (a Touch / Count / Collision / Random ... trigger, a Spawn with a remap, an activator the trigger dump omits), mode 2 = every spawn-triggered trigger (a check of the timing against the shadow's own spawn chain)
    void setSpawnEvents(const std::map<long long, std::vector<int>>& byStep, int mode) { spawnAt_ = byStep; spawnFedOn_ = mode > 0; spawnFedStops_ = true; spawnFedMode_ = mode; }   // mode 0: only the Stops whose activators the shadow cannot run are fired from the record
    // a Touch trigger (1595) turns its group on / off by the player's presses (trigtouch.hpp's TapDecider decides, from the button edges, and hands over the game's toggleGroupTriggered calls). The shadow takes them as given:
    // `step` is the shadow's step (the game's call at tick T is step T+1), applied at the head of that step. They are counted apart (groupToggleFed), not in `fired`; with them the Touch triggers count as run.
    // The call is the game's toggleGroupTriggered (GJEffectManager::playerButton 0x36c910 hands each Tap's group to the layer's delegate, vfunc 0 = GJBaseGameLayer::toggleGroupTriggered 0x32b7f0): the group
    // is switched and, switched on, spawned -- a group that holds spawn-triggered triggers starts its chain at every press, whether it was already on or not. `uid`: the Tap's (the spawn's own name).
    void addGroupToggleEvent(long long step, int group, bool on, int uid = 0) { gtogAt_[step].push_back({group, on, uid}); gtogFed_ = true; }
    // the live search hands the Tap calls over one layer late (its timeline has run the step the press belongs to): set before finish(), so the Touch triggers count as run (modelled_) and the spawn-triggered triggers
    // of a Tap's group are vouched for; a Tap's press is then applied by toggleGroupLate after the step it belongs to has run (TrigLive::feedEvents)
    // `taps`: the Taps whose calls are handed over (the ones whose group holds a trigger); the others stay unrun (their group is State::tog's: shadowApplyTapGates). Empty = every Tap (addGroupToggleEvent's own callers).
    void setGroupToggleFed(const std::set<int>& taps = {}) { gtogFed_ = true; gtogTaps_ = taps; }
    bool groupTogglesFed() const { return gtogFed_; }
    // A Tap call learned after the pass that raised it (the press of the tick the pass is for): the game makes it at the head of the NEXT shadow step (the pass's K.t + 1: GJEffectManager::playerButton runs in the
    // button phase of the tick, its toggleGroupTriggered before the next step's updateSpawns / commands), which is where the timeline is between two steps, so it is made now, with nothing to catch up on.
    void toggleGroupLate(int group, bool on, int uid) {
        if (!gtogFed_) return;
        toggleGroupTriggered(group, on, uid);
        ++groupToggleFed;
    }
    void addArea(const AreaDef& a) { areaDefs_[a.uid] = a; areaOrig_.mut()[a.uid] = a; }
    void addAreaEdit(const AreaEdit& e) { areaEdits_[e.uid] = e; }
    // an Area trigger (Move whose shape is not run, Rotate, Scale) that works on parents (index >= 1): its objects and the children of its parents move without the shadow, so their groups are not vouched for
    void addUncoveredArea(int uid, int index) { uncoveredAreas_.mut().push_back({uid, index}); }
    // the layer's target-group arrays (areatargets.txt): `A i uids` and `D i parent:kids`
    void setAreaFlag(int uid, char which) {   // X / K / P rows of areatargets.txt
        const auto bi = byUid.find(uid);
        if (bi == byUid.end()) return;
        if (which == 'X') objs.mut(bi->second).areaXLock = true;
        else if (which == 'K') objs.mut(bi->second).areaKidSkip = true;
        else if (which == 'P') objs.mut(bi->second).areaAlways = true;
    }
    void addAreaArray(int i, std::vector<int> uids) { areaArrays_.mut()[i] = std::move(uids); }
    void addAreaDict(int i, std::unordered_map<int, std::vector<int>> d) { areaDicts_.mut()[i] = std::move(d); }
    bool areaTablesFor(int index) const { return index < 1 || (areaArrays_.count(index - 1) && areaArrays_.count(index) && areaDicts_.count(index + 1)); }
    static bool ignorableTrigger(int id) { return ignorable(id); }
    // --trigshadowopts settings / trig_replay --settingsinert: the Settings object 3662 moves nothing (22091: EffectGameObject::triggerObject 0x647bf0 has no case 0xe4e -- its switch falls to `default: return`; PlayLayer::addObject 0x50c690 gives
    // 0xe4e, like the UI object 0xe1d, section -1, so it is never in the crossing arrays; the exe has no other reader of 0xe4e than the editor and SetTargetIDLayer), so a group it names is not made uncovered by it
    bool settingsInert = false;
    bool ign(int id) const { return ignorable(id) || (settingsInert && id == 3662); }
    // T30: an Area Move whose areas.txt row displaces nothing (moveDist, moveX, moveY and their variations all 0) moves no object whatever its length / offset / variation says; the loader names it here (addNoopTrigger) and it
    // leaves the coverage like a Pulse does
    std::unordered_set<int> noopUids_;
    void addNoopTrigger(int uid) { noopUids_.insert(uid); }
    bool ignT(const TrigDef& t) const { return ign(t.id) || (!noopUids_.empty() && noopUids_.count(t.uid) > 0); }

    static bool namesGroup(const TrigDef& y, int g) { for (const auto& e : y.seqTargets) if (e.first == g) return true; return false; }   // a Sequence names the groups of its list
    // the triggers the shadow runs, and the groups it cannot vouch for (anything else a trigger acts on)
    void finish() {
        if (!multiMap_.empty()) for (TrigDef& x : trigs_) { const auto mi = multiMap_.find(x.uid); if (mi != multiMap_.end()) x.multi = mi->second; }
        for (auto& kv : members)
            std::stable_sort(kv.second.begin(), kv.second.end(), [&](size_t a, size_t b) { return objs[a].rank < objs[b].rank; });
        // what the shadow runs: the crossed triggers of a known kind, then the spawn-triggered ones whose every activator (a Spawn / Count / Collision ... naming a group the trigger is in) is itself run
        for (size_t i = 0; i < trigs_.size(); ++i) trigByUid_.mut()[trigs_[i].uid] = i;
        if (spawnModel) {
            for (const TrigDef& t : trigs_) if (kindRuns(t, touchFed_, recordComplete_, chanModel_, evFed_)) modelled_.insert(t.uid);
        } else {
            for (const TrigDef& t : trigs_) if (kindRuns(t) && !t.spawn && t.id != 1268) modelled_.insert(t.uid);
        }
        if (gtogFed_) for (const TrigDef& t : trigs_) if (t.id == 1595 && (gtogTaps_.empty() || gtogTaps_.count(t.uid))) modelled_.insert(t.uid);   // its on / off calls are handed over (addGroupToggleEvent)
        for (const TrigDef& t : trigs_)   // an ordered Spawn is run only when the group it spawns keeps the level's order (no order value, no channel on the triggers in it)
            if (t.id == 1268 && t.sordd && modelled_.count(t.uid)) {
                const auto m = members.find(t.target);
                if (m != members.end())
                    for (size_t i : m->second) {
                        const auto ti = trigByUid_.find(objs[i].uid);
                        if (ti != trigByUid_.end() && (trigs_[ti->second].ord != 0 || trigs_[ti->second].chan != 0)) { modelled_.erase(t.uid); break; }
                    }
            }
        for (const TrigDef& t : trigs_)   // a Stop by control ID is not run; a pause / resume of a group that holds an Area trigger is not run either (an Area instance's pause is not ported)
            if (t.id == 1616 && modelled_.count(t.uid)) {
                bool bad = t.stopCtrl;
                if (!bad && t.stopMode != 0) {
                    const auto m = members.find(t.target);
                    if (m != members.end())
                        for (size_t i : m->second) {
                            const auto ti = trigByUid_.find(objs[i].uid);
                            if (ti != trigByUid_.end() && trigs_[ti->second].id >= 3006 && trigs_[ti->second].id <= 3010) { bad = true; break; }
                        }
                }
                if (bad) modelled_.erase(t.uid);
            }
        if (unknownActivators)   // an activator (Advanced Random, Random, Sequence ...) the trigger dump does not list may spawn any group: no spawn-triggered trigger is vouched for
            for (const TrigDef& x : trigs_) if (x.spawn) modelled_.erase(x.uid);
        static const std::set<int> kActivators = {3620, 1268, 1611, 1811, 1815, 1817, 1912, 2068, 3604, 3607, 3608, 3609, 3614, 3615, 1595, 1594, 1598, 1614, 3640, 3643};
        for (bool changed = true; changed;) {
            changed = false;
            for (const TrigDef& x : trigs_) {
                if (!x.spawn || !modelled_.count(x.uid)) continue;
                const auto bi = byUid.find(x.uid);
                bool ok = bi != byUid.end();   // a trigger in no group (the shadow only keeps grouped objects) is never spawned by anything: nothing can fire it, so nothing is waited for
                if (!ok) continue;
                if (ok)
                    for (const TrigDef& y : trigs_) {
                        if (!kActivators.count(y.id) || modelled_.count(y.uid)) continue;
                        for (int g : objs[bi->second].groups) if (g == y.target || ((y.id == 3620 || y.id == 3609 || y.id == 1912) && g == y.center) || ((y.id == 3607 || y.id == 2068) && namesGroup(y, g))) { ok = false; spawnBlockers_.mut()[x.uid].insert(y.id); break; }
                        if (!ok) break;
                    }
                if (!ok) { modelled_.erase(x.uid); changed = true; }
            }
        }
        if (spawnFedOn_) {   // why each spawn-triggered trigger the shadow cannot run is waited for in vain (before it is handed to the record): the activators it cannot run, a report
            auto actName = [](const TrigDef& y) -> std::string {
                switch (y.id) {
                    case 1595: return "Touch"; case 1611: return "Count"; case 1815: return "Collision"; case 1912: return "Random"; case 2068: return "AdvRandom";
                    case 3607: case 3608: case 3609: return "AdvRandom/Sequence"; case 3604: return "Event"; case 1268: return y.remap ? "remap" : "Spawn(other)";
                    default: return "id" + std::to_string(y.id);
                }
            };
            for (const TrigDef& x : trigs_) {
                if (!x.spawn || modelled_.count(x.uid)) continue;
                std::set<std::string>& r = fedReason_.mut()[x.uid];
                if (unknownActivators) r.insert("unknown");
                const auto bi = byUid.find(x.uid);
                if (bi != byUid.end())
                    for (const TrigDef& y : trigs_) {
                        if (!kActivators.count(y.id) || modelled_.count(y.uid)) continue;
                        for (int g : objs[bi->second].groups) if (g == y.target || ((y.id == 3607 || y.id == 2068) && namesGroup(y, g))) { r.insert(actName(y)); break; }
                    }
                if (r.empty()) r.insert("other");
            }
        }
        if (spawnFedOn_)   // the triggers the shadow cannot wait for are fired from the record; a Spawn whose own start the shadow does not run only starts triggers that are fired from the record too
            for (const TrigDef& t : trigs_) {
                if (t.chan != 0 && !chanModel_) continue;   // (a spawn-triggered trigger of any channel runs: spawnObject has no channel test, and a Rotate Gameplay among them is what switches the channel back)
                if (t.spawn) {
                    // (a Stop is not in the record: a spawn-triggered one is run only when its activators are run)
                    if ((t.id == 1268 || (kindRuns(t, true, recordComplete_, chanModel_, evFed_) && (recordComplete_ || t.id != 1616))) && (spawnFedMode_ == 2 || !modelled_.count(t.uid) || (t.id == 901 && t.mvtgt))) { fedSpawn_.insert(t.uid); modelled_.insert(t.uid); }   // (a Move-To reads where the player is when it fires: the step of the record, not the shadow's own chain's)
                } else if (t.id == 1268 && !modelled_.count(t.uid)) modelled_.insert(t.uid);
            }
        if (!spawnFedOn_ && spawnFedStops_ && recordComplete_)   // (a Stop is not in the shadow's own chain unless its activators run: the record has the call)
            for (const TrigDef& t : trigs_)
                if (t.id == 1616 && t.spawn && !modelled_.count(t.uid) && kindRuns(t, true, recordComplete_, chanModel_, evFed_) && !(t.chan != 0 && !chanModel_)) { fedSpawn_.insert(t.uid); modelled_.insert(t.uid); }
        // the groups whose objects depend on the game's record of touch fires: the targets of the touch-triggered triggers that are run, and what a touch-triggered Spawn goes on to spawn
        {   // ...each with the reasons it depends on the record: "touch", or the activators it could not wait for (fedReason_), carried down a Spawn chain to the triggers it starts
            std::map<int, std::set<std::string>> R;
            std::vector<size_t> work;
            for (size_t i = 0; i < trigs_.size(); ++i) {
                const TrigDef& y = trigs_[i];
                if (!modelled_.count(y.uid)) continue;
                if (y.touch || (gtogFed_ && y.id == 1595)) R[y.uid].insert("touch");
                if (fedSpawn_.count(y.uid)) { const auto fr = fedReason_.find(y.uid); if (fr != fedReason_.end()) R[y.uid].insert(fr->second.begin(), fr->second.end()); else R[y.uid].insert("self-check"); }
                if (R.count(y.uid)) work.push_back(i);
            }
            while (!work.empty()) {
                const TrigDef y = trigs_[work.back()];
                work.pop_back();
                const std::set<std::string> ry = R[y.uid];
                for (const std::string& s : ry) { fedGroupReasons_.mut()[y.target].insert(s); if (y.center) fedGroupReasons_.mut()[y.center].insert(s); }
                touchFedGroups_.insert(y.target);
                if (y.center) touchFedGroups_.insert(y.center);
                if (y.id != 1268) continue;
                const auto m = members.find(y.target);
                if (m == members.end()) continue;
                for (size_t oi : m->second) {
                    const auto ti = trigByUid_.find(objs[oi].uid);
                    if (ti == trigByUid_.end() || !trigs_[ti->second].spawn || !modelled_.count(trigs_[ti->second].uid)) continue;
                    std::set<std::string>& rz = R[trigs_[ti->second].uid];
                    const size_t before = rz.size();
                    rz.insert(ry.begin(), ry.end());
                    if (rz.size() != before) work.push_back(ti->second);
                }
            }
        }
        for (const TrigDef& t : trigs_) if (!ign(t.id) && modelled_.count(t.uid) && (t.id == 901 || t.id == 1346 || t.id == 1049 || t.id == 1594 || t.id == 2067 || t.id == 1611 || t.id == 1811 || t.id == 3006 || t.id == 3008 || (gtogFed_ && t.id == 1595))) { if (t.target) modelledTargets_.insert(t.target); if (t.center) modelledTargets_.insert(t.center); }
        std::set<int> other;
        for (const TrigDef& t : trigs_) {
            if (ignT(t)) continue;
            // the groups the shadow could not vouch for before Area Moves were run (coveredObjectBase): every trigger it runs except an Area Move
            if (!(runsOld(t) && t.id != 3006)) { otherBase_.insert(t.target); if (t.center) otherBase_.insert(t.center); }
            if (modelledT(t)) continue;
            if (const auto* ei = editInfoOf(t); ei && ei->second) {   // an Edit Area that finds its Area by EFFECT id: `target` is an effect id, not a group (not the objects of the group of that number); what it can change is the Areas with that id
                for (const auto& kv : areaDefs_) { const auto fx = areaFx_.find(kv.first); if (fx != areaFx_.end() && (fx->second == ei->first || ei->first <= 0)) other.insert(kv.second.target); }
                continue;
            }
            // an Area Stop's `target` is an EFFECT id as well, never a group. [R] 22091 EffectGameObject::triggerObject 0x647bf0 case 0xbd0 compares it (+0x5c8) with the effect id (+0x7d8) of the trigger of each
            // Area instance in the layer's five arrays (+0x658 .. +0x6b8) and erases the ones that match: it reads no group and no place. One that is not run (a dump without the effect ids) leaves the Areas it
            // would end running in the shadow, so what is not vouched for is those Areas' targets -- every Area's when its effect id is not had -- and not the objects of the group of that number
            // ([M] a custom level read without the ext directory: Area Stop 165261 with effect id 1 kept 214 objects of group 1 off the supply).
            if (t.id == 3024) {
                for (const auto& kv : areaDefs_) { const auto fx = areaFx_.find(kv.first); if (fx == areaFx_.end() || fx->second == t.target) other.insert(kv.second.target); }
                continue;
            }
            other.insert(t.target);
            // a Follow (1347) only READS its leader group (key 71, `center`): the objects of that group are not what the trigger moves. a custom level group 108 (603 objects, a Transform about the main object 23385 the
            // shadow runs) was "not covered" because a Follow of group 681 names it as its leader, so its Transform was not supplied and the blocks it scales stood still: window 5000 left the game at 5274.
            if (t.center && t.id != 1347) other.insert(t.center);
        }
        // a Spawn with a group remap hands the triggers it spawns other groups than the ones the table names: a trigger that is not run, in what such a Spawn spawns, acts on the NEW group of the key that names its target
        // or centre (the table only holds the old one, so the new group would pass for covered). The keys of the Spawns met on the way (a nested Spawn) count as well: a superset, never less.
        for (const TrigDef& sp : trigs_) {
            if (sp.id != 1268 || sp.remapTo.empty()) continue;
            RemapKeys keys = sp.remapTo;
            std::set<int> seenG;
            std::vector<int> work{sp.target};
            std::vector<const TrigDef*> unrun;
            while (!work.empty()) {
                const int g = work.back();
                work.pop_back();
                if (!seenG.insert(g).second) continue;
                const auto m = members.find(g);
                if (m == members.end()) continue;
                for (size_t oi : m->second) {
                    const auto ti = trigByUid_.find(objs[oi].uid);
                    if (ti == trigByUid_.end()) continue;
                    const TrigDef& y = trigs_[ti->second];
                    if (y.id == 1268) { work.push_back(y.target); keys.insert(keys.end(), y.remapTo.begin(), y.remapTo.end()); }
                    if (!ignT(y) && !modelledT(y)) unrun.push_back(&y);
                }
            }
            for (const TrigDef* y : unrun)
                for (const auto& kv : keys) if ((y->target == kv.first && y->target) || (y->center == kv.first && y->center)) other.insert(kv.second);
        }
        // a trigger that is not run and acts on the group an Area Move trigger sits in (an Edit Area, a Pause / Resume, a Stop that is not plain ...) changes what that Area does to its objects:
        // those objects are not vouched for either. A plain Stop is run (fire() ends the Area); a Toggle of the group is read from the Area trigger's own counter.
        for (const TrigDef& t : trigs_) {
            if (ignT(t) || modelledT(t) || t.id == 1049 || t.id == 3024) continue;   // (an Area Stop names an effect id: handled above)
            if (const auto* ei = editInfoOf(t); ei && ei->second) continue;   // by effect id: handled above
            if (t.id == 1616 && !t.touch && !t.spawn && (t.chan == 0 || chanModel_)) continue;
            for (const auto& kv : areaDefs_) {
                const auto bi = byUid.find(kv.first);
                if (bi == byUid.end()) continue;
                for (int g : objs[bi->second].groups) if (g == t.target) { other.insert(kv.second.target); break; }
            }
        }
        if (uncoveredAreasOn) {   // the objects a non-run Area works on through the layer's target-group arrays (areatargets.txt): [index - 1] the objects without a parent, [index] the parents, [index + 1] the children
            // the objects themselves (not their groups): the Area moves these, and only these; an object that merely shares a group with one is not touched by it
            int curUa = 0;
            auto addUid = [&](int uid) { uncoveredObjs_.insert(uid); uncoveredObjBy_.emplace(uid, curUa); };
            for (const auto& ua : uncoveredAreas_) {
                if (ua.second < 1) continue;
                curUa = ua.first;
                for (int ai : {ua.second - 1, ua.second}) { const auto ar = areaArrays_.find(ai); if (ar != areaArrays_.end()) for (int u : ar->second) addUid(u); }
                const auto dk = areaDicts_.find(ua.second + 1);
                if (dk != areaDicts_.end()) for (const auto& kv : dk->second) for (int kid : kv.second) addUid(kid);
            }
        }
        other_ = other;
        for (int g : other) uncoveredGroups_.insert(g);
        // the array: unchannelled triggers that are not touch / spawn, (int(x), uid)
        for (size_t i = 0; i < trigs_.size(); ++i)
            if (!trigs_[i].touch && !trigs_[i].spawn && (trigs_[i].chan == 0 || chanModel_)) chanQ_.mut()[trigs_[i].chan].push_back(i);
        // LevelTools::sortChannelOrderObjects: each channel's direction is the one of the FIRST (uid order) 2900 that switches to it; the bucket is sorted by ord, then the axis coordinate cut to an int, then uid
        std::map<int, std::pair<int, int>> chanDir;   // channel -> (uid, direction) of the first switcher
        for (const TrigDef& y : trigs_)
            if (y.id == 2900 && y.rgSwarm && y.rgSwch >= 0) {
                const auto di = chanDir.find(y.rgSwch);
                if (di == chanDir.end() || y.uid < di->second.first) chanDir[y.rgSwch] = std::make_pair(y.uid, y.rgDir);
            }
        for (auto& kv : chanQ_) {
            const auto di = chanDir.find(kv.first);
            const int d = di != chanDir.end() ? di->second.second : 4;
            auto axis = [d](const TrigDef& y) { return d == 1 ? y.cy : d == 2 ? -y.cy : d == 3 ? -y.cx : y.cx; };
            std::sort(kv.second.begin(), kv.second.end(), [&](size_t a, size_t b) {
                const TrigDef &x = trigs_[a], &z = trigs_[b];
                if (x.ord != z.ord) return x.ord < z.ord;
                const int ka = (int)((float)x.ord + (float)axis(x)), kb = (int)((float)z.ord + (float)axis(z));
                return ka != kb ? ka < kb : x.uid < z.uid;
            });
        }
    }
    const std::vector<TrigDef>& triggers() const { return trigs_; }
    // a channel's crossing triggers in the order checkSpawnObjects reads them (trigshadow_live.hpp crossBuild: the x-crossing split of a layer's groups)
    const std::vector<size_t>* chanQueue(int chan) const { const auto qi = chanQ_.find(chan); return qi == chanQ_.end() ? nullptr : &qi->second; }
    size_t chanCursor(int chan) const { const auto ci = chanIdx_.find(chan); return ci == chanIdx_.end() ? 0 : ci->second; }   // how many of them this timeline has passed
    int activeChan() const { return activeChan_; }
    bool travelsVertical() const { return vertical_; }
    // --trigsupply-group: does a trigger that reads the CAMERA act on an object of group g? An Area Move whose centre is a point of the screen (special <= -3) or a Move locked to the camera, by the
    // group it targets (the objects of g that are in that group); `via` = that target group. The camera is an input the loop does not have in every tick (Cam::ok), so such a group is not supplied (AUD-871).
    // --trigsupply-group: does a Move locked to the PLAYER (m_lockToPlayerX / Y, lockx / locky) act on an object of group g? Such a Move integrates the player's displacement step by step, so its value
    // is a function of the whole x curve of the path, not of the world: two states of one group with different curves have different positions (the owner is fixed at load, so the group stays on the old path).
    bool groupReadsPlayerLock(int g, int* via = nullptr) const {
        std::set<int> lockTargets;
        for (const TrigDef& t : trigs_) if (t.lockPlayer || t.lockPX || t.lockPY) lockTargets.insert(t.target);
        if (lockTargets.empty()) return false;
        const auto mi = members.find(g);
        if (mi == members.end()) return false;
        for (size_t i : mi->second) for (int gg : objs[i].groups) if (lockTargets.count(gg)) { if (via) *via = gg; return true; }
        return false;
    }
    // --trigsupply-all (gd-04, a custom level:29000 g4001): a Move locked to the player's Y (lockPY) acts on g's members. The timeline has the player's x curve only, so the y lock's integral is off (the members came out 30 px low).
    bool groupReadsPlayerLockY(int g, int* via = nullptr) const {
        std::set<int> lockTargets;
        for (const TrigDef& t : trigs_) if (t.lockPY) lockTargets.insert(t.target);
        if (lockTargets.empty()) return false;
        const auto mi = members.find(g);
        if (mi == members.end()) return false;
        for (size_t i : mi->second) for (int gg : objs[i].groups) if (lockTargets.count(gg)) { if (via) *via = gg; return true; }
        return false;
    }
    bool groupReadsCamera(int g, int* via = nullptr) const {
        std::set<int> camTargets;
        for (const auto& kv : areaDefs_) if (kv.second.special <= -3) camTargets.insert(kv.second.target);
        for (const TrigDef& t : trigs_) if (t.lockCam) camTargets.insert(t.target);
        if (camTargets.empty()) return false;
        const auto mi = members.find(g);
        if (mi == members.end()) return false;
        for (size_t i : mi->second) for (int gg : objs[i].groups) if (camTargets.count(gg)) { if (via) *via = gg; return true; }
        return false;
    }
    // --trigsupplycam: what kind of camera reader acts on group g. 0 none; 1 a reader the supply cannot take over (a screen Area along y, or a camera-locked Move the loader did not run: no camLock option);
    // 2 only Moves locked to the camera (camLockOk), whose group moves by the displacement of the camera centre of the step before -- a step whose camera is not known on a locked axis blinds the target.
    int groupCameraReader(int g, int* via = nullptr) const {
        const auto mi = members.find(g);
        if (mi == members.end()) return 0;
        std::set<int> hard, soft;
        // a screen Area Move that works along x (dirType 1: the point it measures from is the camera's left edge plus a share of its width, so the camera X is all it reads) is a soft reader like a camera X lock:
        // while the camera model is certain its group moves as the game's does, and a step with an uncertain camera blinds its target (stepAreas). One along y (dirType 2) reads the camera bottom, which the model has not.
        for (const auto& kv : areaDefs_) if (kv.second.special <= -3) ((kv.second.dirType == 1 && kv.second.special >= -11) ? soft : hard).insert(kv.second.target);
        for (const TrigDef& t : trigs_) if (t.lockCam) (t.camLockOk ? soft : hard).insert(t.target);   // (a lock to the camera Y is soft too: a step whose camera Y is not known blinds its target, as an uncertain X does)
        int r = 0;
        for (size_t i : mi->second) for (int gg : objs[i].groups) {
            if (hard.count(gg)) { if (via) *via = gg; return 1; }
            if (soft.count(gg)) { if (!r && via) *via = gg; r = 2; }
        }
        return r;
    }
    // --trigsupply-group (gd-04, a custom level:15000 g593 and :18600 g535): a TOUCH trigger acts on group g and the trigger's own object sits in a group that a (non-touch) Move targets, so its touch box is moved at run time.
    // The model's latch reads the box where the level string put it, the game calls the box where the Move left it, the landing touch never reaches the shadow and its platform does not sink. Such a group is not supplied.
    // `touchUid` = the touch trigger, `moveUid` = the Move that moves it (the first found).
    bool groupTouchBoxMoved(int g, int* touchUid = nullptr, int* moveUid = nullptr) const {
        const auto mi = members.find(g);
        if (mi == members.end()) return false;
        std::map<int, int> moveOf;   // target group -> a non-touch Move that targets it
        for (const TrigDef& t : trigs_) if (t.id == 901 && !t.touch) moveOf.emplace(t.target, t.uid);
        if (moveOf.empty()) return false;
        std::set<int> tgs;
        for (size_t i : mi->second) tgs.insert(objs[i].groups.begin(), objs[i].groups.end());
        for (const TrigDef& t : trigs_) {
            if (!t.touch || ignT(t) || !tgs.count(t.target)) continue;   // (a Pulse / Alpha box acts on colours, not on a place: its box moving with the group changes no geometry)
            const auto bi = byUid.find(t.uid);
            if (bi == byUid.end()) continue;
            for (int h : objs[bi->second].groups) {
                const auto mv = moveOf.find(h);
                if (mv != moveOf.end()) { if (touchUid) *touchUid = t.uid; if (moveUid) *moveUid = mv->second; return true; }
            }
        }
        return false;
    }
    // --trigsupply-group (gd-04, a custom level:46200 g625): a touch Move / Rotate / Toggle that targets a group any member of g belongs to has its box BEHIND the anchor's player x (cx + 15 < x0; a trigger is a 30 px box).
    // The touch fired before the anchor, the shadow's history has no Event for it (the Events come from the model's own passes), so its timeline would not have the move the game made. Such a group is not supplied
    // (unless the anchor's seed gives the shadow that Event, which is not checked here). `touchUid` = that trigger.
    bool groupTouchBehind(int g, double x0, int* touchUid = nullptr) const {
        const auto mi = members.find(g);
        if (mi == members.end()) return false;
        std::set<int> tgs;
        for (size_t i : mi->second) tgs.insert(objs[i].groups.begin(), objs[i].groups.end());
        for (const TrigDef& t : trigs_) {
            if (!t.touch || (t.id != 901 && t.id != 1346 && t.id != 1049) || !tgs.count(t.target)) continue;
            if (t.cx + 15.0 < x0) { if (touchUid) *touchUid = t.uid; return true; }
        }
        return false;
    }
    // --trigsupply-all (gd-04, a custom level:22200 g159): a touch trigger BEHIND the anchor (a Toggle Orb 1594, a touch Toggle / Move ...) acts on a group that a Move / Rotate of g's members sits in -- it gates that Move, not the
    // members. The touch fired (or not) before the anchor and the shadow has no Event for it, so it runs the Move the game did not (g159: Move 43427 in group 827, gated by the Toggle Orb 43426: the members came out 300 px high).
    // `gateUid` = the touch trigger, `moveUid` = the gated mover trigger.
    bool groupGateTouchBehind(int g, double x0, int* gateUid = nullptr, int* moveUid = nullptr) const {
        const auto mi = members.find(g);
        if (mi == members.end()) return false;
        std::set<int> tgs;
        for (size_t i : mi->second) tgs.insert(objs[i].groups.begin(), objs[i].groups.end());
        for (const TrigDef& m : trigs_) {
            if (!tgs.count(m.target) || (m.id != 901 && m.id != 1346 && m.id != 2067 && m.id != 3006 && m.id != 3008)) continue;
            const auto bi = byUid.find(m.uid);
            if (bi == byUid.end()) continue;
            for (const TrigDef& t : trigs_) {
                if (!t.touch || t.uid == m.uid || t.cx + 15.0 >= x0) continue;
                for (int h : objs[bi->second].groups) if (h == t.target) { if (gateUid) *gateUid = t.uid; if (moveUid) *moveUid = m.uid; return true; }
            }
        }
        return false;
    }
    // every touch trigger behind the anchor (cx + 15 < x0) that groupTouchBehind / groupGateTouchBehind would name for group g, not just the first: the group can only be supplied when the anchor's history decides ALL of them
    // (touchBehindDecision in trigshadow_live.hpp). The same two shapes: a touch Move / Rotate / Toggle that targets a group a member is in, and a touch trigger whose target is a group the object of a mover
    // trigger on a member's group is in (the touch gates that mover).
    std::vector<int> touchBehindUids(int g, double x0) const {
        std::vector<int> r;
        const auto mi = members.find(g);
        if (mi == members.end()) return r;
        std::set<int> tgs;
        for (size_t i : mi->second) tgs.insert(objs[i].groups.begin(), objs[i].groups.end());
        const auto add = [&](int u) { if (std::find(r.begin(), r.end(), u) == r.end()) r.push_back(u); };
        for (const TrigDef& t : trigs_)
            if (t.touch && (t.id == 901 || t.id == 1346 || t.id == 1049) && tgs.count(t.target) && t.cx + 15.0 < x0) add(t.uid);
        for (const TrigDef& m : trigs_) {
            if (!tgs.count(m.target) || (m.id != 901 && m.id != 1346 && m.id != 2067 && m.id != 3006 && m.id != 3008)) continue;
            const auto bi = byUid.find(m.uid);
            if (bi == byUid.end()) continue;
            for (const TrigDef& t : trigs_) {
                if (!t.touch || t.uid == m.uid || t.cx + 15.0 >= x0) continue;
                for (int h : objs[bi->second].groups) if (h == t.target) { add(t.uid); break; }
            }
        }
        return r;
    }
    // the box of touch trigger `uid` rides a group that a Move / Rotate / Scale / Area trigger acts on (as a target or a centre): the rows' overlap test, which reads the box where the level string put it, then misses
    // the touches the game made (V2, 64 customs: 65 of the 72 touches GD fired that the rows never enter are such boxes; groupTouchBoxMoved sees the plain Moves only)
    bool touchBoxRidesMover(int uid) const {
        const auto bi = byUid.find(uid);
        if (bi == byUid.end()) return false;
        std::set<int> acted;
        for (const TrigDef& t : trigs_) if (!t.touch && (t.id == 901 || t.id == 1346 || t.id == 2067 || t.id == 3006 || t.id == 3008 || t.id == 3007)) { acted.insert(t.target); if (t.center) acted.insert(t.center); }
        for (const auto& kv : areaDefs_) { acted.insert(kv.second.target); }
        for (int h : objs[bi->second].groups) if (acted.count(h)) return true;
        return false;
    }
    const TrigDef* triggerByUid(int uid) const { const auto it = trigByUid_.find(uid); return it == trigByUid_.end() ? nullptr : &trigs_[it->second]; }
    // set when a Move locked to the camera stepped with a camera that was not certain (Cam::ok false for the step before or the one before that): from then on this timeline's camera-locked groups carry an
    // error the camera model cannot bound, so the supply keeps the model's placement for them (sticky: the displacement is integrated)
    bool camLockBlind = false;
    // a report: for a spawn-triggered trigger the shadow does not run, the ids of the activators (not run themselves) that name a group it is in; -1 = the level has activators the trigger dump omits
    std::set<int> blockersOf(int uid) const {
        std::set<int> r;
        static const std::set<int> kAct = {1268, 1611, 1811, 1815, 1817, 1912, 2068, 3604, 3607, 3608, 3609, 3614, 3615, 1595, 1594, 1598, 1614, 3640, 3643};
        const auto bi = byUid.find(uid);
        if (bi == byUid.end()) return r;
        if (unknownActivators) r.insert(-1);
        for (const TrigDef& y : trigs_) {
            if (!kAct.count(y.id) || modelled_.count(y.uid)) continue;
            for (int g : objs[bi->second].groups) if (g == y.target || ((y.id == 3620 || y.id == 3609 || y.id == 1912) && g == y.center) || ((y.id == 3607 || y.id == 2068) && namesGroup(y, g))) { r.insert(y.id); break; }   // the same rule as finish()'s blocker loop
        }
        return r;
    }
    // is a Move locked to the player (lockx / locky) still running in this timeline (--trigshadowlive's x-curve census, trigshadow_live.hpp members())
    bool lockActive() const { for (const Cmd& c : cmds_) if ((c.lockPX || c.lockPY) && !c.done) return true; return false; }
    size_t activeCommands() const { return cmds_.size(); }
    size_t queued() const { size_t n = 0; for (const auto& kv : chanQ_) n += kv.second.size(); return n; }
    size_t queueIndex() const { size_t n = 0; for (const auto& kv : chanIdx_) n += kv.second; return n; }
    // the 2.2 trigger channels: with the Rotate Gameplay triggers (2900, rotgameplay.txt) loaded, the crossing triggers of every channel are run -- each channel has its own array and cursor, and only
    // the active one is walked (GJBaseGameLayer::checkSpawnObjects 0x322080 reads layer +0x33c; GJBaseGameLayer::rotateGameplay 0x31f980 is the only writer: a 2900 with m_changeChannel set switches to m_targetChannelID)
    void setChannelModel(bool b) { chanModel_ = b; }
    bool channelModel() const { return chanModel_; }
    // an object is covered when none of its groups is acted on by a trigger the shadow does not run
    // covered as the shadow was before it ran Area Moves (an Area Move's groups counted as unvouched for)
    bool coveredObjectBase(size_t i) const {
        for (int g : objs[i].groups) if (otherBase_.count(g)) return false;
        return true;
    }
    // covered, but only because the game said when a touch-triggered trigger fired (the shadow does not decide that itself)
    bool isFedSpawn(int uid) const { return fedSpawn_.count(uid) > 0; }
    // an object in a group that a trigger the shadow runs works on (a Move / Rotate / Toggle / Area ... target or centre): what the shadow can actually move, as against the objects no trigger touches
    bool actedOnByModelled(size_t i) const {
        for (int g : objs[i].groups) if (modelledTargets_.count(g)) return true;
        return false;
    }
    std::set<std::string> fedReasonsOfObject(size_t i) const {
        std::set<std::string> r;
        for (int g : objs[i].groups) { const auto it = fedGroupReasons_.find(g); if (it != fedGroupReasons_.end()) r.insert(it->second.begin(), it->second.end()); }
        return r;
    }
    bool touchFedObject(size_t i) const {
        for (int g : objs[i].groups) if (touchFedGroups_.count(g)) return true;
        return false;
    }
    int triggerIndex(int uid) const { const auto it = trigByUid_.find(uid); return it == trigByUid_.end() ? -1 : (int)it->second; }
    int uncoveredBy(int uid) const { const auto it = uncoveredObjBy_.find(uid); return it == uncoveredObjBy_.end() ? 0 : it->second; }   // a report: the Area trigger that keeps the object from being vouched for
    // why coveredObject() is false (a report): the Area trigger whose target arrays hold the object, or the groups of it that a trigger the shadow does not run acts on
    std::string whyUncovered(size_t i) const {
        std::string r;
        if (!uncoveredObjs_.empty() && uncoveredObjs_.count(objs[i].uid)) { const auto b = uncoveredObjBy_.find(objs[i].uid); r += "area-array(area uid " + std::to_string(b == uncoveredObjBy_.end() ? 0 : b->second) + ")"; }
        for (int g : objs[i].groups) if (uncoveredGroups_.count(g)) r += (r.empty() ? "" : "+") + std::string("group ") + std::to_string(g);
        return r;
    }
    bool coveredObject(size_t i) const {
        if (!uncoveredObjs_.empty() && uncoveredObjs_.count(objs[i].uid)) return false;
        for (int g : objs[i].groups) if (uncoveredGroups_.count(g)) return false;
        return true;
    }
    // a partly covered group (supply): may the covered members of group g be supplied while some other member is not? Yes when no group h of a covered member that also holds an uncovered member
    // is read AS A WHOLE by a trigger the shadow runs: a Rotate / Scale / Area Rotate / Area Scale about its own group's centre (the centre of the members, the uncovered ones included, which the shadow does
    // not place), or any trigger that names h as its centre group (a Move-To destination, a Follow leader, a Transform's anchor). A plain Move / Toggle moves each member by the same offset whatever the others do.
    bool partialSupplyOk(int g, int* via = nullptr) const {
        const auto mi = members.find(g);
        if (mi == members.end()) return false;
        std::set<int> seen;
        for (size_t i : mi->second) {
            if (!coveredObject(i)) continue;
            for (int h : objs[i].groups) {
                if (!seen.insert(h).second) continue;
                const auto hm = members.find(h);
                if (hm == members.end()) continue;
                bool hasUncovered = false;
                for (size_t j : hm->second) if (!coveredObject(j)) { hasUncovered = true; break; }
                if (!hasUncovered) continue;
                for (const TrigDef& t : trigs_) {
                    if (!modelled_.count(t.uid)) continue;
                    const bool wholeReader = t.target == h && (t.id == 1346 || t.id == 2067 || t.id == 3006 || t.id == 3007 || t.id == 3008);
                    if (wholeReader || (t.center == h && t.center != 0)) { if (via) *via = h; return false; }
                }
                // A Move-To (a Move with m_useMoveTarget, id 901) reads the position of its target group as a whole (the offset is the destination's place minus the group's own, sampled when it fires:
                // solver.hpp's note on mvtgt) and the destination's (tmodctr), the centre column only naming another source; a Spawn's remap names the group h in place of the template's (a:a:h). a custom level:
                // group 75 (Spawn 4777: 67 -> 75) holds uid 11469, which the shadow does not place (its other group, 305), the Move-To 4722 of group 67 put uid 4745 30.85 px above where GD had it, and the
                // supply killed no one where GD killed at t=6357. A group whose Move-To reads a group the shadow cannot place whole is left on the recording.
                for (const TrigDef& t : trigs_) {
                    if (!modelled_.count(t.uid) || t.id != 901 || !t.mvtgt) continue;
                    bool reads = t.target == h || t.tmodctr == h;
                    if (!reads)
                        for (const TrigDef& sp : trigs_) {
                            if (sp.id != 1268 || !sp.remap) continue;
                            for (const auto& pr : sp.remapTo) if (pr.second == h && (pr.first == t.target || pr.first == t.tmodctr)) reads = true;
                        }
                    if (reads) { if (via) *via = h; std::printf("movetoreads: group %d is left on the recording: Move-To uid %d reads group %d, which holds a member the shadow does not place\n", g, t.uid, h); return false; }
                }
            }
        }
        return true;
    }
    // --trigsupply-all: the groups a trigger the shadow runs moves (the TARGET of a Move / Rotate / Scale / Area Move / Area Rotate; not the centre group) and, since T29, the groups a Toggle (1049) the shadow runs
    // switches: the supply hands over the group's on/off as well as its place (dynamics.hpp g_supplyOnHook), for an object the recording knows nothing about.
    std::set<int> moverTargetGroups() const {
        std::set<int> r;
        const auto mover = [](int id) { return id == 901 || id == 1346 || id == 2067 || id == 3006 || id == 3008 || id == 1049 || id == 1594; };   // 1594: only a touch Toggle Block the live path told the shadow to run (setToggleBoxes) is modelled
        for (const TrigDef& t : trigs_) if (t.target && modelled_.count(t.uid) && mover(t.id)) r.insert(t.target);
        // a Spawn with a group remap run in the shadow's own chain moves the NEW group of the key that names a spawned Move / Rotate / Scale's target (lv22: Spawn 17647 remap 100:100:443:0 over the
        // chain of group 436 = Move target 100 -> group 443). The keys of the nested Spawns met on the way count as well (a superset, never less: the tests that follow decide per group).
        for (const TrigDef& sp : trigs_) {
            if (sp.id != 1268 || !sp.remapOk || sp.remapTo.empty()) continue;
            RemapKeys keys = sp.remapTo;
            std::set<int> seenG;
            std::vector<int> work{sp.target};
            std::vector<const TrigDef*> moved;
            while (!work.empty()) {
                const int g = work.back();
                work.pop_back();
                if (!seenG.insert(g).second) continue;
                const auto m = members.find(g);
                if (m == members.end()) continue;
                for (size_t oi : m->second) {
                    const auto ti = trigByUid_.find(objs[oi].uid);
                    if (ti == trigByUid_.end()) continue;
                    const TrigDef& y = trigs_[ti->second];
                    if (y.id == 1268) { work.push_back(y.target); keys.insert(keys.end(), y.remapTo.begin(), y.remapTo.end()); }
                    if (y.target && modelled_.count(y.uid) && mover(y.id)) moved.push_back(&y);
                }
            }
            for (const TrigDef* y : moved)
                for (const auto& kv : keys) if (y->target == kv.first) r.insert(kv.second);
        }
        return r;
    }

    // ---- one step -----------------------------------------------------------------------------------------------------------------------------
    // `t` = the dump tick this step ends at; `px` = the player's x after the step's update. Call with t = 1, 2, 3, ... in order.
    // `py` = the player's y (a vertical Area's centre). areaPhase: 1 = the areas run before the triggers fire, so an Area fired in step c first acts in step c+1 (measured: c145493833 0 of 19,001 objects differ; with 0 they differ from the step it fires); 0 = after
    Cam cam;   // this step's camera (the areas use the one of the step before)
    bool floatRotate = true;     // a Rotate is done in float through the cocos transform (processRotationActions); false = the double rotation of the first versions
    std::vector<std::pair<int, int>> unlistedActivators;   // (uid, object id) of every activator object the trigger dump leaves out (--trigshadowtable)
    bool unknownActivators = false;   // the level has trigger objects that start groups (Random / Advanced Random / Sequence ...) with no row in the trigger dump
    bool spawnModel = true;      // Spawn triggers and what they spawn are run (false = the first versions: every spawn-triggered trigger is unvouched for)
    int spawnPhase = 2;          // where in a step the delayed spawns count down: 0 before the commands step, 1 after it, 2 after the crossed triggers fired
    bool uncoveredAreasOn = true;   // the objects a non-run Area reaches through its parents / children are not vouched for (false = only its target group, as before)
    bool rotFloat = true;          // an object's own rotation accumulates in float, as the node's does (false = in double, as before)
    bool nestedRemap = true;       // a trigger no Spawn names (started through an Advanced Random ...) takes the one remap in force in its step (false = none, as before)
    bool remapMainObj = true;      // a Move-To / Rotate fed through a remapping Spawn takes the main object of the remapped group, not the dump's (false = the dump's, as before)
    bool spawnDelayRemap = true;   // applySpawnRemap picks the Spawn whose call plus delay lands on the step (false = the last one called)
    bool sectionWindow = true;   // the Areas look only at the sections near their centre (areaRange); false = every object, as the first version did
    int areaPhase = 1;
    long long stepNo_ = 0;   // the step number of the step being run (diagnosis prints)
    // pyKnown = false: py is not the player's world y (a live layer's group in frame 0 knows its progress axis only; the lateral part is per state). A vertical channel's crossing compares y, so it WAITS then
    // (checkSpawnObjects) instead of reading a 0 that makes every downward trigger look crossed (a custom level:20200, 47894 of a down channel fired 490 ticks early). Default true: every other caller is as before.
    void step(long long t, double px, double py = 0.0, bool pyKnown = true) {
        pyKnown_ = pyKnown;
        stepNo_ = t;
        const bool firstStep = !havePrev_;
        if (firstStep && rngPinFirst_) rng_ = 0x48c67641ULL;   // the draws of the start triggers fire inside the game's reset
        while (!pendDraw_.empty() && pendDraw_.front() <= t) {   // the Advanced Follows called in an earlier step run now: one draw each
            pendDraw_.erase(pendDraw_.begin());
            rng_ = rng_ * 0x343fdULL + 0x269ec3ULL; ++randDraws_;
            if (g_randLogReplay && !g_randLogFile.empty()) if (FILE* lf = std::fopen(g_randLogFile.c_str(), "a")) { std::fprintf(lf, "%lld,0,3016,%u,0,%llu,advfollow\n", t, (unsigned)((rng_ >> 16) & 0x7fff), (unsigned long long)(rng_ & 0xffffffffULL)); std::fclose(lf); }
        }
        // GD Win 2.2091: GJBaseGameLayer::update divides by the full warp at RVA 0x340813 before advancing +0x3d8 (0x340bac), the clock read by Sequence (0x655d85).
        levelTime_ += ((1.0 / 240.0) * std::min(warp_, 1.0)) / warp_;
        curTick_ = t;
        curPx_ = px; curPy_ = py;
        spawnedStep_.clear();
        spawnCountStep_ = 0;
        evDone_.clear();
        if (gtogFed_) {
            const auto gi = gtogAt_.find(t);
            if (gi != gtogAt_.end()) for (const auto& e : gi->second) { toggleGroupTriggered(e.group, e.on, e.uid); ++groupToggleFed; }
        }
        if (spawnPhase == 0) updateSpawns();
        stepCommands();
        if (spawnPhase == 1) updateSpawns();
        if (!havePrev_) { prevPx_ = px; prevPy_ = py; prevCam_ = cam; prev2Cam_ = cam; havePrev_ = true; }
        if (areaPhase == 1) stepAreas(prevPx_, prevPy_);
        if (collisionModel) stepCollisions();   // GJBaseGameLayer::update: updateCollisionBlocks runs after the commands' motion is written and before the player's update / the crossing loop
        fireItems(t);
        fireTouch(t);
        if (evFed_) { const auto ei = evAt_.find(t); if (ei != evAt_.end()) for (const GameEvt& e : ei->second) raiseEvent(e.ev, e.material, e.player); }
        double crossPx = px;  // the crossing loop reads the player's x once, when it starts; a Teleport fired inside it does not change what the rest of the array is compared with (the dump's x of the step has the Teleport in it)
        { const auto ci = crossRef_.find(t); if (ci != crossRef_.end()) crossPx = ci->second; }
        checkSpawnObjects(t, crossPx, py);
        if (spawnPhase == 2) updateSpawns();
        fireSpawnFed(t);
        if (areaPhase == 0) stepAreas(prevPx_, prevPy_);
        prev2Px_ = prevPx_; prev2Py_ = prevPy_; havePrev2_ = havePrevAny_;
        prev2Cam_ = prevCam_; prevPx_ = px; prevPy_ = py; prevCam_ = cam; havePrevAny_ = true;
        if (firstStep && rngPinFirst_) rng_ = 0;   // the mod pins the stream back to 0 as the reset ends (rngfix)
    }
    long long groupToggleFed = 0;   // the group on / off calls handed over (addGroupToggleEvent) and applied
    size_t areasActive() const { return areaInsts_.size(); }

    // ---- the dynamic state as one value (a measurement for a per-group timeline: two shadows with the same value have the same future) ------------
    // Everything a later step reads and an earlier step wrote: the objects' place / rotation / counter / area offsets, the groups' bits, the channel cursors, the commands in flight, the waiting
    // spawns, the Areas in force and their transitions. The player's previous place only where something reads it (an Area in force, a Move locked to the player); the tick itself is not in it
    // (two timelines are compared at the same tick). Sets and maps are folded with a commutative sum, so the container's order does not enter.
    uint64_t dynHash(bool withPlayer = true) const {
        auto mix = [](uint64_t h, uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2); return h * 0xff51afd7ed558ccdULL; };
        auto bits = [](double d) { uint64_t u; std::memcpy(&u, &d, 8); return u; };
        auto bitsf = [](float d) { uint32_t u; std::memcpy(&u, &d, 4); return (uint64_t)u; };
        uint64_t h = 1469598103934665603ULL;
        for (const ObjState& o : objs) {
            h = mix(h, bits(o.x)); h = mix(h, bits(o.y)); h = mix(h, bits(o.rot));
            h = mix(h, (uint64_t)(uint32_t)o.counter); h = mix(h, bitsf(o.ax)); h = mix(h, bitsf(o.ay)); h = mix(h, (uint64_t)o.areaStamp);
        }
        uint64_t s = 0;
        for (const auto& kv : groupBit) s += mix((uint64_t)(uint32_t)kv.first, kv.second ? 1 : 2);
        h = mix(h, s);
        h = mix(h, (controlsDisabled[0] ? 1 : 0) | (controlsDisabled[1] ? 2 : 0) | (playerHidden ? 4 : 0));
        for (const auto& kv : chanIdx_) { h = mix(h, (uint64_t)(uint32_t)kv.first); h = mix(h, (uint64_t)kv.second); }
        h = mix(h, (uint64_t)(uint32_t)activeChan_); h = mix(h, bits(warp_));
        bool lockedCmd = false;
        for (const Cmd& c : cmds_) {
            h = mix(h, c.trig); h = mix(h, (c.rotate ? 1 : 0) | (c.skip ? 2 : 0) | (c.done ? 4 : 0) | (c.paused ? 8 : 0) | (c.lockPX ? 16 : 0) | (c.lockPY ? 32 : 0) | (c.lockCX ? 64 : 0) | (c.lockCY ? 128 : 0));
            h = mix(h, bits(c.dx)); h = mix(h, bits(c.dy)); h = mix(h, bits(c.total)); h = mix(h, bits(c.dur)); h = mix(h, bitsf(c.el));
            h = mix(h, bits(c.lastX)); h = mix(h, bits(c.lastY)); h = mix(h, bits(c.lastR)); h = mix(h, bits(c.time));
            lockedCmd = lockedCmd || c.lockPX || c.lockPY || c.lockCX || c.lockCY;
        }
        s = 0;
        for (int u : spawnedOnce_) s += mix((uint64_t)(uint32_t)u, 7);
        h = mix(h, s);
        { uint64_t sq = 0; for (const auto& kv : seq_) { uint64_t e = mix((uint64_t)kv.first, (uint64_t)(uint32_t)kv.second.idx); uint32_t lb; std::memcpy(&lb, &kv.second.lastT, 4); sq += mix(e, lb); } h = mix(h, sq); }   // a Sequence's counts and last calls
        h = mix(h, (uint64_t)pendLen_);
        h = mix(h, (uint64_t)pendDraw_.size());
        h = mix(h, rng_);   // the trigger LCG: two timelines that drew a different number of times (or other values) have different futures
        for (const CollAction& a : collActions_) { h = mix(h, (uint64_t)(uint32_t)a.trigUid); }   // the fired Collision triggers and the pairs in contact
        s = 0;
        for (const auto& kv : collActive_) s += mix((uint64_t)(uint32_t)kv.first.first * 100003u + (uint32_t)kv.first.second, 11);
        h = mix(h, s);
        for (size_t i = 0; i < pendLen_ && i < pending_.size(); ++i) {
            const PendingSpawn& p = pending_[i];
            h = mix(h, (uint64_t)(uint32_t)p.group); h = mix(h, (uint64_t)(uint32_t)p.objUid); h = mix(h, bits(p.delay)); h = mix(h, bits(p.time));
            h = mix(h, (p.ordered ? 1 : 0) | (p.done ? 2 : 0) | (p.paused ? 4 : 0)); h = mix(h, (uint64_t)(uint32_t)p.trigUid);
            for (const auto& kv : p.remap) h = mix(h, ((uint64_t)(uint32_t)kv.first << 32) | (uint32_t)kv.second);
        }
        for (int u : areaInsts_) h = mix(h, (uint64_t)(uint32_t)u);
        s = 0;
        for (const auto& kv : areaDefs_) s += mix(mix((uint64_t)(uint32_t)kv.first, bits(kv.second.len)), mix(bits(kv.second.off), mix(bits(kv.second.deadzone), mix(bits(kv.second.moveX), bits(kv.second.moveY)))));
        for (const auto& kv : areaDefs_) if (kv.second.anyVar()) s += mix((uint64_t)(uint32_t)kv.first, mix(mix(bits(kv.second.lenV), bits(kv.second.offV)), mix(mix(bits(kv.second.offYV), bits(kv.second.moveXV)), mix(bits(kv.second.moveYV), mix(bits(kv.second.moveDistV), bits(kv.second.angleV))))));
        h = mix(h, s);
        s = 0;
        for (const auto& kv : areaTrans_)
            for (const auto& sl : kv.second) s += mix(mix((uint64_t)(uint32_t)kv.first, (uint64_t)(uint32_t)sl.first), mix(bitsf(sl.second.start), mix(bitsf(sl.second.end), mix(bitsf(sl.second.dur), bitsf(sl.second.el)))));
        h = mix(h, s);
        h = mix(h, (uint64_t)areaStep_);
        {   // T57: the item counters, the Count entries waiting on them and the persistent set (two timelines that differ only in an item's number have different futures); nothing is mixed in where the level has none, so the hash of
            // a level without item state is what it was
            uint64_t si = 0; bool any = false;
            for (const auto& kv : itemCount_) if (kv.second != 0) { si += mix(mix((uint64_t)(uint32_t)kv.first, (uint64_t)(uint32_t)kv.second), 17); any = true; }
            for (const auto& kv : countReg_) for (const CountEntry& e : kv.second) { si += mix(mix(mix((uint64_t)(uint32_t)kv.first, (uint64_t)(uint32_t)e.target), mix((uint64_t)(uint32_t)e.last, (uint64_t)(uint32_t)e.group)), (uint64_t)((e.multi ? 1 : 0) | (e.activate ? 2 : 0)) | ((uint64_t)(uint32_t)e.trigUid << 8)); any = true; }
            for (int p : persistItems_) { si += mix((uint64_t)(uint32_t)p, 19); any = true; }
            if (any) h = mix(h, si);
        }
        if (!evListen_.empty()) {   // the Event triggers' listeners, in the order they will be called (nothing is mixed in where no Event trigger has fired)
            uint64_t se = 0x4576;
            for (const EvListener& L : evListen_) se = mix(mix(se, (uint64_t)(uint32_t)L.ev | ((uint64_t)(uint32_t)L.key << 32)), (uint64_t)(uint32_t)L.target | ((uint64_t)(uint32_t)L.trigUid << 32));
            h = mix(h, se);
        }
        if (withPlayer && (!areaInsts_.empty() || lockedCmd)) { h = mix(h, bits(prevPx_)); h = mix(h, bits(prevPy_)); h = mix(h, bits(curPx_)); h = mix(h, bits(curPy_)); }
        return h;
    }
    // the bytes a snapshot of the part that is not the objects would take (the commands, spawns, Areas, cursors), a size estimate
    size_t dynPrivBytes() const {
        size_t n = cmds_.size() * sizeof(Cmd) + pendLen_ * sizeof(PendingSpawn) + areaInsts_.size() * sizeof(int) + chanIdx_.size() * 16 + groupBit.size() * 8 + spawnedOnce_.size() * 8;
        for (const auto& kv : areaTrans_) n += kv.second.size() * (sizeof(AreaTrans) + 8);
        return n + 96;
    }

    // ---- the content as a 128-bit hash: the identity of a world (E8) --------------------------------------------------------------------------------
    // contentHash(): every member a later step reads and an earlier step (or a call between steps: a touch Event, a fed toggle) wrote, EXCEPT the player's and the camera's track, which is
    // trackHash(). Two shadows of one level at one tick with the same contentHash and the same trackHash step alike for good when they are given the same inputs; with the same contentHash alone
    // they do so once the step that follows is handed its track by the caller (not yet: the step still reads the places it kept).
    // dynHash above is the measurement this grew from; it leaves out what was not needed to tell two timelines of one search apart (the stamps of an ease-out Area, the place two steps back that
    // a lock reads, the layer's clock, the camera of the last steps, most of a command's fields), and it takes the player's place in only while something reads it.
    // NOT in it, by kind:
    //   reports      fired, queueWaits, lastTrig / lastTrigId of an object (written, never read in dp/), itemsFed, groupToggleFed, sclMoves, sclMoved, randDraws_, collEnters, stepNo_
    //   outputs      events, gravChanges, ctrlChanges, hiddenChanges, spawnReach, spawnFires: what the steps so far fired, read by the caller for the step just made
    //   scratch      moveNodes_, spawnedStep_, spawnCountStep_, curSpawnUid_, curRemap_, curOvershoot_, curExtraDelay_, curLate_, arCx_ / arCy_, pyKnown_, curTick_: written before they are read in every step
    //   the level    everything the load fixed (the SharedConst tables, areaEdits_, noopUids_, the loader's switches). The record's feeds (itemAt_, gtogAt_, spawnAt_, crossRef_, lateAt_) are the
    //                level's too -- one base shadow hands them to every timeline of a call -- so only their sizes are in (a timeline that was fed something another was not differs there);
    //                touchAt_ is fed per timeline (addTouchEvent), and its entries still ahead of the last step are in.
    // An object's areaStamp and the Areas' step counter are in as they stand (absolute counts), so two worlds whose Areas ran for a different number of steps differ even when every stamp is as
    // old in the one as in the other: fewer worlds would be found the same if the stamps were taken relative to the counter. Sound as it is.
    // The hash of a set of object pages, the objects' part of contentHash. Also asked for the objects as they stood BEFORE a step (TrigLive's supPre, a copy that shares the pages): the one
    // function for both, since a page keeps its hash and every copy that shares the page reads it.
    static Hash2 objsHash(const CowPages<ObjState>& o) {
        return o.pagesHash([](const ObjState* p, size_t n, Hash2& g) {
            for (size_t i = 0; i < n; ++i, ++p) {
                g.addD(p->x); g.addD(p->y); g.addD(p->rot); g.add((uint64_t)(uint32_t)p->counter | (p->blind ? (1ULL << 32) : 0) | (p->blindArea ? (1ULL << 33) : 0));
                g.addF2(p->ax, p->ay); g.addF2(p->asx, p->asy); g.addF2(p->osx, p->osy); g.add((uint64_t)p->areaStamp);
            }
        });
    }
    Hash2 contentHash() const {
        Hash2 h;
        h.addH(objsHash(objs));
        uint64_t sa = 0, sb = 0;
        for (const auto& kv : groupBit) { Hash2 e; e.add((uint64_t)(uint32_t)kv.first); e.add(kv.second ? 1 : 2); e.sumInto(sa, sb); }
        h.add(0x6701); h.add(sa); h.add(sb); h.add((uint64_t)groupBit.size());
        h.add((controlsDisabled[0] ? 1 : 0) | (controlsDisabled[1] ? 2 : 0) | (playerHidden ? 4 : 0) | (vertical_ ? 8 : 0) | (camLockBlind ? 16 : 0) | (havePrev_ ? 32 : 0));
        h.add(0x6702); for (const auto& kv : chanIdx_) { h.add((uint64_t)(uint32_t)kv.first); h.add((uint64_t)kv.second); }
        h.add(0x6703); for (const auto& kv : chanRev_) { h.add((uint64_t)(uint32_t)kv.first); h.add(kv.second ? 1 : 2); }
        h.add((uint64_t)(uint32_t)activeChan_); h.addD(warp_); h.addD(levelTime_); h.add(rng_);
        h.add(0x6704); h.add((uint64_t)cmds_.size());
        for (const Cmd& c : cmds_) {
            h.add((uint64_t)c.trig);
            h.add((c.rotate ? 1 : 0) | (c.scale ? 2 : 0) | (c.skip ? 4 : 0) | (c.done ? 8 : 0) | (c.paused ? 16 : 0) | (c.lockrot ? 32 : 0) | (c.ctrCol ? 64 : 0) | (c.lockCX ? 128 : 0) | (c.lockCY ? 256 : 0) | (c.lockPX ? 512 : 0) | (c.lockPY ? 1024 : 0));
            h.addD(c.sclX); h.addD(c.sclY); h.addD(c.lastE); h.addD(c.dx); h.addD(c.dy); h.addD(c.total); h.addD(c.dur); h.addD(c.rate); h.addF(c.el);
            h.addD(c.lastX); h.addD(c.lastY); h.addD(c.lastR); h.addD(c.modX); h.addD(c.modY); h.addD(c.time);
            h.add((uint64_t)(uint32_t)c.kind | ((uint64_t)(uint32_t)c.target << 32)); h.add((uint64_t)(uint32_t)c.center | ((uint64_t)(uint32_t)c.uid << 32)); h.add((uint64_t)(uint32_t)c.id | ((uint64_t)(uint32_t)c.ctrObj << 32));
        }
        sa = sb = 0;
        for (int u : spawnedOnce_) { Hash2 e; e.add((uint64_t)(uint32_t)u); e.sumInto(sa, sb); }
        h.add(0x6705); h.add(sa); h.add(sb); h.add((uint64_t)spawnedOnce_.size());
        sa = sb = 0;
        for (const auto& kv : seq_) { Hash2 e; e.add((uint64_t)kv.first); e.add((uint64_t)(uint32_t)kv.second.idx | (kv.second.has ? (1ULL << 32) : 0)); e.addF(kv.second.lastT); e.sumInto(sa, sb); }
        h.add(0x6706); h.add(sa); h.add(sb); h.add((uint64_t)seq_.size());
        h.add(0x6707); h.add((uint64_t)pendDraw_.size()); for (long long d : pendDraw_) h.add((uint64_t)d);
        // the waiting Spawns: the live entries and the stale slots behind them (updateSpawns reads by index up to the length it saw when it started, so a slot behind the end can be read again)
        h.add(0x6708); h.add((uint64_t)pendLen_); h.add((uint64_t)pending_.size());
        for (const PendingSpawn& p : pending_) {
            h.add((uint64_t)(uint32_t)p.group | ((uint64_t)(uint32_t)p.objUid << 32)); h.addD(p.delay); h.addD(p.time);
            h.add((p.ordered ? 1 : 0) | (p.done ? 2 : 0) | (p.paused ? 4 : 0) | ((uint64_t)(uint32_t)p.trigUid << 32));
            h.add((uint64_t)p.remap.size()); for (const auto& kv : p.remap) h.add(((uint64_t)(uint32_t)kv.first << 32) | (uint32_t)kv.second);
        }
        h.add(0x6709); h.add((uint64_t)areaInsts_.size()); for (int u : areaInsts_) h.add((uint64_t)(uint32_t)u);
        sa = sb = 0;
        for (const auto& kv : areaDefs_) {
            const AreaDef& a = kv.second; Hash2 e;
            e.add((uint64_t)(uint32_t)kv.first); e.add((uint64_t)(uint32_t)a.uid | ((uint64_t)(uint32_t)a.target << 32)); e.add((uint64_t)(uint32_t)a.special | ((uint64_t)(uint32_t)a.center << 32));
            e.add((uint64_t)(uint32_t)a.dirType | ((uint64_t)(uint32_t)a.eInType << 32)); e.add((uint64_t)(uint32_t)a.eInBuf | ((uint64_t)(uint32_t)a.index << 32)); e.add((uint64_t)(uint32_t)a.eOutType | ((uint64_t)(uint32_t)a.eOutBuf << 32));
            e.add((a.inbound ? 1 : 0) | (a.twoDir ? 2 : 0) | (a.angleMode ? 4 : 0) | (a.eOutOn ? 8 : 0) | (a.scaleKind ? 16 : 0) | (a.relative ? 32 : 0));
            e.addD(a.len); e.addD(a.off); e.addD(a.modFront); e.addD(a.modBack); e.addD(a.deadzone); e.addD(a.moveX); e.addD(a.moveY); e.addD(a.eInRate); e.addD(a.moveDist); e.addD(a.eOutRate);
            e.addD(a.scaleX); e.addD(a.scaleY); e.addD(a.offY); e.addD(a.relFade); e.addD(a.angle);
            e.sumInto(sa, sb);
        }
        h.add(0x670a); h.add(sa); h.add(sb); h.add((uint64_t)areaDefs_.size());
        sa = sb = 0;
        for (const auto& kv : areaTrans_)
            for (const auto& sl : kv.second) {
                Hash2 e; e.add(((uint64_t)(uint32_t)kv.first << 32) | (uint32_t)sl.first);
                e.addF2(sl.second.start, sl.second.end); e.addF2(sl.second.dur, sl.second.el); e.addF(sl.second.rate); e.add((uint64_t)(uint32_t)sl.second.type | ((uint64_t)(uint32_t)sl.second.buf << 32));
                e.sumInto(sa, sb);
            }
        h.add(0x670b); h.add(sa); h.add(sb);
        sa = sb = 0;
        for (const auto& kv : areaEO_) {
            Hash2 e; e.add((uint64_t)(uint32_t)kv.first); e.add((uint64_t)(uint32_t)kv.second.step);
            e.addH(kv.second.stamp.pagesHash([](const int* v, size_t n, Hash2& g) { for (size_t i = 0; i < n; ++i) g.add((uint64_t)(uint32_t)v[i]); }));
            e.sumInto(sa, sb);
        }
        h.add(0x670c); h.add(sa); h.add(sb); h.add((uint64_t)areaEO_.size());
        h.add((uint64_t)areaStep_);
        h.add(0x670d); h.add((uint64_t)areaTouchedPrev_.size()); for (size_t i : areaTouchedPrev_) h.add((uint64_t)i);
        h.add(0x670e); h.add((uint64_t)collActions_.size());
        for (const CollAction& a : collActions_) { h.add((uint64_t)(uint32_t)a.hi | ((uint64_t)(uint32_t)a.lo << 32)); h.add((uint64_t)(uint32_t)a.target | ((uint64_t)(uint32_t)a.trigUid << 32)); h.add((a.activate ? 1 : 0) | (a.onExit ? 2 : 0)); }
        h.add(0x670f); h.add((uint64_t)collActive_.size());
        for (const auto& kv : collActive_) { h.add((uint64_t)(uint32_t)kv.first.first | ((uint64_t)(uint32_t)kv.first.second << 32)); h.add(kv.second ? 1 : 2); }
        sa = sb = 0;
        // the item counters (T57: Pickup / Item Edit / Item Persist write them, Item Compare and the Counts read them). A counter at 0 is the same as none (itemCountOf), as in dynHash: an entry that
        // holds 0 is left out, so two worlds that differ only in having written a 0 are one
        for (const auto& kv : itemCount_) if (kv.second != 0) { Hash2 e; e.add((uint64_t)(uint32_t)kv.first | ((uint64_t)(uint32_t)kv.second << 32)); e.sumInto(sa, sb); }
        h.add(0x6710); h.add(sa); h.add(sb);
        h.add(0x6714 + (uint64_t)persistItems_.size()); for (int p : persistItems_) h.add((uint64_t)(uint32_t)p);   // the items an Item Persist switched on (a later "all" Persist resets exactly these)
        sa = sb = 0;
        for (const auto& kv : countReg_) {
            Hash2 e; e.add((uint64_t)(uint32_t)kv.first); e.add((uint64_t)kv.second.size());
            for (const CountEntry& c : kv.second) { e.add((c.multi ? 1 : 0) | (c.activate ? 2 : 0) | ((uint64_t)(uint32_t)c.last << 32)); e.add((uint64_t)(uint32_t)c.target | ((uint64_t)(uint32_t)c.group << 32)); e.add((uint64_t)(uint32_t)c.trigUid); }
            e.sumInto(sa, sb);
        }
        h.add(0x6711); h.add(sa); h.add(sb); h.add((uint64_t)countReg_.size());
        h.add(0x6712);
        for (auto it = touchAt_.upper_bound(curTick_); it != touchAt_.end(); ++it) { h.add((uint64_t)it->first); h.add((uint64_t)it->second.size()); for (int u : it->second) h.add((uint64_t)(uint32_t)u); }
        if (!evListen_.empty() || !evAt_.empty()) {   // the Event triggers (3604): the listeners in the order they are called (with the keys of the call that registered them), and the game events still to be raised; nothing where a level has none
            h.add(0x6716); h.add((uint64_t)evListen_.size());
            for (const EvListener& L : evListen_) { h.add((uint64_t)(uint32_t)L.ev | ((uint64_t)(uint32_t)L.key << 32)); h.add((uint64_t)(uint32_t)L.target | ((uint64_t)(uint32_t)L.trigUid << 32)); h.add((uint64_t)L.remap.size()); for (const auto& r : L.remap) h.add((uint64_t)(uint32_t)r.first | ((uint64_t)(uint32_t)r.second << 32)); }
            for (auto it = evAt_.upper_bound(curTick_); it != evAt_.end(); ++it) { h.add((uint64_t)it->first); h.add((uint64_t)it->second.size()); for (const GameEvt& e : it->second) { h.add((uint64_t)(uint32_t)e.ev | ((uint64_t)(uint32_t)e.material << 32)); h.add((uint64_t)(uint32_t)e.player); } }
        }
        h.add(0x6713); h.add((uint64_t)itemAt_.size()); h.add((uint64_t)gtogAt_.size()); h.add((uint64_t)spawnAt_.size()); h.add((uint64_t)crossRef_.size()); h.add((uint64_t)lateAt_.size()); h.add((uint64_t)touchAt_.size());
        return h;
    }
    // the player's and the camera's places the next steps read: after the last step, the one before it and the one before that; and whether each is there yet
    Hash2 trackHash() const {
        Hash2 h;
        h.addD(curPx_); h.addD(curPy_); h.addD(prevPx_); h.addD(prevPy_); h.addD(prev2Px_); h.addD(prev2Py_);
        h.add((havePrev_ ? 1 : 0) | (havePrev2_ ? 2 : 0) | (havePrevAny_ ? 4 : 0));
        for (const Cam* c : {&cam, &prevCam_, &prev2Cam_}) { h.addD(c->l); h.addD(c->b); h.addD(c->w); h.addD(c->h); h.add((c->ok ? 1 : 2) + (c->okY ? 4 : 0)); }
        return h;
    }
    // the element counts of the members a copy of the shadow still copies whole (a report: what a timeline copy costs beside the objects' page table)
    std::string copiedSizes() const {
        size_t trans = 0; for (const auto& kv : areaTrans_) trans += kv.second.size();
        size_t evStr = 0; for (const Event& e : events) evStr += e.raw.size();
        char b[640];
        std::snprintf(b, sizeof b, "groupBit %zu chanIdx %zu chanRev %zu cmds %zu seq %zu pendDraw %zu pending %zu spawnedOnce %zu areaDefs %zu areaEdits %zu areaTrans %zu areaEO %zu areaInsts %zu areaTouched %zu+%zu "
                      "collActions %zu collActive %zu itemCount %zu countReg %zu touchAt %zu itemAt %zu gtogAt %zu spawnAt %zu lateAt %zu crossRef %zu noop %zu unlisted %zu sclMoved %zu "
                      "events %zu (raw %zu B) gravChanges %zu ctrlChanges %zu hiddenChanges %zu spawnReach %zu spawnFires %zu",
                      groupBit.size(), chanIdx_.size(), chanRev_.size(), cmds_.size(), seq_.size(), pendDraw_.size(), pending_.size(), spawnedOnce_.size(), areaDefs_.size(), areaEdits_.size(), trans, areaEO_.size(), areaInsts_.size(),
                      areaTouchedPrev_.size(), areaTouchedCur_.size(), collActions_.size(), collActive_.size(), itemCount_.size(), countReg_.size(), touchAt_.size(), itemAt_.size(), gtogAt_.size(), spawnAt_.size(), lateAt_.size(),
                      crossRef_.size(), noopUids_.size(), unlistedActivators.size(), sclMoved.size(), events.size(), evStr, gravChanges.size(), ctrlChanges.size(), hiddenChanges.size(), spawnReach.size(), spawnFires.size());
        return b;
    }
    // does a step of this shadow read the player's place beyond the crossing test: an Area in force, or a Move locked to the player still running (a report; the Collision triggers' player block is not modelled)
    bool readsPlayerPlace() const {
        if (!areaInsts_.empty()) return true;
        for (const Cmd& c : cmds_) if (!c.done && (c.lockPX || c.lockPY)) return true;
        return false;
    }

private:
    struct Cmd {
        size_t trig = 0;
        bool rotate = false;
        bool scale = false; double sclX = 1.0, sclY = 1.0, lastE = 0.0;   // a Transform that scales: the scale it runs to, and the eased share of the step before
        double dx = 0, dy = 0, total = 0, dur = 0, rate = 2.0;
        int kind = 0;
        float el = 0.0f;
        bool skip = true, done = false;
        double lastX = 0, lastY = 0, lastR = 0;
        int target = 0, center = 0;
        int uid = 0, id = 0;
        bool lockrot = false;   // the Rotate leaves the objects' own rotation alone (they orbit)
        bool paused = false;    // a Pause (Stop trigger, mode 1): the command is not stepped at all until a Resume
        int ctrObj = 0; bool ctrCol = false;   // a Rotate's centre object as the dump has it (TrigDef::ctrObj)
        bool lockCX = false, lockCY = false;   // a Move locked to the camera: the displacement of the camera centre of the step before (camera model: l + w / 2, b + h / 2), times modX / modY
    bool lockPX = false, lockPY = false;   // a Move locked to the player: each step it moves the group by the player's displacement of the step before, times modX / modY
        double modX = 1.0, modY = 1.0, time = 0.0;
    };
    SharedConst<std::vector<TrigDef>> trigs_;
    SharedConst<std::map<int, std::vector<size_t>>> chanQ_;   // per channel: the crossing triggers in (int(x), uid) order
    std::map<int, size_t> chanIdx_;              // ...and the channel's cursor (kept while another channel is active)
    std::map<int, bool> chanRev_;                // the channel's reverse bit: the player travels towards smaller x (a trigger is reached when px <= its x)
    int activeChan_ = 0;
    bool vertical_ = false;   // the player travels along y (checkSpawnObjects compares y)
    bool pyKnown_ = true;     // the py of the step being run is the player's world y (step)
    bool chanModel_ = false;
    std::vector<Cmd> cmds_;
public:
    long long sclMoves = 0; std::set<int> sclMoved;   // (a count for whoever tests the Scale: place changes made by it)
    double orbDelay = 0.0;   // seconds added to the ordered delays of a chain a Toggle Orb (1594) starts (trig_replay --orbdelay); 0 = off
    bool areaScaleFirst = false;   // the Area Scale instances before the Area Moves: a stand-in for the game's sort of the instances by EnterEffectObject +0x7f8 / +0x7fc (m_priority ...: processAreaEffects 0x331030 sorts its instances with 0x30b140), which the dump does not carry; measured on c141972420 (a Scale and a Move on one group)
    bool rotReplace = false;   // the level is of the old version (levelsettings.txt fix* = 0): a Rotate replaces a running Rotate of the same (target, centre) instead of adding to it (see fireNow)
private:
    struct SeqState { int idx = 0; float lastT = -1.0f; bool has = false; };
    std::unordered_map<long long, SeqState> seq_;   // a Sequence trigger's state per (trigger uid, remap key): the count and the time of its last call
public:
    // the trigger LCG a Random / Advanced Random draws from. The mod pins the game's to 0x48c67641 while the attempt is reset (the start triggers fire inside PlayLayer::resetLevel) and to 0 once it is over
    // (mod/rng_early.hpp, cfg rngearly / rngfix); here the first step's draws are the in-reset ones when rngResetSeed_ is set (--trigshadowopts random), the later ones the stream from 0
    uint64_t rng_ = 0;
    bool drawOnFollow = false;   // --trigshadowopts random*: an Advanced Follow (3016) draws once, the step after its call
    std::vector<long long> pendDraw_;   // the steps at which the Advanced Follow draws are due
    bool rngPinFirst_ = false;   // the first step's draws use 0x48c67641, then the stream restarts at 0
    long long randDraws_ = 0;
private:
    double levelTime_ = 0.0;                        // the layer's clock in seconds
    double warp_ = 1.0;   // the full m_timeWarp; the physics step instead uses min(warp, 1)
    float physicsStepDt() const { return (float)((1.0 / 240.0) * std::min(warp_, 1.0)); }
    bool touchFed_ = false;
    bool evFed_ = false;   // setEventsFed: the Event triggers (3604) are run (their listeners are registered, the game events the caller hands over spawn their groups)
    // the Event triggers' listeners, in the order they were registered (GJBaseGameLayer +0x588: map<(event, key), vector<EventTriggerInstance>>; gameEventTriggered walks the one of its key)
    struct EvListener { int ev = 0, key = 0, target = 0, trigUid = 0; std::vector<std::pair<int, int>> remap; };   // remap: the keys of the call that registered it (RemapKeys)
    std::vector<EvListener> evListen_;
    struct GameEvt { int ev = 0, material = 0, player = 0; };
    std::map<long long, std::vector<GameEvt>> evAt_;   // step -> the game events raised there (addGameEvent)
    std::set<std::pair<int, int>> evDone_;             // (event, key) already raised in this step (gameEventTriggered's stamp map +0x598)
    bool gtogFed_ = false;
    std::set<int> gtogTaps_;   // the Taps (uid) whose calls are handed over; empty = all of them
    struct GTog { int group; bool on; int uid; };
    std::map<long long, std::vector<GTog>> gtogAt_;
    std::map<long long, std::vector<int>> touchAt_;
    std::map<long long, std::vector<std::pair<int, int>>> itemAt_;   // step -> (item, delta) of the pickups collected there
    std::unordered_map<int, int> itemCount_;
    struct CountEntry { bool multi = false, activate = false; int last = 0, target = 0, group = 0, trigUid = 0; };
    std::unordered_map<int, std::vector<CountEntry>> countReg_;   // item -> the Count entries waiting on it (GJEffectManager +0x238)
    std::set<int> persistItems_;   // the items an Item Persist (3641) switched on (GJEffectManager +0x370); the values they would carry into a next attempt are not modelled (the loader only runs what a reset at the start makes certain)
    std::set<std::pair<long long, int>> lateAt_;   // (step, uid) of the calls made in the button phase
    bool curLate_ = false;
    SharedConst<std::unordered_map<int, bool>> multiMap_;
    std::set<int> spawnedOnce_;
    bool recordComplete_ = false;
    bool spawnFedOn_ = false;
    bool spawnFedStops_ = false;   // the record has the spawn-triggered calls: a Stop the shadow cannot wait for is fired from it, whatever the spawn-feed mode
    int spawnFedMode_ = 1;
    std::map<long long, std::vector<int>> spawnAt_;
    SharedConst<std::set<int>> fedSpawn_;
    SharedConst<std::map<int, std::set<std::string>>> fedReason_;         // a spawn-triggered trigger handed to the record -> why (the activators the shadow could not run)
    SharedConst<std::map<int, std::set<std::string>>> fedGroupReasons_;   // a group whose objects depend on the record -> the reasons
    SharedConst<std::map<int, std::set<int>>> spawnBlockers_;   // a spawn-triggered trigger the shadow does not run -> the ids of the activators (that are not run) that name a group it is in (the first found; a report)
    SharedConst<std::set<int>> modelledTargets_;
    SharedConst<std::set<int>> uncoveredObjs_;   // objects a non-run Area works on through its parent / child arrays (finish)
    SharedConst<std::set<int>> other_, uncoveredGroups_, otherBase_, modelled_, touchFedGroups_;
    SharedConst<std::unordered_map<int, size_t>> trigByUid_;
    bool anyRemap_ = false;                                  // some Spawn has a group remap (applySpawnRemap)
    SharedConst<std::unordered_map<int, std::vector<int>>> spawnersOf_;   // trigger uid -> the Spawn triggers whose group holds it
    SharedConst<std::unordered_map<int, std::vector<long long>>> fireSteps_;   // trigger uid -> the steps the record has it called at
    using RemapKeys = std::vector<std::pair<int, int>>;   // (source group, target group), the first match of a source wins
    struct PendingSpawn { int group = 0; int objUid = 0; bool ordered = false; double delay = 0, time = 0; bool done = false; int trigUid = 0; bool paused = false; RemapKeys remap; };   // trigUid: the Spawn trigger that made it (a Stop of its group erases it)   // objUid != 0: one trigger of an ordered spawn   // remap: the keys the call carries
    int curSpawnUid_ = 0;         // the Spawn trigger whose spawn is being done
    // The remap keys of the spawn call that is running (GJBaseGameLayer::spawnGroup / spawnObject / triggerObject all carry a `remapKeys` argument). A Spawn trigger with a remap hands its own keys followed by the
    // ones it was called with (SpawnTriggerGameObject::updateRemapKeys 0x65a140); a Spawn with none hands nothing down. A trigger a call spawns acts on the remapped groups (applyRemapKeys). Empty outside a spawn chain.
    RemapKeys curRemap_;
    static void applyRemapKeys(TrigDef& x, const RemapKeys& keys, bool remapMainObj) {
        const int ga0 = x.tmodctr > 0 ? x.tmodctr : x.target, center0 = x.center;
        for (const auto& kv : keys) if (x.target == kv.first) { x.target = kv.second; break; }
        for (const auto& kv : keys) if (x.center == kv.first) { x.center = kv.second; break; }
        for (const auto& kv : keys) if (x.tmodctr == kv.first) { x.tmodctr = kv.second; break; }
        if (remapMainObj) {
            if ((x.tmodctr > 0 ? x.tmodctr : x.target) != ga0) x.mvaCol = false;
            if (x.center != center0) x.ctrCol = false;
        }
    }
    double curOvershoot_ = 0.0;   // how far past its delay the spawn that is running now got (a Spawn it triggers takes it off its own delay)
    // GJEffectManager's waiting-Spawn vector (+0x5b0, 0x48-byte entries) as the game keeps it: `pendLen_` live entries at the front of `pending_`; slots beyond it are stale, because the game's
    // erase moves the tail down and shortens the end by one entry without clearing the slot (controlActionsForControlID 0x368280), and updateSpawns' loop reads its entries by index up to the length it saw at its start
    std::vector<PendingSpawn> pending_;
    size_t pendLen_ = 0;
    void pendPush(const PendingSpawn& p) { if (pendLen_ < pending_.size()) pending_[pendLen_] = p; else pending_.push_back(p); ++pendLen_; }
    int spawnDepth_ = 0;
    std::set<int> spawnedStep_;   // the groups being spawned right now (one chain)
    long long spawnCountStep_ = 0;
    std::unordered_map<int, AreaDef> areaDefs_;   // trigger uid -> its Area Move (the instance's values: an Edit Area changes them)
    SharedConst<std::unordered_map<int, AreaDef>> areaOrig_;   // ...as the trigger object holds them (the Area firing again reloads them: EnterEffectInstance::loadValuesFromObject)
    SharedConst<std::unordered_map<int, int>> uncoveredObjBy_;   // object uid -> the (first) non-run Area whose target arrays reach it (a report)
    SharedConst<std::vector<std::pair<int, int>>> uncoveredAreas_;   // (trigger uid, target-group index) of the Areas the shadow does not run
    std::unordered_map<int, AreaEdit> areaEdits_;   // Edit Area trigger uid -> its changes
    struct AreaTrans { float start = 0, end = 0, dur = 0, el = 0, rate = 2; int type = 0, buf = -1; };
    std::unordered_map<int, std::map<int, AreaTrans>> areaTrans_;   // Area uid -> its running transitions by slot index (EnterEffectInstance's map: loadTransitions replaces the entry of a slot)
    static double* areaSlot(AreaDef& a, int idx) {
        switch (idx) { case 0: return &a.len; case 2: return &a.off; case 6: return &a.deadzone; case 11: return &a.moveX; case 13: return &a.moveY;
                       case 7: return &a.moveDist; case 15: return &a.scaleX; case 17: return &a.scaleY; case 25: return &a.offY; case 27: return &a.relFade;   // the wide set (--areaeditwide): the loader hands these over only then
                       case 21: case 22: case 23: case 24: { static thread_local double sink = 0.0; return &sink; } }   // the tint / opacity slots: nothing the shadow reads
        return nullptr;
    }
    // EnterEffectInstance::updateTransitions 0x20a810, once per processAreaEffects for an instance in force: the elapsed time (float) goes up by the step's dt; before the end the slot gets
    // (end - start) * eased + start (eased: -1 the share itself, -2 the curve, a table index the 101-point table of the curve), at the end the end value and the entry goes
    void updateTransitions(int uid) {
        const auto ti = areaTrans_.find(uid);
        if (ti == areaTrans_.end()) return;
        AreaDef& a = areaDefs_[uid];
        const float dt = physicsStepDt();
        for (auto it = ti->second.begin(); it != ti->second.end();) {
            AreaTrans& tr = it->second;
            tr.el = dt + tr.el;
            double* slot = areaSlot(a, it->first);
            if (tr.el < tr.dur) {
                float f = tr.el / tr.dur;
                if (tr.buf != -1) {
                    if (tr.buf == -2) f = eased(f, tr.type, tr.rate);
                    else {
                        const int k = (int)(f * 100.0f);
                        const float lo = eased((float)k / 100.0f, tr.type, tr.rate), hi = eased((float)(k + 1) / 100.0f, tr.type, tr.rate);
                        f = (f - (float)k * 0.01f) * (hi - lo) * 100.0f + lo;
                    }
                }
                if (slot) *slot = (double)((tr.end - tr.start) * f + tr.start);
                ++it;
            } else {
                if (slot) *slot = (double)tr.end;
                it = ti->second.erase(it);
            }
        }
    }
    // GJBaseGameLayer::triggerAreaEffect 0x32fe50 (ids 0xbc3..0xbc7) -> EnterEffectInstance::loadTransitions 0x209ae0 for each Area it finds
    void fireAreaEdit(const TrigDef& t) {
        const auto ei = areaEdits_.find(t.uid);
        if (ei == areaEdits_.end()) return;
        const AreaEdit& e = ei->second;
        std::vector<int> hit;
        if (e.useEffect) {   // by effect id: in each of the five instance lists the first one with that id (every one for an id of 0 or less)
            for (int di : areaInsts_) {
                const auto fx = areaFx_.find(di);
                if (fx != areaFx_.end() && fx->second == e.target) { hit.push_back(di); if (e.target > 0) break; }
            }
        } else {   // by group: each Area trigger object in the group (tryGetObject order), its instance
            const auto gm = members.find(e.target);
            if (gm != members.end())
                for (size_t oi : gm->second)
                    for (int di : areaInsts_) if (di == objs[oi].uid) hit.push_back(di);
        }
        for (int di : hit) {
            AreaDef& a = areaDefs_[di];
            for (const auto& fv : e.fields) {
                double* slot = areaSlot(a, fv.first);
                if (!slot) continue;
                if (e.dur > 0.0f) {
                    AreaTrans tr;
                    tr.start = (float)*slot; tr.end = fv.second; tr.dur = e.dur; tr.rate = e.rate; tr.type = e.type; tr.buf = e.buf;
                    areaTrans_[di][fv.first] = tr;
                } else *slot = (double)fv.second;
            }
        }
    }
    double curPx_ = 0, curPy_ = 0;       // the player's place after this step's update
    double prev2Px_ = 0, prev2Py_ = 0;   // ...and the one before that (a lock reads the displacement between the two)
    bool havePrev2_ = false, havePrevAny_ = false;
    double prevPx_ = 0, prevPy_ = 0;              // the player's place after the step before: the areas read it (they run ahead of the player's update)
    bool havePrev_ = false;
    Cam prevCam_, prev2Cam_;   // the camera of the step before and of the one before that (a camera-locked Move reads the displacement between the two)
    long long curTick_ = 0;
    SharedConst<std::unordered_map<int, std::vector<int>>> areaArrays_;                      // target-group arrays by layer index
    SharedConst<std::unordered_map<int, std::unordered_map<int, std::vector<int>>>> areaDicts_;   // ...and the parent -> children dictionaries
    long long areaStep_ = 0;
    std::vector<size_t> areaTouchedPrev_, areaTouchedCur_;   // objects whose offset was reset (touched) in the last step / this step
    std::map<long long, double> crossRef_;
    SharedConst<std::unordered_map<int, std::pair<int, bool>>> areaEditInfo_;
    SharedConst<std::unordered_map<int, int>> areaFx_;         // Area trigger uid -> its effect id (EnterEffectObject::m_effectID, areafx.txt), what an Area Stop matches
    std::vector<int> areaInsts_;               // the Area Moves in force, in the order they fired (GJBaseGameLayer's EnterEffectInstance vector)

    // GJBaseGameLayer::getEasedAreaValue 0x330eb0: -1 the value itself, -2 the curve, a table index the 101-point table of the curve (linear between)
    static float areaEaseWith(int buf, int type, double rate, float u) {
        if (buf == -1) return u;
        if (buf == -2) return eased(u, type, (float)rate);
        const int k = (int)(u * 100.0f);
        if (k >= 100) return eased(1.0f, type, (float)rate);
        const float lo = eased((float)k / 100.0f, type, (float)rate), hi = eased((float)(k + 1) / 100.0f, type, (float)rate);
        return (u - (float)k * 0.01f) * (hi - lo) * 100.0f + lo;
    }
    static float areaEase(const AreaDef& a, float u) { return areaEaseWith(a.eInBuf, a.eInType, a.eInRate, u); }
    // the Areas with a separate ease-out (AreaDef::eOutOn): per instance (the Area trigger's uid) a step counter that processAreaEffects adds 2 to once per step (instance +0xb8), and per object (its index
    // in the array the pass walks) a stamp (instance +0xc8): in the transit zone (0.01 < u < 0.99) the side is latched -- a stamp of step-2 / step-1 says the last step latched out (0) / in (1) and it stays;
    // anything else takes the side the object is on (the centre against the object, getAreaObjectValue's out flag); an object past the Area (u >= 1) clears its stamp
    struct AreaEO { int step = 0; CowPages<int, 9> stamp; };   // (one stamp per object of the level: shared between the copies of the shadow like objs, written through mut())
    std::unordered_map<int, AreaEO> areaEO_;
    bool areaMoveOf(const AreaDef& a, float c, float cRaw, double ox, double oy, size_t objIdx, float& dx, float& dy) {
        setVarObj(objIdx);
        const float pos = (float)(a.dirType == 2 ? oy : ox);
        const float u = areaU(a, ox, oy, c);
        if (a.eOutOn) {
            AreaEO& eo = areaEO_[a.uid];
            if (eo.stamp.size() != objs.size()) eo.stamp.assign(objs.size(), 0);
            if (!(u < 1.0f)) { if (eo.stamp[objIdx] != 0) eo.stamp.mut(objIdx) = 0; return false; }   // (written only when it changes: every object past the Area comes here every step)
            // the side flag (getAreaObjectValue's out flag): 1 = the ease-in curve, 0 = the ease-out curve. Measured on lv22's two eOut Areas (inbound = 1, two-sided; the saws, uid 13870 ...): the ease-in curve
            // while the object is ahead of the centre (the player approaching it), the ease-out curve once the player has passed.
            // Measured on 9 levels with an Area that has a separate ease-out (inbound = 1 in lv22, 0 in c149725478; 0 differences on 8 of them): the ease-in curve while the object is not behind the centre (pos >= centre), whatever
            // the Area's inbound. (getAreaObjectValue 0x330cc0 compares by an instance byte at +0xa8, not by the object's inbound flag +0x7ec: read as 0 on all of them. Taking `inbound` for it, as this port did first,
            // put 888 objects of c149725478 up to 31 px off.)
            int side = (pos >= cRaw) ? 1 : 0;
            if (u > 0.01f && u < 0.99f) {
                int& st = eo.stamp.mut(objIdx);
                const int s = eo.step;
                if (st == s - 2) { side = 0; st = s; }
                else { int v = side; if (st == s - 1) v = 1; side = v; st = v + s; }
            }
            const float e = side == 0 ? areaEaseWith(a.eOutBuf, a.eOutType, a.eOutRate, u) : areaEase(a, u);
            return areaDispAt(a, e, ox, oy, dx, dy);
        }
        if (!(u < 1.0f)) return false;
        return areaDispAt(a, areaEase(a, u), ox, oy, dx, dy);
    }
    // the variation table V (GJBaseGameLayer +0x11ac, 2000 floats) and the objects' m_varianceIndex (GameObject +0x3f4), from areavar.txt (setVariance); empty = unknown, and an Area with a variation term is then not run
    std::shared_ptr<const std::vector<float>> varTab_;
    std::shared_ptr<const std::vector<int>> varIdx_;   // by object index in objs
    int arVi_ = 0;                                      // the variance index of the object the Area is being evaluated for
public:
    bool varKnown() const { return varTab_ != nullptr && varIdx_ != nullptr; }
    void setVariance(std::vector<float> tab, const std::unordered_map<int, int>& idxOfUid) {
        auto vi = std::make_shared<std::vector<int>>(objs.size(), 0);
        for (const auto& kv : idxOfUid) { const auto it = byUid.find(kv.first); if (it != byUid.end()) (*vi)[it->second] = kv.second; }
        varTab_ = std::make_shared<const std::vector<float>>(std::move(tab));
        varIdx_ = std::move(vi);
    }
private:
    float vAt(int k) const { const int i = arVi_ + k; return (varTab_ && i >= 0 && (size_t)i < varTab_->size()) ? (*varTab_)[(size_t)i] : 0.0f; }
    void setVarObj(size_t i) { arVi_ = (varIdx_ && i < varIdx_->size()) ? (*varIdx_)[i] : 0; }
    // GJBaseGameLayer::getAreaObjectValue 0x330cc0 (direction type 1: x, 2: y) for an object at `pos`, the centre at `c` (the offset already added)
    float areaValue(const AreaDef& a, float pos, float c) const {
        const float s0 = a.offV == 0 ? pos - c : (pos - c) + (float)a.offV * vAt(1);   // + offV * V[idx + 1] moves the object's coordinate (dirType 1 / 2)
        const float s = s0 * (s0 < 0.0f ? (float)a.modFront : (float)a.modBack);
        const float x = s / (float)areaLen(a);
        const float dz = (float)a.deadzone;
        float r = dz == 0.0f ? x : (x - dz) / (1.0f - dz);
        r = r > 1.0f ? 1.0f : (r < 0.0f ? 0.0f : r);
        return a.inbound ? 1.0f - r : r;
    }

    // the length the value is divided by: (int)(lenV * V[idx] + len) (getAreaObjectValue 0x330e3a..0x330e67); a constant Area keeps (int)len
    int areaLen(const AreaDef& a) const { return a.lenV == 0 ? (int)a.len : (int)((float)a.lenV * vAt(0) + (float)a.len); }
    // the tail of getAreaObjectValue for a distance s (FUN_1403307e0 and the inbound flip), as areaValue ends
    float areaTail(const AreaDef& a, float s) const {
        const float x = s / (float)areaLen(a);
        const float dz = (float)a.deadzone;
        float r = dz == 0.0f ? x : (x - dz) / (1.0f - dz);
        r = r > 1.0f ? 1.0f : (r < 0.0f ? 0.0f : r);
        return a.inbound ? 1.0f - r : r;
    }
    float arCx_ = 0.0f, arCy_ = 0.0f;   // the centre point of the Area being walked, without its offset (stepAreas)
    // the value for an object at (ox, oy): the distance to the centre point + (off, offY) for direction type 0, else the one-axis value (areaValue; `c` the centre with the offset on that axis)
    float areaU(const AreaDef& a, double ox, double oy, float c) const {
        if (a.dirType != 0) return areaValue(a, (float)(a.dirType == 2 ? oy : ox), c);
        const float tx = a.offV == 0 ? (float)a.off + arCx_ : (float)a.offV * vAt(1) + ((float)a.off + arCx_), ty = a.offYV == 0 ? (float)a.offY + arCy_ : (float)a.offYV * vAt(2) + ((float)a.offY + arCy_);   // dirType 0: the variance moves the centre point
        const float dx = (float)ox - tx, dy = (float)oy - ty;
        return areaTail(a, std::sqrt(dx * dx + dy * dy));
    }
    // the displacement of one Area for an object at (ox, oy) with the eased value: along the centre -> object line for `relative`, else areaMoveDisp
    bool areaDispAt(const AreaDef& a, float easedU, double ox, double oy, float& dx, float& dy) const {
        if (!(a.dirType == 0 && a.relative && a.angleMode)) return areaMoveDisp(a, easedU, dx, dy);
        const float dist = a.moveDistV == 0 ? (float)a.moveDist : (float)a.moveDistV * vAt(10) + (float)a.moveDist;
        if (dist == 0.0f) return false;
        const float vx = (float)ox - arCx_, vy = (float)oy - arCy_;
        const float len = std::sqrt(vx * vx + vy * vy);
        float f = (1.0f - easedU) * dist;
        const float fade = std::max(0.0f, (float)a.relFade);
        if (len < fade) f = (1.0f - easedU) * (dist * len * (1.0f / fade));
        if (!(len > 0.0f)) { dx = 0.0f; dy = 0.0f; return true; }
        dy = f * (vy / len); dx = f * (vx / len);
        return true;
    }
    // processAreaEffects 0x331030, once per step here. resetAreaObjectValues (0x3308a0) takes the offset the last step added off an object's position, at most once per step (its stamp) -- it is called
    // for every object a call looks at, and by moveAreaObject for every object it moves; then each Area's displacement (processAreaMoveGroupAction 0x333120 -> moveAreaObject 0x333700:
    // float offsets added to a double position) goes on. An Area with a target-group index (AreaDef::index >= 1) runs in two passes: the objects that have no parent (array [index - 1]), each at
    // its own place, and the parents (array [index]), each parent's place deciding one value that moves all its children (dictionary [index + 1]); a child is reset only when it is moved,
    // so it keeps its last offset while its parent's value is 1.
    bool blindAny_ = false;   // some object is blind (sticky like the objects' own bit; derived from them, so not in contentHash)
    bool blindSticky_ = false;           // some object is blind for good (blindAny_ falls back to this when the Areas' marks come off)
    std::vector<size_t> areaBlind_;      // the objects setBlindArea marked since the last certain step (derived from the objects' bits, so not in contentHash)
    void setBlind(size_t i) { blindSticky_ = true; if (!objs[i].blind || objs[i].blindArea) { ObjState& o = objs.mut(i); o.blind = true; o.blindArea = false; blindAny_ = true; } }   // (sticky, so an object already blind for good is not written again; one an Area marked becomes blind for good)
    // A screen Area that ran with an uncertain camera, in the one shape whose result holds nothing of the steps before: its target is blind only until the camera is certain again.
    // [R] 2.2091: FUN_140331c00 (base+0x331c00) runs processAreaEffects (0x331030) three times a step and then takes the offset off every object the step before touched and this one did not
    // (resetAreaObjectValues 0x3308a0: once per step by the stamp +0x4e0, the float offsets +0x2a0 / +0x2a4 subtracted from the place +0x3b8 / +0x3c0); processAreaEffects computes the centre from the
    // camera position (+0x410) and the window size over the zoom on every call; processAreaMoveGroupAction (0x333120) resets each object it looks at, takes getAreaObjectValue (0x330cc0: the object's
    // place, the centre and the instance's fields only) and adds the displacement (moveAreaObject 0x333700). So the offset inside an object's place is made within the step. What does reach across steps:
    // (1) a separate ease-out (+0x7c5): getEasedAreaValue 0x330eb0 reads and writes the per-object stamp (instance +0xc8) only then; (2) the section test (the object's section, filed by its place WITH
    // the last step's offset) -- it lets everything through for direction type 1, inbound, two-sided (processAreaEffects: lower bounds 0, upper bounds the last section); (3) a target-group index
    // (parents / children: a child keeps its offset while its parent's value is 1). An Area Scale / Transform (0x331e60) was not read for this. Those shapes stay blind for good.
    // [M] a custom level t=13400: group 488 (Area Moves 54995 / 54996) was blinded at step 5018 (a Static Camera's exit window) and the shadow's place of uid 28680 at t=13401 / 13446 / 13449 is the game's row to 0.001 px.
    static bool areaStateless(const AreaDef& a) { return a.dirType == 1 && a.inbound && a.twoDir && !a.eOutOn && !a.scaleKind && a.index < 1; }
    void setBlindArea(size_t i) { if (!objs[i].blind) { ObjState& o = objs.mut(i); o.blind = true; o.blindArea = true; areaBlind_.push_back(i); blindAny_ = true; } }
public:
    bool anyBlind() const { return blindAny_; }
private:
    void resetArea(size_t i) {
        if (objs[i].areaStamp == areaStep_) return;   // (read first: an object already reset this step is not written, so its page is not cloned for it)
        ObjState& o = objs.mut(i);
        o.areaStamp = areaStep_;
        o.x -= (double)o.ax; o.y -= (double)o.ay; o.ax = 0.0f; o.ay = 0.0f; o.asx = 1.0f; o.asy = 1.0f;
        areaTouchedCur_.push_back(i);
    }
    void moveArea(size_t i, float dx, float dy, int trigUid) {
        resetArea(i);
        ObjState& o = objs.mut(i);
        if (dy != 0.0f) { o.ay = dy + o.ay; o.y += (double)dy; }
        if (dx != 0.0f && !o.areaXLock) { o.ax = dx + o.ax; o.x += (double)dx; }
        o.lastTrig = trigUid; o.lastTrigId = 3006;
    }
    // an Area Scale (processAreaTransformGroupAction 0x331e60, no target-group index): the object's factor f = (S - 1) * (1 - eased) + 1 per axis (float), for the object's place and the centre c; the factors of
    // several Areas multiply (each works on the size the one before left). false = none (u >= 1, or S = 0 on both axes)
    bool areaScaleOf(const AreaDef& a, float c, double ox, double oy, float& fx, float& fy, size_t objIdx = (size_t)-1) {
        setVarObj(objIdx);
        const float u = areaU(a, ox, oy, c);
        if (!(u < 1.0f)) return false;
        const float sx = (float)a.scaleX, sy = (float)a.scaleY;
        if (sx == 0.0f && sy == 0.0f) return false;
        const float e = areaEase(a, u);
        fx = (sx - 1.0f) * (1.0f - e) + 1.0f; fy = (sy - 1.0f) * (1.0f - e) + 1.0f;
        return true;
    }
    void scaleArea(size_t i, float fx, float fy, int trigUid) {
        resetArea(i);
        ObjState& o = objs.mut(i);
        o.asx = o.asx * fx; o.asy = o.asy * fy;
        o.lastTrig = trigUid; o.lastTrigId = 3008;
    }
    // processAreaEffects' range of sections along x for a direction-1 Area (the centre c WITHOUT the offset): which objects processAreaMoveGroupAction looks at at all. lower / upper are
    // (c -/+ R) in sections (R = (len + off) / the smaller of the two side multipliers); one-sided Areas read from 0 or to infinity: outbound + two-sided [lower, upper], inbound + one-sided
    // [lower, inf), outbound + one-sided [0, upper], inbound + two-sided everything.
    void areaRange(const AreaDef& a, float c, int& lo, int& hi) const {
        const float kSec = 0.01f;
        const float mF = (float)a.modFront, mB = (float)a.modBack;
        const float m = std::fabs(mB) <= std::fabs(mF) ? mB : mF;
        const float R = (1.0f / std::fabs(m)) * (float)(int)(a.lenV == 0 && a.offV == 0 ? (float)a.len + (float)a.off : (((float)a.lenV + (float)a.len) + (float)a.off) + (float)a.offV);   // 0x3314ab: the variances are plain addends here (no V)
        auto sec = [&](float v) -> int { return v > 0.0f ? (int)(v < 1e7f ? v * kSec : kSec * 1e7f) : 0; };
        const int lower = sec(c - R), upper = sec(c + R), big = (int)(kSec * 1e7f);
        if (!a.inbound) { lo = a.twoDir ? lower : 0; hi = upper; }
        else { lo = a.twoDir ? 0 : lower; hi = big; }
    }
    // the displacement of one Area for an object whose place (after its own reset) is (ox, oy), the centre at c; false = none (u >= 1, or the Area moves nothing)
    bool areaMoveDisp(const AreaDef& a, float easedU, float& dx, float& dy) const {
        const float k = 1.0f - easedU;
        if (a.angleMode) {   // (1 - eased) * the distance, along the direction (angle 0: down: measured on c127737483, a positive distance moves the objects down)
            // processAreaMoveGroupAction: distance = (distV == 0 ? 0 : distV * V[idx + 10]) + moveDist, angle = V[idx + 11] * angleV + angle (the spec 1030 note, section 2)
            const float dist = a.moveDistV == 0 ? (float)a.moveDist : (float)a.moveDistV * vAt(10) + (float)a.moveDist;
            if (dist == 0.0f) return false;
            const float f = k * dist;
            if (a.angleV != 0) {
                const float ang = vAt(11) * (float)a.angleV + (float)a.angle;
                const float rad = (ang - 90.0f) * 0.017453292f;
                dx = f * std::cos(rad); dy = f * std::sin(rad); return true;
            }
            if (a.angle == 0.0) { dx = f * 0.0f; dy = f * -1.0f; return true; }
            // ccpForAngle: (cos, sin) of the angle in radians; 0 degrees is "down" (the dump's moveAngle counts from there), so the direction is (cos, sin) of (angle - 90) degrees (measured: 180 up, 90 right, -90 left; the other sign is 119 objects off on c142671515)
            const float rad = (float)((a.angle - 90.0) * 0.017453292519943295);
            dx = f * std::cos(rad); dy = f * std::sin(rad);
            return true;
        }
        const float mx = a.moveXV == 0 ? (float)a.moveX : vAt(8) * (float)a.moveXV + (float)a.moveX, my = a.moveYV == 0 ? (float)a.moveY : vAt(9) * (float)a.moveYV + (float)a.moveY;
        if (mx == 0.0f && my == 0.0f) return false;
        dx = k * mx; dy = k * my;
        return true;
    }
    void stepAreas(double px, double py) {
        if (prevCam_.ok && !areaBlind_.empty()) {   // the camera is certain again: every screen Area in force is computed afresh below, so the marks of the stateless shape come off (setBlindArea)
            for (size_t i : areaBlind_) if (objs[i].blindArea) { ObjState& o = objs.mut(i); o.blind = false; o.blindArea = false; }
            areaBlind_.clear(); blindAny_ = blindSticky_;
        }
        if (areaInsts_.empty() && areaTouchedPrev_.empty()) return;
        ++areaStep_;
        struct EndOfStep {   // FUN_140331c00, after the three processAreaEffects: an object touched in the step before and not in this one is reset (its offset taken off), then this step's set becomes the next one's
            TrigShadow& s;
            ~EndOfStep() {
                for (size_t i : s.areaTouchedPrev_) {
                    if (s.objs[i].areaStamp == s.areaStep_) continue;
                    ObjState& o = s.objs.mut(i);
                    o.x -= (double)o.ax; o.y -= (double)o.ay; o.ax = 0.0f; o.ay = 0.0f; o.asx = 1.0f; o.asy = 1.0f;
                }
                s.areaTouchedPrev_.swap(s.areaTouchedCur_);
                s.areaTouchedCur_.clear();
            }
        } endOfStep{*this};
        std::vector<int> order = areaInsts_;
        if (areaScaleFirst) std::stable_sort(order.begin(), order.end(), [&](int a, int b) { const auto ia = areaDefs_.find(a), ib = areaDefs_.find(b); return (ia != areaDefs_.end() && ia->second.scaleKind) && !(ib != areaDefs_.end() && ib->second.scaleKind); });
        for (int di : order) {
            updateTransitions(di);
            const AreaDef& a = areaDefs_[di];
            if (a.eOutOn) areaEO_[di].step += 2;   // processAreaEffects: instance +0xb8 += 2
            float cx = (float)px, cy = (float)py;
            if (a.special <= -3) {   // a point of the screen: processAreaEffects 0x331030, the camera position plus the window size / zoom times (0, .5 or 1)
                if (!prevCam_.ok) {   // the camera of the step before is not certain: the area did nothing here that the shadow knows of -- its target stays where the shadow has it, which is not the game's: blind (the supply leaves it to the model)
                    if (a.index < 1) { const auto bm = members.find(a.target); if (bm != members.end()) { if (areaStateless(a)) for (size_t i : bm->second) setBlindArea(i); else for (size_t i : bm->second) setBlind(i); } }
                    else {
                        const auto pl = areaArrays_.find(a.index - 1);
                        if (pl != areaArrays_.end()) for (int uid : pl->second) { const auto bi = byUid.find(uid); if (bi != byUid.end()) setBlind(bi->second); }
                        // ...and the kids of the parents in the second array (areaDicts_[index + 1]): with a certain camera they take the parent's displacement (the loop below), so with an uncertain one they stand where the shadow has them, not where
                        // the game has them (a custom level t=9401: group 2708's 55 parents, e.g. uid 99889, area 100288 of the screen's middle; the camera model was uncertain at all 9801 steps and none of them was blind, so the supply
                        // wrote their load place while GD's rows moved them)
                        const auto pa = areaArrays_.find(a.index);
                        const auto kd = areaDicts_.find(a.index + 1);
                        if (pa != areaArrays_.end() && kd != areaDicts_.end())
                            for (int uid : pa->second) {
                                const auto ks = kd->second.find(uid);
                                if (ks == kd->second.end()) continue;
                                for (int kid : ks->second) { const auto ki = byUid.find(kid); if (ki != byUid.end()) setBlind(ki->second); }
                            }
                    }
                    continue;
                }
                const int s = -a.special;   // 3 middle, 4..6 left (bottom, middle, top), 7..8 centre column (bottom, top), 9..11 right (bottom, middle, top)
                const double fx = s == 3 ? 0.5 : (s <= 6 ? 0.0 : (s <= 8 ? 0.5 : 1.0));
                const double fy = s == 3 || s == 5 || s == 10 ? 0.5 : (s == 4 || s == 7 || s == 9 ? 0.0 : 1.0);
                cx = (float)prevCam_.l + (float)fx * (float)prevCam_.w; cy = (float)prevCam_.b + (float)fy * (float)prevCam_.h;
            } else if (a.special == 0) {   // an object centre: the first member of its group, at its place without the offset
                const auto cm = members.find(a.center);
                if (cm == members.end() || cm->second.empty()) continue;
                resetArea(cm->second.front());
                const ObjState& c = objs[cm->second.front()];
                cx = (float)c.x; cy = (float)c.y;
            }
            arCx_ = cx; arCy_ = cy;
            const float c = (a.dirType == 2 ? cy : cx) + (float)a.off;
            float dx = 0, dy = 0;
            int secLo = 0, secHi = 1 << 30;
            if (a.dirType == 1 && sectionWindow) areaRange(a, (a.dirType == 2 ? cy : cx), secLo, secHi);
            // GJBaseGameLayer::updateObjectSection 0x330ba0 re-files an object by its current x (the double at +0x3b8, area offset included), so a moved object is looked at where it is now
            auto inRange = [&](size_t i) {
                const ObjState& o = objs[i];
                if (o.areaAlways) return true;
                const double xx = o.x;
                const int sx = xx > 0.0 ? (int)(xx < 10000000.0 ? (double)0.01f * xx : (double)(0.01f * 1e7f)) : 0;
                return sx >= secLo && sx <= secHi;
            };
            if (a.index < 1) {
                const auto m = members.find(a.target);
                if (m == members.end()) continue;
                for (size_t i : m->second) {
                    if (!inRange(i)) continue;
                    resetArea(i);
                    if (a.scaleKind) { float fx = 1, fy = 1; if (areaScaleOf(a, c, objs[i].x, objs[i].y, fx, fy, i)) scaleArea(i, fx, fy, a.uid); continue; }
                    if (areaMoveOf(a, c, a.dirType == 2 ? cy : cx, objs[i].x, objs[i].y, i, dx, dy)) moveArea(i, dx, dy, a.uid);
                }
            } else {
                const auto pl = areaArrays_.find(a.index - 1);
                const auto pa = areaArrays_.find(a.index);
                const auto kd = areaDicts_.find(a.index + 1);
                if (pl == areaArrays_.end() || pa == areaArrays_.end() || kd == areaDicts_.end()) continue;
                for (int uid : pl->second) {
                    const auto bi = byUid.find(uid);
                    if (bi == byUid.end() || !inRange(bi->second)) continue;
                    resetArea(bi->second);
                    if (a.scaleKind) { float fx = 1, fy = 1; if (areaScaleOf(a, c, objs[bi->second].x, objs[bi->second].y, fx, fy, bi->second)) scaleArea(bi->second, fx, fy, a.uid); continue; }
                    if (areaMoveOf(a, c, a.dirType == 2 ? cy : cx, objs[bi->second].x, objs[bi->second].y, bi->second, dx, dy)) moveArea(bi->second, dx, dy, a.uid);
                }
                for (int uid : pa->second) {
                    const auto bi = byUid.find(uid);
                    if (bi == byUid.end() || !inRange(bi->second)) continue;
                    resetArea(bi->second);
                    if (!areaMoveOf(a, c, a.dirType == 2 ? cy : cx, objs[bi->second].x, objs[bi->second].y, bi->second, dx, dy)) continue;
                    const auto ks = kd->second.find(uid);
                    if (ks == kd->second.end()) continue;
                    for (int kid : ks->second) {
                        const auto ki = byUid.find(kid);
                        if (ki != byUid.end() && !objs[ki->second].areaKidSkip) moveArea(ki->second, dx, dy, a.uid);
                    }
                }
            }
        }
    }
    static bool ignorable(int id) {
        static const int extra = [] { const char* e = std::getenv("TSIGNORE_ID"); return e ? std::atoi(e) : -1; }();   // diagnosis only: treat this trigger id as one that moves nothing (to see what the coverage would be)
        // 1585 Animate: EnhancedGameObject::animationTriggered (2.2081 base+0x1a4b70) sets the object's animation state (+0x5a1, +0x306, the frame counter) and starts the animation through its vtable; it reads or writes no position, no
        // disable counter (+0x4c0 / +0x28e) and no hitbox. It is in the editor-icon look-only table too (triggers.hpp actLookOnlyId). lv22 uid 757 (touch, group 66) kept group 66, and with it the blocks of group 95 that share objects with
        // it, uncovered: 37 of 43 members covered, group 95 NOT supplied, and the window 1101 (the ship against the rising blocks 729 / 730 / 835 / 5760) lost the platform's step.
        // The shader triggers (2904 / 2905 / 2907 / 2909..2917 / 2919..2924): EffectGameObject::triggerObject (2.2081) sends these ids (cases 0xb58 .. 0xb6c without 0xb5a / 0xb5c / 0xb66) to GJBaseGameLayer::triggerShaderCommand, which calls only
        // resetAllShaders / timesyncShaderAction / tweenValue and writes the shader layer's own settings (+0x2ec .. +0x30c of its state): no position, no disable counter, no hitbox, no player. [R] decompile, read 11-17 (gdghidra --decomp
        // EffectGameObject::triggerObject, GJBaseGameLayer::triggerShaderCommand); the editor-icon look-only table (triggers.hpp actLookOnlyId) lists the same ids. lv22 2913 (x 20805 / 20835, group 344) and 2919 (spawn, x 4503 / 9285, groups 209 / 288)
        // kept groups 432 (187 of 188 members covered), 93 and 288 NOT supplied (39 movers with no supply slot, gd-08's list).
        const bool shader = (id >= 2904 && id <= 2924) && id != 2906 && id != 2908 && id != 2918;
        return id == 1006 || id == 1007 || id == 1585 || id == 3009 || id == 3010 || id == 3014 || id == 3015 || shader || id == extra;
    }

    // FUN_140361e80: the eased share of a command's total, as float
    static double share(const Cmd& c, double total) {
        const double d = std::max(c.dur, 1.1920928955078125e-07);
        const double ratio = std::min(std::max((double)c.el / d, 0.0), 1.0);
        const float f = eased((float)ratio, c.kind, (float)c.rate);
        const float s = (float)total - (1.0f - f) * (float)total;
        return (double)s;
    }

    // A group's Move node of one step (GJEffectManager's move node: +0x38 / +0x40, the sum of its commands' steps): processActions (FUN_140334220) runs the Rotations first (FUN_140334450), and a Rotation
    // about a centre takes the node's total into the position it writes (FUN_140336780) and marks the node used; processMoveActions then moves the members of every node still unused by the total in float
    static double camCenterX(const Cam& c) { return c.l + c.w * 0.5; }   // the camera's centre: left edge + half the visible width (569 / zoom); measured on a custom level g77 against wtrace, the lock follows its displacement of the step before
    static double camCenterY(const Cam& c) { return c.b + c.h * 0.5; }
    struct MoveNode { double dx = 0, dy = 0; bool locked = false, used = false; int uid = 0, id = 0; };
    std::map<int, MoveNode> moveNodes_;
    void stepCommands() {
        moveNodes_.clear();
        std::vector<std::pair<const Cmd*, double>> rots;
        std::vector<std::pair<Cmd*, double>> scales;
        for (Cmd& c : cmds_) {
            if (c.done || c.paused) continue;
            const float dtf = physicsStepDt();
            c.time += (double)dtf;   // GroupCommandObject2::step 0x361db0: the clock runs on every step, the elapsed time of an eased command skips the first
            if (c.skip) c.skip = false;
            else c.el = c.el + dtf;
            if (c.scale) {
                scales.push_back({&c, share(c, 1.0)});
            } else if (!c.rotate) {
                const double sx = share(c, c.dx), sy = share(c, c.dy);
                double mx = sx - c.lastX, my = sy - c.lastY;
                c.lastX = sx; c.lastY = sy;
                const bool locked = c.lockPX || c.lockPY || c.lockCX || c.lockCY;
                if (locked) {   // FUN_140369ae0 case 0: the lock delta is not gated by the first-step skip; it is the player's displacement of the step before (GJBaseGameLayer::update 0x340f70), in float
                    if (c.lockPX) mx = havePrev2_ ? (double)((float)prevPx_ - (float)prev2Px_) * c.modX : 0.0;
                    else if (c.lockCX) mx = (prevCam_.ok && prev2Cam_.ok) ? (double)(float)(camCenterX(prevCam_) - camCenterX(prev2Cam_)) * c.modX : 0.0;   // FUN_140369ae0 case 0: the player lock first, else the camera lock (command byte +0x75)
                    if (c.lockPY) {
                        float d = havePrev2_ ? (float)prevPy_ - (float)prev2Py_ : 0.0f;
                        if (std::fabs(d) > 4.0f) d = 0.0f;   // 16 * (frame dt * 60 / sub-steps): a jump of the player (a portal) does not drag the group
                        my = (double)d * c.modY;
                    }
                    if ((c.lockCX && !(prevCam_.ok && prev2Cam_.ok)) || (c.lockCY && !(prevCam_.okY && prev2Cam_.okY))) {   // the camera of this step is not certain: the displacement above is 0, not the game's (see camLockBlind): the members of the target are blind from here on (the other groups' places are not touched by it)
                        camLockBlind = true;
                        const auto bm = members.find(c.target);
                        if (bm != members.end()) for (size_t i : bm->second) setBlind(i);
                    }
                    if (!c.lockPY && c.lockCY) my =(prevCam_.okY && prev2Cam_.okY) ? (double)(float)(camCenterY(prevCam_) - camCenterY(prev2Cam_)) * c.modY : 0.0;
                    { static const int perturb = [] { const char* e = std::getenv("TSCAM_PERTURB"); return e ? std::atoi(e) : 0; }();   // diagnosis: 1 = no camera Y, 2 = the camera of THIS step (no lag), 3 = the camera left edge in place of its centre
                      if (perturb == 1 && c.lockCY && !c.lockPY) my = 0.0;
                      if (perturb == 2 && c.lockCX && !c.lockPX) mx = (cam.ok && prevCam_.ok) ? (double)(float)(camCenterX(cam) - camCenterX(prevCam_)) * c.modX : 0.0;
                      if (perturb == 3 && c.lockCX && !c.lockPX) mx = (prevCam_.ok && prev2Cam_.ok) ? (double)(float)(prevCam_.l - prev2Cam_.l) * c.modX : 0.0; }
                    mx = (double)(float)mx; my = (double)(float)my;   // processMoveActions hands moveObjects (double)(float)node delta
                    static const char* const envTsLock = std::getenv("TSLOCK");   // read once (shadow_step.hpp shDbgUid)
                    if (const char* ld = envTsLock) { static int shownL = 0; if (c.uid == std::atoi(ld) && shownL < 100000) { ++shownL; std::printf("tslock: cmd uid %d step %lld px %.6f prevPx %.6f prev2Px %.6f have2 %d mx %.6f my %.6f time %.6f el %.6f\n", c.uid, stepNo_, curPx_, prevPx_, prev2Px_, havePrev2_ ? 1 : 0, mx, my, c.time, c.el); } }
                }
                MoveNode& n = moveNodes_[c.target];
                n.dx += mx; n.dy += my; n.locked = n.locked || locked;
                if (mx != 0.0 || my != 0.0) { n.uid = c.uid; n.id = c.id; }
                if (locked && !(c.dx != 0.0 || c.dy != 0.0)) {   // no eased axis: it ends by its clock, dur -1 never
                    if (c.dur <= c.time && c.dur != -1.0) c.done = true;
                    continue;
                }
            } else {
                const double s = share(c, c.total);
                const double dth = s - c.lastR;
                c.lastR = s;
                if (dth != 0.0) rots.push_back({&c, dth});
            }
            if ((double)c.el > 0.0 && (double)c.el >= c.dur) c.done = true;
        }
        // GJBaseGameLayer::claimRotationAction 0x336530: the Rotations of one (target group, centre group) pair that are due in a step are ONE rotation, by the float sum of their angles (the first one's own, the
        // others' added in order; the angle for the objects' own turning leaves out the ones that keep it, lockrot)
        std::vector<char> taken(rots.size(), 0);
        for (size_t k = 0; k < rots.size(); ++k) {
            if (taken[k]) continue;
            taken[k] = 1;
            const Cmd& c0 = *rots[k].first;
            float f4 = (float)rots[k].second, f8 = c0.lockrot ? 0.0f : f4;
            for (size_t j = k + 1; j < rots.size(); ++j) {
                if (taken[j]) continue;
                const Cmd& cj = *rots[j].first;
                if (cj.target != c0.target || cj.center != c0.center) continue;
                taken[j] = 1;
                const float d = (float)rots[j].second;
                f4 = d + f4;
                if (!cj.lockrot) f8 = d + f8;
            }
            rotateGroup(c0, (double)f4, (double)f8);
        }
        for (auto& sc : scales) { scaleGroup(*sc.first, sc.second); sc.first->lastE = sc.second; }   // (a Rotate due in the same step for the same pair is folded into the transform by the game: not done)
        for (auto& kv : moveNodes_) {   // processMoveActions 0x336170 -> moveObjects 0x3368d0 (float totals; a node a Rotation used is skipped)
            MoveNode& n = kv.second;
            if (n.used) continue;
            const double mx = (double)(float)n.dx, my = (double)(float)n.dy;
            if (mx == 0.0 && my == 0.0) continue;
            const auto it = members.find(kv.first);
            if (it == members.end()) continue;
            for (size_t i : it->second) {
                ObjState& o = objs.mut(i);
                if (o.areaXLock) o.x += 0.0; else o.x += mx;   // moveObjects 0x3368d0 drops x for an object with +0x2c8 (a Move of any kind: the call has no lock argument)
                o.y += my;
                o.lastTrig = n.uid; o.lastTrigId = n.id;
            }
        }
        cmds_.erase(std::remove_if(cmds_.begin(), cmds_.end(), [](const Cmd& c) { return c.done; }), cmds_.end());
    }

    // Rotate about a centre: the centre is the first object of the centre group (tryGetObject); every member turns round it, clockwise for a positive angle (y up)
    void rotateGroup(const Cmd& c, double dthDeg, double ownDeg) {
        const auto it = members.find(c.target);
        if (it == members.end()) return;
        // FUN_140334450: the centre object is looked up by the centre group (FUN_14032c200); with none (no centre group, or an empty one) the members only turn on the spot -- addRotation, no change of place
        // A dump with the ctrobj column says which object the game took for the centre group (tryGetMainObject: the group's registered parent, else its only member; -1 none, 0 no centre group):
        // that one is the centre, and with none the members turn on the spot. Without the column, the centre group's first member.
        size_t ci = 0; bool haveCentre = false;
        if (c.ctrCol) {
            if (c.ctrObj > 0) { const auto bi = byUid.find(c.ctrObj); if (bi != byUid.end()) { ci = bi->second; haveCentre = true; } }
        } else {
            const auto ct = c.center ? members.find(c.center) : members.end();
            if (ct != members.end() && !ct->second.empty()) { ci = ct->second.front(); haveCentre = true; }
        }
        if (!haveCentre) {
            for (size_t i : it->second) {
                ObjState& o = objs.mut(i);
                if (ownDeg != 0.0 && !o.noOwnRot) o.rot += ownDeg;
                o.lastTrig = c.uid; o.lastTrigId = c.id;
            }
            return;
        }
        if (objs[ci].blind) for (size_t i : it->second) setBlind(i);   // turned about a place that is not the game's: so is the result
        if (floatRotate) {
            // processRotationActions (FUN_140334450) 0x334450, in float as the game does it: the node it turns (CCNode: rotation theta, position = the centre) gives
            // x' = a x + c y + tx, y' = b x + d y + ty (in double inside, then float) of p = the object's place less its area offset less the centre's; the object goes to (float)(group move + q) plus its area offset.
            // The group's own Move of the step (FUN_140336780) is not read here: this is exact only while the group is not being moved in the same step.
            const float th = (float)dthDeg;
            const float rad = -(th * 0.01745329252f);
            const float cs = std::cos(rad), sn = std::sin(rad);
            const ObjState& C = objs[ci];   // (read into cox / coy here, before the loop writes the members: the centre may be one of them)
            const float cox = (float)(C.x - (double)C.ax), coy = (float)(C.y - (double)C.ay);
            float mvx = 0.0f, mvy = 0.0f;   // FUN_140336780: the group's Move node of the step, once (the node is then used up)
            {
                const auto nd = moveNodes_.find(c.target);
                if (nd != moveNodes_.end() && !nd->second.used) { nd->second.used = true; mvx = (float)nd->second.dx; mvy = (float)nd->second.dy; }
            }
            for (size_t i : it->second) {
                ObjState& o = objs.mut(i);
                const float px = (float)((o.x - (double)o.ax) - (double)cox), py = (float)((o.y - (double)o.ay) - (double)coy);
                const float qx = mvx + (float)((double)cs * px + (double)(-sn) * py + (double)cox), qy = mvy + (float)((double)sn * px + (double)cs * py + (double)coy);
                o.x = (((double)qx - o.x) + (double)o.ax) + o.x;
                o.y = (((double)qy - o.y) + (double)o.ay) + o.y;
                if (ownDeg != 0.0 && !o.noOwnRot) o.rot = rotFloat ? (double)((float)o.rot + (float)ownDeg) : o.rot + ownDeg;
                o.lastTrig = c.uid; o.lastTrigId = c.id;
            }
            return;
        }
        const double a = dthDeg * 3.14159265358979323846 / 180.0;
        const double ca = std::cos(a), sa = std::sin(a);
        const double cxp = objs[ci].x, cyp = objs[ci].y;
        for (size_t i : it->second) {
            ObjState& o = objs.mut(i);
            if (i != ci || c.center) {
                const double ox = o.x - cxp, oy = o.y - cyp;
                if (i != ci) { o.x = cxp + ox * ca + oy * sa; o.y = cyp - ox * sa + oy * ca; }
            }
            if (ownDeg != 0.0 && !o.noOwnRot) o.rot += ownDeg;
            o.lastTrig = c.uid; o.lastTrigId = c.id;
        }
    }

    // A Transform that scales (FUN_140335280 0x335280, in float as the game does it): the scale reached this step is (S - 1) * e + 1, the one of the step before (S - 1) * lastE + 1, each held off 0 (|s| < 0.01 becomes 0.01);
    // the members' places (less their area offsets) are taken from the centre object (the first object of the centre group, or the one the dump names) through the nodes' product, cur * (1 / prev) per axis.
    // With no centre object each member is scaled about itself: its place stays and only its own size changes (o.osx / osy, which the supply hands the model as the member's scale ratio, scaleAt).
    // That size was not tracked: a custom level Transform uid 29856 (x 6258.5, target group 1056, centre 0, scale to below 1 over 1.9 s) shrinks the green gravity orb uid 194871 from t~4086, and the
    // timeline kept its scale ratio at 1, so the search's orb stayed at its loaded 25.2 x 25.2 and a press at 4095 flipped gravity on it where GD's orb was 23.9 (the cube then died at 4113).
    // [M] GD's recorded rect of that orb (flight 15): 25.200 (t=4025), 24.822 (4090), 23.925 (4095), 23.753 (4096), its centre at x 6247.5 / y 1871.05 all along -- the size follows, the place does not.
    // [R] the gd08_rscdbg bed: the supply's ratio for the orb at 4094..4096 was 1.00000 with this tick's stamp (the command ran, this branch returned before the members).
    // The skew and the centre's own rotation (relative rotation) are not modelled: exact for an unrotated centre.
    void scaleGroup(const Cmd& c, double e) {
        const auto it = members.find(c.target);
        if (it == members.end()) return;
        size_t ci = 0; bool haveCentre = false;
        if (c.ctrCol) {
            if (c.ctrObj > 0) { const auto bi = byUid.find(c.ctrObj); if (bi != byUid.end()) { ci = bi->second; haveCentre = true; } }
        } else {
            const auto ct = c.center ? members.find(c.center) : members.end();
            if (ct != members.end() && !ct->second.empty()) { ci = ct->second.front(); haveCentre = true; }
        }
        auto held = [](double s) { return (std::fabs(s) <= 0.01 && std::fabs(s) != 0.01) ? (s < 0.0 ? -0.01 : 0.01) : s; };
        const double curX = held((c.sclX - 1.0) * e + 1.0), curY = held((c.sclY - 1.0) * e + 1.0);
        const double prvX = held((c.sclX - 1.0) * c.lastE + 1.0), prvY = held((c.sclY - 1.0) * c.lastE + 1.0);
        const float ax = (float)curX * (float)(1.0 / prvX), ay = (float)curY * (float)(1.0 / prvY);
        if (!haveCentre) {   // each member about itself: the own size only
            for (size_t i : it->second) {
                ObjState& o = objs.mut(i);
                o.osx *= ax; o.osy *= ay;
                o.lastTrig = c.uid; o.lastTrigId = c.id;
            }
            return;
        }
        if (objs[ci].blind) for (size_t i : it->second) setBlind(i);   // scaled about a place that is not the game's: so is the result
        const ObjState& C = objs[ci];
        const float cox = (float)(C.x - (double)C.ax), coy = (float)(C.y - (double)C.ay);
        float mvx = 0.0f, mvy = 0.0f;
        {
            const auto nd = moveNodes_.find(c.target);
            if (nd != moveNodes_.end() && !nd->second.used) { nd->second.used = true; mvx = (float)nd->second.dx; mvy = (float)nd->second.dy; }
        }
        for (size_t i : it->second) {
            ObjState& o = objs.mut(i);
            const float px = (float)((o.x - (double)o.ax) - (double)cox), py = (float)((o.y - (double)o.ay) - (double)coy);
            const float qx = mvx + (float)((double)ax * px + (double)cox), qy = mvy + (float)((double)ay * py + (double)coy);
            const double nx = (((double)qx - o.x) + (double)o.ax) + o.x, ny = (((double)qy - o.y) + (double)o.ay) + o.y;
            if (std::fabs(nx - o.x) > 1e-4 || std::fabs(ny - o.y) > 1e-4) { ++sclMoves; sclMoved.insert(o.uid); }
            o.x = nx; o.y = ny;
            o.osx *= ax; o.osy *= ay;   // the member's own scale goes with the transform (FUN_140335280: the new scale is the length of the matrix's columns; for an unrotated, unskewed one the old scale times the factor)
            o.lastTrig = c.uid; o.lastTrigId = c.id;
        }
    }

    void toggle(int group, bool on, int uid) {
        bool& bit = groupBit.try_emplace(group, true).first->second;
        if (bit == on) return;
        bit = on;
        const auto it = members.find(group);
        if (it == members.end()) return;
        for (size_t i : it->second) { ObjState& o = objs.mut(i); o.counter += on ? 1 : -1; o.lastTrig = uid; o.lastTrigId = 1049; }
    }

    void fireSpawnFed(long long t) {
        if (!spawnFedOn_ && !spawnFedStops_) return;
        const auto it = spawnAt_.find(t);
        if (it == spawnAt_.end()) return;
        for (int uid : it->second) {
            if (!fedSpawn_.count(uid)) continue;
            const auto ti = trigByUid_.find(uid);
            if (ti == trigByUid_.end()) continue;
            const double keep = curOvershoot_;
            curOvershoot_ = 0.0;
            TrigDef x = trigs_[ti->second];
            applySpawnRemap(x, t);
            fire(x);
            curOvershoot_ = keep;
        }
    }
    // A trigger spawned out of a group by a Spawn with a remap acts on the remapped groups (its target and centre). The record does not say which Spawn started a call, so it is the last Spawn
    // (whichever kind) to have been called at or before this step whose group holds the trigger; only that one's remap counts. A trigger started through a nested Spawn is not followed.
    void applySpawnRemap(TrigDef& x, long long t) {
        if (!anyRemap_) return;
        const auto bi = byUid.find(x.uid);
        if (bi == byUid.end()) return;
        auto sp = spawnersOf_.find(x.uid);
        if (sp == spawnersOf_.end()) {
            std::vector<int> v;
            for (const TrigDef& s : trigs_) {
                if (s.id != 1268) continue;
                const auto m = members.find(s.target);
                if (m != members.end() && std::find(m->second.begin(), m->second.end(), bi->second) != m->second.end()) v.push_back(s.uid);
            }
            sp = spawnersOf_.emplace(x.uid, std::move(v)).first;
        }
        if (fireSteps_.empty()) for (const auto& kv : touchAt_) for (int u : kv.second) fireSteps_.mut()[u].push_back(kv.first);   // ascending steps
        int best = 0; long long bestT = -1, bestErr = -1;
        if (sp->second.empty()) {   // no Spawn names a group the trigger is in: it was started through another activator (an Advanced Random, a Sequence ...) that a remapping Spawn started in turn. The remap that reaches it is the one
            // of the remapping Spawn called in this very step -- when there is exactly one such remap in force (the record does not say which chain a call belongs to)
            if (!nestedRemap) return;
            const auto ci = touchAt_.find(t);
            if (ci == touchAt_.end()) return;
            for (int u : ci->second) {
                const auto si3 = trigByUid_.find(u);
                if (si3 == trigByUid_.end() || trigs_[si3->second].id != 1268 || trigs_[si3->second].remapTo.empty()) continue;
                if (best && trigs_[trigByUid_.find(best)->second].remapTo != trigs_[si3->second].remapTo) return;   // two different remaps in this step: ambiguous
                best = u;
            }
            if (!best) return;
            bestT = t;
        }
        // Several Spawns of one group can be called in the same tick, each with its own delay (a Spawn with a delay spawns d * 240 - 1 ticks after its call): the one that started this call is the one whose
        // call plus delay lands on `t`. A tie goes to the later call, then to the Spawn found first.
        if (!best) for (int su : sp->second) {
            const auto fs = fireSteps_.find(su);
            if (fs == fireSteps_.end()) continue;
            const auto up = std::upper_bound(fs->second.begin(), fs->second.end(), t);
            if (up == fs->second.begin()) continue;
            const long long ts = *(up - 1);
            long long delay = 0;
            const auto si2 = trigByUid_.find(su);
            if (spawnDelayRemap && si2 != trigByUid_.end() && trigs_[si2->second].sdelay > 0.0) delay = std::max(0LL, (long long)std::llround(trigs_[si2->second].sdelay * 240.0) - 1);
            if (spawnDelayRemap && si2 != trigByUid_.end() && trigs_[si2->second].sordd) {   // an ordered Spawn starts each trigger of its group (x - the leftmost's x) / 311.58 seconds after its own start (FUN_140322560)
                double x0 = 1e18;
                const auto gm = members.find(trigs_[si2->second].target);
                if (gm != members.end())
                    for (size_t oi : gm->second) { const auto ti2 = trigByUid_.find(objs[oi].uid); if (ti2 != trigByUid_.end()) x0 = std::min(x0, trigs_[ti2->second].cx); }
                if (x0 < 1e17 && x.cx >= x0) delay += (long long)std::llround((x.cx - x0) / 311.58 * 240.0);
            }
            const long long err = spawnDelayRemap ? std::llabs((t - ts) - delay) : 0;
            if (best == 0 || err < bestErr || (err == bestErr && ts > bestT)) { bestT = ts; best = su; bestErr = err; }
        }
        if (!best) return;
        const auto si = trigByUid_.find(best);
        if (si == trigByUid_.end()) return;
        const int ga0 = x.tmodctr > 0 ? x.tmodctr : x.target, center0 = x.center;
        for (const auto& kv : trigs_[si->second].remapTo) {
            if (x.target == kv.first) { x.target = kv.second; break; }
        }
        for (const auto& kv : trigs_[si->second].remapTo) {
            if (x.center == kv.first) { x.center = kv.second; break; }
        }
        for (const auto& kv : trigs_[si->second].remapTo) {
            if (x.tmodctr == kv.first) { x.tmodctr = kv.second; break; }
        }
        // a Move-To measures from / to the main object of a group (the dump's mva / ctrobj name the object of the group as written): for a group the remap changed, that object is the wrong one, so the group's
        // only member is taken instead (a Group Parent of the remapped group is not known here)
        if (remapMainObj) {
            if ((x.tmodctr > 0 ? x.tmodctr : x.target) != ga0) x.mvaCol = false;
            if (x.center != center0) x.ctrCol = false;
        }
    }
    // GJBaseGameLayer::toggleGroupTriggered 0x32b7f0: the group is switched, and spawned when it is switched on
    void toggleGroupTriggered(int group, bool activate, int uid) {
        toggle(group, activate, uid);
        if (activate) { const int keepSp = curSpawnUid_; curSpawnUid_ = uid; spawnGroup(group, false, 0.0); curSpawnUid_ = keepSp; }
    }
    // GJEffectManager::updateCountForItem 0x36cc30: the item's number is written, and every entry waiting on the item whose number moved across its target fires its group (an entry that is not multi goes after it)
    void updateCountForItem(int item, int now) {
        if (item < 0) item = 0; if (item > 9999) item = 9999;
        int& current = itemCount_[item];
        const bool descending = current > now;
        current = now;
        const auto rg = countReg_.find(item);
        if (rg == countReg_.end()) return;
        // GD Win 2.2091: updateCountForItem sorts at RVA 0x36ce12 using the target comparators 0x362d60 / 0x362d70.
        std::sort(rg->second.begin(), rg->second.end(), [descending](const CountEntry& a, const CountEntry& b) {
            return descending ? b.target < a.target : a.target < b.target;
        });
        std::vector<CountEntry> fire;
        for (size_t i = 0; i < rg->second.size();) {
            CountEntry& e = rg->second[i];
            if (e.last != now) {
                const int old = e.last;
                e.last = now;
                if ((e.target > old && e.target <= now) || (e.target < old && e.target >= now)) {
                    fire.push_back(e);
                    if (!e.multi) { rg->second.erase(rg->second.begin() + (long)i); continue; }
                }
            }
            ++i;
        }
        for (const CountEntry& e : fire) toggleGroupTriggered(e.group, e.activate, e.trigUid);
    }
    void fireItems(long long t) {
        const auto it = itemAt_.find(t);
        if (it == itemAt_.end()) return;
        for (const auto& e : it->second) { updateCountForItem(e.first, itemCountOf(e.first) + e.second); ++itemsFed; }
    }
    void fireTouch(long long t) {
        if (!touchFed_) return;
        const auto it = touchAt_.find(t);
        if (it == touchAt_.end()) return;
        for (int uid : it->second) {
            const auto ti = trigByUid_.find(uid);
            if (ti == trigByUid_.end()) continue;
            const TrigDef x = trigs_[ti->second];
            if (!x.touch || !modelled_.count(x.uid)) continue;
            curLate_ = lateAt_.count({t, uid}) > 0;
            fire(x);
            curLate_ = false;
        }
    }
    // GJEffectManager::updateSpawnTriggers 0x36c5e0: every waiting Spawn counts one step of dt up and, once its delay has run out, spawns its group (the loop's end is read before it starts: a Spawn made inside
    // it waits for the next call)
    void updateSpawns(size_t from = 0) {   // from: only the entries from this index on (the ones a late touch call has just queued, fireTouchLate)
        if (pendLen_ == 0) return;
        const double dt = (double)physicsStepDt();
        const size_t n = pendLen_;
        for (size_t i = from; i < n; ++i) {
            // 0x36c5e0: an entry whose "due" flag is set spawns again at once; otherwise a paused one waits, and a running one counts dt up and sets the flag when its delay has run out
            if (!pending_[i].done) {
                if (pending_[i].paused) continue;
                pending_[i].time += dt;
                pending_[i].done = pending_[i].delay <= pending_[i].time;
            }
            if (pending_[i].done) {
                const PendingSpawn p = pending_[i];
                const double over = p.time - p.delay;
                const RemapKeys keepR = curRemap_;
                curRemap_ = p.remap;
                if (p.objUid) spawnObject(p.objUid, over);
                else { const int keepSp = curSpawnUid_; curSpawnUid_ = p.trigUid; spawnGroup(p.group, p.ordered, over); curSpawnUid_ = keepSp; }
                curRemap_ = keepR;
            }
        }
        size_t keep = 0;   // the end-of-call compaction: the entries that are not due close up and the end moves down (the slots beyond stay as they were)
        for (size_t i = 0; i < pendLen_; ++i) if (!pending_[i].done) { if (keep != i) pending_[keep] = pending_[i]; ++keep; }
        pendLen_ = keep;
    }
    // GJBaseGameLayer::spawnObject 0x3227a0: a spawn-triggered trigger that is on is triggered; `over` = how far past its delay the spawn got
    void spawnObject(int uid, double over) {
        const auto bi = byUid.find(uid);
        if (bi == byUid.end() || !objs[bi->second].on()) return;
        if (fedSpawn_.count(uid)) return;   // fired from the game's record
        const auto ti = trigByUid_.find(uid);
        if (ti == trigByUid_.end()) return;
        const TrigDef x = trigs_[ti->second];
        if (!x.spawn) return;
        if (!modelled_.count(x.uid)) {   // a spawn-triggered trigger the shadow does not run (a camera trigger, ...): the chain reached it, which is all a reader of its fire tick needs (spawnReach, a report)
            if (x.multi || spawnedOnce_.insert(x.uid).second) spawnReach.push_back({curTick_, x.uid});
            return;
        }
        if (!x.multi && !spawnedOnce_.insert(x.uid).second) return;   // spawnObject's gate: without m_isMultiTriggered a trigger is spawned once (measured: 3,649 of 5,111 fire once in the record, 1 fires twice, none more)
        const double keep = curOvershoot_;
        curOvershoot_ = over;
        spawnFires.push_back({curTick_, x.uid});   // the shadow's own spawn chain fired this trigger (a report)
        if (curRemap_.empty()) fire(x);
        else { TrigDef y = x; applyRemapKeys(y, curRemap_, remapMainObj); fire(y); }
        curOvershoot_ = keep;
    }
    // GJBaseGameLayer::spawnGroup 0x322370: if the group's bit is on, its triggers are spawned in the level's order -- all at once, or (ordered) the first now and each of the others after
    // (its x - the first's x) / 311.58 seconds less what the spawn is already late by (FUN_140322560)
    double curExtraDelay_ = 0.0;   // see orbDelay
    void spawnGroup(int g, bool ordered = false, double over = 0.0) {
        const auto gb = groupBit.find(g);
        if (gb != groupBit.end() && !gb->second) return;
        if (spawnDepth_ > 32 || pendLen_ > 100000) {   // (ours, not the game's: the game's own stop is the 10,000 spawns a step, below)
            guardHit(spawnDepth_ > 32 ? "trigshadow spawnGroup: the spawn depth" : "trigshadow spawnGroup: the pending queue", spawnDepth_ > 32 ? 32 : 100000, "a Spawn group was not spawned");
            return;
        }
        // a group is not spawned again inside its own spawn (the layer's visited map at +0x1000), and a step spawns at most 10,000 times (m_spawnLoopCount at +0x33b4 stops everything beyond it)
        if (spawnedStep_.count(g) || ++spawnCountStep_ > 10000) return;
        spawnedStep_.insert(g);
        ++spawnDepth_;
        const auto m = members.find(g);
        if (m != members.end()) {
            const std::vector<size_t> list = m->second;   // a copy: firing may toggle groups
            // an ordered spawn is timed from the leftmost spawn-triggered trigger of the group (not the first one in the level's order, not the Spawn trigger's own x): the record's delays follow
            // (x - min x) / 311.58 within 1.5 steps for 5,427 of 5,563 triggers, against 3,638 for the first in level order and 889 for the Spawn trigger's x
            double x0 = 0.0;
            if (ordered) {
                bool any = false;
                for (size_t i : list) {
                    const auto ti = trigByUid_.find(objs[i].uid);
                    if (ti == trigByUid_.end() || !trigs_[ti->second].spawn) continue;
                    const double xk = (double)(float)trigs_[ti->second].cx;
                    if (!any || xk < x0) { x0 = xk; any = true; }
                }
            }
            for (size_t i : list) {
                const auto ti = trigByUid_.find(objs[i].uid);
                if (ti == trigByUid_.end() || !trigs_[ti->second].spawn) continue;   // not a spawn-triggered trigger (isSpawnableTrigger)
                if (!ordered) { spawnObject(objs[i].uid, over); continue; }
                const double xk = (double)(float)trigs_[ti->second].cx;
                float d = (float)((xk - x0) / 311.5801086425781 - over);
                if (d <= 0.0f) { spawnObject(objs[i].uid, 0.0); continue; }
                if (curExtraDelay_ != 0.0) d += (float)curExtraDelay_;
                PendingSpawn p;
                p.remap = curRemap_;
                p.objUid = objs[i].uid; p.delay = (double)d;
                p.trigUid = curSpawnUid_;   // the entry of one trigger of an ordered spawn belongs to the Spawn trigger that made it: a Stop of a group that Spawn is in erases it (c141972420: Spawn 2626 spawns a group it is in, and loops until the Stop 2624 at x 8205 ends it; c113256289: a Stop of the members' group erases nothing, the Spawn is elsewhere)
                pendPush(p);
            }
        }
        --spawnDepth_;
        spawnedStep_.erase(g);
    }
    // the values an Item Compare can read here: mode 1 is an item counter, which the level does not write (trig_replay checks that before it sets itemCmp), so 0; mode 5 is the attempt number
    int attemptNo_ = 1;
    // T57: mode 1 is the item's counter (countForItem 0x36cb70: the item clamped to 0..9999, none = 0) as the timeline holds it -- the loader only runs a trigger whose counters every writer of is run -- and 5 the attempt number
    double itemValue(int mode, int item) const { return mode == 5 ? (double)attemptNo_ : mode == 1 ? (double)itemCountOf(item) : 0.0; }
    // GJBaseGameLayer::activateItemCompareTrigger 0x33d310, exactly: left = f(item1), right = mod2 or f(item2) combined with it, each rounded and signed, then compared with the tolerance
    bool itemCompare(const TrigDef& t) const {
        const int m1 = t.i1mode < 1 ? 1 : t.i1mode, o1 = t.res1 < 1 ? 3 : t.res1, o2 = t.res2 < 1 ? 3 : t.res2;
        const float f16 = std::roundf(t.mod1 * 1000.0f) / 1000.0f, f17 = std::roundf(t.mod2 * 1000.0f), f18 = std::roundf(t.tol * 1000.0f) / 1000.0f;
        const bool useItem2 = !(t.i2mode == 0 || (t.item2 < 1 && (unsigned)(t.i2mode - 3) > 1u));
        double L = itemValue(m1, t.item1);
        if (o1 == 1) L += (double)f16; else if (o1 == 2) L -= (double)f16; else if (o1 == 3) L *= (double)f16; else if (o1 == 4) L = f16 == 0.0f ? 0.0 : L / (double)f16; else L = 0.0;
        if (t.rnd1 == 1) L = std::round(L); else if (t.rnd1 == 2) L = std::floor(L); else if (t.rnd1 == 3) L = std::ceil(L);
        if (t.sgn1 == 1) L = std::fabs(L); else if (t.sgn1 == 2) L = -std::fabs(L);
        double R = (double)(f17 / 1000.0f);
        if (useItem2) {
            const double d = itemValue(t.i2mode, t.item2);
            if (o2 == 1) R = d + R; else if (o2 == 2) R = d - R; else if (o2 == 3) R = d * R; else if (o2 == 4) R = R == 0.0 ? 0.0 : d / R; else R = 0.0;
        }
        if (t.rnd2 == 1) R = std::round(R); else if (t.rnd2 == 2) R = std::floor(R); else if (t.rnd2 == 3) R = std::ceil(R);
        if (t.sgn2 == 1) R = std::fabs(R); else if (t.sgn2 == 2) R = -std::fabs(R);
        const double tl = (double)f18;
        switch (t.res3) {
            case 0: return std::fabs(L - R) <= tl;
            case 1: return !(tl + L < R) && !(tl + L == R);
            case 2: return R <= tl + L;
            case 3: return !(R < L - tl) && !(R == L - tl);
            case 4: return L - tl <= R;
            case 5: return !(std::fabs(L - R) < tl) && !(std::fabs(L - R) == tl);
            default: return false;
        }
    }
    // GJBaseGameLayer::activateItemEditTrigger 0x33cf40, exactly (T57): d1 / d2 = the values of item 1 / item 2 (item 2 alone is taken as item 1), combined by res2 (+0x758, below 1 is 1), then with the modifier by res3 (+0x75c,
    // below 3 is 3); rounded (rnd1) and signed (sgn1); then with the target item's own value by res1 (+0x754: 0 none, 1 X + T, 2 T - X, 3 X * T, 4 T / X); rounded (rnd2) and signed (sgn2); written to the target item (mode 1)
    void fireItemEdit(const TrigDef& t) {
        int m1 = t.i1mode, m2 = t.i2mode, id1 = t.item1, id2 = t.item2;
        const int tm = t.edTmode < 1 ? 1 : t.edTmode, tgt = t.pkItem;   // (the item written is kept in pkItem: `target` is a group everywhere else)
        const int r2 = t.res2 < 1 ? 1 : t.res2, r3 = t.res3 < 3 ? 3 : t.res3;
        const float f16 = std::roundf(t.mod1 * 1000.0f) / 1000.0f;
        if (tgt < 1 && tm != 3) return;
        bool u1 = !(m1 == 0 || (id1 < 1 && (unsigned)(m1 - 3) > 1u));
        bool u2 = !(m2 == 0 || (id2 < 1 && (unsigned)(m2 - 3) > 1u));
        if (!u1 && u2) { u1 = true; u2 = false; id1 = id2; m1 = m2; }
        double d1 = itemValue(m1, id1);
        const double d2 = itemValue(m2, t.item2);
        const double dT = itemValue(tm, tgt);
        if (u2) {
            if (r2 == 1) d1 = d1 + d2; else if (r2 == 2) d1 = d1 - d2; else if (r2 == 3) d1 = d1 * d2; else if (r2 == 4) d1 = d2 == 0.0 ? 0.0 : d1 / d2; else d1 = 0.0;
        }
        double X = (double)f16;
        if (u1) {
            if (r3 == 1) X = d1 + X; else if (r3 == 2) X = d1 - X; else if (r3 == 3) X = d1 * X; else if (r3 == 4) X = X == 0.0 ? 0.0 : d1 / X; else X = 0.0;
        }
        if (t.rnd1 == 1) X = std::round(X); else if (t.rnd1 == 2) X = std::floor(X); else if (t.rnd1 == 3) X = std::ceil(X);
        if (t.sgn1 == 1) X = std::fabs(X); else if (t.sgn1 == 2) X = -std::fabs(X);
        double V = X;
        if (t.res1 != 0) {
            if (t.res1 == 1) V = X + dT; else if (t.res1 == 2) V = dT - X; else if (t.res1 == 3) V = X * dT; else if (t.res1 == 4) V = X == 0.0 ? 0.0 : dT / X; else V = 0.0;
        }
        if (t.rnd2 == 1) V = std::round(V); else if (t.rnd2 == 2) V = std::floor(V); else if (t.rnd2 == 3) V = std::ceil(V);
        if (t.sgn2 == 1) V = std::fabs(V); else if (t.sgn2 == 2) V = -std::fabs(V);
        if (tm == 1) updateCountForItem(tgt, (int)V);
    }
    // GJBaseGameLayer::triggerMoveCommand 0x21ea40, the target mode (m_useMoveTarget, not the dynamic one): the Move does not take its own offset, it takes the distance from the object of the target group
    // (the centre-modifier group when there is one) to the object of the centre group, as the objects stand when it fires -- getRealPosition, the float of the double position (area offset in) -- and
    // only on the axis m_moveTargetMode leaves; tryGetMainObject finds an object only for a group of exactly one (a Group Parent is not known here); the player locks are dropped
    // GJBaseGameLayer::tryGetObject 0x32c2b0 (what the Move-To takes for a group): the group's main object (a registered parent, else its only member) and, when it has none, a RANDOM member
    // ((int)(count * r) of the trigger stream: one draw per call). The dump's ctrobj / mva columns hold the main object only (-1: none). The pick is not known here, but it does not matter when every member of the
    // group stands at the same place on the axes the Move-To reads: then any member gives the distance. Returns the first member in that case, -1 otherwise (members differ, or fewer than two).
    long agreeMember(int g, int axis) const {   // axis: 0 both, 1 x only (m_moveTargetMode), 2 y only
        const auto m = members.find(g);
        if (m == members.end() || m->second.size() < 2) return -1;
        const ObjState& f0 = objs[m->second.front()];
        for (size_t k : m->second) {
            const ObjState& o = objs[k];
            if (axis != 2 && (float)o.x != (float)f0.x) return -1;
            if (axis != 1 && (float)o.y != (float)f0.y) return -1;
        }
        return (long)m->second.front();
    }
    TrigDef moveToOf(const TrigDef& t) const {
        TrigDef u = t;
        u.mvtgt = false; u.lockPX = u.lockPY = false; u.lockPlayer = false;
        auto sole = [&](int g) -> long { const auto m = members.find(g); return m != members.end() && m->second.size() == 1 ? (long)m->second.front() : -1; };
        // the destination: the dump's ctrobj when it has the column (the group's registered parent first, else its only member), else the group's only member
        long b = -1;
        if (t.ctrCol) { if (t.ctrObj > 0) { const auto bi = byUid.find(t.ctrObj); if (bi != byUid.end()) b = (long)bi->second; } else if (t.ctrObj == -1 && moveToAnyMember) b = agreeMember(t.center, t.mvaxis); }
        else b = sole(t.center);
        long a = -1;   // the group it measures from: the dump's object (trigext.txt) when it has the column, else the group's only member
        if (t.mvaCol) { if (t.mvaObj > 0) { const auto ai = byUid.find(t.mvaObj); if (ai != byUid.end()) a = (long)ai->second; } else if (t.mvaObj == -1 && moveToAnyMember) a = agreeMember(t.tmodctr > 0 ? t.tmodctr : t.target, t.mvaxis); }
        else a = sole(t.tmodctr > 0 ? t.tmodctr : t.target);
        float wx = 0.0f, wy = 0.0f;
        if (a >= 0 && (t.tpCol ? t.tpPlayer : t.center == 0)) {   // no centre group: the Move-To's destination is the player (keys 138 / 200, which the dump does not carry), where the record has it for the call (the dump's place of the step)
            wx = (float)curPx_ - (float)objs[(size_t)a].x; wy = (float)curPy_ - (float)objs[(size_t)a].y;
        } else if (a >= 0 && b >= 0 && a != b) { wx = (float)objs[(size_t)b].x - (float)objs[(size_t)a].x; wy = (float)objs[(size_t)b].y - (float)objs[(size_t)a].y; }
        if (t.mvaxis == 1) wy = 0.0f;
        if (t.mvaxis == 2) wx = 0.0f;
        u.ox = (double)wx; u.oy = (double)wy;
        return u;
    }
    // ---- Collision triggers (1815) and collision blocks (object id 1816) ---------------------------------------------------------------------------
    // GJEffectManager::registerCollisionTrigger 0x366710 keeps {blockIdMax, blockIdMin, target, activate, triggerOnExit, uid} per fired trigger. GJBaseGameLayer::updateCollisionBlocks 0x31ff10 takes each collision
    // block in the layer's array order, and checks the blocks it has not visited yet (FUN_140320450: the object's rect against the other's, inclusive on all four edges, a rotated one by OBB -- not here)
    // for overlap; an overlapping pair goes to GJBaseGameLayer::objectsCollided 0x320700: ids of 10002 and above are ignored, and a pair already in the effect manager's active set (+0x2a8) is only marked
    // seen; a new pair is added and handleObjectCollision 0x366820 runs the actions of that id pair whose triggerOnExit differs from the event (an enter event runs those with triggerOnExit 0): the group
    // is toggled (toggleGroupTriggered 0x32b7f0: toggleGroup, and if "activate", spawnGroup). GJEffectManager::postCollisionCheck 0x3665c0 (the end of the step) drops the pairs not seen this step
    // (and runs their exit actions).
public:
    // --camy (a): the y of group g's first object (the lowest rank: tryGetObject's, a Rotate's centre) as the shadow holds it now -- only for an object no trigger the shadow does not run acts on (coveredObject)
    bool firstMemberY(int g, float* y, int* why = nullptr) const {   // why: 1 no group, 2 not covered, 3 no member loaded, 4 blind
        if (why) *why = 0;
        if (g <= 0) { if (why) *why = 1; return false; }
        const auto m = members.find(g);
        if (m == members.end() || m->second.empty()) { if (why) *why = 3; return false; }
        size_t best = m->second.front();
        for (size_t k : m->second) if (objs[k].rank >= 0 && (objs[best].rank < 0 || objs[k].rank < objs[best].rank)) best = k;
        if (!coveredObject(best)) { if (why) *why = 2; return false; }   // a trigger the shadow does not run acts on one of its groups (uncoveredGroups_ / uncovered Areas)
        if (objs[best].blind) { if (why) *why = 4; return false; }   // a blind object's place is no longer the game's (a camera-locked Move stepped it with an uncertain camera)
        *y = (float)objs[best].y; return true;
    }
    // the same object's place on both axes (a follow Static Camera reads it every tick: CamModel::resolveLive)
    bool firstMemberPos(int g, float* x, float* y) const {
        float yy = 0.0f;
        if (!firstMemberY(g, &yy)) return false;
        const auto m = members.find(g);
        size_t best = m->second.front();
        for (size_t k : m->second) if (objs[k].rank >= 0 && (objs[best].rank < 0 || objs[k].rank < objs[best].rank)) best = k;
        *x = (float)objs[best].x; *y = yy; return true;
    }
    // the camera of the step just made, placed after it (the game's camera of a tick reads a follow target as that tick's movement left it): what the next step's camera-locked Moves take their displacement from
    void setStepCam(const Cam& c) { cam = c; prevCam_ = c; }
    bool moveToAnyMember = true;   // a Move-To whose group has no main object takes a random member (tryGetObject); when the members agree on the axes read, the first one is the answer (agreeMember). false = the old reading (no distance)
    bool collisionModel = false;   // cfg: run the collision blocks and the Collision triggers (needs collblocks.txt)
    struct CollBlock { int uid; int blockId; double w, h; };
    SharedConst<std::vector<CollBlock>> collBlocks;
    void addCollisionBlock(int uid, int blockId, double w, double h) { collBlocks.mut().push_back({uid, blockId, w, h}); }
    long long collEnters = 0;
    struct CollAction { int hi, lo, target; bool activate, onExit; int trigUid; };
    std::vector<CollAction> collActions_;
    std::map<std::pair<int, int>, bool> collActive_;   // (idMax, idMin) -> seen this step
    void registerCollision(const TrigDef& t) {
        int hi = t.collA, lo = t.collB;
        if (hi < lo) std::swap(hi, lo);
        collActions_.push_back({hi, lo, t.target, t.collActivate, false, t.uid});
    }
    void objectsCollided(int idA, int idB) {
        if (idA >= 0x2712 || idB >= 0x2712) return;
        const int hi = std::max(idA, idB), lo = std::min(idA, idB);
        const auto key = std::make_pair(hi, lo);
        const auto ai = collActive_.find(key);
        if (ai != collActive_.end()) { ai->second = true; return; }
        ++collEnters;
        for (size_t k = 0; k < collActions_.size(); ++k) {
            const CollAction a = collActions_[k];
            if (a.onExit || a.hi != hi || a.lo != lo) continue;   // an enter event runs the actions whose triggerOnExit is 0
            toggle(a.target, a.activate, a.trigUid);
            if (a.activate) { const int keepSp = curSpawnUid_; curSpawnUid_ = a.trigUid; spawnGroup(a.target, false, 0.0); curSpawnUid_ = keepSp; }
        }
        collActive_[key] = true;
    }
    void stepCollisions() {
        if (collBlocks.empty() || collActions_.empty()) { if (!collActive_.empty()) collActive_.clear(); return; }
        for (auto& kv : collActive_) kv.second = false;
        auto rectOf = [&](const CollBlock& b, float& x0, float& x1, float& y0, float& y1) -> bool {
            const auto bi = byUid.find(b.uid);
            if (bi == byUid.end() || !objs[bi->second].on()) return false;
            const ObjState& o = objs[bi->second];
            x0 = (float)o.x - (float)b.w * 0.5f; x1 = x0 + (float)b.w; y0 = (float)o.y - (float)b.h * 0.5f; y1 = y0 + (float)b.h;
            return true;
        };
        for (size_t i = 0; i < collBlocks.size(); ++i) {
            float ax0, ax1, ay0, ay1;
            if (!rectOf(collBlocks[i], ax0, ax1, ay0, ay1)) continue;
            for (size_t j = i + 1; j < collBlocks.size(); ++j) {
                float bx0, bx1, by0, by1;
                if (!rectOf(collBlocks[j], bx0, bx1, by0, by1)) continue;
                if (bx0 <= ax1 && ax0 <= bx1 && by0 <= ay1 && ay0 <= by1) objectsCollided(collBlocks[i].blockId, collBlocks[j].blockId);
            }
        }
        for (auto it = collActive_.begin(); it != collActive_.end();) {   // postCollisionCheck: a pair not seen this step is dropped (its exit actions are not run: no dump of triggerOnExit)
            if (!it->second) it = collActive_.erase(it); else ++it;
        }
    }
private:
    void fire(const TrigDef& t0) {
        if (t0.id == 901 && t0.mvtgt && !t0.dynmode) { fireNow(moveToOf(t0)); return; }
        fireNow(t0);
    }
    void fireNow(const TrigDef& t) {
        if (drawOnFollow && t.id == 3016) {   // an Advanced Follow: GJBaseGameLayer::processAdvancedFollowAction (22091 base+0x337e60) draws one number of the trigger LCG when the command first runs, the step after the call (a custom level: the 11 Advanced Follow triggers of the level, each one draw at its crossing tick + 1; the game's LCG trace, cfg rngtrace)
            // ... for a target group that has objects: a custom level's AFs 27067 (target 96) and 51586 (target 219) have no member and the game's trace has no draw at their crossing (tick ~3410), where 26226 / 27069 (groups of 1 and 2 objects) draw at theirs (ticks 123, 3273)
            const auto af = members.find(t.target);
            if (af != members.end() && !af->second.empty()) pendDraw_.push_back(stepNo_ + 1);
        }
        ++fired;
        if (t.id == 901 && t.silent) {   // a silent Move (FUN_1403264c0): no command, the whole offset goes onto the group's objects at once (x not onto an object whose +0x2c8 is set)
            const auto it = members.find(t.target);
            if (it != members.end()) {
                const double dx = (double)(float)t.ox, dy = (double)(float)t.oy;
                for (size_t i : it->second) {
                    ObjState& o = objs.mut(i);
                    if (dx != 0.0 && !o.areaXLock) o.x += dx;
                    if (dy != 0.0) o.y += dy;
                    o.lastTrig = t.uid; o.lastTrigId = t.id;
                }
            }
        } else if (t.id == 901) {
            Cmd c;
            c.dx = (t.lockPX || (t.lockCX && t.camLockOk)) ? 0.0 : t.ox; c.dy = (t.lockPY || (t.lockCY && t.camLockOk)) ? 0.0 : t.oy; c.dur = t.dur; c.kind = t.ease; c.rate = t.erate; c.target = t.target; c.uid = t.uid; c.id = t.id;
            c.lockPX = t.lockPX; c.lockPY = t.lockPY; c.lockCX = t.lockCX && t.camLockOk; c.lockCY = t.lockCY && t.camLockOk; c.modX = t.modX; c.modY = t.modY;   // createMoveCommand 0x3669e0: a locked axis gets no eased share; a modifier of exactly 0 is 1
            if (!(t.ox != 0.0 || t.oy != 0.0 || t.lockPX || t.lockPY || c.lockCX || c.lockCY)) return;
            cmds_.push_back(c);
        } else if (t.id == 1049) {
            if (t.togon >= 0) toggle(t.target, t.togon == 1, t.uid);
        } else if (t.id == 3604) {   // an Event trigger (activateEventTrigger 0x33ada0): a listener for each of its events, registered with the keys of the call
            if (t.evOk) for (const int e : t.evList) { EvListener L; L.ev = e; L.key = t.evKey; L.target = t.target; L.trigUid = t.uid; L.remap = curRemap_; evListen_.push_back(L); }
        } else if (t.id == 1594) {   // a Toggle Orb (object type 36, clicked while the player is on it): "activate group" switches the target group on AND spawns its triggers (trigfire.csv: the group's spawn-triggered triggers fire at the click's tick); off switches it off
            if (t.togon >= 0) {
                if (!t.spawnOnly) toggle(t.target, t.togon == 1, t.uid);   // (a spawn-only 1594 switches nothing: its press only spawns the group)
                if (t.togon == 1) { const int keepSp = curSpawnUid_; const RemapKeys keepR = curRemap_; const double keepX = curExtraDelay_; curSpawnUid_ = t.uid; curRemap_.clear(); curExtraDelay_ = curLate_ ? orbDelay : 0.0; spawnGroup(t.target, false, 0.0); curExtraDelay_ = keepX; curRemap_ = keepR; curSpawnUid_ = keepSp; }
            }
        } else if (t.id == 1268) {   // Spawn: no delay spawns its group now, a delay waits (spawnGroup of the effect manager 0x36c4f0)
            const float d = (float)((double)(float)t.sdelay - curOvershoot_);   // SpawnTriggerGameObject::triggerObject 0x65a240
            RemapKeys keys;   // what the call it makes carries: its own keys, then the ones it was called with; nothing for a Spawn with no keys of its own
            if (!t.remapTo.empty()) { keys = t.remapTo; keys.insert(keys.end(), curRemap_.begin(), curRemap_.end()); }
            if (!(d > 0.0f)) { const int keepSp = curSpawnUid_; const RemapKeys keepR = curRemap_; curSpawnUid_ = t.uid; curRemap_ = keys; spawnGroup(t.target, t.sordd, d < 0.0f ? -(double)d : 0.0); curRemap_ = keepR; curSpawnUid_ = keepSp; }
            else { PendingSpawn p; p.group = t.target; p.ordered = t.sordd; p.delay = (double)d; p.trigUid = t.uid; p.remap = keys; pendPush(p); }
        } else if (t.id == 3607) {   // Sequence (0x655c50): the count per remap key (the trigger's maps +0x758 last call [float, -1 none], +0x798 count), then the group of the list that holds that count
            const int key = t.seqUnique && !curRemap_.empty() ? -curRemap_.back().second : 0;
            SeqState& s = seq_[(long long)t.uid * 4294967296LL + (unsigned)key];
            const float lastT = s.has ? s.lastT : -1.0f;
            int idx = s.idx;
            const float dt = (float)(levelTime_ - (double)lastT);
            const float minInt = (float)t.seqMinInt, reset = (float)t.seqReset;
            if (minInt <= 0.0f || lastT < 0.0f || minInt <= dt) {
                if (idx > 0 && reset > 0.0f && 0.0f <= lastT && reset < dt) {
                    if (t.seqResetMode == 0) idx = 0;
                    else { const float f = std::floor(dt / reset); const float v = (float)idx - f; idx = (int)(0.0f <= v ? v : 0.0f); }
                }
                s.lastT = (float)levelTime_; s.has = true;
                int total = 0; for (const auto& e : t.seqTargets) total += e.second;
                idx += 1;
                if (total < idx) {
                    if (t.seqMode == 0) return;
                    if (t.seqMode == 1) idx = 1; else if (t.seqMode == 2) idx = total;
                }
                int group = 0, acc = 0;
                for (const auto& e : t.seqTargets) { acc += e.second; if (idx <= acc) { group = e.first; break; } }
                s.idx = idx;
                const int keepSp = curSpawnUid_;
                curSpawnUid_ = t.uid;
                spawnGroup(group, false, 0.0);
                curSpawnUid_ = keepSp;
            }
        } else if ((t.id == 1912 || t.id == 2068) && t.randOk) {   // Random / Advanced Random: one draw of the trigger LCG (the global at base+0x90bf68 on 22091: x' = x * 0x343fd + 0x269ec3, r = (x' >> 16) & 0x7fff), then the chosen group is spawned
            rng_ = rng_ * 0x343fdULL + 0x269ec3ULL;
            const float r = (float)(unsigned)((rng_ >> 16) & 0x7fff) / 32767.0f;
            int group = 0;
            if (t.id == 1912) group = (float)t.randChance < r * 100.0f ? t.center : t.target;
            else {
                int sum = 0; for (const auto& e : t.seqTargets) sum += e.second;
                const int x = (int)std::round(r * (float)sum);
                int acc = 0;
                for (const auto& e : t.seqTargets) { acc += e.second; if (x <= acc) { group = e.first; break; } }
            }
            ++randDraws_;
            if (g_randLogReplay && !g_randLogFile.empty()) if (FILE* lf = std::fopen(g_randLogFile.c_str(), "a")) { std::string cands; if (t.id == 2068) for (const auto& e : t.seqTargets) cands += (cands.empty() ? "" : ".") + std::to_string(e.first); else cands = std::to_string(t.target) + "." + std::to_string(t.center); std::fprintf(lf, "%lld,%d,%d,%u,%d,%llu,%s\n", stepNo_, t.uid, t.id, (unsigned)((rng_ >> 16) & 0x7fff), group, (unsigned long long)(rng_ & 0xffffffffULL), cands.c_str()); std::fclose(lf); }
            if (group > 0) { const int keepSp = curSpawnUid_; curSpawnUid_ = t.uid; spawnGroup(group, false, 0.0); curSpawnUid_ = keepSp; }
        } else if (t.id == 2900) {   // Rotate Gameplay: only its channel half is here (rotateGameplay 0x31f980: m_changeChannel -> layer +0x33c = m_targetChannelID, and the channel's reverse bit = direction 2 / 3)
            if (t.rgSwarm && t.rgSwch >= 0) { activeChan_ = t.rgSwch; chanRev_[t.rgSwch] = (unsigned)(t.rgDir - 2) < 2u; }   // the channel's reverse bit is the pure predicate gnddir - 2 <u 2 (dp/src/dp/step.hpp applyRotation)
            if (!t.rgChanOnly) vertical_ = t.rgDir == 1 || t.rgDir == 2;   // the rotation half: up / down travel along y, left / right along x (measured: 24 of 24 fires after a dir-2 turn and 68 of 74 after a dir-1 turn line up with y, 831 of 849 after a dir-4 turn and all after dir 3 with x)
        } else if (t.id == 3022) {   // Teleport: the player is put somewhere (outside the collision pass); the event says when and which trigger
            events.push_back({curTick_, t.uid, t.id, t.raw});
        } else if (t.id == 2066) {   // Gravity: the trigger sets the value (EffectGameObject::triggerObject; +0xb8c of the player it names)
            gravChanges.push_back({curTick_, t.gpl, t.gmod, t.uid});
        } else if (t.id == 2899) {   // Options (processOptionsTrigger 0x32ba90): the controls of a player are disabled / enabled
            if (t.optp1 != 0) { controlsDisabled[0] = t.optp1 == 1; ctrlChanges.push_back({curTick_, 0, controlsDisabled[0]}); }
            if (t.optp2 != 0) { controlsDisabled[1] = t.optp2 == 1; ctrlChanges.push_back({curTick_, 1, controlsDisabled[1]}); }
        } else if (t.id == 1612 || t.id == 1613) {
            playerHidden = t.id == 1612;
            hiddenChanges.push_back({curTick_, playerHidden});
        } else if (t.id == 1616) {   // Stop: an Area Move whose trigger is in the target group ends (controlAreaEffect 0x330670, command 0); its objects stay put and are reset by the next step's end-of-step rule
            {   // the Stop (TriggerControlGameObject::triggerObject -> controlTriggersInGroup 0x325a90 -> FUN_140368bc0 0x368bc0), mode Stop: for each trigger object in the target group, the commands it made
                // (cmd.+0x158 == its uid; Move, Rotate, ... ) get +0x72 = 1 (no more motion from the next step, erased there); a Spawn's delayed spawns (+0x1c == its uid) are erased.
                // The mode (+0x760: 0 stop, 1 pause, 2 resume) and "use control ID" (+0x69c) are not in triggers.txt: every Stop is read as a plain stop of the group.
                const auto gm = members.find(t.target);
                if (gm != members.end())
                    for (size_t oi : gm->second) {
                        const auto ti = trigByUid_.find(objs[oi].uid);
                        if (ti == trigByUid_.end()) continue;
                        const TrigDef& x = trigs_[ti->second];
                        if (x.id == 901 || x.id == 1346) {
                            for (Cmd& c : cmds_) {
                                if (c.uid != x.uid) continue;
                                if (t.stopMode == 0) c.done = true; else c.paused = t.stopMode == 1;
                            }
                        } else if (x.id == 1268) {
                            if (t.stopMode == 0) {   // controlActionsForControlID 0x368280: each match is erased in place (the tail moves down by one, the end by one entry); the loop of updateSpawns, if it is the caller, keeps its start length and reads the stale slots at the end
                                for (size_t k = 0; k < pendLen_;) {
                                    if (pending_[k].trigUid != x.uid) { ++k; continue; }
                                    for (size_t m = k; m + 1 < pendLen_; ++m) pending_[m] = pending_[m + 1];
                                    --pendLen_;
                                }
                            }
                            else for (size_t k = 0; k < pendLen_; ++k) if (pending_[k].trigUid == x.uid) pending_[k].paused = t.stopMode == 1;
                        }
                    }
            }
            if (t.stopMode == 0 && !t.touch && !t.spawn && (t.chan == 0 || chanModel_)) {
                areaInsts_.erase(std::remove_if(areaInsts_.begin(), areaInsts_.end(), [&](int au) {
                    const auto bi = byUid.find(au);
                    if (bi == byUid.end()) return false;
                    for (int g : objs[bi->second].groups) if (g == t.target) return true;
                    return false;
                }), areaInsts_.end());
            }
        } else if (t.id == 1815) {   // Collision trigger: registerCollisionTrigger (the pairs are checked by stepCollisions)
            if (collisionModel) registerCollision(t);
        } else if (t.id == 3620) {   // Item Compare (GJBaseGameLayer::activateItemCompareTrigger 0x33d310): the true group (target) or the false one (centre) is spawned
            const int g = itemCompare(t) ? t.target : t.center;
            if (g > 0) { const int keepSp = curSpawnUid_; curSpawnUid_ = t.uid; spawnGroup(g, false, 0.0); curSpawnUid_ = keepSp; }
        } else if (t.id == 3619) {   // Item Edit (activateItemEditTrigger 0x33cf40)
            fireItemEdit(t);
        } else if (t.id == 1817) {   // Pickup trigger (CountTriggerGameObject::triggerObject 0x65c2b0, case 0x719): mode 1 multiplies the item's number by the factor, mode 2 divides it (a factor of 0: nothing is written), else count is added (override: set)
            const int cur = itemCountOf(t.pkItem);
            if (t.pkMode == 1) updateCountForItem(t.pkItem, (int)std::roundf((float)cur * t.pkFactor));
            else if (t.pkMode == 2) { if (t.pkFactor != 0.0f) updateCountForItem(t.pkItem, (int)std::roundf((float)cur / t.pkFactor)); }
            else updateCountForItem(t.pkItem, t.pkOverride ? t.pkCount : cur + t.pkCount);
        } else if (t.id == 3641) {   // Item Persist (activatePersistentItemTrigger 0x33d730): one item switched on / off the persistent set and, with reset, set to 0; "all": reset every persistent item, and (switched off) empty the set
            if (!t.psAll) {
                if (t.psEnable) persistItems_.insert(t.pkItem); else persistItems_.erase(t.pkItem);
                if (t.psReset) updateCountForItem(t.pkItem, 0);
            } else {
                if (t.psReset) { const std::set<int> all = persistItems_; for (int it : all) updateCountForItem(it, 0); }
                if (!t.psEnable) persistItems_.clear();
            }
        } else if (t.id == 3024) {   // Area Stop (EffectGameObject::triggerObject case 0xbd0): the first Area in force whose effect id is the trigger's target ends (an id of 0 or less: every one of them)
            for (auto it = areaInsts_.begin(); it != areaInsts_.end();) {
                const auto fx = areaFx_.find(*it);
                if (fx != areaFx_.end() && fx->second == t.target) { it = areaInsts_.erase(it); if (t.target > 0) break; }
                else ++it;
            }
        } else if (t.id >= 3011 && t.id <= 3015) {
            fireAreaEdit(t);
        } else if (t.id == 1611) {   // a Count: the entry is registered with the item's number as it stands
            CountEntry e; e.multi = t.cntMulti; e.activate = t.cntAct; e.last = itemCountOf(t.cntItem); e.target = t.cntCount; e.group = t.target; e.trigUid = t.uid;
            countReg_[t.cntItem < 0 ? 0 : (t.cntItem > 9999 ? 9999 : t.cntItem)].push_back(e);
        } else if (t.id == 1811) {   // an Instant Count: the number against the count at once (mode 0 equal, 1 more than the count, 2 less than it)
            const int n = itemCountOf(t.cntItem);
            const bool ok = t.cntMode == 0 ? n == t.cntCount : (t.cntMode == 1 ? t.cntCount < n : (t.cntMode == 2 ? n < t.cntCount : false));
            if (ok) toggleGroupTriggered(t.target, t.cntAct, t.uid);
        } else if (t.id == 3006 || t.id == 3008) {
            if (areaDefs_.count(t.uid)) {
                const auto bi = byUid.find(t.uid);
                if (bi == byUid.end() || objs[bi->second].on()) {   // a Toggle that switched the trigger off before it fired
                    areaDefs_[t.uid] = areaOrig_.at(t.uid); areaTrans_.erase(t.uid); areaEO_.erase(t.uid);   // (firing again: the instance reloads the trigger's values)
                    areaInsts_.push_back(t.uid);
                }
            }
        } else if (t.id == 1935) {
            if (t.tw > 0.0) warp_ = std::clamp((float)t.tw, 0.1f, 2.0f);   // PlayLayer::updateTimeWarp, GD Win 2.2091 RVA 0x50a264..0x50a285.
        } else if (t.id == 1346) {
            Cmd c;
            c.uid = t.uid; c.id = t.id; c.lockrot = t.lockrot;
            c.rotate = true;
            c.total = t.deg + 360.0 * t.t360; c.dur = t.dur; c.kind = t.ease; c.rate = t.erate; c.target = t.target; c.center = t.center; c.ctrObj = t.ctrObj; c.ctrCol = t.ctrCol;
            // FUN_140369920 (GJEffectManager, called with the manager's byte +0x820 when a command is registered under its key [target group, centre group]): with the byte set the key's list is emptied first, so a Rotate
            // that fires while an earlier one of the same (target, centre) is running REPLACES it; with the byte clear both run and add (claimRotationAction). The byte is set by GJBaseGameLayer::resetLevelVariables
            // from the level (`level+0x1c8 == 0`): measured on 64 customs it follows the levels of the old version (levelsettings.txt fixRadiusCollision = fixGravityBug = fixNegativeScale = 0), see rotReplace
            if (rotReplace) for (Cmd& o : cmds_) if (o.rotate && !o.done && o.target == c.target && o.center == c.center) o.done = true;
            cmds_.push_back(c);
        } else if (t.id == 2067 && t.sclOk) {
            Cmd c;
            c.uid = t.uid; c.id = t.id; c.scale = true;
            c.sclX = t.sclX; c.sclY = t.sclY; c.dur = t.dur; c.kind = t.ease; c.rate = t.erate; c.target = t.target; c.center = t.center; c.ctrObj = t.ctrObj; c.ctrCol = t.ctrCol;
            // triggerTransformCommand 0x327220: with Relative Scale and a centre object, each factor S (after the divide) becomes (S - 1) / r + 1, r = the centre object's own scale / the one it was loaded with
            if (t.sclRel) {
                size_t ci = 0; bool haveCentre = false;
                if (c.ctrCol) {
                    if (c.ctrObj > 0) { const auto bi = byUid.find(c.ctrObj); if (bi != byUid.end()) { ci = bi->second; haveCentre = true; } }
                } else {
                    const auto ct = c.center ? members.find(c.center) : members.end();
                    if (ct != members.end() && !ct->second.empty()) { ci = ct->second.front(); haveCentre = true; }
                }
                if (haveCentre) {
                    const double rx = (double)objs[ci].osx, ry = (double)objs[ci].osy;
                    if (rx != 0.0) c.sclX = (c.sclX - 1.0) / rx + 1.0;
                    if (ry != 0.0) c.sclY = (c.sclY - 1.0) / ry + 1.0;
                }
            }
            cmds_.push_back(c);
        }
    }

    void checkSpawnObjects(long long, double px, double py) {
        for (;;) {   // the channel is read again for every trigger: a 2900 that switches it sends the rest of the loop to the new channel; the cursor that moves is the one of the channel the trigger was in
            const auto qi = chanQ_.find(activeChan_);
            if (qi == chanQ_.end()) break;
            size_t& cur = chanIdx_[activeChan_];
            if (cur >= qi->second.size()) break;
            const TrigDef& t = trigs_[qi->second[cur]];
            const auto ri = chanRev_.find(activeChan_);
            const bool rev = ri != chanRev_.end() && ri->second;
            if (vertical_ && !pyKnown_) { ++queueWaits; ++g_vertWaitsY; break; }   // the axis the trigger is compared on is not known this step: it does not fire on a guess
            const double ref = vertical_ ? py : px, at = vertical_ ? t.cy : t.cx;   // the player's +0x9cb: the trigger is compared on the axis the player travels along
            if (rev ? (at < ref) : (ref < at)) { ++queueWaits; break; }
            ++cur;
            // a trigger whose own object is switched off (a Toggle of one of its groups) is passed over without firing (checkSpawnObjects)
            const auto bi = byUid.find(t.uid);
            if (bi == byUid.end() || objs[bi->second].on()) fire(t);
        }
    }
};

}  // namespace gdtrig
}  // namespace dp
