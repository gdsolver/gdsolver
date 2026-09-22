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
    // The chain reaches a coin (set at load for Count, Tap and Item Compare
    // roots; the window's feeder test needs it after the walk).
    bool reachesCoin = false;
    // --movetarget only: the Move triggers a Stop in this box's chain halts
    // (the trigger members of the Stop's target group), and the chain's spawn
    // delay to that Stop in ticks.
    std::vector<int> stops;
    double stopDelay = 0.0;
};
// The uids of this level's coins, filled by the CLI before the touch window is
// built (level_loader.hpp's coinUids). Empty = not known, and then the filter
// that uses it does nothing: a build that forgets to fill it keeps the old
// behaviour instead of silently dropping every counter.
inline std::unordered_set<int> g_coinUids;
// uid -> the item it gives, for EVERY row of items.txt and not only the ones
// the player can touch (g_collect keeps those). lv22's first coin is fed by
// Pickup triggers that a touch box spawns -- nothing touches them -- and the
// window has to know that those boxes are the ones it must not drop.
inline std::unordered_map<int, int> g_itemGiver;
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
        for (const TrigCtl& c : g_touch[b].ctl)
            d = std::max(d, c.durTicks);
        for (const TrigCtl& c : g_touch[b].ctl) d = std::max(d, c.lockTicks);
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
inline TouchMask g_trigReported = 0;
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
inline TouchMask g_needTrig = 0;
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
inline TouchMask g_needSkip = 0;
// The turned box is used instead of the bound (see Obj::oriented), since the
// rotation sign was fixed: lv18 goes from STUCK at x=27,713 (five sessions) to
// CLEARED cold in 9 iterations. Always on (--no-oriented is gone since the flag
// clean-up).
// --obb-all: apply the **2nd stage (oriented box vs oriented box)** of GD's
// hazard test to the modes other than wave as well. What the disassembly shows
// is "GJBaseGameLayer::checkCollisions's hazard loop has two tests:
//  (1) rect-vs-rect AABB (2) oriented box vs oriented box if m_shouldUseOuterOb",
// and that is not a per-mode matter. The only reason the model applies the 2nd
// stage to the wave alone is that that is where it was measured.
//
// **Nothing has been fixed yet.** The motive for adding it (at lv21 x=858 the
// frontier collapses 44->3, the model killing on a 0.6px AABB overlap with the
// rotated 4.12x2.66 dart uid236) was rejected: injecting the same conditions
// into GD showed that **GD also dies on the same tick**. Taking the sweep's
// "alive" at face value was the mistake; hitbox_sweep **advances only 1 tick**
// (it cannot see the next tick's death). When measuring a boundary, inject and
// run for tens of ticks.
//
// The flag itself stays. The "two-stage hazard test" the disassembly shows is
// not a per-mode matter, so the current form that applies it to the wave alone
// will be fixed eventually. But it is a change in the **under-killing
// direction**, so it was kept default OFF in a form that can be A/B'd.
//
// [2026-09-21] **ON by default** (audit AUD-20260921-12). The rejection above
// was not "the rule is false" -- it was "hitbox_sweep advances only 1 tick and
// cannot see the next tick's death". That failure mode does not reach the new
// witness: lv20 t=15,125, where the model kills on the AABB of a spike turned
// -312 degrees while GD's own reference carries both bodies 8,547 ticks past it
// to the end of the level. Over 22 whole-run replays this flag changes exactly
// one reading and only with --touchprey=button, which is what lets lv20 reach
// that spike at all -- it has no independent witness in the corpus, and that is
// recorded rather than glossed. `--no-obb-all` is the off arm.
inline bool g_obbAll = true;

// One row of the MOD's triggers.txt, shared by both loaders below.
// The trailing ease/erate/lock columns were added 2026-08-09; a dump written
// before that has 13 columns and still parses, with the easing defaulting to
// linear (which is what the model assumed anyway).
struct TrigRow {
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
        trig[r.uid] = r;
    }
    return true;
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
        if (item != 0) g_itemGiver[uid] = item;
        if (!pickup || item == 0) continue;
        g_collect.push_back({cx, cy, w * 0.5, h * 0.5, item, uid});
    }
    std::sort(g_collect.begin(), g_collect.end(),
              [](const Collectible& a, const Collectible& b) {
                  return a.cx < b.cx || (a.cx == b.cx && a.cy < b.cy);
              });
    return true;
}

