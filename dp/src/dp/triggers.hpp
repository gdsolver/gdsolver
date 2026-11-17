#pragma once
#include "dp/groups.hpp"

namespace dp {

// ---- TOUCH TRIGGERS -------------------------------------------------------
//
// The timeline above is pure observation, and that is exactly why it cannot see
// a door nobody opened. lv19 x=28,095 is the case that cost three sessions: two
// 30x30 solids (uid 13514/13521) fill the ONLY gap in a slab wall and never
// move in any recording, so the model reads the wall as closed, dies at
// x=28,075, and the next iteration plans against the same closed wall. The plan
// never touches the trigger, so the recording never shows the door open, so the
// plan never touches the trigger. A closed loop -- and the reason the level sat
// at 86.6% while the search was blamed.
//
// GD has the missing fact and it is STATIC: EffectGameObject carries
// m_isTouchTriggered / m_targetGroupID / m_moveOffset, GameObject carries its
// group ids. The MOD dumps both (triggers.txt, objgroups.txt) and the chain
// resolves without implementing any trigger SEMANTICS:
//
//   uid 13505  id 1268 spawn, touch=1, box (27913,397) 30x30   -> group 89
//   group 89 = uid 13484/13485, id 901 move, offset (0,-45)/(0,+45)
//                                                              -> groups 87/88
//   groups 87/88 = uid 13514/13521 = the two door blocks
//
// 285-45 = 240 and 315+45 = 360, which is exactly where GD puts them once the
// box is touched (measured through grouptrace), and 0.4229 s = 101 ticks
// against 98 ticks of recorded motion.
//
// FIRING CONDITION, measured on that trigger (mini UFO, half 9, injected at the
// box centre x=27,913):
//   y=372 -> player top 381, 1 px BELOW the box -> does not fire
//   y=374 -> player top 383, 1 px inside        -> fires
// so it is plain rect overlap of the player's box with the trigger's rect. No
// click is involved (the nearest input was 300 ticks away) and it is not
// x-crossing either: the SAME run flew through the other touch trigger
// (27553,269) and opened that gate, then passed under this one 83 px low.
//
// Only 3 of lv19's 313 targeted triggers have touch=1 and lv1-18 have none, so
// this is inert everywhere it is not needed.
// (kTouchBits, TouchMask and touchBit are in prelude.hpp -- the root of the
// include chain, because State, the recording's masks and the run's outcome
// all hold this mask and see each other only through that file.)
struct TouchTrig {
    double cx, cy, hw, hh;          // the box the player has to enter
    std::vector<TrigCtl> ctl;
    // The TRIGGER's own uid. Bit numbering here is a property of this build's
    // window (the first 32 boxes from the anchor), so anything arriving from
    // outside -- an anchor payload written by the mod, say -- has to name the
    // trigger and let this side do the mapping. Carrying a bit index across
    // that boundary would silently mean a different box whenever the window
    // moved.
    int uid = -1;
    // The root row's own id and, for a Toggle (1049), which way it switches its
    // target group (TrigRow::togon: 1 on, 0 off). -1 for everything else. Read
    // only by --rotqtoggle, which needs to know that entering this box switches
    // off a group holding rotation-queue objects.
    int id = 0;
    int togOn = -1;
    // --countactgrp: a Count whose counter switches its target group (togEv holds that group's objects), so
    // its switches are the state's on/off as an activator's are (boxOnEvents, level_loader's --activators)
    bool countAct = false;
    // The objects a Toggle INSIDE this box's chain switches ON -- the root row's
    // own toggle is `togOn` just above, and a nested one had nowhere to go. It
    // is read by the coin gate (cli.hpp coinEnable): `Dynamics::on` comes from
    // the RECORDING, so a coin whose group is off until some box is entered is
    // held off for every state by a recording of a run that never entered it.
    // SubZero 4002's first coin is exactly that -- the touch Spawn uid 500 at
    // (1661,263) spawns group 679, whose Toggle uid 505 switches the coin's
    // group 678 on -- and the game says the coin cannot be collected before it
    // (teleported onto the coin at t=1,376: no pickup; onto the second coin,
    // whose group is on: pickup).
    std::vector<int> togOnUids;
    // --spawnroots (g_spawnRoots): the Counts and Taps this box's chain spawns (their uids, the
    // walk stops at them), and -- on such a root -- the box bits that arm it. Empty / zero without
    // the flag, so nothing reads either.
    std::vector<int> spawnsRoots;
    TouchMask armBy{};
    // ...and the same thing with what a general on/off state needs beside it:
    // which WAY the Toggle switches, and how long after the box fires it does
    // so (the chain's spawn delays, in ticks). `togOnUids` is the "switches on,
    // no delay" subset that the coin gate reads today; this table is what the
    // on/off state will be built from, and it is collected now so that the two
    // can be compared on the corpus before anything reads it.
    //
    // A mask alone cannot serve: it is monotone (a bit that is set stays set),
    // so it cannot express a switch-off later in the same chain, two chains
    // that fight over one object, or a delay before either happens. Each event
    // carries its own effective tick, and the last one whose tick has passed is
    // the object's state.
    struct TogEv {
        int uid;
        uint8_t on;        // 1 = the Toggle activates the group, 0 = deactivates
        double delay;      // ticks after this box fires (the chain's spawn delays)
        int grp = 0;       // the group the Toggle switches (read by --tapdisarm, Dynamics::disarmEv)
    };
    std::vector<TogEv> togEv;
    // --tapdisarm (g_tapDisarm): ...and the TRIGGERS those Toggles switch, the same way (uid, which way,
    // delay). togEv keeps objects only. Tied to g_autoTrig entries by the CLI (g_autoDisarm). Empty
    // without the flag.
    std::vector<TogEv> trigTog;
    // A pickup that switches its group on when touched (--activators, g_activators). Its bit
    // is set the way an ordinary box's is -- the player's box overlapping its rect.
    bool activator = false;
    // ...and, for a toggle block (1594), only while the button is down (markTouched's
    // `btnHeld`): overlapping it without pressing does nothing in the game.
    bool press = false;
    // ...and a toggle ring whose m_isSpawnOnly is set (--orbspawn): a fresh press inside it
    // goes to the ring, so the grounded jump or tap it would have made does not happen
    // (step.hpp, `ringTookPress`).
    bool spawnRing = false;
    // --boxoff: an activator whose switch is OFF. Its objects stay on their recording, and the
    // box only takes them out once it has fired (Dynamics::offBox) -- see the loader.
    bool offSwitch = false;
    // ---- a COUNT trigger rather than a box (1611 / 1811) ------------------
    // Same chain, same bit, same fire tick -- only the firing CONDITION
    // differs: not "the player entered this rect" but "the item counter has
    // reached this value". lv21's third coin is behind one (item 1 == 10 opens
    // group 222, which holds the Move that brings the coin into reach) and
    // lv22's first behind another (item 2 == 5). `count` < 0 means this entry
    // is an ordinary touch box; markTouched skips the ones where it is not.
    int item = 0;
    int count = -1;
    // 0 = equals, 1 = larger, 2 = smaller (a Count's own order); 3 = at least,
    // which only an Item Compare root carries (itemCompareGate).
    int cmode = 0;
    // ---- a TAP trigger (1595) rather than a box ---------------------------
    // GD's "Touch trigger", which is NOT the same thing as a trigger marked
    // touch-triggered: this one fires on the PLAYER'S TAP. Measured on the rig
    // calib_coingate5 (2026-09-20), with a coin whose group the trigger
    // switches on: a tap 185 px BEFORE the trigger does nothing, a tap 6 px
    // before it does nothing, and a tap 146 px past it fires. So it arms when
    // the player's x crosses it and any press after that sets it off.
    bool tap = false;
    // ...and a Tap that feeds a counter fires on EVERY press, not once: lv22's
    // third coin needs six (x=15,525 on, confirmed in the game). Each press
    // spawns the chain again, and a Pickup in it adds to the item. The window
    // shuts where an x-crossing Stop halts the group holding that Pickup
    // (lv22: uid17961 at x=15,765). 1e18 = no such Stop, the window never
    // shuts. Read by the tap counter in cli.hpp (State::taps).
    double tapCloseX = 1e18;
    // ...and where each end is IN GD'S TERMS. On a level with rotated gameplay
    // an x-crossing trigger is not fired by x: it sits in its CHANNEL's queue
    // (the dump's `chan`) and fires when that channel is active and the player
    // passes it in the channel's direction (step.hpp's queue walk). lv22's tap
    // is on channel 9 (x-) and its Stop on channel 10 (x+), so the window is
    // open on the maze's second pass, not the first -- the game counted a press
    // at t=14,223, x~15,73x and none on the first pass through the same x.
    int tapChan = 0, tapCloseChan = 0;
    double tapCloseY = 0.0;
    // ...and whether that Stop is a TOUCH box (its m_isTouchTriggered), with the box's half sizes
    // (triggers.txt w/h, the trigger's getObjectRect) and its uid. Read only under --tapclosebox
    // (g_tapCloseBox): such a Stop shuts the window when the player's rect overlaps its rect, not
    // when the player's x passes its centre.
    bool tapCloseTouch = false;
    double tapCloseHw = 0.0, tapCloseHh = 0.0;
    int tapCloseUid = -1;
    // The chain reaches a coin (set at load for Count, Tap and Item Compare
    // roots; the window's feeder test needs it after the walk).
    bool reachesCoin = false;
    // --movetarget only: the Move triggers a Stop in this box's chain halts
    // (the trigger members of the Stop's target group), and the chain's spawn
    // delay to that Stop in ticks.
    std::vector<int> stops;
    double stopDelay = 0.0;
    // ---- a box that MOVES with its own group ------------------------------
    // The trigger object sits in groups too, and a Move aimed at one of them
    // carries the box along. SubZero 4002 x~31,353: the touch Toggle uid 18126
    // that brings the saw 18043 in shares the saw's group 87, which a Move
    // (x=31,899) drops 600 px; the reversed player then touches the box where it
    // came DOWN to, the saw appears in front of it and kills it. With the box at
    // its placed height the model never saw that touch (the user watched the
    // replay and confirmed the route: it goes past x=31,899 into the coin
    // passage, and that is what arms it).
    // The trigger itself is not recorded, so it follows a recorded object that
    // is moved by exactly the same Moves (index into the level's dynamic
    // objects, set by the CLI after the level is loaded): the box's centre is
    // (cx, cy) plus how far that object has come from (followX0, followY0).
    // -1 = the box does not move (every box in lv1-21).
    int followIdx = -1;
    double followX0 = 0.0, followY0 = 0.0;
    // --boxarm: a toggle block that switches Moves back on, with rows of its own in the recording (its group moves
    // and turns it, which followIdx refuses): markTouched tests it where the recording has it on the tick, turned.
    // Set by the CLI; null otherwise.
    const std::vector<DynSample>* recBox = nullptr;
    // --touchobb: a turned box (g_obb has its trigger), tested as its oriented box: half sides and the turn in GD's
    // degrees (clockwise). Set by the CLI; obb false otherwise.
    bool obb = false;
    double obbHw = 0.0, obbHh = 0.0, obbRot = 0.0;
    // ---- a box whose own trigger a Toggle switches (--touchoff) -----------
    // The x-crossing Toggles whose target group holds this box's trigger, in x order: which
    // way each switches and its g_autoTrig index (resolved by the CLI once g_autoTrig is
    // loaded). markTouched skips the box while the last of them to fire switched it off
    // (touchOwnOn, step.hpp). Empty without the flag, and empty when a spawned or touched
    // Toggle switches that group too -- a chain this does not follow, so the box is left as
    // it was.
    struct OwnTog {
        int uid;
        double cx;
        uint8_t on;
        int autoIdx;
    };
    std::vector<OwnTog> ownTog;
};
// The uids of this level's coins, filled by the CLI before the touch window is
// built (level_loader.hpp's coinUids). Empty = not known, and then the filter
// that uses it does nothing: a build that forgets to fill it keeps the old
// behaviour instead of silently dropping every counter.
inline std::unordered_set<int> g_coinUids;
// The level's dynamic objects, for boxes that follow one (TouchTrig::followIdx).
// Their rects are this tick's (Dynamics::seek writes them in place before the
// layer is stepped); markTouched only reads them. Set by the CLI once the level
// is loaded, nullptr before that and in a build that never sets it.
inline const std::vector<Obj>* g_touchFollowObjs = nullptr;
// uid -> the item it gives, for EVERY row of items.txt and not only the ones
// the player can touch (g_collect keeps those). lv22's first coin is fed by
// Pickup triggers that a touch box spawns -- nothing touches them -- and the
// window has to know that those boxes are the ones it must not drop.
inline std::unordered_map<int, int> g_itemGiver;
// --itemsnoblock: a Collision Block (id 1816) is not an item giver. It carries its BLOCK ID in
// m_itemID -- property 80, the key a Pickup's item uses -- so items.txt lists it with an
// "item". lv22's two (uid 18153 = block 1, uid 18154 = block 2, which rides the switch band's
// ceiling, group 265) are the blocks of Collision trigger uid 18155, and every box whose chain
// moves the ceiling was credited with uid 18154's "item 2": the yellow 33x33 boxes counted 2
// (the Pickup uid 18173 their chain spawns, plus the block) and the 21x33 ones 1 (the block
// alone). GD's `itemcnt:` gives 1 and 0 -- on the coin route boxes 2195, 2303, 2459, 18179 and
// 2563 add one each (5 = the gate), and 18091, overlapped for 11 ticks, adds none -- so the
// search fired `item 2 == 5` at t=2,652 on a path where GD's counter stood at 2.
// ON BY DEFAULT since 2026-09-24 (GD's per-box `itemcnt:` matched on every box type
// of lv22's switch band); --no-itemsnoblock counts the block again.
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --walkgates: the single-trajectory walks (--replay, the witness resim) fire the item gates --
// pickups, Count and Tap triggers, the counting tap -- the way the search's step does (cli.hpp
// itemGates). markTouched skips them, so without this a walk never fires one, and the search and
// its own resim plan in different worlds wherever a gate moves geometry. Needs --coins (the
// gate tables are built only then).
// ON BY DEFAULT since 2026-09-24. Inert without --coins (no gate tables, and the walks'
// trigger lists only gain entries markTouched skips); --no-walkgates turns it off.
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --fbforce <box>:<tick> (diagnostic, --replay only): see the use in cli.hpp.
inline int g_fbForceBox = -1;
inline int g_fbForceTick = 0;
// --replayon (diagnostic, --replay only): walk past the first death (cli.hpp).
inline bool g_replayOn = false;
// The collectibles a Count trigger is waiting for (items.txt, pickup=1). Their
// geometry is read the same way a coin's is -- the player's own box against the
// object -- and the set a lineage has taken lives in State::items.
struct Collectible {
    double cx, cy, hw, hh;
    int item = 0;
    int uid = -1;
};
inline std::vector<Collectible> g_collect;
// Bit b of State::trig is g_touch[b]. Global because the step function, the
// layer loop and the witness resim all need the same numbering, and there is
// exactly one level in flight.
inline std::vector<TouchTrig> g_touch;
// The toggle blocks among them (TouchTrig::press), for the dedupe key (search_key.hpp): near one
// the state has not fired, whether the button is down decides whether the next tick fires it, so
// a held child and a released one are not the same node there. Frame-0 x range, grown by the
// widest player half and a tick's travel, and the box's bit. Built beside g_touch (cli.hpp).
struct PressWin {
    double lo, hi;
    int bit;
};
inline std::vector<PressWin> g_pressWin;
// ...and the bits of every ACTIVATOR box (TouchTrig::activator: a pickup or a toggle block that
// switches its group on). Under --coins the alive cap's water-fill treats "which of these has
// fired" as a class of its own (cli.hpp classOf), the same way it treats the collected coins: the
// lineage that switched a coin's way open is a minority of the layer and was thinned at the
// majority's stride. Empty without --activators. Built beside g_touch (cli.hpp).
inline TouchMask g_actMask{};
// --activators: a PICKUP (objrects type 30) that switches its target group on when the player
// touches it becomes a box of its own. SubZero 4002's key (uid 6166, id 1275) is one: touching
// it switches on group 683, the ten platforms under the second coin, on the next tick
// (measured on worker 98 by teleporting into it; 45 px below it, nothing). ON BY DEFAULT with the
// SubZero coin set below (--no-activators turns it off): the release reports what the defaults
// solve, and a flag that has to be picked per level is an answer brought in from outside.
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --orbspawn (on by default since 2026-10, needs --activators): a toggle ring (1594) with
// m_isSpawnOnly becomes a press activator whose effect is the chain its target group spawns (see
// the construction).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --movenospawn (on by default since 2026-10): the autonomous walk does not descend from a Move
// root into the spawn-triggered triggers of its target group (loadAutoTriggers, where the
// measurement is).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --coinpick (off): under --coins, the state a horizon cut, a memory cut or a PARTIAL emits is the
// first one holding the MOST coins, not simply the first. Measured on SubZero 4003 (2026-09-24):
// from t=6,946 the search took coin 1 on some branches (t=8,438) and still emitted a lineage
// without it, because both reached the last layer and the pick was `front()`. ON BY DEFAULT
// (--no-coinpick); inert without --coins.
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --airpress (off): an airborne cube's press is expanded too, and its buffer (State::jumpBuf) is in
// the dedupe key while it is airborne. A press held from the air into a landing jumps on GD's
// physics pass (y moves on the tick after the landing), while a press on the landing tick takes the
// rising-edge pass (y frozen that tick) -- 2.5 px apart at a cube's jump speed. With the press child
// pruned as moot, the search could only ever do the second. Measured on SubZero 4003 (2026-09-24):
// a human route lands on a ceiling at t=8,191 with the button held from 8,190 and is at y=402.48
// on 8,192; the search's own lineage, same inputs, is at 405.0, and the route is lost there.
// OFF BY DEFAULT, unlike the rest of the SubZero coin set: on, it alone changed official lv22's
// first coin-on plan (4 -> 10 inputs; --no-airpress put the plan back byte for byte, no other
// flag of the set did) and the run never got past x=4,125 in 1,200 s, where the baseline and the
// run with it off clear in 39 rounds.
// On by default since --airpressnear keeps the air press child only where a landing is within reach: the two
// together clear lv22's coin run (3 of 3) where --airpress alone stalled at x=4,125.
inline constexpr bool kDefAirPress = true;
inline bool g_airPress = kDefAirPress;
// --airpressnear (default on, with --airpress): the air press child is expanded only where a surface lies within
// reach of a landing (search_dp.hpp landingNear). The press held into a landing is one future whichever tick of
// the arc it began on, so --airpress alone pays for the children of every tick of every air arc.
inline bool g_airPressNear = true;
// --refadopt (off): let --refwatch carry on through a merge on a SURVIVOR that differs from the
// reference only in fields the key leaves out (see the ADOPT branch in cli.hpp). Off, the watch is
// strict: the first merge ends it, as it always did. Those fields are NOT inert in general --
// `action` is the press edge, `rot` feeds the OBB tests, `groundUid` picks the support, `fireB`
// places geometry -- so everything past the first ADOPT is about a substitute
// lineage, and the watch says so (g_refAdoptT, g_refAdoptFields).
inline bool g_refAdopt = false;
// --refadoptmem (off; implies --refadopt): ...and also through a merge whose survivor differs in
// `jumpBuf` (the press buffer) or the ring memory (usedOrb, usedOrbOld, usedOrbHist, both bodies),
// which the key leaves out too. Diagnostic: it asks whether such a merge loses the reference's
// route or only relabels it. A ship's thrust, a cube's buffered landing jump and a ring's one-use
// rule read these, so a lineage past such an ADOPT is a substitute, named as the others are.
inline bool g_refAdoptMem = false;
// --spawnroots (off): under --coins, a Count (1611/1811) or a Tap (1595) that a box's chain SPAWNS
// is a root of its own and the walk stops at it (as it already does at an Item Compare), and the
// root is ARMED by that box (TouchTrig::armBy) rather than by its x: the Count listens, and the
// Tap counts presses, only once the box has fired; a spawned Tap with no Stop keeps counting.
// SubZero 4003's third coin (confirmed in the game, 2026-09-24): box 6102 spawns a Tap that gives
// item 1 per press and a Count `item 1 == 4` that switches the coin on. Walked through, the box
// itself was read as switching the coin on and giving the item, so the search planned a world
// where entering the box was enough -- and in the game the coin stayed off. Four presses after
// the box took it to 3/3. ON BY DEFAULT (--no-spawnroots); inert without --coins.
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --tapclosebox (off): a counting Tap whose window a TOUCH-box Stop shuts (TouchTrig::tapCloseTouch,
// found under --spawnroots) shuts it where GD fires that Stop -- on the first tick the player's rect
// overlaps the Stop's rect -- instead of where the player's x passes the Stop's centre. GD
// (2.2081): collisionCheckObjects 0x214960 tests the player's getObjectRect against the object's
// getObjectRect, edges inclusive (min <= max on both axes), and a Modifier (type 20) with
// m_isTouchTriggered goes to playerTouchedTrigger 0x217f50, which fires it in that same pass. A
// tick's collision pass comes before its press (markTouched's note; SubZero 4003's Tap counts a
// press on its own tick, `itemcnt: t=13238` for the press at 13238), so a press on the tick the
// rects first overlap counts nothing. SubZero 4003: the Stop uid 6817 (10695,1499) 63.9x63.9 is
// overlapped from t=13517 (x=10649.1, 45.9 px short of its centre); the centre test shut the window
// 35 ticks later, and the search planned a fourth press at 13550 that GD does not count (attempt 77:
// itemcnt 1..3 only).
// ...and at an anchor, a box that arms a spawned Count or Tap (TouchTrig::armBy) and that the
// attempt had entered by t0 (--touchentered uid:tick) starts set, and a window whose touch-box Stop
// the attempt had entered by t0 starts shut. The recording scan below opens a box only from the
// motion of what its chain moves, and 6102's chain reaches the Count and the Tap (roots the walk
// stops at), so an anchor inside the window (t0=13212, box entered at 13139) armed neither.
// On by default since 2026-10 (was opt-in).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --no-offmoves turns this off: an x-crossing Move whose own group a Toggle has switched off by the
// time the player reaches it does not fire (offAtCrossing), and a touch box whose Toggle switches
// that group back on moves the Move's group instead (loadTouchTriggers). Confirmed in the game on
// Meltdown 1003 (2026-09-27): the Move uid 3437 at x=7,125 (group 143 down 60) sits in group 145,
// which the Toggle uid 20 at x=1 switches off and only the touch Toggle uid 1711 at (4,889, 421)
// switches back on. A cube put at (7,303.2, 303.4) with vy 9.452 on t=5,620 dies on t=5,630 to the
// spike uid 3632 at y=315, where it was loaded; put into the box first, the same cube passes over
// the lowered platform. The model moved the group either way, placed that spike at y=255 and let
// the cube live, so every plan through there died in the game on the same tick.
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
inline long long g_refAdoptT = -1;       // the first ADOPT's tick, -1 = none
inline std::string g_refAdoptFields;     // every field any ADOPT had to overlook
// The on/off switches the touch boxes give each object (TouchTrig::togEv), by object uid, each
// tied to its box's bit. NOT from a Tap, Count or Item Compare root: there the counter decides
// and the box's own bit is set by the first press -- lv22's third coin sits behind six presses,
// and reading its Toggle off the first one would switch it on five presses early (the coin gate
// in cli.hpp skips them for the same reason).
inline std::unordered_map<int, std::vector<OnEvent>> boxOnEvents(
        const std::vector<TouchTrig>& touch) {
    std::unordered_map<int, std::vector<OnEvent>> out;
    for (size_t b = 0; b < touch.size() && b < (size_t)kTouchBits; ++b) {
        const TouchTrig& T = touch[b];
        // (--countactgrp: a Count's own switch is fired by its counter, which sets its bit; TouchTrig::countAct)
        if (T.tap || (T.count >= 0 && !T.countAct)) continue;
        for (const TouchTrig::TogEv& e : T.togEv) {
            OnEvent ev;
            ev.box = (int)b;
            ev.on = e.on;
            ev.delay = e.delay;
            out[e.uid].push_back(ev);
        }
    }
    return out;
}
// How long box b's chain is still MOVING after it is entered, in ticks. The
// dedupe key carries a state's per-box fire tick only while the move it started
// is still running: once everything that box set off has come to rest, two
// states that punched it at different ticks are in the same world again and
// must merge, or the frontier splits forever on a difference that no longer
// exists. Built from the same durTicks the chain walk sums, plus the measured
// box->motion latency, and rebuilt whenever g_touch is.
inline std::vector<int> g_touchMoveTicks;
// fireB's invariant counters (--firebcheck). See the check at the key site.
inline bool g_fireBCheck = false;
inline unsigned long long g_fireBNoTick = 0, g_fireBNoBit = 0, g_fireBTooEarly = 0;
// THE ERROR IS NOT SYMMETRIC, so this rounds up at every step. Too SHORT and
// two states that are still at different points of the same move get the same
// key and one is thrown away -- a wrong answer. Too LONG and a box stays in the
// key after its motion has stopped -- only cost, and only until the state
// leaves that stretch of the level. So: ceil rather than lround (a fractional
// duration must never be truncated), plus the 5-tick box->motion latency the
// chain players already apply (`u = (t - F - 5) / dur`, step.hpp), plus one
// 4-tick quantum so the bucketing at the key cannot clip the move's last
// bucket. If the frontier turns out too wide, the quantum is the knob -- not
// this margin.
// THE WINDOW IS THE MAXIMUM OVER EVERY EFFECT THE BOX HAS, and whoever adds a
// new kind of effect to a box has to widen it here. That is not a style note:
// the lock was added with the window still measuring only the eased move, and
// lv19's door slides for 69.5 ticks while the lock that carries the same object
// sideways runs 284.1 -- so the key merged states 78 ticks in while their
// platforms were still at different x, which is the exact failure this window
// exists to prevent, reintroduced by the new effect. A shorter window is a
// wrong answer, a longer one is only cost.
inline void buildTouchMoveTicks() {
    g_touchMoveTicks.assign(g_touch.size(), 0);
    g_touchStops.assign(g_touch.size(), {});   // --movetarget (see the field)
    for (size_t b = 0; b < g_touch.size(); ++b) g_touchStops[b] = g_touch[b].stops;
    for (size_t b = 0; b < g_touch.size(); ++b) {
        double d = 0.0;
        // --spawnlag: a queued hop starts later, so its move runs that much longer after the
        // entry (spawnLag is 0 without the flag).
        for (const TrigCtl& c : g_touch[b].ctl)
            d = std::max(d, c.durTicks + (double)c.spawnLag);
        for (const TrigCtl& c : g_touch[b].ctl) d = std::max(d, c.lockTicks);
        // A switch this box sets off lands `delay` ticks after it fires (resolveOn), so until
        // then two states that fired it at different ticks are in different worlds. With no
        // delay there is nothing to hold: once both have fired, both see the switch.
        for (const TouchTrig::TogEv& e : g_touch[b].togEv) d = std::max(d, e.delay);
        if (d <= 0.0) continue;             // nothing moves: contributes nothing
        g_touchMoveTicks[b] = (int)std::ceil(d) + 5 + 4;
    }
}
// The same boxes read in a TURNED gameplay frame (see RotTrig / frameLevel).
// markTouched compares the STATE's (xAbs, y) -- which are frame coordinates --
// against the box, so a world-coordinate box becomes unreachable the moment the
// frame turns: NO touch trigger could fire anywhere inside a rotated section.
//
// Measured on lv22 (2026-08-13): box 30 at (2277,273) 30x42 is what drops the
// spike row uid 18201..18206 (cy 213.25 -> 63.25). The recording fires it at
// t=1,755; the player riding the rotated ceiling at v=2,266.5 reaches the
// turned box (u=-273+-21, v=2277+-15) at u=-307.5, i.e. t=1,758. Without this
// the model kept those spikes at rest and killed its own frontier on them at
// x=2,196 -- from the head as well as from an anchor.
//
// Indices (= the State::trig bit numbering) are preserved: turn in place, never
// re-sort. `ctl` is only read at load time, so the copies carry geometry only.
inline std::array<std::vector<TouchTrig>, 4> g_touchFrame;
inline const std::vector<TouchTrig>& touchFor(int f) {
    f &= 3;
    if (f == 0 || g_touch.empty()) return g_touch;
    auto& v = g_touchFrame[(size_t)f];
    if (v.empty()) {
        v = g_touch;
        for (TouchTrig& T : v) {
            T.ctl.clear();
            double u, w;
            toFrame(f, T.cx, T.cy, u, w);
            T.cx = u; T.cy = w;
            if (f & 1) std::swap(T.hw, T.hh);
        }
    }
    return v;
}
// which bits have already been announced (diagnostic only)
inline TouchMask g_trigReported{};
// --bands <file>: per-layer frontier width, for the driver's segment cuts.
// One row per layer. `capdrop` / `merged` and the per-class breakdown were
// added for the lv16 diagnosis (2026-08-04): "which lane/speed lineage died,
// and did it die of physics, dedupe or the cap" is unanswerable from a single
// total. `cls` is a ';'-joined list of mode.mini.dual.dxmilli:alive:ylo:yhi.
struct BandRow {
    long long t; int alive; float ylo, yhi, x;
    int capdrop = 0, merged = 0;
    std::string cls;
};
inline std::string g_bandPath;
inline std::vector<BandRow> g_bands;
// --needtrig <n>: require that the plan goes THROUGH touch trigger n's box.
//
// Why this exists. The search finds the box on its own -- lv19's frontier enters
// all three -- but the DRIVER picks the deepest plan, and the branch that turns
// aside to touch a box is always shallower until the effect is known. lv19's
// third trigger is a moving lift: touching it is worth 900 px, and not touching
// it costs 50, so the run settles on not touching it forever. The recording can
// only ever show what some plan actually did, so the loop needs one plan whose
// whole purpose is to enter the box; after that the trajectory is in grouptrace
// and the ordinary search can use it (see applyTriggers).
// This is exploration, not a seed: the box comes out of the level's own trigger
// map, no prior solution is involved (CLAUDE.md's cold rule).
inline TouchMask g_needTrig{};
// --needtrig-unseen: require every box whose effect is still UNKNOWN, i.e. whose
// objects have not moved in any recording. That is the standing form of the rule
// above and the one the driver uses: "if you have never seen what this box does,
// go and find out". A box stops being required the moment one replay records it,
// so it costs one iteration per trigger and nothing afterwards.
inline bool g_needUnseen = false;
// --needtrig-skip <n>: never require box n, even when unseen. Repeatable.
// A box entirely BEHIND the anchor cannot be entered by the tail, so requiring
// it drops every state on the first layer -- PARTIAL t=0, which reads exactly
// like a physics wall. But that same dead end is what drives the ladder back
// behind the box, and going back is how an unknown ride gets bought. Whether a
// given trip is worth its iterations is therefore a DRIVER decision, not a
// property of the level: the driver goes back once and skips the box afterwards
// if the trip bought nothing. The `needtrig:` line below reports every box so
// the driver can see which one is blocking.
inline TouchMask g_needSkip{};
// --needtrig-uid <uid>: require the box whose trigger has this uid (repeatable). The loop's route
// prerequisites (the mod's cfg routeprereq) name boxes by uid, because a bit means a different box
// in every call's window.
inline std::vector<int> g_needTrigUids;
// --needtrigrev (on; --no-needtrigrev off): a box is behind an anchor that travels backwards when it
// is to the anchor's RIGHT. The test that drops a box the anchor has passed read x as if every
// anchor moved right, so a reversed anchor dropped the box still ahead of it. SubZero 4002 with
// coins: the third coin's box uid 18126 (cx 31,445) was "behind the anchor x=31,688" of a reversed
// spider, and the searches at that wall were never told to enter it.
inline bool g_needTrigRev = true;
// --portalobb (default on): a turned portal's firing box is GD's own oriented box (obb.txt: the four
// corners of the OBB2D), not the one rebuilt from the bounding box and the sprite's raw size. The
// rebuilt box assumes one scale for both sides, so it is wrong for a portal scaled unevenly: a custom
// level's hidden mini-size portal, scaled 0.25 wide and 1.31 tall and turned -22 degrees (a thin strip
// 7.75 x 117.5), came out as 25.4 x 73.7, a box 3.3 times too wide. The wave beside it, 32 px off the
// strip, turned mini there at t=13,880 where GD's did not (the dump's size stays 1), so the model flew
// a mini wave through a slope GD's full-size wave crashes on at 13,886, and every route it planned died
// in GD at the same tick, eight iterations running. Hazards have read the corners from the start.
// Gravity and mode portals (types 4 and 5) take the corners too: a hidden gravity portal scaled 0.5 wide and
// 2.2 tall and turned 22 degrees (a strip 7.8 x 176.5 by GD's corners) flipped a mini wave at t=14,947, one
// tick before GD's flip, and the routes that followed died at the next hazard (the wall at t=14,950).
inline bool g_portalObb = true;
// --portalrot (default on): a portal or speed portal with no recording rows keeps its own angle in the
// one row that places it (the row's angle was 0, so its first redraw stood the turned box upright).
inline bool g_portalRot = true;

// One row of the MOD's triggers.txt, shared by both loaders below.
// The trailing ease/erate/lock columns were added 2026-08-09; a dump written
// before that has 13 columns and still parses, with the easing defaulting to
// linear (which is what the model assumed anyway).
struct TrigRow {
    // An Advanced Follow's (3016) own save string, "key=value|key=value" (the advp column); empty where the dump has none (an older exporter).
    std::string advp;
    int uid, id, target, center, touch, spawn;
    double cx, cy, w, h, dur, ox, oy;
    int ease = 0; double erate = 2.0;
    int lockx = 0, locky = 0;
    // Everything past locky is read only to REACH the angle: the columns are
    // positional, so a reader that wants field 25 has to consume 18-24 too.
    double grav = 1.0; int gravmod = 0;
    // deg is NOT the angle -- it is the remainder of a whole turn. The turns
    // live in t360, so the total is t360*360 + deg, and lv21's three rotates
    // whose group is also moved all dump deg=0 with t360=+-2 (measured against
    // the recording: uid15367 turns +90 -> -629.997). See solver.hpp's dumper.
    double deg = 0.0;
    int ord = 0, chan = 0, sord = 0, sordd = 0;
    int t360 = 0;
    // Does the object's own facing follow the orbit? Decides its hitbox, not
    // its centre (lv22 uid254 travels its arc with rot pinned at 0).
    int lockrot = 0;
    // False when the dump predates the t360 column, i.e. "the angle in this row
    // is not trustworthy" -- distinct from an angle that is genuinely zero.
    bool hasAngle = false;
    // A Toggle's (1049) m_activateGroup, the 35th column: 1 turns its target
    // group on, 0 off. -1 on other ids and on dumps that predate the column.
    // Read only by --rotqtoggle: a group switched off makes GD's rotation queue
    // consume its 2900s without firing them (checkSpawnObjects 0x21aad8).
    int togon = -1;
    // The item columns (37th on), for the counter gates. `count` < 0 means the
    // dump predates them or the row is not a Count trigger at all.
    int item = 0, item2 = 0, count = -1, actgrp = -1, cmode = -1;
    // A Touch trigger's (1595) hold and toggle modes (thold / ttog: properties 81 / 82). Read only by
    // --ctrltouch (modifiers.hpp), for a box a held button switches off.
    int thold = 0, ttog = 0;
    // An ITEM COMPARE's (3620) own columns, the 46th on (i1mode .. res3):
    // ItemTriggerGameObject's m_item1Mode / m_item2Mode / m_targetItemMode /
    // m_mod1 / m_mod2 / m_resultType1..3. `cmpCols` is false on dumps that
    // predate them. Only the one shape measured in the game is read as a gate
    // (itemCompareGate below); the others are parsed so a new level can say
    // which shape it has.
    bool cmpCols = false;
    int i1mode = 0, i2mode = 0, res1 = 0, res2 = 0, res3 = -1;
    double mod1 = 0.0, mod2 = 0.0;
    // A Spawn's (1268) group remap, the 36th column: (named group -> group it
    // acts on) for the triggers it spawns. Empty on other ids and on dumps that
    // predate the column. Applied only under --spawnremap (see g_spawnRemap).
    std::vector<std::pair<int, int>> remap;
    // A Move's target mode, the 28th-30th columns (0 on dumps that predate them).
    // mvtgt=1: the group goes to the position of the object in group `center`,
    // measured from the object in group `tmodctr` (which moves with it); mvaxis
    // 1 = x only, 2 = y only, 0 = both. Read only by --movetarget.
    int mvtgt = 0, mvaxis = 0, tmodctr = 0;
    // A Spawn's delay, the 27th column, in seconds (0 on dumps that predate it).
    double sdelay = 0.0;
    // A toggle ring's (1594) m_isSpawnOnly, the 59th column: 1 = pressing it SPAWNS the
    // target group rather than toggling it. 0 on everything else and on dumps that predate it.
    int sponly = 0;
    // Follow Player Y's (1814) speed / delay / offset / max speed, the 60th-63rd columns
    // (properties 90 / 91 / 92 / 105). `fyCols` is false on dumps that predate them.
    bool fyCols = false;
    double fySpeed = 0.0, fyDelay = 0.0, fyMax = 0.0;
    int fyOffset = 0;
    // A Move's lock to the camera's x / y (properties 141 / 142), the 64th-65th columns. 0 on dumps
    // that predate them. --camlockx reads lockcamx as lockx.
    int lockcamx = 0, lockcamy = 0;
    // An Event trigger's (3604) m_eventIDs and m_extraID / m_extraID2, the 66th and 67th columns (events /
    // evextra; empty and 0 on any other row and on dumps that predate them). --eventtrig.
    std::vector<int> events;
    int evx1 = 0, evx2 = 0;
    // The object GD takes for the centre group (tryGetMainObject 0x224520: the group's registered parent,
    // key 274, else its only member), the 68th column (ctrobj): its uid, -1 when GD finds none, 0 when the
    // trigger has no centre group. `ctrCol` is false on dumps that predate it. --rotparent.
    int ctrObj = 0;
    bool ctrCol = false;
    // A Move's m_moveModX / m_moveModY, the 69th and 70th columns: the multiplier on the player's step for a Move locked to the player (1 where the dump
    // predates them or the row sets none). --lockmod.
    double moveModX = 1.0, moveModY = 1.0;
};


// --spawnremap: follow a Spawn's group remap (property 442) in the chain walks.
// A spawned trigger acts on the REMAPPED group. lv22's touch box uid17771
// spawns group 493, whose two Moves name group 100 -- six decorations -- while
// the remap sends them to group 508, the block row the ball lands on. Without
// it that row is controlled by no box, so its recording plays on the recording
// run's own clock, and a plan that touches the box 25 ticks earlier stands on a
// platform that has not started to sink (measured on worker 98: the row moves
// two ticks after the ball's rect first overlaps the box, landing or not).
// The dump's field order was checked against the level string: of the four
// ints per entry the first is the named group and the THIRD the target
// ("100:100:508:0"), whatever the binding calls them. Only lv22 has remaps
// (45 of 141 spawns), so this is inert on lv1-21. On by default since v0.1.4,
// after cold runs judged it; --no-spawnremap turns it off.
inline bool g_spawnRemap = true;

// --locknegdur (default on): a lockToPlayer Move with a negative duration locks for good. The lock window was
// dur * 240 ticks, negative, which every reader takes as no lock. A custom level's block uid 85569 (Move uid
// 104330, lockx, duration -1, offset 0): GD's rows follow the player's x tick by tick from its crossing at
// 27,753 to the recording's end at 28,140, past any 240-tick window; the model left the block at its first row
// (and --anchormove, finding no moving controller, anchored it on a Toggle 2,500 px ahead), and the mini cube
// GD stands on it fell from the first tick of the 28,044 anchor.
inline bool g_lockNegDur = true;
// --camlockx: a Move locked to the camera's x (m_lockToCameraX, property 141; triggers.txt lockcamx) is read as
// locked to the player's x, which the lock machinery here models (the window, --locknegdur's "for good", --lockx's
// placement from this run's own player). While the camera follows the player the two are one motion: a custom
// level's spike uid 54936 rides a duration -1 Move with 141 set from its crossing at x 42,135, and GD's rows put it
// 17.36 px behind the player on every tick from 20,900 to the attempt's death at 21,024 (the camera at the player's
// x - 209.5 throughout). Read as unlocked it held its last row past the recording's end, and the model flew the
// ship through the spike GD dies on. A static camera (1914) breaks the equality and is not modelled here. Dumps
// without the column (before 2026-10-12) read 0. On by default; --no-camlockx is the off arm.
inline bool g_camLockX = true;
// --sdelaychains (default off): the touch chains' and the autonomous triggers' walks read each Spawn's delay
// (chainDelayS). Until the 10-13 export fix (9a03b57d) every dump wrote 0 there, so these walks were built and
// measured on immediate chains only, and fed the real delays they lose a level: a custom level the 10-12 night
// cleared in 551 s (and its build again in 504 s) stalls at x 15,615 on trunk with the fix, with or without
// every 10-13 dp flag; the same trunk with only this read put back to 0 clears it in 422 s. Off, the walks read
// 0 as before; the delays the 10-13 resolvers read (spawnFiresOf: --tptrigspawn, --gravchain, --tptouch,
// --gravtouch, --eventtrig) are the real ones either way. On is for measuring the walks against GD's timing.
inline bool g_spawnDelayChains = true;   // on by default (release flags, 2026-10-17); --no-sdelaychains is the off arm
inline double chainDelayS(const TrigRow& r) { return g_spawnDelayChains ? r.sdelay : 0.0; }
inline constexpr double kLockForever = 1e7;   // ticks: longer than any level
inline double lockWindowTicks(int lock, double dur) {
    if (!lock) return 0.0;
    return (g_lockNegDur && dur < 0.0) ? kLockForever : dur * 240.0;
}

inline bool visualOnly(int id) { return id == 1006 || id == 1007; }

// Apply a remap to a group id; unmapped ids pass through.
inline int remapGroup(const std::vector<std::pair<int, int>>& rm, int g) {
    for (const auto& p : rm)
        if (p.first == g) return p.second;
    return g;
}

// The remap a trigger hands to what IT spawns: its own entries first, then
// whatever it inherited for ids it does not map itself. How GD merges nested
// remaps (SpawnTriggerGameObject::updateRemapKeys) is NOT measured -- lv22's
// remapping spawns sit at chain roots, so no measured case composes two.
inline std::vector<std::pair<int, int>> composeRemap(
        const std::vector<std::pair<int, int>>& own,
        const std::vector<std::pair<int, int>>& inherited) {
    if (own.empty()) return inherited;
    std::vector<std::pair<int, int>> out;
    for (const auto& p : own) out.push_back({p.first, remapGroup(inherited, p.second)});
    for (const auto& p : inherited) {
        bool shadowed = false;
        for (const auto& q : own) shadowed |= (q.first == p.first);
        if (!shadowed) out.push_back(p);
    }
    return out;
}
inline bool loadTrigRows(const std::string& path,
                         std::unordered_map<int, TrigRow>& trig) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    std::getline(in, line);   // header
    while (std::getline(in, line)) {
        TrigRow r{};
        const int n = std::sscanf(
            line.c_str(),
            "%d,%d,%lf,%lf,%lf,%lf,%d,%d,%d,%d,%lf,%lf,%lf,%d,%lf,%d,%d,"
            "%lf,%d,%lf,%d,%d,%d,%d,%d,%d",
            &r.uid, &r.id, &r.cx, &r.cy, &r.w, &r.h, &r.target, &r.center,
            &r.touch, &r.spawn, &r.dur, &r.ox, &r.oy, &r.ease, &r.erate,
            &r.lockx, &r.locky,
            &r.grav, &r.gravmod, &r.deg, &r.ord, &r.chan, &r.sord, &r.sordd,
            &r.t360, &r.lockrot);
        if (n < 13) continue;
        if (n < 15) { r.ease = 0; r.erate = 2.0; }
        // 26 = through lockrot. Anything shorter predates the column and its
        // deg (if any) is only part of the angle, so the angle is unusable.
        r.hasAngle = (n >= 26);
        {   // togon, the 35th column: found by position past the 26 parsed above,
            // so the columns between are skipped rather than read. A dump that
            // predates it has 34 columns and leaves -1.
            size_t p = 0;
            int commas = 0;
            while (commas < 34 && (p = line.find(',', p)) != std::string::npos) {
                ++p;
                ++commas;
            }
            if (commas == 34 && p < line.size()) r.togon = std::atoi(line.c_str() + p);
            // mvtgt / mvaxis / tmodctr, the 28th-30th columns, by position the
            // same way (a dump that predates them has 13-26 columns).
            {
                auto field = [&](int k) -> int {
                    size_t q = 0;
                    for (int c = 0; c < k; ++c) {
                        q = line.find(',', q);
                        if (q == std::string::npos) return 0;
                        ++q;
                    }
                    return std::atoi(line.c_str() + q);
                };
                r.mvtgt = field(27);
                r.mvaxis = field(28);
                r.tmodctr = field(29);
                // sdelay, the 27th column, in SECONDS: how long after a Spawn
                // fires the row it spawns goes off. Read only by the chain's
                // on/off events (TouchTrig::togEv) -- the moves take their
                // duration from the hop that moves, and the chains measured so
                // far carry 0 here in front of one.
                auto dfield = [&](int k) -> double {
                    size_t q = 0;
                    for (int c = 0; c < k; ++c) {
                        q = line.find(',', q);
                        if (q == std::string::npos) return 0.0;
                        ++q;
                    }
                    return std::atof(line.c_str() + q);
                };
                r.sdelay = dfield(26);
            }
            // remap, the 36th column: "a:b:c:d;..." or "-". The source is the
            // first field and the target the third (checked against lv22's
            // level string, uid17771 = "100:100:508:0").
            if (commas == 34 && (p = line.find(',', p)) != std::string::npos) {
                std::stringstream es(line.substr(p + 1));
                std::string ent;
                while (std::getline(es, ent, ';')) {
                    int a = 0, b = 0, c = 0, d = 0;
                    if (std::sscanf(ent.c_str(), "%d:%d:%d:%d", &a, &b, &c, &d) == 4
                        && a != 0 && c != 0)
                        r.remap.push_back({a, c});
                }
            }
        }
        {   // The item columns, the 37th on (item,item2,count,subcount,actgrp,
            // thold,ttog,tdual,cmode). Counted from the start rather than
            // continued from the remap parse above, which has moved its cursor;
            // the remap field holds ':' and ';' but never a comma, so the count
            // is exact. A dump without them leaves count = -1 = "not a counter".
            size_t q = 0;
            int cm = 0;
            while (cm < 36 && (q = line.find(',', q)) != std::string::npos) {
                ++q;
                ++cm;
            }
            if (cm == 36 && q < line.size()) {
                int it = 0, it2 = 0, cnt = -1, sub = 0, act = -1,
                    th = 0, tt = 0, td = 0, cmd = -1;
                int m1 = 0, m2 = 0, tm = 0, r1 = 0, r2 = 0, r3 = -1;
                double md1 = 0.0, md2 = 0.0;
                const int got = std::sscanf(
                    line.c_str() + q,
                    "%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%lf,%lf,%d,%d,%d",
                    &it, &it2, &cnt, &sub, &act, &th, &tt, &td, &cmd,
                    &m1, &m2, &tm, &md1, &md2, &r1, &r2, &r3);
                if (got >= 5) {
                    r.item = it;
                    r.item2 = it2;
                    r.count = cnt;
                    r.actgrp = act;
                    r.cmode = cmd;
                }
                if (got >= 7) {
                    r.thold = th;
                    r.ttog = tt;
                }
                // sponly, the 59th column (58 commas in): after the thirteen Item Compare
                // columns, which every row carries (-1/0 where the row is not one).
                {
                    size_t s = 0;
                    int cs = 0;
                    while (cs < 58 && (s = line.find(',', s)) != std::string::npos) {
                        ++s;
                        ++cs;
                    }
                    if (cs == 58 && s < line.size()) r.sponly = std::atoi(line.c_str() + s);
                    // fyspd,fydel,fyoff,fymax: the four columns after sponly.
                    if (cs == 58 && (s = line.find(',', s)) != std::string::npos) {
                        double sp = 0.0, de = 0.0, mx = 0.0;
                        int of = 0, lcx = 0, lcy = 0;
                        const int nf = std::sscanf(line.c_str() + s + 1, "%lf,%lf,%d,%lf,%d,%d", &sp, &de,
                                                   &of, &mx, &lcx, &lcy);
                        if (nf >= 4) {
                            r.fyCols = true;
                            r.fySpeed = sp;
                            r.fyDelay = de;
                            r.fyOffset = of;
                            r.fyMax = mx;
                        }
                        // lockcamx,lockcamy: the two columns after fymax
                        if (nf == 6) {
                            r.lockcamx = lcx;
                            r.lockcamy = lcy;
                            // events,evextra: the two after those (sponly is 58 commas in; 65 in, events)
                            size_t e = s;
                            int ce = cs;
                            while (ce < 65 && (e = line.find(',', e)) != std::string::npos) {
                                ++e;
                                ++ce;
                            }
                            if (ce == 65 && e < line.size()) {
                                const size_t end = line.find(',', e);
                                const std::string evs = line.substr(e, end == std::string::npos ? std::string::npos
                                                                                                : end - e);
                                if (evs != "-")
                                    for (size_t p = 0; p < evs.size();) {
                                        const size_t q = evs.find(';', p);
                                        r.events.push_back(std::atoi(evs.c_str() + p));
                                        if (q == std::string::npos) break;
                                        p = q + 1;
                                    }
                                if (end != std::string::npos) {
                                    std::sscanf(line.c_str() + end + 1, "%d:%d", &r.evx1, &r.evx2);
                                    // ctrobj: the column after evextra
                                    const size_t c = line.find(',', end + 1);
                                    if (c != std::string::npos && c + 1 < line.size()) {
                                        r.ctrObj = std::atoi(line.c_str() + c + 1);
                                        r.ctrCol = true;
                                        // movemodx,movemody: the two columns after ctrobj
                                        const size_t c2 = line.find(',', c + 1);
                                        if (c2 != std::string::npos && c2 + 1 < line.size()) {
                                            double mx = 1.0, my = 1.0;
                                            if (std::sscanf(line.c_str() + c2 + 1, "%lf,%lf", &mx, &my) == 2) {
                                                r.moveModX = mx;
                                                r.moveModY = my;
                                                // advp, the column after movemody
                                                const size_t c3 = line.find(',', c2 + 1);
                                                const size_t c4 = c3 == std::string::npos ? c3 : line.find(',', c3 + 1);
                                                if (c4 != std::string::npos && c4 + 1 < line.size()) {
                                                    r.advp = line.substr(c4 + 1);
                                                    while (!r.advp.empty() && (r.advp.back() == '\r' || r.advp.back() == '\n')) r.advp.pop_back();
                                                    if (r.advp == "-") r.advp.clear();
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                if (got >= 17) {
                    r.cmpCols = true;
                    r.i1mode = m1;
                    r.i2mode = m2;
                    r.mod1 = md1;
                    r.mod2 = md2;
                    r.res1 = r1;
                    r.res2 = r2;
                    r.res3 = r3;
                }
            }
        }
        // --camlockx: a Move locked to the camera's x is read as one locked to the player's x
        if (g_camLockX && r.id == 901 && r.lockcamx && !r.lockx) {
            r.lockx = 1;
        }
        trig[r.uid] = r;
    }
    return true;
}

// ---- parse caches for the in-process loop (see cli.hpp's) ----
// A call reads triggers.txt and objgroups.txt up to three times -- loadTouchTriggers for the
// window probe and again for the list, then loadAutoTriggers -- each a line-by-line parse, which
// on a custom level (7,806 triggers, 89k grouped objects) was ~0.4 s of every call. As in
// cli.hpp, a parse is reused only while the file holds the bytes it came from; the parse itself
// still reads the file exactly as before.
struct TrigRowsCache {
    bool valid = false;
    bool camLockX = false;   // the --camlockx the parse was made under (it rewrites lockx)
    std::string path, bytes;
    std::unordered_map<int, TrigRow> rows;
};
// (namespace scope, not a function's static, so cli.hpp's releaseCrossCallCaches can empty it)
inline TrigRowsCache g_trigRowsCache;
inline const std::unordered_map<int, TrigRow>* trigRowsCached(const std::string& path) {
    TrigRowsCache& c = g_trigRowsCache;
    std::string bytes;
    if (!readFileBytes(path, bytes)) return nullptr;
    if (!(c.valid && c.path == path && c.bytes == bytes && c.camLockX == g_camLockX)) {
        c = TrigRowsCache{};
        if (!loadTrigRows(path, c.rows)) return nullptr;
        c.valid = true;
        c.camLockX = g_camLockX;
        c.path = path;
        c.bytes = std::move(bytes);
    }
    return &c.rows;
}
// objgroups.txt, "uid g1 g2 ..." -- space separated because one object can sit in up to ten
// groups -- as group id -> uids.
struct GroupMembersCache {
    bool valid = false;
    std::string path, bytes;
    std::unordered_map<int, std::vector<int>> byGroup;
};
inline GroupMembersCache g_groupMembersCache;   // (namespace scope, as g_trigRowsCache)
inline const std::unordered_map<int, std::vector<int>>* groupMembersCached(
        const std::string& path) {
    GroupMembersCache& c = g_groupMembersCache;
    std::string bytes;
    if (!readFileBytes(path, bytes)) return nullptr;
    if (!(c.valid && c.path == path && c.bytes == bytes)) {
        c = GroupMembersCache{};
        std::ifstream in(path);
        if (!in) return nullptr;
        std::string line;
        std::getline(in, line);   // header
        while (std::getline(in, line)) {
            std::stringstream ss(line);
            int uid = 0;
            if (!(ss >> uid)) continue;
            int g = 0;
            while (ss >> g) c.byGroup[g].push_back(uid);
        }
        c.valid = true;
        c.path = path;
        c.bytes = std::move(bytes);
    }
    return &c.byGroup;
}

// --actlookchain: the triggers that change only what is seen, by their editor icon (the mod's levelwarn::kLookOnly,
// src/mod/level_warnings.hpp, read off ObjectToolbox::init in 2.2081). Sorted. The two tables are the same list kept in two places:
// tools/check_look_only_tables.py fails when they differ, so change both together.
inline bool actLookOnlyId(int id) {
    static constexpr int kLook[] = {
        22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 55, 56, 57, 58, 59, 105, 744, 899, 900, 915,
        1006, 1007, 1520, 1585, 1612, 1613, 1818, 1819, 1913, 1915, 1934, 2016, 2903, 2904, 2905, 2907,
        2909, 2910, 2911, 2912, 2913, 2914, 2915, 2916, 2917, 2919, 2920, 2921, 2922, 2923, 2924, 2999,
        3009, 3010, 3014, 3015, 3020, 3021, 3029, 3030, 3031, 3602, 3603, 3605, 3606, 3608, 3612, 3613,
        3642, 3662};
    return std::binary_search(std::begin(kLook), std::end(kLook), id);
}
// --actlookchain: is everything a trigger can reach look-only? A look-only trigger is; a Spawn (1268) is when every trigger in
// the group it spawns is (its non-trigger members do not react to a spawn). Depth-bounded, each trigger visited once.
inline bool actLookOnlyChain(const TrigRow& M, const std::unordered_map<int, TrigRow>& trig,
                             const std::unordered_map<int, std::vector<int>>& byGroup, int depth,
                             std::unordered_set<int>& seen) {
    if (actLookOnlyId(M.id)) return true;
    if (M.id != 1268 || depth >= 6) return false;
    const auto gi = byGroup.find(M.target);
    if (gi == byGroup.end()) return true;
    for (const int u : gi->second) {
        const auto t = trig.find(u);
        if (t == trig.end() || !seen.insert(u).second) continue;
        if (!actLookOnlyChain(t->second, trig, byGroup, depth + 1, seen)) return false;
    }
    return true;
}

// g_offMoves: is the trigger T switched off when the player crosses its x? GD does not fire a
// trigger whose group a Toggle has switched off. Read from the x-crossing Toggles (touch=0,
// spawn=0) whose target group holds T and whose x is before T's: they fire in x order, so the last
// of them decides (the uid breaks a tie, so the answer does not hang on the map's order). A SPAWNED
// Toggle that switches T's group on is a chain this does not follow; `spawnOn` reports one, and the
// callers leave such a trigger as it was (lv19's Move uid 14066 is that case).
// Every Toggle (1049, with a direction) whose target group holds the trigger `uid`, in the map's
// order: the switches that decide whether that trigger is on. Read by offAtCrossing (an x-crossing
// Move) and, under --touchoff, by the touch boxes (TouchTrig::ownTog).
template <class Fn>
inline void forTogglesHolding(int uid, const std::unordered_map<int, TrigRow>& trig,
                              const std::unordered_map<int, std::vector<int>>& byGroup, Fn&& fn) {
    for (const auto& kv : trig) {
        const TrigRow& X = kv.second;
        if (X.id != 1049 || X.togon < 0 || X.target == 0 || X.uid == uid) continue;
        const auto gi = byGroup.find(X.target);
        if (gi == byGroup.end()
            || std::find(gi->second.begin(), gi->second.end(), uid) == gi->second.end())
            continue;
        fn(X);
    }
}
inline bool offAtCrossing(const TrigRow& T, const std::unordered_map<int, TrigRow>& trig,
                          const std::unordered_map<int, std::vector<int>>& byGroup,
                          bool* spawnOn = nullptr) {
    const TrigRow* last = nullptr;
    bool sOn = false;
    forTogglesHolding(T.uid, trig, byGroup, [&](const TrigRow& X) {
        if (X.spawn) {
            if (X.togon == 1) sOn = true;
            return;
        }
        if (X.touch || X.cx >= T.cx) return;
        if (!last || X.cx > last->cx || (X.cx == last->cx && X.uid > last->uid)) last = &X;
    });
    if (spawnOn) *spawnOn = sOn;
    return last && last->togon == 0;
}
// --boxarm: is there a toggle block (1594, "activate group", not spawn-fired) whose target group holds the trigger
// `uid`? Such a block switches the group back on when it fires (the press inside it), and a Move in it that an
// x-crossing Toggle switched off fires at its own x again. Read by loadAutoTriggers (the Move is kept, offInit)
// and loadTouchTriggers (the block's trigTog).
inline bool boxArmsTrigger(int uid, const std::unordered_map<int, TrigRow>& trig,
                           const std::unordered_map<int, std::vector<int>>& byGroup) {
    for (const auto& kv : trig) {
        const TrigRow& B = kv.second;
        if (B.id != 1594 || B.actgrp != 1 || B.spawn || B.target == 0) continue;
        const auto gi = byGroup.find(B.target);
        if (gi != byGroup.end() && std::find(gi->second.begin(), gi->second.end(), uid) != gi->second.end())
            return true;
    }
    return false;
}

// The collectibles (items.txt). Only the ones GD calls a PICKUP item: a
// collectible that is not one cannot move a counter, and the counter is the
// whole reason this file is read.
inline bool loadCollectibles(const std::string& path) {
    g_collect.clear();
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    std::getline(in, line);   // header
    while (std::getline(in, line)) {
        int uid = 0, id = 0, item = 0, points = 0, pickup = 0, toggle = 0, sub = 0;
        double cx = 0, cy = 0, w = 0, h = 0;
        if (std::sscanf(line.c_str(), "%d,%d,%lf,%lf,%lf,%lf,%d,%d,%d,%d,%d",
                        &uid, &id, &cx, &cy, &w, &h, &item, &points, &pickup,
                        &toggle, &sub) < 9)
            continue;
        // EVERY row that names an item goes in the giver map, touchable or
        // not: lv22's first coin is fed by Pickup triggers a touch box spawns,
        // and those have pickup=0 because nothing touches them. g_collect is
        // still only what the player can walk into.
        // ...and not a Counter (1615) either: GameObject::createWithKey builds it as a
        // LabelGameObject (case 0x64f), whose methods only draw the value -- it DISPLAYS the item it
        // names and never writes a counter (GJBaseGameLayer::updateCounters does). A chain that moves
        // the readout was credited as adding to the counter: on lv21 under --worldgates each of the
        // "n of 10" Counts moves the readout, so each one firing "gave" item 1 again.
        if (item != 0 && id != 1816 && id != 1615) g_itemGiver[uid] = item;
        if (!pickup || item == 0) continue;
        g_collect.push_back({cx, cy, w * 0.5, h * 0.5, item, uid});
    }
    std::sort(g_collect.begin(), g_collect.end(),
              [](const Collectible& a, const Collectible& b) {
                  return a.cx < b.cx || (a.cx == b.cx && a.cy < b.cy);
              });
    return true;
}

// --countactgrp: is every giver of `item` a pickup the player walks into (g_collect)? Then the state's counter is the
// game's. A giver the state does not count (a Pickup trigger, Item Persistence) leaves a Count on that item to the
// recording: a custom level's Count (item 1 == 1) switches a group on that an x-crossing Pickup trigger and an Item
// Persistence feed, and with the switch taken over the group stayed off for every state.
inline bool countItemWalkFed(int item) {
    bool any = false;
    for (const Collectible& c : g_collect)
        if (c.item == item) any = true;
    if (!any) return false;
    for (const auto& kv : g_itemGiver) {
        if (kv.second != item) continue;
        bool walk = false;
        for (const Collectible& c : g_collect)
            if (c.uid == kv.first) { walk = true; break; }
        if (!walk) return false;
    }
    return true;
}

// --tap2: like countItemWalkFed, but a giver that sits in a Tap's own group (giverBehindTap: its presses are counted by the state) is the game's counter too -- and at least one giver is such.
template <class F>
inline bool countItemTapFed(int item, F&& giverBehindTap) {
    bool tap = false;
    for (const auto& kv : g_itemGiver) {
        if (kv.second != item) continue;
        bool walk = false;
        for (const Collectible& c : g_collect)
            if (c.uid == kv.first) { walk = true; break; }
        if (walk) continue;
        if (!giverBehindTap(kv.first)) return false;
        tap = true;
    }
    return tap;
}

// The MOD's two dumps, joined into "enter this box -> these uids move by this".
// Chains are followed through intermediate triggers (spawn -> move -> geometry)
// because that is how the levels are actually built.
// (--touch-from-anchor, the opt-in that kept the 32 touch triggers AHEAD of the anchor instead of the first 32
// in the level, is gone; the window is now opened only by the auto-window gate and --touchwinahead.)
// --touchwinahead: ...and window from the anchor also when the plain list (the first 32) leaves out
// a box ahead that the anchor can reach -- the first box it drops lies within the horizon (3,000
// ticks without one) at the fastest start speed. lv22: an anchor at x=7,655 kept x=511..7,785 and
// dropped the touch Toggle at x=8,229 that switches a gameplay rotation off, so the model rotated
// where the game did not (cli.hpp, the window gate). With it, an anchor window also never starts
// inside a run of boxes that move the same objects (the cap block below). On by default since
// 2026-10-06; --no-touchwinahead is the off arm. The run guard covers adjacent boxes with the same
// control uid only: a box of another group between two of them stops the step back.
inline bool g_touchWinAhead = true;
// --trigdump: print one line per touch box BEFORE the 32-cap (see the print
// site). Diagnostic only -- nothing reads it and no run that omits the flag
// changes by a byte.
inline bool g_trigDump = false;
// --trigeffect: one line per touch box saying what its chain does AFTER the
// per-(box, uid) fold in level_loader, next to the raw counts the selection
// reads. Print-only; see the note at the print itself.
inline bool g_trigEffect = false;
// --stopdump: what a Stop (id 1616) would freeze, and how it is fired. Print
// only. See the block at the end of loadAutoTriggers for what it is for; the
// short version is that the model does not decode 1616, and this counts what
// that costs before anything is written that acts on it.
inline bool g_stopDump = false;
// --trigrelevant: choose the 32 by RELEVANCE rather than by x alone. A box is
// kept when its chain moves something, or reaches an object the player can
// collide with, or switches a group on/off (the Toggle's own effect is not a
// motion, and --rotqtoggle reads it through TouchTrig::togOn).
//
// The cap is not the defect the counting found. On the one level where it
// bites, of the 32 boxes it currently keeps only 8 pass this test and of the
// 120 it drops 52 do -- so 24 bits go to boxes that cannot touch the player
// while 52 that can are thrown away. Narrowing the population first is worth
// more than widening the mask: with the population at 60 instead of 152, the
// same 32 bits all go to boxes that matter.
//
// Needs the type map. Without one the test cannot be made and is skipped
// rather than guessed at.
//
// ON BY DEFAULT since 2026-09-20 (user ruling). Measured at width 32: the two
// levels under the cap keep every box, the one over it goes from 152 to 61, and
// the cold run clears 22/22 with that level twelve iterations cheaper and every
// other level's fingerprints bit-identical -- which is the positive control,
// since the filter provably drops nothing on any of them. Always on since the
// flag clean-up.
// --csvtypes: an in-process call reads the object types for the
// selection above from the level table (g_levelCsv) instead of argv[1], which is
// a placeholder there -- so the loop's selection sees the collidable reach the
// CLI's does. Off keeps the loop's historical selection (movers and toggles
// only), which every blessed number was measured with; on changes WHICH boxes
// lv22 keeps (relevant 45 -> 61).
// ON since 2026-09-21: reading the types from a
// placeholder was a known inconsistency, and --no-csvtypes, which reproduced
// it, is gone since the flag clean-up.
// --trigwinsel (on by default since 2026-10): the auto-window gate probes the population it
// will actually load, instead of the unselected one. See the gate in cli.hpp.
// It exists because the selection went on by default (934c21e) and that gate
// was not updated with it; the two populations have disagreed since.
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// World-x window: an anchor in a ROTATED frame asks the window gate
// with its WORLD x, and keeps the kTouchBits boxes nearest that x. The gate
// compared the touch boxes' world cx with x0 after --start had mapped x0 into the
// frame's travel coordinate (world Y in frame 3), so in a rotated section it never
// opened: lv22's eight frame-3 anchors of a whole cold run (x0 ~ 16,000) each kept
// the first 32 relevant boxes, all at x <= 3,675, and dropped the 29 ahead. And
// "ahead" is not a world-x direction there -- frame 2 travels -X, and lv22's maze
// goes back and forth over world x 15,400..16,200 with its touch boxes at
// 15,075..16,425 -- so the window is the nearest boxes, not the ones past x0.
// ON by default since 2026-09-22: the eight anchors window with nothing dropped
// ahead, and lv22's route is the same to the plan hash. Always on since the 0.2.0
// flag clean-up (it was --trigwinworld).
// Set by cli.hpp for one loadTouchTriggers call from a rotated-frame anchor: keep
// the kTouchBits boxes nearest this world x instead of the ones from fromX on.
inline double g_trigWinNear = -1e18;
// WHAT THE CAP ACTUALLY COVERED, published rather than only printed. The
// consumers of a whole-run comparison need it: a first-divergence past
// `maxKeptX` was measured in a world missing `droppedRelevant` boxes, and
// reporting it as a pass or an improvement is reporting a different level.
// Filled by loadTouchTriggers; zero before it runs.
//   total            boxes the chain walk produced
//   relevant         ...that survive the selection (== total when it is off)
//   kept             ...that fit in kTouchBits
//   droppedRelevant  relevant - kept. NON-ZERO IS THE UNCOVERED CASE.
//   maxKeptX         the cx of the last box kept; past it nothing is modelled
inline size_t g_trigTotal = 0, g_trigRelevantN = 0, g_trigKept = 0,
              g_trigDroppedRelevant = 0;
// ...and WHICH SIDE they fell off, which the sum above cannot say and the
// window code has always known (it prints "dropped N behind, M ahead").
// Collapsing them made the UNCOVERED label wrong for a windowed anchor: the
// boxes behind it are dropped ON PURPOSE -- the world they already moved is in
// the recording -- while only the ones ahead are a world this call cannot see.
// 0 / 0 when no window was cut.
inline size_t g_trigDroppedBehind = 0, g_trigDroppedAhead = 0;
inline double g_trigMaxKeptX = 0.0;
// ...and the cx of the first box dropped ahead (1e300 when none was), which --touchwinahead's gate
// reads off the plain probe (cli.hpp).
inline double g_trigFirstAheadX = 1e300;

// `fromX` = where this solve starts (the --start anchor, or the level's head).
// The mask in State::trig has 32 bits, so a level with more touch triggers than
// that has to drop some -- and dropping them by GLOBAL x order silently deletes
// everything past the 32nd. lv22 has **155** of them (lv19 has 3, lv20 has 6,
// every other level none), so the model could not see a single trigger past
// x=2,283: the whole switch band at x=3,260-3,900 -- where hitting the hanging
// blocks is what raises the descending spikes -- was invisible, and the DP had
// no representable way through it. Keeping the 32 nearest AHEAD of the anchor
// instead makes the window follow the ladder. Triggers behind the anchor are
// not lost information: whatever they already moved is in the grouptrace
// recording the driver passes as --groups.
// uid -> GD object type, read from the objrects dump by NAME rather than by
// position (the columns move with the mod's dump schedule, and a position
// guessed in advance is the one failure this parser family already records).
// Only the two columns are taken, so this costs one pass over the file and no
// allocation per object beyond the map.
//
// It exists because the touch-box selection needs to know whether a chain
// reaches anything the player can collide with, and loadTouchTriggers runs
// BEFORE loadLevel -- there is no Level to ask yet. Empty map = "unknown",
// which every caller must read as "do not drop anything on this basis".
inline std::unordered_map<int, int> loadObjTypesFrom(std::istream& in);
inline std::unordered_map<int, int> loadObjTypes(const std::string& objPath) {
    std::ifstream in(objPath);
    if (!in) return {};
    return loadObjTypesFrom(in);
}
// The same table from a stream. An IN-PROCESS call's argv[1] is the placeholder
// "(in-process level)" (dpcore.cpp) and the level lives in g_levelCsv, so the
// path form above returns an empty map there: every box then reads as reaching
// nothing collidable and --trigrelevant keeps the movers only. That is lv22's
// "mod relevant 45 vs CLI 61" (open since 2026-09-21 morning). The caller picks
// the source; see the --csvtypes note above.
inline std::unordered_map<int, int> loadObjTypesFrom(std::istream& in) {
    std::unordered_map<int, int> out;
    std::string line;
    if (!std::getline(in, line)) return out;
    int iu = -1, it = -1, n = 0;
    {
        std::stringstream hs(line);
        std::string c;
        while (std::getline(hs, c, ',')) {
            if (c == "uid") iu = n;
            else if (c == "type") it = n;
            ++n;
        }
    }
    if (iu < 0 || it < 0) return out;
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        std::string c;
        int i = 0, uid = -1, ty = -1;
        while (std::getline(ss, c, ',')) {
            if (i == iu) uid = std::atoi(c.c_str());
            else if (i == it) ty = std::atoi(c.c_str());
            ++i;
        }
        if (uid >= 0 && ty >= 0) out[uid] = ty;
    }
    return out;
}
// uid -> (cx, cy) from the same table, for --movetarget: a target-mode Move goes
// to where the object in its `center` group is, measured from the object in its
// `tmodctr` group. Read by header like loadObjTypes, from a stream so the caller
// can hand it g_levelCsv in-process (argv[1] is a placeholder there -- the first
// cold with --movetarget read an empty map and the flag did nothing).
inline std::unordered_map<int, std::pair<double, double>> loadObjPos(std::istream& in) {
    std::unordered_map<int, std::pair<double, double>> out;
    std::string line;
    if (!std::getline(in, line)) return out;
    int iu = -1, ix = -1, iy = -1, n = 0;
    {
        std::stringstream hs(line);
        std::string c;
        while (std::getline(hs, c, ',')) {
            if (c == "uid") iu = n;
            else if (c == "cx") ix = n;
            else if (c == "cy") iy = n;
            ++n;
        }
    }
    if (iu < 0 || ix < 0 || iy < 0) return out;
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        std::string c;
        int i = 0, uid = -1;
        double x = 0.0, y = 0.0;
        while (std::getline(ss, c, ',')) {
            if (i == iu) uid = std::atoi(c.c_str());
            else if (i == ix) x = std::atof(c.c_str());
            else if (i == iy) y = std::atof(c.c_str());
            ++i;
        }
        if (uid >= 0) out[uid] = {x, y};
    }
    return out;
}
// --spawnlag: uid -> cx of the objects the table marks spawn-triggered (its `spawn` column), for an
// ordered Spawn's "first trigger by x" (g_spawnableX). Empty when the table has no such column.
inline std::unordered_map<int, double> loadObjSpawnX(std::istream& in) {
    std::unordered_map<int, double> out;
    std::string line;
    if (!std::getline(in, line)) return out;
    int iu = -1, ix = -1, is = -1, n = 0;
    {
        std::stringstream hs(line);
        std::string c;
        while (std::getline(hs, c, ',')) {
            if (c == "uid") iu = n;
            else if (c == "cx") ix = n;
            else if (c == "spawn") is = n;
            ++n;
        }
    }
    if (iu < 0 || ix < 0 || is < 0) return out;
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        std::string c;
        int i = 0, uid = -1, sp = 0;
        double x = 0.0;
        while (std::getline(ss, c, ',')) {
            if (i == iu) uid = std::atoi(c.c_str());
            else if (i == ix) x = std::atof(c.c_str());
            else if (i == is) sp = std::atoi(c.c_str());
            ++i;
        }
        if (uid >= 0 && sp == 1) out[uid] = x;
    }
    return out;
}
// The GD object types the player can be stopped or killed by. A chain that
// reaches none of these cannot change the collision geometry, whatever kind of
// trigger sits at its root -- which is what makes the test independent of the
// trigger ids, most of which are not decoded anywhere in this tree.
inline bool collidableType(int t) {
    return t == 0 || t == 2 || t == 21 || t == 47;
}
// (--activators, g_activators, is declared with g_touch above.)
// What an activator may switch on and still be handled: things that are only there to be
// stood on, collected or pressed -- a solid (0), a one-way platform (21), a coin (22, 31) or an
// orb (11/12/13/29/32/35/37/38/43). NOT a hazard (bringing one in can kill later), NOT a pad (it
// launches on contact, pressed or not), NOT a trigger (a chain the walk would have to follow).
// Custom levels have all three of those behind pickups (the census of 2026-09-23: pickups that
// remove a coin, remove hazards, or start a chain), and none is measured, so they are left out
// and named rather than guessed at.
// A decoration (7) is allowed too: the level loader never makes a collider of one, so switching
// it changes nothing the model sees. SubZero 4003's hidden ring (uid 4256) shares its group with
// its own decoration (uid 4255, id 1519, same position), and refusing that would refuse the ring.
inline bool activatorTargetSafe(int t) {
    return t == 0 || t == 21 || t == 22 || t == 31 || t == 11 || t == 12 || t == 13
        || t == 29 || t == 32 || t == 35 || t == 37 || t == 38 || t == 43 || t == 7;
}
// --boxoff: a toggle block or touch Toggle that switches its group OFF (loadTouchTriggers). On by
// default since 2026-10-05 (three GD cases the replay matches, a kitref of 0 fixed and 0 broken in
// 1,773 episodes on one exe, two custom levels cleared in cold solves); --no-boxoff is the off arm.
inline bool g_boxOff = true;
// An ITEM COMPARE (3620) that reads "item A at least C", as a gate: returns C,
// or 0 for every other shape. Only this one is measured:
//   - the operator: resultType3 = 2 is "at least" (rig calib_coingate6, constant
//     3: N = 0 and 2 stay shut, 3 and 4 open);
//   - the constant: m_mod2 with item2Mode 0 (the level string's 483, checked
//     against lv22's uid17962, which is 6 -- and six taps is what opens that
//     coin in the game);
//   - the left side: item1Mode 1 (an item counter) times m_mod1 = 1. The
//     operator between them (resultType1) is not measured, so any m_mod1 other
//     than 1 is refused rather than guessed at.
inline int itemCompareGate(const TrigRow& T) {
    if (T.id != 3620 || !T.cmpCols) return 0;
    if (T.i1mode != 1 || T.i2mode > 0 || T.res3 != 2 || T.mod1 != 1.0) return 0;
    if (T.item <= 0 || T.mod2 < 1.0 || T.mod2 != std::floor(T.mod2)) return 0;
    return (int)T.mod2;
}
// --tapdisarm: a trigger the player's x fires (checkSpawnObjects' queue) -- neither spawned nor touched, and not
// one of the roots that x only arms (Tap, Count, Item Compare, toggle block).
inline bool xCrossingRow(const TrigRow& T) {
    return !T.spawn && !T.touch && T.id != 1595 && T.id != 1594 && T.id != 1611 && T.id != 1811 && T.id != 3620;
}
inline std::vector<TouchTrig> loadTouchTriggers(const std::string& trigPath,
                                                const std::string& grpPath,
                                                double fromX = -1e18,
                                                const std::unordered_map<int, int>*
                                                    objTypes = nullptr,
                                                const std::unordered_map<int,
                                                    std::pair<double, double>>*
                                                    objPos = nullptr) {
    std::vector<TouchTrig> out;
    size_t nIrrelevant = 0;                             // --trigrelevant
    size_t nGateCosmetic = 0;                           // --worldgates (see below)
    const std::unordered_map<int, TrigRow>* trigP = trigRowsCached(trigPath);
    if (!trigP) {
        std::fprintf(stderr, "triggers: cannot open %s\n", trigPath.c_str());
        return out;
    }
    const std::unordered_map<int, TrigRow>& trig = *trigP;   // uid -> row
    const std::unordered_map<int, std::vector<int>>* membersP = groupMembersCached(grpPath);
    if (!membersP) {
        std::fprintf(stderr, "objgroups: cannot open %s\n", grpPath.c_str());
        return out;
    }
    // group id -> uids. A COPY: the Toggle root below looks its group up with operator[], which
    // adds an empty entry for a group nobody is in, and the finds after it see that entry -- as
    // they did when this map was parsed here, fresh, on every call.
    std::unordered_map<int, std::vector<int>> byGroup = *membersP;
    std::vector<TouchTrig> deferredTaps;   // --tapfeed: cosmetic-looking Taps kept back until the kept Counts are known
    for (const auto& kv : trig) {
        const TrigRow& T = kv.second;
        // A COUNT trigger (1611 instant / 1811) is a root here as well: its
        // chain is walked exactly like a box's, and it takes a bit in the same
        // mask. What differs is only who sets that bit (the step child's item
        // test rather than markTouched).
        // (Only while the gates are modelled: --coins, or --worldgates.)
        const bool isCount = (T.id == 1611 || T.id == 1811) && T.count >= 0
                             && gatesModelled();
        // ...and a TAP trigger (1595), armed by crossing and fired by a press.
        // (--tapdisarm: also without the gates, for what its chain switches off -- isTapDisarm.)
        const bool isTapDisarm = (T.id == 1595) && !gatesModelled() && g_tapDisarm && !T.touch && !T.spawn;
        const bool isTap = ((T.id == 1595) && gatesModelled()) || isTapDisarm;
        // ...and an ITEM COMPARE (3620) in the one shape itemCompareGate reads.
        // Its chain is the TRUE branch (its target), and it fires the way a
        // Count does, from the counter; the chains that spawn it stop at it
        // (the walk below), so a tap alone no longer reaches what it guards.
        const int cmpNeed = gatesModelled() ? itemCompareGate(T) : 0;
        const bool isCmp = cmpNeed > 0;
        // ...and an ACTIVATOR (--activators, see g_activators): a pickup that switches its
        // target group on when touched. Only when everything in that group is something it is
        // safe to have there (activatorTargetSafe); otherwise it is named and left out.
        // ...and a TOGGLE BLOCK (1594) with "activate group" is one too, fired not by the touch
        // but by the button: SubZero 4003's uid 4001 switches the hidden ring on when the button
        // is down while the player overlaps it -- pressed inside, or held from before it -- and
        // not on contact alone (measured on worker 98, 2026-09-23). The dump marks that row
        // touch=1, so without this it was read as a plain box (and then dropped, since its
        // chain moves nothing and reaches nothing collidable).
        bool isActivator = false, isPress = false, isSpawnRing = false;
        // --orbspawn (default off): ...and a toggle ring whose m_isSpawnOnly is set. Pressing
        // it SPAWNS its target group instead of toggling it (PlayerObject::ringJump: the second
        // slot of the layer's +0x198 handler when ring+0x741 is set), so the group holds the
        // triggers that act, and the walk below follows them as it follows a Spawn's. Taken only
        // when that group's triggers are spawn-fired Toggles and Stops, and every object those
        // Toggles reach is safe -- here including the pads (8/9/10/34). A custom level (an
        // Extreme Demon) hides one at x=5,914: its group spawns a Toggle that switches ON the
        // staircase of 17 pink pads an x-crossing Toggle turned OFF at the start, and a Stop.
        // GD's plan dies walking into the spike past the staircase unless it presses there, and
        // with the pads read off a recording the model never knew the press mattered.
        if (objTypes && T.id == 1594 && T.sponly == 1
            && T.target != 0 && !T.spawn) {
            const auto gi = byGroup.find(T.target);
            int bad = -1, badType = 0, nTog = 0;
            if (gi != byGroup.end())
                for (const int ou : gi->second) {
                    const auto t2 = trig.find(ou);
                    if (t2 == trig.end()) continue;   // its own objects are inert (decoration)
                    const TrigRow& S = t2->second;
                    if (!S.spawn || (S.id != 1049 && S.id != 1616)) { bad = ou; badType = -1; break; }
                    if (S.id != 1049) continue;
                    ++nTog;
                    const auto tg = byGroup.find(S.target);
                    if (tg == byGroup.end()) continue;
                    for (const int su : tg->second) {
                        if (trig.count(su)) continue;
                        const auto ot = objTypes->find(su);
                        const int t = ot == objTypes->end() ? -1 : ot->second;
                        // --acthazoff (speed.hpp): a hazard the Toggle switches OFF
                        if (g_actHazOff && t == 2 && S.togon == 0) continue;
                        if (!activatorTargetSafe(t) && t != 8 && t != 9 && t != 10 && t != 34) {
                            bad = su;
                            badType = t;
                            break;
                        }
                    }
                    if (bad >= 0) break;
                }
            if (gi == byGroup.end() || nTog == 0) {
                std::printf("activators: spawn ring uid %d left out - group %d spawns no "
                            "Toggle\n", T.uid, T.target);
            } else if (bad >= 0 && badType == -1) {
                std::printf("activators: spawn ring uid %d left out - group %d holds trigger "
                            "uid %d, not a spawned Toggle or Stop\n", T.uid, T.target, bad);
            } else if (bad >= 0) {
                std::printf("activators: spawn ring uid %d left out - a Toggle it spawns reaches "
                            "uid %d of type %d\n", T.uid, bad, badType);
            } else {
                isActivator = true;
                isPress = true;
                isSpawnRing = true;
                std::printf("activators: spawn ring uid %d at (%.0f,%.0f) spawns group %d "
                            "(%d Toggle(s))\n", T.uid, T.cx, T.cy, T.target, nTog);
            }
        }
        // --boxarm: the x-crossing Moves a toggle block's group holds that an x-crossing Toggle has switched off
        // (offAtCrossing) -- the block switches them back on (TouchTrig::trigTog) instead of being left out.
        std::vector<int> armUids;
        std::vector<int> armSkip;   // ...and the unmodelled trigger objects such a block's group holds besides
        if (!isSpawnRing && objTypes && T.actgrp == 1 && T.target != 0
            && !T.spawn) {
            const auto ty = objTypes->find(T.uid);
            const bool pickup = !T.touch && ty != objTypes->end() && ty->second == 30;
            const bool press = T.id == 1594;
            if (pickup || press) {
                const auto gi = byGroup.find(T.target);
                int bad = -1, badType = 0;
                if (gi != byGroup.end())
                    for (const int ou : gi->second) {
                        if (press && trig.count(ou)) {
                            const TrigRow& M = trig.at(ou);
                            bool spawnOn = false;
                            if (M.id == 901 && xCrossingRow(M) && M.target != 0
                                && offAtCrossing(M, trig, byGroup, &spawnOn) && !spawnOn) {
                                armUids.push_back(ou);
                                continue;
                            }
                        }
                        if (g_actLookChain && press && trig.count(ou)) {
                            std::unordered_set<int> seenChain;
                            if (actLookOnlyChain(trig.at(ou), trig, byGroup, 0, seenChain)) continue;
                        }
                        if (trig.count(ou)) { bad = ou; badType = -1; break; }
                        const auto ot = objTypes->find(ou);
                        const int t = ot == objTypes->end() ? -1 : ot->second;
                        // --boxarm: ...and a trigger object the table does not hold (type 20; nothing models
                        // it) does not keep out a block that switches Moves back on. A custom level's group
                        // holds one (id 1292) beside its Moves in one dump, and the block was left out there.
                        if (press && t == 20) {
                            armSkip.push_back(ou);
                            continue;
                        }
                        if (!activatorTargetSafe(t) && !(press && t == 28)) {
                            bad = ou;
                            badType = t;
                            break;
                        }
                    }
                // (only for a block that does switch Moves back on: without them the old refusal stands)
                if (bad < 0 && armUids.empty() && !armSkip.empty()) {
                    bad = armSkip.front();
                    badType = 20;
                }
                if (gi == byGroup.end()) {
                    std::printf("activators: uid %d (id %d) left out - group %d is empty\n",
                                T.uid, T.id, T.target);
                } else if (bad >= 0 && badType == -1) {
                    std::printf("activators: uid %d (id %d) left out - group %d holds the "
                                "trigger uid %d, a chain this does not follow\n",
                                T.uid, T.id, T.target, bad);
                } else if (bad >= 0) {
                    std::printf("activators: uid %d (id %d) left out - group %d holds uid %d "
                                "of type %d, not something to stand on, collect or press\n",
                                T.uid, T.id, T.target, bad, badType);
                } else {
                    isActivator = true;
                    isPress = press;
                    if (!armUids.empty()) {
                        std::printf("boxarm: uid %d (id %d, x %.0f) switches group %d back on, %zu Move(s):",
                                    T.uid, T.id, T.cx, T.target, armUids.size());
                        for (const int u : armUids) std::printf(" %d", u);
                        std::printf("\n");
                    }
                }
            }
        }
        if (!isActivator) {
            armUids.clear();
            armSkip.clear();
        }
        // --boxoff (default on): ...and the same two without "activate group", which switch
        // their group OFF -- a toggle block (1594, actgrp 0) when the button is down inside it, a
        // touch-fired Toggle (1049, togon 0) on contact. A custom level's wall is five hazards in
        // one group, a 1594 box 120 px before it targeting that group; the model kept the wall and
        // GD killed the player there exactly as the model did, so the replay instruments passed
        // while no route existed. Switching a hazard OFF cannot bring a kill in, so hazards are
        // allowed here as well as everything an activator may switch on.
        bool isOffBox = false;
        if (g_boxOff && !isSpawnRing && !isActivator && objTypes && T.target != 0 && !T.spawn
            && ((T.id == 1594 && T.actgrp == 0) || (T.id == 1049 && T.touch && T.togon == 0))) {
            const auto gi = byGroup.find(T.target);
            int bad = -1, badType = 0;
            if (gi != byGroup.end())
                for (const int ou : gi->second) {
                    if (trig.count(ou)) { bad = ou; badType = -1; break; }
                    const auto ot = objTypes->find(ou);
                    const int t = ot == objTypes->end() ? -1 : ot->second;
                    if (!activatorTargetSafe(t) && t != 2) { bad = ou; badType = t; break; }
                }
            if (gi == byGroup.end()) {
                std::printf("boxoff: uid %d (id %d) left out - group %d is empty\n",
                            T.uid, T.id, T.target);
            } else if (bad >= 0) {
                std::printf("boxoff: uid %d (id %d) left out - group %d holds uid %d of type %d\n",
                            T.uid, T.id, T.target, bad, badType);
            } else {
                isActivator = true;
                isPress = T.id == 1594;
                isOffBox = true;
                std::printf("boxoff: uid %d (id %d) at (%.0f,%.0f) switches group %d off "
                            "(%zu objects)\n", T.uid, T.id, T.cx, T.cy, T.target,
                            gi->second.size());
            }
        }
        if ((!T.touch && !isCount && !isTap && !isCmp && !isActivator) || T.target == 0)
            continue;
        if (visualOnly(T.id)) continue;   // --touchvisual: a tint is no box
        TouchTrig tt{T.cx, T.cy, T.w * 0.5, T.h * 0.5, {}};
        tt.uid = T.uid;   // so an outside payload can name the box (see the field)
        tt.id = T.id;
        tt.togOn = (T.id == 1049) ? T.togon : -1;   // --rotqtoggle (see the field)
        // --countactgrp (constants.hpp): a Count's "activate group" switches its target group, as a Toggle root's
        // does -- where the state's counter is the game's, i.e. every giver of its item is a pickup the player
        // walks into (countItemWalkFed)
        // --tap2: ...or by a Tap whose presses the state counts (State::taps / tapsX) -- every giver of the item a walk-in pickup or in a Tap's own group
        auto giverBehindTap = [&](int giverUid) {
            for (const auto& kv : trig) {
                if (kv.second.id != 1595 || kv.second.target == 0) continue;
                const auto gi = byGroup.find(kv.second.target);
                if (gi != byGroup.end() && std::find(gi->second.begin(), gi->second.end(), giverUid) != gi->second.end())
                    return true;
            }
            return false;
        };
        const bool countAct = g_countActGrp && isCount && T.actgrp >= 0
            && (countItemWalkFed(T.item) || countItemTapFed(T.item, giverBehindTap));
        if (countAct) tt.togOn = T.actgrp;
        if (isCount) {
            tt.item = T.item;
            tt.count = T.count;
            tt.cmode = T.cmode > 0 ? T.cmode : 0;
        }
        if (isCmp) {
            tt.item = T.item;
            tt.count = cmpNeed;
            tt.cmode = 3;
        }
        tt.tap = isTap;
        // --touchoff (g_touchOwnOff): the x-crossing Toggles that switch this box's own trigger,
        // through a group it sits in (TouchTrig::ownTog). The same Toggles offAtCrossing reads
        // for an x-crossing Move; a box is touched more than once (the way there and the way
        // back), so it is judged by when each of them fired rather than at one crossing.
        if (g_touchOwnOff && T.touch) {
            bool chained = false;
            forTogglesHolding(T.uid, trig, byGroup, [&](const TrigRow& X) {
                if (X.spawn || X.touch) {
                    chained = true;
                    return;
                }
                tt.ownTog.push_back({X.uid, X.cx, (uint8_t)(X.togon == 1), -1});
            });
            if (chained) tt.ownTog.clear();
            std::sort(tt.ownTog.begin(), tt.ownTog.end(),
                      [](const TouchTrig::OwnTog& a, const TouchTrig::OwnTog& b) {
                          return a.cx < b.cx || (a.cx == b.cx && a.uid < b.uid);
                      });
        }
        // The root's own switch, when the root is a Toggle itself: the walk below
        // collects the Toggles it REACHES, and this is the one it starts from.
        // Only `togEv` takes it -- `togOnUids` (the coin gate) stays as it was.
        if (T.id == 1049 && T.togon >= 0) {
            const auto gi = byGroup.find(T.target);
            if (gi != byGroup.end())
                for (const int ou : gi->second)
                    if (!trig.count(ou))
                        tt.togEv.push_back({ou, (uint8_t)(T.togon == 1), 0.0, T.target});
                    else if (g_tapDisarm && xCrossingRow(trig.at(ou)))
                        tt.trigTog.push_back({ou, (uint8_t)(T.togon == 1), 0.0});
        }
        // --countactgrp: ...and a Count's, on its target group's objects when the counter fires it
        if (countAct) {
            tt.countAct = true;
            const auto gi = byGroup.find(T.target);
            if (gi != byGroup.end())
                for (const int ou : gi->second)
                    if (!trig.count(ou)) tt.togEv.push_back({ou, (uint8_t)(T.actgrp == 1), 0.0, T.target});
        }
        // ...and an activator switches its whole group on, with no delay (the key's
        // platforms are there on the tick after the touch, which is when the player's box is
        // first tested against it).
        tt.activator = isActivator;
        tt.press = isPress;
        tt.spawnRing = isSpawnRing;
        tt.offSwitch = isOffBox;
        // (A spawn ring switches nothing itself: its group's Toggles do, collected by the walk.)
        // (--boxoff: an off box switches it off; a 1049 root has already pushed its own switch.)
        if (isActivator && !isSpawnRing && !(isOffBox && T.id == 1049))
            for (const int ou : byGroup[T.target]) {
                // --boxarm: a Move it switches back on is a trigger, not an object to show (trigTog below),
                // and so is an unmodelled trigger object beside it
                if (!armUids.empty() && (std::find(armUids.begin(), armUids.end(), ou) != armUids.end()
                                         || std::find(armSkip.begin(), armSkip.end(), ou) != armSkip.end()))
                    continue;
                tt.togEv.push_back({ou, (uint8_t)(isOffBox ? 0 : 1), 0.0, T.target});
            }
        for (const int u : armUids) tt.trigTog.push_back({u, 1, 0.0});
        struct Item {
            int group; float dx, dy; double dur; int ease; double erate;
            double lock, lockY;
            // the remap the triggers in `group` act under (--spawnremap)
            std::vector<std::pair<int, int>> remap;
            // --movetarget only (see TrigCtl::mover / tmode / tdx / tdy)
            int mover = 0;
            uint8_t tmode = 0;
            float tdx = 0.f, tdy = 0.f;
            // The spawn delays this chain has accumulated, in ticks. Read only
            // by the on/off events (TouchTrig::togEv): the MOVES take their
            // duration from the hop that moves (the note at the push below),
            // and the chains measured so far carry sdelay 0 in front of one.
            double sdelay = 0.0;
            bool root = false;   // the box's own target group
            // --spawnlag: when the triggers in `group` fire, in seconds after the
            // entry (queued spawns only; an immediate chain is 0), and whether the Spawn that
            // fires them is an ordered one (each then queued by its x behind the first).
            double fireS = 0.0;
            bool ordered = false;
            // --tapdisarm: `group` is a Toggle's target. A Toggle spawns nothing (triggerObject case
            // 0x419 calls toggleGroup alone), so the triggers in it are not followed.
            bool togHop = false;
        };
        // Seed with the BOX'S OWN move. A touch row is often a bare Spawn whose
        // effect is nested (all 3 of lv19's are), and starting the walk at zero
        // was right for those -- but a Move can carry touch=1 itself, and then
        // dropping its ox/oy left the whole chain with offset (0,0).
        // Measured on lv22 (2026-08-13): uid 1337 (2205,315, target 57,
        // oy=-120, 0.5 s) is the wall that becomes the FLOOR of the first
        // rotated section. With a zero offset the anchor scan below cannot tell
        // "already open" from "never fired" -- `full` is 0 -- so every
        // re-anchored tail ran with that wall at rest. lv19 has no such row, so
        // this is inert there; lv20 has 4 and lv22 has 5.
        //
        // ...and a ROTATE root acts without an ox/oy, the same as a rotate hop
        // below. lv22's boxes 0, 2 and 5 are exactly this -- their touch
        // trigger IS the Rotate (uid199/255/321, id 1346, deg -5/7/-8) -- so
        // reading only the offset gave them a key window of 0 where an ordinary
        // box has 129, and a zero window means the box contributes nothing to
        // the key at all while its geometry is still turning.
        const bool rootTurns = (T.deg != 0.0 || T.t360 != 0);
        const bool rootMoves = (T.ox != 0.0 || T.oy != 0.0 || rootTurns);
        // --trigdump only: how much of this chain ACTS. `ctl` is filled from the
        // else-branch below, which pushes every plain object the walk reaches
        // whether or not anything moved it -- deliberately, because GD's
        // "move to target" carries no offset. The consequence is that
        // `!tt.ctl.empty()` keeps a box whose chain only tints its group, and on
        // lv22 that is most of them: 155 touch rows, 152 boxes, and the two
        // largest id families (66 + 52 of 155) carry no ox/oy/deg of their own.
        // These counters exist to put a number on that before any rule is
        // written. They are read nowhere else.
        // Counted at the ctl push below rather than at each hop, because that is
        // where a selection rule would have to decide: the entry it pushes
        // carries everything the walk accumulated (offset, duration, lock).
        size_t nInert = 0;
        size_t nTogHopSkip = 0;   // --tapdisarm: spawn-fired triggers in a Toggle's group, no longer followed
        double maxDur = 0.0, maxLock = 0.0, maxOff = 0.0;
        std::vector<Item> stack{{T.target, (float)T.ox, (float)T.oy,
                                 rootMoves ? T.dur * 240.0 : 0.0,
                                 rootMoves ? T.ease : 0,
                                 rootMoves ? T.erate : 2.0,
                                 lockWindowTicks(T.lockx, T.dur),
                                 lockWindowTicks(T.locky, T.dur),
                                 g_spawnRemap ? T.remap
                                              : std::vector<std::pair<int, int>>{}}};
        stack[0].mover = rootMoves ? T.uid : 0;
        stack[0].root = true;
        stack[0].togHop = g_tapDisarm && T.id == 1049;
        if (T.id == 1268) {
            stack[0].fireS = chainDelayS(T);
            stack[0].ordered = T.sordd != 0;
        }
        // A group can contain the trigger that targets it, so the walk needs a
        // hard bound rather than a visited set (the same group legitimately
        // appears twice under different offsets).
        int guard = 0;
        while (!stack.empty() && guard++ < 4096) {
            const Item it = stack.back();
            stack.pop_back();
            const auto g = byGroup.find(it.group);
            if (g == byGroup.end()) continue;
            // --spawnlag: an ordered Spawn fires its group's first spawnable trigger (by x) on
            // the spot and queues each other one (x - xFirst) / 311.58 s behind it.
            double xFirst = 1e18;
            if (it.ordered)
                for (const int ou : g->second) {
                    const auto sx = g_spawnableX.find(ou);
                    if (sx != g_spawnableX.end()) xFirst = std::min(xFirst, sx->second);
                    const auto tr = trig.find(ou);
                    if (tr != trig.end() && tr->second.spawn)
                        xFirst = std::min(xFirst, tr->second.cx);
                }
            for (const int uid : g->second) {
                const auto t2 = trig.find(uid);
                if (t2 != trig.end()) {
                    // --tapdisarm (Item::togHop): a Toggle spawns nothing, so the SPAWN-fired rows
                    // in its group are not followed. Only those: the root Toggle's box still moves
                    // an x-crossing Move it switches back on (g_offMoves, reOn below). Skipping
                    // every row here dropped that, and Meltdown 1003's coin platform (the Move uid
                    // 3437, confirmed in the game, see g_offMoves above) stayed up in the model.
                    if (it.togHop && t2->second.spawn) {
                        ++nTogHopSkip;
                        continue;
                    }
                    if (t2->second.target == 0) continue;
                    // Only SPAWN-fired rows belong to this chain. A row that is
                    // neither touched nor spawned is autonomous -- it fires on
                    // its own x crossing and g_autoTrig already owns it -- and a
                    // row with touch=1 is its own box. Following either one hands
                    // the touch their effects as well.
                    // lv22 x=3,195 is what that costs: the hanging block's group
                    // contains the autonomous Move (uid 2032, lockx=1) that locks
                    // the chasing spike to the player, so bonking the block
                    // teleported that spike onto every branch in the frontier
                    // (41 alive at t=2,530 -> 0 at t=2,534, four ticks later).
                    // OPT-IN with the window: it is not free elsewhere --
                    // lv19's (28693,275) chain goes 105 -> 79 objects and
                    // lv20's (7003,406) 69 -> 33, and lv19's ride across
                    // x=28,783..29,001 hangs off that chain. Replays of all 21
                    // are bit-identical, but the SEARCH is not proven, so the
                    // default stays as it was until a regression says otherwise.
                    // A row only fires from a chain if it is SPAWN-fired. This
                    // filter was here already but opt-in behind
                    // the (removed) touch window, so by default the walk followed
                    // autonomous Moves as well -- and lv19's group 96 is what
                    // that costs: the touch chain absorbed uid14066's -75 (an
                    // x-crossing Move) on top of uid13925's +36 and reported
                    // the NET, -39, over uid14066's duration. GD does neither:
                    // it raises the group 36 and then drops it 75, which the
                    // recording shows to the decimal (174.5 -> 210.5 -> 135.5).
                    //
                    // g_offMoves: ...except an x-crossing Move that this box's own Toggle
                    // switches back on and that is off otherwise (offAtCrossing, which also
                    // stops it firing on its own). GD fires it at its x once the group is on;
                    // the model moves its group from this box instead, at the touch -- earlier
                    // than GD by the ride from the box to the Move's x. Only a Move ahead of the
                    // box whose objects all sit at or past the Move's own x, where the player
                    // cannot be before the crossing, so the early start is not seen.
                    bool reOn = false;
                    if (it.root && T.id == 1049 && T.togon == 1 && T.touch
                        && !t2->second.spawn && !t2->second.touch && t2->second.id == 901
                        && t2->second.cx > T.cx && objPos) {
                        bool spawnOn = false;
                        if (offAtCrossing(t2->second, trig, byGroup, &spawnOn) && !spawnOn) {
                            const auto mg = byGroup.find(t2->second.target);
                            reOn = mg != byGroup.end() && !mg->second.empty();
                            if (reOn)
                                for (const int ou : mg->second) {
                                    const auto pi = objPos->find(ou);
                                    if (pi != objPos->end() && pi->second.first < t2->second.cx) {
                                        reOn = false;
                                        break;
                                    }
                                }
                            std::printf("triggers: box uid %d switches the Move uid %d (x=%.0f) "
                                        "back on - %s\n", T.uid, t2->second.uid, t2->second.cx,
                                        reOn ? "the box moves its group"
                                             : "left out, its group is not all past its x");
                        }
                    }
                    if (!t2->second.spawn && !reOn) continue;
                    if (visualOnly(t2->second.id)) continue;   // --touchvisual
                    // --spawnlag: when this trigger fires (see Item::fireS)
                    const double fireT2 = it.fireS
                        + ((it.ordered && xFirst < 1e17)
                               ? std::max(0.0, (t2->second.cx - xFirst) / 311.5801086425781)
                               : 0.0);
                    // An Item Compare this build reads is a root of its own
                    // (see isCmp): what it spawns happens only when the counter
                    // says so. Walking through it credited lv22's tap box with
                    // switching the third coin on at the first press, where the
                    // game needs six.
                    if (gatesModelled() && itemCompareGate(t2->second) > 0) continue;
                    // --spawnroots: and at a spawned Count or Tap (g_spawnRoots). It is
                    // armed by this box instead (TouchTrig::armBy, set once bits exist).
                    if (gatesModelled()
                        && ((((t2->second.id == 1611 || t2->second.id == 1811)
                              && t2->second.count >= 0))
                            || t2->second.id == 1595)) {
                        tt.spawnsRoots.push_back(t2->second.uid);
                        continue;
                    }
                    // --movetarget: a STOP halts the Moves in its target group.
                    // Those are ordinary triggers (touch or x-crossing), which
                    // the walk skips, so without this the Stop reached nothing
                    // at all. Recorded on the box, and the group is not walked
                    // (it holds triggers, not geometry). Spawn delays are not
                    // accumulated: lv22's trap chain (the one case measured)
                    // has sdelay 0 at every hop.
                    if (t2->second.id == 1616) {
                        const int sg = remapGroup(it.remap, t2->second.target);
                        const auto sgi = byGroup.find(sg);
                        if (sgi != byGroup.end())
                            for (const int su : sgi->second)
                                if (trig.count(su)) tt.stops.push_back(su);
                        continue;
                    }
                    // --movetarget: a TARGET-MODE Move. Its ox/oy are 0 by
                    // construction; where it takes the group is "the object in
                    // group `center`" measured from "the object in group
                    // `tmodctr`", which rides with the moved group -- so the
                    // destination is a fixed offset from the group's placement
                    // and the distance still to go is decided when it fires.
                    if (objPos && t2->second.id == 901
                        && t2->second.mvtgt == 1) {
                        auto posOfGroup = [&](int grp, double& x, double& y) {
                            const auto gi = byGroup.find(grp);
                            if (gi == byGroup.end()) return false;
                            for (const int ou : gi->second) {
                                const auto pi = objPos->find(ou);
                                if (pi != objPos->end()) {
                                    x = pi->second.first; y = pi->second.second;
                                    return true;
                                }
                            }
                            return false;
                        };
                        double tx = 0, ty = 0, rx = 0, ry = 0;
                        if (posOfGroup(remapGroup(it.remap, t2->second.center), tx, ty)
                            && posOfGroup(remapGroup(it.remap, t2->second.tmodctr),
                                          rx, ry)) {
                            Item nx{remapGroup(it.remap, t2->second.target),
                                    it.dx, it.dy, t2->second.dur * 240.0,
                                    t2->second.ease, t2->second.erate,
                                    it.lock, it.lockY, it.remap};
                            nx.mover = t2->second.uid;
                            // 1 = both axes, 2 = x only, 3 = y only (mvaxis + 1):
                            // a zero destination on an axis is not the same as
                            // not moving on it.
                            nx.tmode = (uint8_t)(1 + std::clamp(t2->second.mvaxis, 0, 2));
                            nx.tdx = (float)(tx - rx);
                            nx.tdy = (float)(ty - ry);
                            nx.fireS = fireT2;   // --spawnlag
                            stack.push_back(std::move(nx));
                            continue;
                        }
                    }
                    // Duration belongs to the hop that actually MOVES something.
                    // The spawn trigger in front of lv19's door carries 0.5 s of
                    // its own, and adding that would stretch the "still opening"
                    // window past the tick the player arrives -- i.e. it would
                    // keep the door shut and undo the whole fix. Measured: the
                    // move starts 5 ticks after the box is touched, not 120.
                    // The easing rides along with the duration: it belongs to
                    // the same hop, and a curve read off a hop that does not
                    // move is meaningless.
                    //
                    // "MOVES SOMETHING" HAS TO INCLUDE TURNING SOMETHING. A
                    // Rotate acts without an ox/oy, so reading only the offset
                    // left lv22's three Rotate-driven boxes with a key window
                    // of 0 where an ordinary box has 129 -- and a zero window
                    // means the box contributes nothing to the key at all, so
                    // two states that entered it at different ticks merge while
                    // the geometry is still turning. The same zero offset is
                    // what hid those boxes from the anchor scan (cli.hpp), so
                    // one reading of "moves" was costing both.
                    // A Toggle in the chain: remember what it switches ON, for
                    // the objects (not the triggers) of its target group. The
                    // walk carries on as before -- the group is pushed below
                    // like any other hop, which is how the coin gets its ctl
                    // entry at all.
                    if (t2->second.id == 1049 && t2->second.togon >= 0) {
                        const int tg = remapGroup(it.remap, t2->second.target);
                        const auto gi = byGroup.find(tg);
                        const double dl = it.sdelay + chainDelayS(t2->second) * 240.0;
                        if (gi != byGroup.end())
                            for (const int ou : gi->second)
                                if (!trig.count(ou)) {
                                    tt.togEv.push_back({ou,
                                        (uint8_t)(t2->second.togon == 1), dl, tg});
                                    // Unchanged from the coin gate's first
                                    // form: every nested switch-on, no delay
                                    // in the test. The delay lives in togEv.
                                    if (t2->second.togon == 1)
                                        tt.togOnUids.push_back(ou);
                                } else if (g_tapDisarm && xCrossingRow(trig.at(ou))) {
                                    tt.trigTog.push_back({ou,
                                        (uint8_t)(t2->second.togon == 1), dl});
                                }
                    }
                    const bool turns = (t2->second.deg != 0.0
                                        || t2->second.t360 != 0);
                    const bool moves = (t2->second.ox != 0.0
                                        || t2->second.oy != 0.0 || turns);
                    const double d2 = t2->second.dur * 240.0;
                    const bool longer = moves && d2 >= it.dur;
                    stack.push_back({remapGroup(it.remap, t2->second.target),
                                     it.dx + (float)t2->second.ox,
                                     it.dy + (float)t2->second.oy,
                                     longer ? d2 : it.dur,
                                     longer ? t2->second.ease : it.ease,
                                     longer ? t2->second.erate : it.erate,
                                     std::max(it.lock, lockWindowTicks(t2->second.lockx, t2->second.dur)),
                                     std::max(it.lockY, lockWindowTicks(t2->second.locky, t2->second.dur)),
                                     g_spawnRemap
                                         ? composeRemap(t2->second.remap, it.remap)
                                         : std::vector<std::pair<int, int>>{}});
                    // --movetarget: the Move that set this hop's motion, and a
                    // target mode inherited from an earlier hop.
                    stack.back().mover = moves ? t2->second.uid : it.mover;
                    stack.back().tmode = it.tmode;
                    stack.back().tdx = it.tdx;
                    stack.back().tdy = it.tdy;
                    // ...and the spawn delays down to here, for togEv only.
                    stack.back().sdelay = it.sdelay + chainDelayS(t2->second) * 240.0;
                    stack.back().togHop = g_tapDisarm && t2->second.id == 1049;
                    // --spawnlag: what this trigger fires goes off its own delay later, and
                    // in x order if it is an ordered Spawn.
                    {
                        const bool sp = t2->second.id == 1268;
                        stack.back().fireS = fireT2 + (sp ? chainDelayS(t2->second) : 0.0);
                        stack.back().ordered = sp && t2->second.sordd != 0;
                    }
                } else {
                    // Recorded WITH a zero offset too. m_moveOffset is empty for
                    // GD's "move to target" mode, and lv19's third touch trigger
                    // is exactly that: (28693,275) fires a 0.29 s +36 lift AND a
                    // 1.18 s move-to, and the second one is the one that matters
                    // -- GD's own trace has the ball riding it, grounded, from
                    // x=28,783 to x=29,001 across a gap that is otherwise a
                    // spike floor. Dropping those objects here would leave the
                    // model with the lift but not the ride.
                    // The offset is only a fallback for the first iteration
                    // anyway; once a plan touches the box, the replay's
                    // grouptrace carries the real trajectory (see applyTriggers).
                    // --trigdump only: is this entry one the player can feel?
                    const double off = std::max(std::fabs((double)it.dx),
                                                std::fabs((double)it.dy));
                    if (off == 0.0 && it.dur == 0.0 && it.lock == 0.0
                        && it.lockY == 0.0)
                        ++nInert;
                    maxOff = std::max(maxOff, off);
                    maxDur = std::max(maxDur, it.dur);
                    maxLock = std::max(maxLock, std::max(it.lock, it.lockY));
                    tt.ctl.push_back({uid, it.dx, it.dy, it.dur, it.ease,
                                      it.erate, it.lock, it.lockY});
                    tt.ctl.back().mover = it.mover;
                    tt.ctl.back().tmode = it.tmode;
                    tt.ctl.back().tdx = it.tdx;
                    tt.ctl.back().tdy = it.tdy;
                    tt.ctl.back().spawnLag = spawnLagTicks(it.fireS);
                }
            }
        }
        // --tapdisarm: a Tap spawns its group only when its press switches it ON (toggleGroupTriggered);
        // one that switches it off, or that acts on the release (hold mode), switches no trigger
        // off from the press this chain is fired by.
        if (isTap && !(T.thold == 0 && T.ttog == 1)) tt.trigTog.clear();
        // ...and only a switch-OFF can stop a trigger (none = on, autoDisarmed), so only that keeps a box the
        // tests below would drop. Keeping more grows the box count past the window's 32 and changes which
        // boxes a re-anchored call keeps.
        const bool trigOff = std::any_of(tt.trigTog.begin(), tt.trigTog.end(),
                                         [](const TouchTrig::TogEv& e) { return e.on == 0; });
        // ...and a Tap is a box under the flag alone only for that: without the gates modelled
        // (isTapDisarm), what its chain moves stays on the recording as before. Its switches stay
        // (togEv): they are read only by the by-group on/off of the objects a disarm reaches
        // (Dynamics::disarmEv) -- boxOnEvents leaves a Tap out.
        if (isTapDisarm) {
            if (!trigOff) continue;
            tt.ctl.clear();
            tt.togOnUids.clear();
            tt.spawnsRoots.clear();
            tt.stops.clear();
        }
        // A COUNT OR TAP ROOT EARNS ITS BIT ONLY IF ITS CHAIN REACHES A COIN.
        //
        // These two are roots only because coin routing is on, and a root MOVES
        // WHAT ITS CHAIN NAMES. lv21 has ten Counts: one reads item 0 and moves
        // 1,395 objects, and six are the level's own "n of 10" readout moving 59
        // each -- and at a counter of zero every `<= k` of them fires at once.
        // So --coins alone moved some 350 objects the model otherwise never
        // touches, and the SAME plan that GD flies to x=21,840 died at x=14,245
        // -- with the flag on and only with it.
        //
        // CORRECTED 2026-09-20, having first written it up as the window
        // evicting ordinary boxes (the window keeps 32 and never drops a
        // Count). That is a real hazard but it is NOT what happened here:
        // **lv21 has no touch boxes at all** -- 0 rows with touch=1 and a
        // target -- so out.size() never passed 32 and nothing was trimmed. The
        // eviction story fitted the symptom and was never checked against the
        // level; counting the rows takes one grep and would have refused it.
        // A trigger that is also a touch box (T.touch) keeps its bit either
        // way: it is in this list for a reason that has nothing to do with
        // coins, and dropping it would change a run that never asked for them.
        // --worldgates: every gate is the world's, whatever its chain reaches -- GD runs them all.
        if ((isCount || isTap || isCmp) && !T.touch && !g_coinUids.empty() && !g_worldGates) {
            for (const TrigCtl& c : tt.ctl)
                if (g_coinUids.count(c.uid)) { tt.reachesCoin = true; break; }
            // A TAP IS KEPT ON A SECOND GROUND: its chain feeds an item (a
            // Pickup in it), and a coin's gate may read that item. lv22's tap
            // box reaches the third coin only through the Item Compare, which
            // the walk now stops at. Whether the item is one a coin reads is
            // known only once every root is walked, so the window block below
            // drops the taps that turn out not to be.
            bool feedsItem = false;
            if (isTap && !tt.reachesCoin)
                for (const TrigCtl& c : tt.ctl)
                    if (g_itemGiver.count(c.uid)) { feedsItem = true; break; }
            if (!tt.reachesCoin && !feedsItem) continue;
            // ...and where its window shuts: the nearest x-crossing Stop past
            // it whose target group holds the tap itself or a Pickup it spawns.
            if (feedsItem)
                for (const auto& sv : trig) {
                    const TrigRow& S = sv.second;
                    // A TOUCH-box Stop shuts it too, where the player passes it
                    // (SubZero 4003: the Stop box uid 6817 at x=10,695 halts the tap
                    // group, and presses after it count nothing -- measured in the game).
                    if (S.id != 1616 || S.spawn || S.cx <= T.cx)
                        continue;
                    const auto sg = byGroup.find(S.target);
                    if (sg == byGroup.end()) continue;
                    bool halts = false;
                    for (const int u : sg->second) {
                        if (u == T.uid) { halts = true; break; }
                        for (const TrigCtl& c : tt.ctl)
                            if (c.uid == u && g_itemGiver.count(u)) { halts = true; break; }
                        if (halts) break;
                    }
                    if (halts && S.cx < tt.tapCloseX) {
                        tt.tapCloseX = S.cx;
                        tt.tapCloseY = S.cy;
                        tt.tapCloseChan = S.chan;
                        tt.tapCloseTouch = S.touch != 0;
                        tt.tapCloseHw = S.w * 0.5;
                        tt.tapCloseHh = S.h * 0.5;
                        tt.tapCloseUid = S.uid;
                    }
                }
            if (isTap) tt.tapChan = T.chan;
        }
        // --trigdump: one line per box BEFORE the 32-cap, so the population the
        // cap chooses from can be counted. `inert` is the number of ctl entries
        // the walk reached without any offset, duration or lock -- objects the
        // box is credited with but never seen to act on. Printed with the uids
        // so the types can be joined against objrects offline; this header does
        // not read the level.
        // How many of the objects this chain reaches are ones the player
        // collides with. -1 when no type map was given, so "no types" never
        // reads as "no collidables" -- a distinction that matters: the probe
        // call at cli.hpp builds the same boxes without a map, and counting
        // its -1 as zero drops two real boxes on lv20.
        if (g_tapDisarm && (nTogHopSkip || !tt.trigTog.empty())) {
            std::printf("tapdisarm: box uid %d (id %d, x %.0f) switches %zu trigger(s)", tt.uid, tt.id,
                        tt.cx, tt.trigTog.size());
            for (const TouchTrig::TogEv& e : tt.trigTog)
                std::printf(" %d:%s+%.0f", e.uid, e.on ? "on" : "off", e.delay);
            std::printf("; %zu spawn-fired trigger(s) in a Toggle's group no longer followed\n", nTogHopSkip);
        }
        int hard = objTypes ? 0 : -1;
        if (objTypes)
            for (const TrigCtl& c : tt.ctl) {
                const auto t3 = objTypes->find(c.uid);
                if (t3 != objTypes->end() && collidableType(t3->second))
                    ++hard;
            }
        if (g_trigDump && !tt.ctl.empty()) {
            std::printf("trigdump: uid=%d id=%d cx=%.0f cy=%.0f ctl=%zu "
                        "inert=%zu hard=%d maxoff=%.2f maxdur=%.1f "
                        "maxlock=%.1f uids=",
                        tt.uid, tt.id, tt.cx, tt.cy, tt.ctl.size(), nInert,
                        hard, maxOff, maxDur, maxLock);
            for (size_t i = 0; i < tt.ctl.size(); ++i)
                std::printf("%s%d", i ? "," : "", tt.ctl[i].uid);
            std::printf("\n");
        }
        // --trigrelevant: drop the boxes that cannot reach the player at all,
        // BEFORE the cap picks 32. Without a type map the test is not
        // available and every box is kept, which is the old behaviour.
        // ...and only over the boxes that would have existed anyway. A chain
        // that reached no object at all is dropped by the test below whether or
        // not this flag is on, so counting it here would make `total` mean a
        // different population than the same word on the line without the flag.
        // ...but never a coin root (Count, Tap, Item Compare -- all of them
        // passed the coin test above). What those act on is a counter or a
        // coin, neither of which is collidable, so the test read lv22's tap
        // box and its Item Compare as moving nothing and dropped both: the
        // third coin had no gate at all in any call that loaded types.
        // ...nor an activator: an orb it switches on is not collidable either.
        if (objTypes && !tt.ctl.empty() && !isCount && !isTap && !isCmp && !isActivator) {
            const bool moves = nInert < tt.ctl.size();
            if (!moves && hard <= 0 && tt.togOn < 0 && !trigOff) {
                ++nIrrelevant;
                continue;
            }
        }
        // --worldgates without --coins: a gate is the world's only where it acts on the world --
        // its chain reaches something the player collides with, or it toggles. One that only
        // moves decorations or a coin (lv22's two Counts: Pulses and an Alpha on decorations, and a
        // Move of a coin) would take a slot in the touch window and push real boxes out of it:
        // measured on lv22, four, among them the switch band's 18179 (35 collidables) and 2563.
        // Without a type map nothing is known about the chain, and the gate stays.
        if (g_worldGates && !g_coinRoute && (isCount || isTap || isCmp) && objTypes
            && !tt.ctl.empty() && hard <= 0 && tt.togOn < 0 && !trigOff) {
            // --tapfeed: a Tap whose chain only feeds a Pickup is cosmetic by itself; it is the world's when a kept
            // Count reads that item (below, once the Counts are known)
            if (isTap) deferredTaps.push_back(std::move(tt));
            else ++nGateCosmetic;
            continue;
        }
        // (--tapdisarm: a chain that only switches triggers off still has its box.)
        // (--boxarm: ...and so does a toggle block that only switches Moves back on.)
        if (!tt.ctl.empty() || trigOff || !armUids.empty()) out.push_back(std::move(tt));
    }
    // --tapfeed: the Taps kept back above come in when a kept Count (or Item Compare) reads an item their Pickup gives.
    // A custom level: a Tap at x=22,905 feeds item 10 through a Pickup; a Count (item 10 == 4) starts the Move of 529
    // objects, among them a spike the wave meets. The Tap was dropped as a gate that moves nothing collidable, the Count
    // never reached 4 in the model, and the spike stood 24 ticks longer than in the game (the cold loop sat on that
    // wall for ~1,800 s).
    if (!deferredTaps.empty()) {
        std::vector<int> readItems;
        for (const TouchTrig& t : out)
            if (t.count >= 0 && t.item != 0) readItems.push_back(t.item);
        for (TouchTrig& t : deferredTaps) {
            bool feeds = false;
            for (const TrigCtl& c : t.ctl) {
                const auto gi = g_itemGiver.find(c.uid);
                if (gi == g_itemGiver.end()) continue;
                for (const int w : readItems) if (w == gi->second) { feeds = true; break; }
                if (feeds) break;
            }
            if (feeds) out.push_back(std::move(t));
            else ++nGateCosmetic;
        }
    }
    if (nGateCosmetic)
        std::printf("triggers: --worldgates left out %zu gate(s) whose chain reaches nothing "
                    "collidable and toggles nothing\n", nGateCosmetic);
    if (objTypes)
        std::printf("triggers: --trigrelevant dropped %zu box(es) that move "
                    "nothing, reach nothing collidable and toggle nothing; "
                    "%zu left to choose from\n", nIrrelevant, out.size());
    g_trigTotal = out.size() + nIrrelevant;
    g_trigRelevantN = out.size();
    // ...and clear the two sides HERE, not in the cap block: a call that keeps
    // everything never enters that block, and would otherwise publish the
    // previous call's split. That is the defect this file already carries a
    // fix for once (the five coverage numbers), in the same shape.
    g_trigDroppedBehind = 0;
    g_trigDroppedAhead = 0;
    g_trigFirstAheadX = 1e300;
    std::sort(out.begin(), out.end(),
              [](const TouchTrig& a, const TouchTrig& b) { return a.cx < b.cx; });
    // one bit per trigger in the state's mask
    // NOTE: the window used to be opt-in (`--touch-from-anchor`, removed), and it is NOT ready. Measured on
    // lv22 at the switch band: with the window on, the DP sees the hanging
    // block at x=3,195 and the whole frontier dies FOUR TICKS after touching it
    // (41 alive at t=2,530 -> 0 at t=2,534) where the same solve without it
    // reaches t=2,774. Two suspects, both unproven: the chain still moves 222
    // objects on one touch, and the recording already carries the touched
    // world, so a state that has NOT touched may be reading a base position
    // that never existed. Left in as a switch so the next session can pick it
    // up with a measurement instead of re-deriving the 32-cap.
    // A COUNT trigger is never dropped by the window below. There are at most a
    // handful in a level (lv21 has 10, lv22 two), they have no x at which the
    // player is "near" them -- the counter is what fires them, from anywhere --
    // and dropping the one that opens a coin's group would make the coin
    // unreachable without saying so. Pulled out, trimmed, put back.
    // ...AND SO IS A BOX THAT FEEDS A COIN'S COUNTER. lv22 keeps 32 of its 152
    // and drops 122 ahead, so the window spans x=511..2,277 -- while the seven
    // boxes that raise the first coin sit at x=3,263..3,690. They were not in
    // the model at all, item 2 could never move, and the coin could never come
    // up to 303: the search had no representable way to take it. Measured
    // 2026-09-20 off the window's own line.
    //
    // Which boxes those are is derived, not listed: a coin-gating Count names
    // the item it reads, and a box is a feeder when its chain reaches an object
    // that items.txt says gives that item. Both ends come out of the dump.
    std::vector<int> wantedItems;
    for (const TouchTrig& t : out)
        if (t.count >= 0 && t.item != 0) {
            bool reachesCoin = false;
            for (const TrigCtl& c : t.ctl)
                if (g_coinUids.count(c.uid)) { reachesCoin = true; break; }
            if (reachesCoin) wantedItems.push_back(t.item);
        }
    auto feedsACoin = [&](const TouchTrig& t) {
        if (wantedItems.empty() || g_itemGiver.empty()) return false;
        for (const TrigCtl& c : t.ctl) {
            auto it = g_itemGiver.find(c.uid);
            if (it == g_itemGiver.end()) continue;
            for (int w : wantedItems) if (w == it->second) return true;
        }
        return false;
    };
    // The taps kept above only for feeding SOME item, and whose item no coin's
    // gate reads after all. (Not under --worldgates, which keeps every gate.)
    if (!g_worldGates)
        out.erase(std::remove_if(out.begin(), out.end(),
                                 [&](const TouchTrig& t) {
                                     // (--tapdisarm: not one kept for the triggers it switches off)
                                     const bool off = std::any_of(
                                         t.trigTog.begin(), t.trigTog.end(),
                                         [](const TouchTrig::TogEv& e) { return e.on == 0; });
                                     return t.tap && !t.reachesCoin && !feedsACoin(t) && !off;
                                 }),
                  out.end());
    std::vector<TouchTrig> counts;
    if ((int)out.size() > kTouchBits) {
        std::vector<TouchTrig> boxes;
        for (TouchTrig& t : out) {
            // ...and a TAP trigger with it, for the same reason: the tap that
            // fires it can come from anywhere past its x.
            if (t.count >= 0 || t.tap || feedsACoin(t))
                counts.push_back(std::move(t));
            else boxes.push_back(std::move(t));
        }
        out.swap(boxes);
    }
    if ((int)(out.size() + counts.size()) > kTouchBits) {
        const size_t keep = (size_t)kTouchBits
                          - std::min<size_t>(counts.size(), (size_t)kTouchBits - 1);
        size_t first = 0;
        if (g_trigWinNear > -1e17) {
            // World-x window: the nearest kTouchBits boxes to a point on a line are
            // a contiguous run of the cx-sorted list -- the run whose farther end
            // is closest to the point.
            double best = 1e300;
            for (size_t f = 0; f + (size_t)kTouchBits <= out.size(); ++f) {
                const double reach = std::max(
                    std::fabs(out[f].cx - g_trigWinNear),
                    std::fabs(out[f + (size_t)kTouchBits - 1].cx - g_trigWinNear));
                if (reach < best) { best = reach; first = f; }
            }
            std::printf("triggers: the rotated-frame window keeps the %d boxes nearest world "
                        "x=%.0f (within %.0f px)\n", kTouchBits, g_trigWinNear, best);
        } else if (fromX > -1e17) {
            // 200 px of slack behind the anchor: a box the player is standing
            // in when the tail is anchored still has to be enterable.
            const double lo = fromX - 200.0;
            while (first < out.size() && out[first].cx < lo) ++first;
            // --touchwinahead: never start inside a run of boxes that move the same objects. A box
            // dropped behind loses its offset while its neighbour's is kept, and a formula-driven
            // object then sits off by the dropped move -- a platform moved -12 by one box and +12 by
            // an autonomous Move sat 12 px high and killed one tick past the anchor. Walk back to
            // the head of the run; the window gives up boxes at its far end instead.
            if (g_touchWinAhead && first > 0 && first < out.size()) {
                const size_t was = first;
                while (first > 0) {
                    std::unordered_set<int> moved;
                    for (const TrigCtl& c : out[first].ctl) moved.insert(c.uid);
                    bool shares = false;
                    for (const TrigCtl& c : out[first - 1].ctl)
                        if (moved.count(c.uid)) { shares = true; break; }
                    if (!shares) break;
                    --first;
                }
                if (first != was)
                    std::printf("triggers: --touchwinahead starts the window %zu box(es) back, at "
                                "x=%.0f, so it does not cut a run that moves the same objects\n",
                                was - first, out[first].cx);
            }
            // Never leave fewer than the window in hand (an anchor near the end
            // of the level would otherwise keep only a handful).
            if (out.size() - first < keep) first = out.size() - keep;
        }
        const size_t total = out.size();
        if (first + keep < total) g_trigFirstAheadX = out[first + keep].cx;
        out = std::vector<TouchTrig>(out.begin() + (long long)first,
                                     out.begin() + (long long)(first + keep));
        // The two sides, published and not only printed -- see the globals.
        g_trigDroppedBehind = first;
        g_trigDroppedAhead = total - first - keep;
        std::printf("triggers: %zu touch triggers, keeping %zu from x=%.0f "
                    "(dropped %zu behind, %zu ahead)\n",
                    total, keep, out.front().cx,
                    g_trigDroppedBehind, g_trigDroppedAhead);
    }
    // ...and back in, at the end, so the boxes keep the numbering they had.
    for (TouchTrig& t : counts) out.push_back(std::move(t));
    // COVERAGE, on one machine-readable line, always -- including the happy
    // case. A consumer that only hears about trouble cannot tell "covered" from
    // "this build does not report it", which is how 120 dropped boxes went
    // unnoticed for as long as they did.
    g_trigKept = out.size();
    g_trigDroppedRelevant = g_trigRelevantN - g_trigKept;
    g_trigMaxKeptX = out.empty() ? 0.0 : out.back().cx;
    // UNCOVERED is AHEAD only. A windowed anchor drops the boxes behind it on
    // purpose -- their world is already in the recording -- so labelling those
    // "uncovered" would wave a flag at the normal case and blunt the flag that
    // matters. Behind-drops are still reported, under
    // their own name, because "reported" and "a warning" are different things.
    std::printf("triggers: coverage total=%zu relevant=%zu kept=%zu "
                "droppedRelevant=%zu droppedBehind=%zu droppedAhead=%zu "
                "maxKeptX=%.0f%s\n",
                g_trigTotal, g_trigRelevantN, g_trigKept,
                g_trigDroppedRelevant, g_trigDroppedBehind, g_trigDroppedAhead,
                g_trigMaxKeptX,
                g_trigDroppedAhead ? "  UNCOVERED"
                                   : (g_trigDroppedBehind ? "  behind-only" : ""));
    // The uid travels with the box because bit numbering is a property of THIS
    // window (see the anchor payload): without it there is no way to say which
    // object a State::trig bit stands for, and the payload's uid->bit mapping
    // cannot be checked against what GD reports activating.
    for (size_t b = 0; b < out.size(); ++b) {
        if (out[b].count >= 0 && out[b].cmode == 3)
            std::printf("triggers: bit %zu uid %d ITEM COMPARE item %d at least %d,"
                        " moves %zu objects\n",
                        b, out[b].uid, out[b].item, out[b].count, out[b].ctl.size());
        else if (out[b].count >= 0)
            std::printf("triggers: bit %zu uid %d COUNT item %d %s %d, moves %zu objects\n",
                        b, out[b].uid, out[b].item,
                        out[b].cmode == 1 ? ">=" : out[b].cmode == 2 ? "<=" : "==",
                        out[b].count, out[b].ctl.size());
        else if (out[b].tap && out[b].tapCloseX < 1e17)
            std::printf("triggers: box %zu uid %d TAP at x=%.0f chan %d, counts presses"
                        " until the Stop at x=%.0f chan %d, moves %zu objects\n",
                        b, out[b].uid, out[b].cx, out[b].tapChan, out[b].tapCloseX,
                        out[b].tapCloseChan, out[b].ctl.size());
        else
            std::printf("triggers: box %zu uid %d (%.0f,%.0f) %.0fx%.0f moves %zu objects\n",
                        b, out[b].uid, out[b].cx, out[b].cy, out[b].hw * 2,
                        out[b].hh * 2, out[b].ctl.size());
    }
    // Each spawned root is armed by the box(es) whose chain spawns it.
    for (size_t b = 0; b < out.size() && b < (size_t)kTouchBits; ++b)
            for (const int u : out[b].spawnsRoots)
                for (size_t r = 0; r < out.size() && r < (size_t)kTouchBits; ++r)
                    if (out[r].uid == u) {
                        out[r].armBy |= touchBit((int)b);
                        std::printf("triggers: bit %zu uid %d is armed by box %zu uid %d "
                                    "(spawned by its chain)\n", r, u, b, out[b].uid);
                    }
    // the ownTouch proximity gate's table (see g_touchBoxU)
    for (size_t b = 0; b < (size_t)kTouchBits && b < out.size(); ++b)
        g_touchBoxU[b] = (float)out[b].cx;
    return out;
}

// --tptrigspawn / --gravchain (modifiers.hpp): where an x crossing fires a SPAWNED trigger. The roots are the
// x-crossing Spawns (1268) and the Toggles that turn their group on (1049, togon 1; toggleGroupTriggered spawns
// the group it turns on) -- touch 0, spawn 0, channel 0. The walk goes on through the spawn-triggered rows of
// the same two kinds and stops at anything else: what a touch, a count, an item compare or a tap fires is not
// a function of x. A hit carries the root and the chain's delay in seconds: each Spawn's own, and for an
// ordered Spawn (sordd) the hit's (x - xFirst) / 311.58 s behind the group's first spawnable trigger
// (spawnObjectsInOrder 0x21ae00, the --spawnlag measurement). `want` names the spawned triggers sought, each
// with its own x (a gravity trigger has no triggers.txt row). One (uid, root, delay) is kept once.
// With `touchRoots` the roots are the touch-triggered Spawns and Toggles-on instead (--tptouch / --gravtouch),
// and a hit carries the root's box as well.
struct SpawnFire {
    int uid;
    int rootUid;
    double rootX;
    double delayS;
    double rootY = 0.0, rootHw = 0.0, rootHh = 0.0;
};
// The walk from given roots (spawnFiresOf picks them; --eventtrig hands it an Event trigger, whose target group
// gameEventTriggered spawns with no delay). A root that is not a Spawn starts with no delay of its own.
inline std::vector<SpawnFire> spawnFiresFromRoots(const std::vector<const TrigRow*>& roots,
                                                  const std::unordered_map<int, double>& want,
                                                  const std::unordered_map<int, TrigRow>& trig,
                                                  const std::unordered_map<int, std::vector<int>>& byGroup);
inline std::vector<SpawnFire> spawnFiresOf(const std::unordered_map<int, double>& want,
                                           const std::unordered_map<int, TrigRow>& trig,
                                           const std::unordered_map<int, std::vector<int>>& byGroup,
                                           bool touchRoots = false) {
    auto spawner = [](const TrigRow& r) { return r.target != 0 && (r.id == 1268 || (r.id == 1049 && r.togon == 1)); };
    std::vector<const TrigRow*> roots;
    for (const auto& kv : trig)
        if (spawner(kv.second) && !kv.second.spawn
            && (touchRoots ? kv.second.touch != 0 : (!kv.second.touch && kv.second.chan == 0)))
            roots.push_back(&kv.second);
    std::sort(roots.begin(), roots.end(), [](const TrigRow* a, const TrigRow* b) { return a->uid < b->uid; });
    return spawnFiresFromRoots(roots, want, trig, byGroup);
}
inline std::vector<SpawnFire> spawnFiresFromRoots(const std::vector<const TrigRow*>& roots,
                                                  const std::unordered_map<int, double>& want,
                                                  const std::unordered_map<int, TrigRow>& trig,
                                                  const std::unordered_map<int, std::vector<int>>& byGroup) {
    std::vector<SpawnFire> out;
    auto spawner = [](const TrigRow& r) { return r.target != 0 && (r.id == 1268 || (r.id == 1049 && r.togon == 1)); };
    struct Item {
        int group;
        double delayS;
        bool ordered;
    };
    for (const TrigRow* R : roots) {
        std::vector<Item> stack{{R->target, R->id == 1268 ? R->sdelay : 0.0, R->id == 1268 && R->sordd != 0}};
        int guard = 0;
        while (!stack.empty() && guard++ < 4096) {
            const Item it = stack.back();
            stack.pop_back();
            const auto g = byGroup.find(it.group);
            if (g == byGroup.end()) continue;
            double xFirst = 1e18;
            if (it.ordered)
                for (const int ou : g->second) {
                    const auto w = want.find(ou);
                    if (w != want.end()) xFirst = std::min(xFirst, w->second);
                    const auto tr = trig.find(ou);
                    if (tr != trig.end() && tr->second.spawn) xFirst = std::min(xFirst, tr->second.cx);
                }
            for (const int ou : g->second) {
                const auto w = want.find(ou);
                const auto tr = trig.find(ou);
                const double ownX = w != want.end() ? w->second : tr != trig.end() ? tr->second.cx : 0.0;
                const double d = it.delayS + ((it.ordered && xFirst < 1e17)
                                                  ? std::max(0.0, (ownX - xFirst) / 311.5801086425781) : 0.0);
                if (w != want.end()) {
                    bool dup = false;
                    for (const SpawnFire& f : out)
                        dup = dup || (f.uid == ou && f.rootUid == R->uid && std::fabs(f.delayS - d) < 1e-9);
                    if (!dup) out.push_back({ou, R->uid, R->cx, d, R->cy, R->w * 0.5, R->h * 0.5});
                    continue;
                }
                if (tr == trig.end()) continue;
                const TrigRow& S = tr->second;
                if (!S.spawn || S.touch || !spawner(S)) continue;
                stack.push_back({S.target, d + (S.id == 1268 ? S.sdelay : 0.0), S.id == 1268 && S.sordd != 0});
            }
        }
    }
    return out;
}

// --chantog: the rotate-gameplay triggers (2900) of the level, {channel, x}, read from rotgameplay.txt before the autonomous triggers are built.
inline std::vector<std::pair<int, double>> g_chanSwitch;
inline void readChanSwitchers(const std::string& path) {
    g_chanSwitch.clear();
    std::ifstream in(path);
    if (!in) return;
    std::string line;
    std::getline(in, line);   // header
    while (std::getline(in, line)) {
        int uid = 0, id = 0, chan = 0;
        double px = 0.0, py = 0.0;
        if (std::sscanf(line.c_str(), "%d,%d,%lf,%lf,%d", &uid, &id, &px, &py, &chan) == 5 && id == 2900)
            g_chanSwitch.push_back({chan, px});
    }
}
// A Toggle on channel c that the player passes before some switch of an earlier channel: the channel is not the active one yet at that
// first crossing, so the crossing does not fire it. One whose earlier channels all lie behind it can be the active one and fires as before.
inline bool chanNotYetActive(const TrigRow& T) {
    if (T.chan == 0) return false;
    for (const std::pair<int, double>& s : g_chanSwitch)
        if (s.first < T.chan && s.second > T.cx) return true;
    return false;
}

// The autonomous counterpart (see AutoTrig): move (901) and toggle (1049) rows
// with touch=0 / spawn=0, chains walked exactly like the touch loader's. The
// root row itself is the actor, so the walk is seeded with the root's own
// offset and duration (a hop's duration only counts when that hop moves --
// same rule as below); a toggle carries no offset and contributes nothing but
// its timing, which is the whole point -- the `on` flag comes from the
// recording and only its PHASE was ever wrong.
// Rotate (1346) roots are NOT left out -- see the second loop at the bottom of
// this function, which anchors them with the same delay of 1. (This comment used
// to say "nothing measured yet"; it was already stale when the retiming landed
// on 2026-08-10, and a plan was written off it in 2026-08-28 before the code
// below was read.) The delay is now measured level-wide rather than at one site:
// replaying lv21's plan with grouptrace on puts the recording and the player
// trace on ONE clock, and 106 of 107 (trigger, object) pairs put the first
// turned tick at exactly crossing+2 -- i.e. fire at crossing+1, then one tick
// before the motion shows -- against a Move control of +2 x181 in the same run.
// The phase is settled; what is NOT is an object carrying a move AND a rotation
// (see the note over g_rotated at the bottom).
// lv1-18 dump no triggers at all, so this is inert everywhere the suite is
// green; lv19 has one autonomous toggle and lv20 has fifteen.
inline std::vector<AutoTrig> loadAutoTriggers(const std::string& trigPath,
                                              const std::string& grpPath) {
    std::vector<AutoTrig> out;
    const std::unordered_map<int, TrigRow>* trigP = trigRowsCached(trigPath);
    if (!trigP) return out;
    const std::unordered_map<int, TrigRow>& trig = *trigP;   // uid -> row
    const std::unordered_map<int, std::vector<int>>* membersP = groupMembersCached(grpPath);
    if (!membersP) return out;
    const std::unordered_map<int, std::vector<int>>& byGroup = *membersP;   // read only here
    for (const auto& kv : trig) {
        const TrigRow& T = kv.second;
        if (T.touch || T.spawn || T.target == 0) continue;
        if (T.id != 901 && T.id != 1049) continue;
        // --chantog: a channelled Toggle is not fired by the crossing (the --chantog note in speed.hpp). Left in, its anchor re-times the
        // recorded on/off of everything it reaches to ITS crossing -- a group that a later, unchannelled Toggle switches.
        if (!g_chanMove && T.id == 1049 && chanNotYetActive(T)) continue;   // --chanmove reads the channel per tick instead (AutoTrig::chan)
        // g_offMoves: a Move switched off when the player reaches it does not fire (see
        // offAtCrossing); a box that switches it back on moves its group (loadTouchTriggers).
        // Only a Move: an autonomous Toggle's on/off is read from the recording, which is what GD
        // did, while a Move is placed from the row's own offset.
        bool offInit = false;
        if (T.id == 901) {
            bool spawnOn = false;
            if (offAtCrossing(T, trig, byGroup, &spawnOn) && !spawnOn) {
                // --boxarm: ...unless a toggle block switches its group back on; then it is kept, off at its
                // crossing until a box of the state has fired (dynamics.hpp autoFireT).
                if (boxArmsTrigger(T.uid, trig, byGroup)) {
                    offInit = true;
                    std::printf("autotrig: uid %d (move, x=%.0f) is switched off when the player reaches it "
                                "- kept, a toggle block switches it back on (--boxarm)\n", T.uid, T.cx);
                } else {
                    std::printf("autotrig: uid %d (move, x=%.0f) is switched off when the player "
                                "reaches it - it moves nothing on its own\n", T.uid, T.cx);
                    continue;
                }
            }
        }
        AutoTrig at;
        at.offInit = offInit ? 1 : 0;
        at.uid = T.uid;
        at.cx = T.cx;
        at.cy = T.cy;
        if (g_chanMove && (T.id == 901 || T.id == 1049)) {   // --chanmove (speed.hpp)
            at.chan = T.chan;
            at.chanStatic = T.id == 1049 && chanNotYetActive(T);
        }
        at.delay = (T.id == 1049) ? 0 : 1;
        at.toggleRoot = (T.id == 1049) ? 1 : 0;
        // The root's own switch, when the root is a Toggle (see AutoTrig::tog).
        if (T.id == 1049 && T.togon >= 0) {
            const auto gi = byGroup.find(T.target);
            if (gi != byGroup.end())
                for (const int ou : gi->second)
                    if (!trig.count(ou)) at.tog.push_back({ou, (uint8_t)(T.togon == 1), 0.0, T.target});
        }
        struct Item {
            int group; float dx, dy; double dur; int ease; double erate;
            double lock, lockY;
            std::vector<std::pair<int, int>> remap;   // as in the touch walk
            double sdelay = 0.0;   // the spawn delays so far, in ticks (AutoTrig::tog only)
            double lmx = 1.0;      // m_moveModX of the Move that opened the lock (--lockmod)
        };
        const bool rootMoves = (T.ox != 0.0 || T.oy != 0.0);
        std::vector<Item> stack{{T.target, (float)T.ox, (float)T.oy,
                                 rootMoves ? T.dur * 240.0 : 0.0,
                                 rootMoves ? T.ease : 0,
                                 rootMoves ? T.erate : 2.0,
                                 lockWindowTicks(T.lockx, T.dur),
                                 lockWindowTicks(T.locky, T.dur)}};
        if (T.lockx) stack.back().lmx = T.moveModX;
        int guard = 0;
        while (!stack.empty() && guard++ < 4096) {
            const Item it = stack.back();
            stack.pop_back();
            const auto g = byGroup.find(it.group);
            if (g == byGroup.end()) continue;
            for (const int uid : g->second) {
                const auto t2 = trig.find(uid);
                if (t2 != trig.end()) {
                    if (t2->second.target == 0) continue;
                    // Same rule as the touch walk, which this one lacked
                    // entirely: every trigger sitting in a group was followed
                    // whatever fires it. lv19's uid13827 is an id-1049 TOGGLE
                    // whose target group 101 contains the Move uid14066, so the
                    // walk entered it and carried uid14066's -75 into group 96
                    // a second time -- ady -150 against a -75 trigger, and the
                    // anchor taken from the Toggle at cx=28,305 instead of the
                    // Move at 28,862, 557 px early.
                    if (!t2->second.spawn) continue;
                    // --movenospawn: ...and a MOVE root spawns nothing at all. It carries the
                    // triggers in its target group as objects; a spawn-triggered one among them
                    // still waits for a Spawn or a box. A custom level's Moves 5425 (+270,+135)
                    // and 5442 (+60,-150) target group 172, which holds the spawn=1 Toggle 14792
                    // that switches group 171 (17 pads) on; the walk carried both offsets into
                    // group 171 (adx +750/-75). GD's own recording of that group ends at
                    // (6,334.5, 122.325) from (5,914.5, 182.325) -- +420/-60, the two Moves that
                    // target 171 itself (5427, 5446) and nothing else.
                    if (T.id == 901) continue;
                    if (visualOnly(t2->second.id)) continue;   // --touchvisual
                    const double dl = it.sdelay + chainDelayS(t2->second) * 240.0;
                    // A Toggle spawned in the chain: its switch, after the delays so far.
                    if (t2->second.id == 1049 && t2->second.togon >= 0) {
                        const int tg = remapGroup(it.remap, t2->second.target);
                        const auto gi = byGroup.find(tg);
                        if (gi != byGroup.end())
                            for (const int ou : gi->second)
                                if (!trig.count(ou))
                                    at.tog.push_back({ou, (uint8_t)(t2->second.togon == 1), dl, tg});
                    }
                    const bool moves = (t2->second.ox != 0.0 || t2->second.oy != 0.0);
                    const double d2 = t2->second.dur * 240.0;
                    const bool longer = moves && d2 >= it.dur;
                    const double lmxNext = (lockWindowTicks(t2->second.lockx, t2->second.dur) > it.lock) ? t2->second.moveModX : it.lmx;
                    stack.push_back({remapGroup(it.remap, t2->second.target),
                                     it.dx + (float)t2->second.ox,
                                     it.dy + (float)t2->second.oy,
                                     longer ? d2 : it.dur,
                                     longer ? t2->second.ease : it.ease,
                                     longer ? t2->second.erate : it.erate,
                                     std::max(it.lock, lockWindowTicks(t2->second.lockx, t2->second.dur)),
                                     std::max(it.lockY, lockWindowTicks(t2->second.locky, t2->second.dur)),
                                     g_spawnRemap
                                         ? composeRemap(t2->second.remap, it.remap)
                                         : std::vector<std::pair<int, int>>{},
                                     dl, lmxNext});
                } else {
                    at.ctl.push_back({uid, it.dx, it.dy, it.dur, it.ease,
                                      it.erate, it.lock, it.lockY});
                    if (it.lock > 0.0) at.lockModX = it.lmx;   // --lockmod
                }
            }
        }
        // --touchoff (g_touchOwnOff): ...and a Toggle that switches nothing the model places,
        // but does switch a touch box's own trigger. Its crossing tick (fireT) is what
        // markTouched reads for that box.
        bool keepForTouch = false;
        if (g_touchOwnOff && at.ctl.empty() && T.id == 1049 && T.togon >= 0) {
            const auto gi = byGroup.find(T.target);
            if (gi != byGroup.end())
                for (const int ou : gi->second) {
                    const auto t2 = trig.find(ou);
                    if (t2 != trig.end() && t2->second.touch) {
                        keepForTouch = true;
                        break;
                    }
                }
        }
        if (!at.ctl.empty() || keepForTouch) out.push_back(std::move(at));
    }
    // ...and the uids some ROTATE turns, by the same walk. Still not placed by
    // the model -- an object a rotation carries cannot be described by offsets,
    // which is what g_rotated vetoes (autoClosed is forced to 0 for them).
    //
    // But they DO get an anchor now. Placement and RE-TIMING are two different
    // things: the recording's shape is right, only its phase is tied to the run
    // that recorded it, and an autonomous trigger's phase is a pure function of
    // the crossing. Leaving these unanchored replays the recording at raw tick
    // indices, so the object sits wherever the recording run happened to put it.
    //
    // The note above this function said Rotate was left out because "nothing
    // measured yet, and a wrong retiming is worse than the recording as-is".
    // Measured now (2026-08-10), on lv21's wall at x=18,705:
    //   killer  uid=18432 id=1582, position (18093,107) but rect (18231.5,186.8)
    //   GD      t=14,040  rect centre (18241.815, 197.29)  20.55 x 20.94
    //   model   t=14,040  recording  (18245.605, 207.264)  19.715 x 20.167
    //   model   t=14,047  recording  (18241.814, 197.291)  20.547 x 20.944  <-- =GD
    // i.e. exactly 7 ticks late. uid 18432 is in group 220, turned by the
    // autonomous Rotate uid=17974 at cx=17,595; the recording run crossed that
    // at t=13,557 (first motion 13,558, minus the delay) and this run crosses at
    // t=13,550. 13,557 - 13,550 = 7. The delay is 1, the same as MOVE.
    //
    // The ctl entries carry ZERO offset, so nothing downstream places anything
    // from them: `parts` skips them (|dx|>0.001), adx/ady stay 0, and autoClosed
    // is already vetoed. All they do is give the uid an aAnchor.
    //
    // WHAT ONE ANCHOR CANNOT DO (measured 2026-08-28, lv21). Replaying the plan
    // with grouptrace on gives GD's own answer for every recorded object, and the
    // base recording shifted by the anchor can be held against it directly. Split
    // by what drives the object, over lv21's 106 recorded rotated objects:
    //
    //   1 rotate + 0 move   46 reproduce to <=0.01 px, 0 fail
    //   1 rotate + 1 move    9 reproduce, 12 fail
    //   1 rotate + 2 move   20 reproduce, 19 fail
    //
    // A rigid rotation IS a pure time shift, so the first row is exact by
    // construction and confirms the delay. The failures are structural, not a
    // tuning error: a move and a rotation cross at DIFFERENT x (uid 15877 turns
    // from cx=15,372.7 but is moved from 15,224.7 and 15,432.7), so their phases
    // shift by different amounts and NO single shift carries both. Searching
    // every shift leaves 1.65-2.18 px on the table. Over the 31 objects and
    // 16,849 object-ticks: median 0.208 px, p90 0.591, p99 1.858, max 2.176 --
    // on id-1582 hazards whose kill radius is 4 px.
    //
    // Those 31 are also where the deaths are. Hazard deaths per near-path hazard
    // of the same class (lv19-22, exposure hazard-matched): rotate+move 79.8x,
    // rotate-only 16.7x, move-only 1.63x, static 0.45x. Small n on the first
    // (13 hazards, 37 deaths), so read the ORDER, not the ratio.
    //
    // A ROTATION IS A CLOSED FORM TOO -- measured the same day, so the recording
    // is not needed for either half:
    //
    //   pos(t) = C(t) + Rot(theta(t - t0)) * (entry - C(0))
    //   theta  = degrees * gdEase(ease, erate, (t - t0) / (dur * 240))
    //   t0     = the player's crossing of the trigger + 1        (106/107 above)
    //   C      = the object in the `center` group -- ALWAYS EXACTLY ONE, and its
    //            dumped position is the recovered orbit centre to 0.00 px
    //
    // Fitting `degrees` against the recording over lv21's autonomous rotates:
    // every object with no move controller has a constant radius, lands on a
    // round figure (-720, +720, 580, -720, -720) and reproduces to 0.003-0.013
    // px. The three that failed at 181 px are rotations about a MOVING centre --
    // their radius about the dumped centre swings 8.8..164.1, but about the
    // recorded centre it is 60.000..60.005, spread 0.005 px. So the composition
    // is a rotation riding on the centre's own move, and both halves are already
    // machinery this file has.
    //
    // The ONE thing missing is `degrees`, which the trigger dumper does not emit
    // (solver.hpp: target/center/dur/ox/oy/ease/erate, and a rotate has ox=oy=0).
    // The bindings carry it as EffectGameObject::m_rotationDegrees plus
    // m_times360 -- checked in the generated header that also holds the
    // m_centerGroupID this dumper already uses, not guessed from a .bro.
    //
    // NOT DONE -- it rewrites the hot placement path, and "a wrong retiming is
    // worse than the recording as-is" still holds until it is proven on the
    // suite.
    g_rotated.clear();
    g_rotSpec.clear();
    g_rotTailParts.clear();
    for (const auto& kv : trig) {
        const TrigRow& T = kv.second;
        if (T.id != 1346 || T.target == 0) continue;
        AutoTrig rot;
        rot.uid = T.uid;
        rot.cx = T.cx;
        rot.delay = 1;            // measured, see above
        // The centre group must hold EXACTLY ONE object for the orbit to have a
        // pivot at all. Measured: it always does where a rotate carries a group
        // (lv21 g101={15390}, g104, g110; lv22 g47/g48/g49), and where it does
        // not the group id is 0 -- a rotate about the object's own position,
        // which moves nothing and needs no spec.
        int centre = 0;
        if (T.center != 0) {
            const auto cg = byGroup.find(T.center);
            if (cg != byGroup.end() && cg->second.size() == 1)
                centre = cg->second.front();
        }
        // --rotparent: the tail's centre is GD's (the dump's ctrobj); none found = each object turns about itself.
        // The orbit spec (g_rotSpec) keeps the one-member rule.
        int tailCentre = centre;
        bool tailOwn = T.center == 0;
        if (g_rotParent && T.ctrCol && T.center != 0) {
            tailCentre = T.ctrObj > 0 ? T.ctrObj : 0;
            tailOwn = T.ctrObj <= 0;
        }
        RotSpec spec;
        spec.trigUid = T.uid;
        spec.centreUid = centre;
        spec.total = (double)T.t360 * 360.0 + T.deg;
        spec.durT = T.dur * 240.0;
        spec.ease = T.ease;
        spec.erate = T.erate;
        spec.lockrot = T.lockrot;
        rot.rotTotal = spec.total;
        rot.rotDurT = spec.durT;
        rot.rotEase = spec.ease;
        rot.rotErate = spec.erate;
        rot.rotLock = T.lockrot ? 1 : 0;
        rot.rotAngleOk = T.hasAngle ? 1 : 0;
        // No angle means the dump predates t360, and `deg` alone would be the
        // remainder of a turn -- silently wrong rather than absent. Refuse the
        // spec instead, so the recording stays in charge for that object.
        const bool usable = T.hasAngle && centre != 0 && spec.durT > 0.0;
        const bool autonomous = (!T.touch && !T.spawn);
        std::vector<int> stack{T.target};
        int guard = 0;
        while (!stack.empty() && guard++ < 4096) {
            const int g = stack.back();
            stack.pop_back();
            const auto it = byGroup.find(g);
            if (it == byGroup.end()) continue;
            for (const int uid : it->second) {
                const auto t2 = trig.find(uid);
                if (t2 != trig.end() && t2->second.target != 0) {
                    // --rotnospawn: the Rotate turns this trigger and fires nothing (dynamics.hpp)
                } else {
                    g_rotated.insert(uid);
                    // The pivot is not carried by its own rotation, so a spec
                    // for it would place it on top of itself.
                    if (usable && uid != centre) g_rotSpec[uid] = spec;
                    // --rottail: an autonomous Rotate with a known angle about a single centre object,
                    // or about each object's own position (no centre group)
                    if (autonomous && T.hasAngle && uid != tailCentre && (tailOwn || tailCentre != 0))
                        g_rotTailParts[uid].push_back({T.uid, tailCentre, spec.total, spec.durT, spec.erate,
                                                      spec.ease, (uint8_t)(T.lockrot ? 1 : 0), T.cx});
                    if (autonomous) {
                        TrigCtl c{};
                        c.uid = uid;
                        // any centre group, the target's own included (--rotparent: one GD finds an object for)
                        c.orbit = (!tailOwn && uid != tailCentre) ? 1 : 0;
                        rot.ctl.push_back(c);
                    }
                }
            }
        }
        if (autonomous && !rot.ctl.empty()) out.push_back(std::move(rot));
    }
    // ...in crossing order, so Rotates about different centres turn the object in the order GD fires them
    // (the trigger map's own order is a hash's)
    for (auto& kv : g_rotTailParts)
        std::stable_sort(kv.second.begin(), kv.second.end(),
                         [](const RotTailPart& a, const RotTailPart& b) {
                             return a.trigCx != b.trigCx ? a.trigCx < b.trigCx : a.trigUid < b.trigUid;
                         });
    // --autostop: an x-crossing Stop is an entry of its own, so its crossing is resolved the way
    // any root's is (delay 0: fireT is the crossing tick), and it names the Moves of its target
    // group. Its pause/resume mode and control-ID option are not in the dump; every one is read
    // as a plain stop, and each is printed so a level that relies on the others can be told apart.
    if (g_autoStop)
        for (const auto& kv : trig) {
            const TrigRow& T = kv.second;
            if (T.id != 1616 || T.touch || T.spawn || T.target == 0) continue;
            const auto gi = byGroup.find(T.target);
            if (gi == byGroup.end()) continue;
            AutoTrig st;
            st.uid = T.uid;
            st.cx = T.cx;
            st.delay = 0;
            for (const int u : gi->second) {
                const auto t2 = trig.find(u);
                if (t2 != trig.end() && t2->second.id == 901) st.stopMoves.push_back(u);
            }
            std::printf("autostop: Stop uid %d at x=%.0f halts %zu Move(s) of group %d\n", T.uid,
                        T.cx, st.stopMoves.size(), T.target);
            if (!st.stopMoves.empty()) out.push_back(std::move(st));
        }
    std::sort(out.begin(), out.end(),
              [](const AutoTrig& a, const AutoTrig& b) { return a.cx < b.cx; });
    if (!out.empty())
        std::printf("autotrig: %zu autonomous moves (first x=%.0f, last x=%.0f),"
                    " %zu uids turned by a rotate\n",
                    out.size(), out.front().cx, out.back().cx, g_rotated.size());
    if (!g_rotated.empty())
        std::printf("rotspec: %zu of %zu turned uids have a computable orbit "
                    "(centre + whole angle)\n", g_rotSpec.size(),
                    g_rotated.size());
    // --stopdump: WHAT A Stop (id 1616) WOULD FREEZE, and when. Print only --
    // nothing here changes a position, exactly as --trigdump changed nothing
    // before the touch-box selection was written on top of what it counted.
    //
    // The model does not decode 1616 at all, so a group GD has parked keeps
    // running in the search (the replay is safe: it follows the recording).
    // Measured on lv22 before any of this was written: uid18096 stops two Moves
    // on group 265 whose commands are -360 and -480, and GD's own recording of
    // those 35 collidables ends after -122.4 -- cut short, not completed.
    //
    // Two shapes, and only the first can be a root here: a Stop with
    // touch=0 spawn=0 fires on an x crossing like any autonomous trigger, while
    // spawn=1 is fired by a Spawn chain this function never walks. The line
    // says which, rather than printing the first and staying quiet about the
    // second -- the witness above is a spawn=1 one.
    if (g_stopDump) {
        size_t n = 0;
        for (const auto& kv : trig) {
            const TrigRow& T = kv.second;
            if (T.id != 1616 || T.target == 0) continue;
            ++n;
            const char* how = T.touch ? "touch" : (T.spawn ? "SPAWN (not a root here)"
                                                           : "x-crossing");
            std::printf("stopdump: uid=%d at x=%.0f target=%d fired-by=%s\n",
                        T.uid, T.cx, T.target, how);
            const auto g = byGroup.find(T.target);
            if (g == byGroup.end()) { std::printf("stopdump:   (empty group)\n"); continue; }
            for (const int uid : g->second) {
                const auto t2 = trig.find(uid);
                if (t2 == trig.end()) continue;
                const TrigRow& S = t2->second;
                std::printf("stopdump:   stops uid=%d id=%d -> group %d "
                            "dur=%.3f ox=%.1f oy=%.1f fired-by=%s\n",
                            S.uid, S.id, S.target, S.dur, S.ox, S.oy,
                            S.touch ? "touch" : (S.spawn ? "spawn"
                                                         : "x-crossing"));
            }
        }
        std::printf("stopdump: %zu stop triggers in this level\n", n);
    }
    return out;
}

}  // namespace dp