// The MOD's two dumps, joined into "enter this box -> these uids move by this".
// Chains are followed through intermediate triggers (spawn -> move -> geometry)
// because that is how the levels are actually built.
// --touch-from-anchor: keep the 32 touch triggers AHEAD of the anchor instead
// of the first 32 in the level. Off by default -- see the note at the cap.
inline bool g_touchFromAnchor = false;
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
// ON since 2026-09-21 (audit AUD-20260921-20): reading the types from a
// placeholder was a known inconsistency, and --no-csvtypes, which reproduced
// it, is gone since the flag clean-up.
// --trigwinsel (default off): the auto-window gate probes the population it
// will actually load, instead of the unselected one. See the gate in cli.hpp.
// It exists because the selection went on by default (934c21e) and that gate
// was not updated with it; the two populations have disagreed since.
inline bool g_trigWinSel = false;
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
// reporting it as a pass or an improvement is reporting a different level
// (audit AUD-20260920-09). Filled by loadTouchTriggers; zero before it runs.
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
// the recording -- while only the ones ahead are a world this call cannot see
// (audit AUD-20260921-15). 0 / 0 when no window was cut.
inline size_t g_trigDroppedBehind = 0, g_trigDroppedAhead = 0;
inline double g_trigMaxKeptX = 0.0;

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
// "(in-process level)" (dp_bridge.cpp) and the level lives in g_levelCsv, so the
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
// The GD object types the player can be stopped or killed by. A chain that
// reaches none of these cannot change the collision geometry, whatever kind of
// trigger sits at its root -- which is what makes the test independent of the
// trigger ids, most of which are not decoded anywhere in this tree.
inline bool collidableType(int t) {
    return t == 0 || t == 2 || t == 21 || t == 47;
}
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
    std::unordered_map<int, TrigRow> trig;              // uid -> row
    std::unordered_map<int, std::vector<int>> byGroup;  // group id -> uids
    if (!loadTrigRows(trigPath, trig)) {
        std::fprintf(stderr, "triggers: cannot open %s\n", trigPath.c_str());
        return out;
    }
    {
        // "uid g1 g2 ..." -- space separated because one object can sit in up to
        // ten groups, and the join below needs every one of them.
        std::ifstream in(grpPath);
        if (!in) {
            std::fprintf(stderr, "objgroups: cannot open %s\n", grpPath.c_str());
            return out;
        }
        std::string line;
        std::getline(in, line);   // header
        while (std::getline(in, line)) {
            std::stringstream ss(line);
            int uid = 0;
            if (!(ss >> uid)) continue;
            int g = 0;
            while (ss >> g) byGroup[g].push_back(uid);
        }
    }
    for (const auto& kv : trig) {
        const TrigRow& T = kv.second;
        // A COUNT trigger (1611 instant / 1811) is a root here as well: its
        // chain is walked exactly like a box's, and it takes a bit in the same
        // mask. What differs is only who sets that bit (the step child's item
        // test rather than markTouched).
        const bool isCount = (T.id == 1611 || T.id == 1811) && T.count >= 0
                             && g_coinRoute;
        // ...and a TAP trigger (1595), armed by crossing and fired by a press.
        const bool isTap = (T.id == 1595) && g_coinRoute;
        // ...and an ITEM COMPARE (3620) in the one shape itemCompareGate reads.
        // Its chain is the TRUE branch (its target), and it fires the way a
        // Count does, from the counter; the chains that spawn it stop at it
        // (the walk below), so a tap alone no longer reaches what it guards.
        const int cmpNeed = g_coinRoute ? itemCompareGate(T) : 0;
        const bool isCmp = cmpNeed > 0;
        if ((!T.touch && !isCount && !isTap && !isCmp) || T.target == 0) continue;
        TouchTrig tt{T.cx, T.cy, T.w * 0.5, T.h * 0.5, {}};
        tt.uid = T.uid;   // so an outside payload can name the box (see the field)
        tt.id = T.id;
        tt.togOn = (T.id == 1049) ? T.togon : -1;   // --rotqtoggle (see the field)
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
        struct Item {
            int group; float dx, dy; double dur; int ease; double erate;
            double lock, lockY;
            // the remap the triggers in `group` act under (--spawnremap)
            std::vector<std::pair<int, int>> remap;
            // --movetarget only (see TrigCtl::mover / tmode / tdx / tdy)
            int mover = 0;
            uint8_t tmode = 0;
            float tdx = 0.f, tdy = 0.f;
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
        double maxDur = 0.0, maxLock = 0.0, maxOff = 0.0;
        std::vector<Item> stack{{T.target, (float)T.ox, (float)T.oy,
                                 rootMoves ? T.dur * 240.0 : 0.0,
                                 rootMoves ? T.ease : 0,
                                 rootMoves ? T.erate : 2.0,
                                 T.lockx ? T.dur * 240.0 : 0.0,
                                 T.locky ? T.dur * 240.0 : 0.0,
                                 g_spawnRemap ? T.remap
                                              : std::vector<std::pair<int, int>>{}}};
        stack[0].mover = rootMoves ? T.uid : 0;
        // A group can contain the trigger that targets it, so the walk needs a
        // hard bound rather than a visited set (the same group legitimately
        // appears twice under different offsets).
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
                    // --touch-from-anchor, so by default the walk followed
                    // autonomous Moves as well -- and lv19's group 96 is what
                    // that costs: the touch chain absorbed uid14066's -75 (an
                    // x-crossing Move) on top of uid13925's +36 and reported
                    // the NET, -39, over uid14066's duration. GD does neither:
                    // it raises the group 36 and then drops it 75, which the
                    // recording shows to the decimal (174.5 -> 210.5 -> 135.5).
                    if (!t2->second.spawn) continue;
                    // An Item Compare this build reads is a root of its own
                    // (see isCmp): what it spawns happens only when the counter
                    // says so. Walking through it credited lv22's tap box with
                    // switching the third coin on at the first press, where the
                    // game needs six.
                    if (g_coinRoute && itemCompareGate(t2->second) > 0) continue;
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
                                     std::max(it.lock, t2->second.lockx
                                         ? t2->second.dur * 240.0 : 0.0),
                                     std::max(it.lockY, t2->second.locky
                                         ? t2->second.dur * 240.0 : 0.0),
                                     g_spawnRemap
                                         ? composeRemap(t2->second.remap, it.remap)
                                         : std::vector<std::pair<int, int>>{}});
                    // --movetarget: the Move that set this hop's motion, and a
                    // target mode inherited from an earlier hop.
                    stack.back().mover = moves ? t2->second.uid : it.mover;
                    stack.back().tmode = it.tmode;
                    stack.back().tdx = it.tdx;
                    stack.back().tdy = it.tdy;
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
                }
            }
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
        if ((isCount || isTap || isCmp) && !T.touch && !g_coinUids.empty()) {
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
                    if (S.id != 1616 || S.spawn || S.touch || S.cx <= T.cx) continue;
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
        if (objTypes && !tt.ctl.empty() && !isCount && !isTap && !isCmp) {
            const bool moves = nInert < tt.ctl.size();
            if (!moves && hard <= 0 && tt.togOn < 0) {
                ++nIrrelevant;
                continue;
            }
        }
        if (!tt.ctl.empty()) out.push_back(std::move(tt));
    }
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
    std::sort(out.begin(), out.end(),
              [](const TouchTrig& a, const TouchTrig& b) { return a.cx < b.cx; });
    // one bit per trigger in the state's mask
    // NOTE: opt-in (`--touch-from-anchor`), and it is NOT ready. Measured on
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
    // gate reads after all.
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&](const TouchTrig& t) {
                                 return t.tap && !t.reachesCoin && !feedsACoin(t);
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
            // Never leave fewer than the window in hand (an anchor near the end
            // of the level would otherwise keep only a handful).
            if (out.size() - first < keep) first = out.size() - keep;
        }
        const size_t total = out.size();
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
    // matters (audit AUD-20260921-15). Behind-drops are still reported, under
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
    // the ownTouch proximity gate's table (see g_touchBoxU)
    for (size_t b = 0; b < (size_t)kTouchBits && b < out.size(); ++b)
        g_touchBoxU[b] = (float)out[b].cx;
    return out;
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
    std::unordered_map<int, TrigRow> trig;              // uid -> row
    std::unordered_map<int, std::vector<int>> byGroup;  // group id -> uids
    if (!loadTrigRows(trigPath, trig)) return out;
    {
        std::ifstream in(grpPath);
        if (!in) return out;
        std::string line;
        std::getline(in, line);   // header
        while (std::getline(in, line)) {
            std::stringstream ss(line);
            int uid = 0;
            if (!(ss >> uid)) continue;
            int g = 0;
            while (ss >> g) byGroup[g].push_back(uid);
        }
    }
    for (const auto& kv : trig) {
        const TrigRow& T = kv.second;
        if (T.touch || T.spawn || T.target == 0) continue;
        if (T.id != 901 && T.id != 1049) continue;
        AutoTrig at;
        at.uid = T.uid;
        at.cx = T.cx;
        at.delay = (T.id == 1049) ? 0 : 1;
        struct Item {
            int group; float dx, dy; double dur; int ease; double erate;
            double lock, lockY;
            std::vector<std::pair<int, int>> remap;   // as in the touch walk
        };
        const bool rootMoves = (T.ox != 0.0 || T.oy != 0.0);
        std::vector<Item> stack{{T.target, (float)T.ox, (float)T.oy,
                                 rootMoves ? T.dur * 240.0 : 0.0,
                                 rootMoves ? T.ease : 0,
                                 rootMoves ? T.erate : 2.0,
                                 T.lockx ? T.dur * 240.0 : 0.0,
                                 T.locky ? T.dur * 240.0 : 0.0}};
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
                    const bool moves = (t2->second.ox != 0.0 || t2->second.oy != 0.0);
                    const double d2 = t2->second.dur * 240.0;
                    const bool longer = moves && d2 >= it.dur;
                    stack.push_back({remapGroup(it.remap, t2->second.target),
                                     it.dx + (float)t2->second.ox,
                                     it.dy + (float)t2->second.oy,
                                     longer ? d2 : it.dur,
                                     longer ? t2->second.ease : it.ease,
                                     longer ? t2->second.erate : it.erate,
                                     std::max(it.lock, t2->second.lockx
                                         ? t2->second.dur * 240.0 : 0.0),
                                     std::max(it.lockY, t2->second.locky
                                         ? t2->second.dur * 240.0 : 0.0),
                                     g_spawnRemap
                                         ? composeRemap(t2->second.remap, it.remap)
                                         : std::vector<std::pair<int, int>>{}});
                } else {
                    at.ctl.push_back({uid, it.dx, it.dy, it.dur, it.ease,
                                      it.erate, it.lock, it.lockY});
                }
            }
        }
        if (!at.ctl.empty()) out.push_back(std::move(at));
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
    // suite. GDSOLVER_LAB/notes/measure-rotate-1346-2026-08-28.md.
    g_rotated.clear();
    g_rotSpec.clear();
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
        RotSpec spec;
        spec.trigUid = T.uid;
        spec.centreUid = centre;
        spec.total = (double)T.t360 * 360.0 + T.deg;
        spec.durT = T.dur * 240.0;
        spec.ease = T.ease;
        spec.erate = T.erate;
        spec.lockrot = T.lockrot;
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
                if (t2 != trig.end() && t2->second.target != 0)
                    stack.push_back(t2->second.target);
                else {
                    g_rotated.insert(uid);
                    // The pivot is not carried by its own rotation, so a spec
                    // for it would place it on top of itself.
                    if (usable && uid != centre) g_rotSpec[uid] = spec;
                    if (autonomous) {
                        TrigCtl c{};
                        c.uid = uid;
                        rot.ctl.push_back(c);
                    }
                }
            }
        }
        if (autonomous && !rot.ctl.empty()) out.push_back(std::move(rot));
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
