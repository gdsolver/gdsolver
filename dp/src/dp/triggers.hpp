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
struct TouchTrig {
    double cx, cy, hw, hh;          // the box the player has to enter
    std::vector<TrigCtl> ctl;
};
// Bit b of State::trig is g_touch[b]. Global because the step function, the
// layer loop and the witness resim all need the same numbering, and there is
// exactly one level in flight.
inline std::vector<TouchTrig> g_touch;
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
inline uint32_t g_trigReported = 0;
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
inline uint32_t g_needTrig = 0;
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
inline uint32_t g_needSkip = 0;
// Use the turned box instead of the bound (see Obj::oriented). ON by default
// since the rotation sign was fixed: lv18 goes from STUCK at x=27,713 (five
// sessions) to CLEARED cold in 9 iterations. `--no-oriented` restores the bound
// for A/B.
inline bool g_oriented = true;
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
// direction**, so it is kept default OFF in a form that can be A/B'd.
inline bool g_obbAll = false;

// One row of the MOD's triggers.txt, shared by both loaders below.
// The trailing ease/erate/lock columns were added 2026-08-09; a dump written
// before that has 13 columns and still parses, with the easing defaulting to
// linear (which is what the model assumed anyway).
struct TrigRow {
    int uid, id, target, center, touch, spawn;
    double cx, cy, w, h, dur, ox, oy;
    int ease = 0; double erate = 2.0;
    int lockx = 0, locky = 0;
};
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
            "%d,%d,%lf,%lf,%lf,%lf,%d,%d,%d,%d,%lf,%lf,%lf,%d,%lf,%d,%d",
            &r.uid, &r.id, &r.cx, &r.cy, &r.w, &r.h, &r.target, &r.center,
            &r.touch, &r.spawn, &r.dur, &r.ox, &r.oy, &r.ease, &r.erate,
            &r.lockx, &r.locky);
        if (n < 13) continue;
        if (n < 15) { r.ease = 0; r.erate = 2.0; }
        trig[r.uid] = r;
    }
    return true;
}

// The MOD's two dumps, joined into "enter this box -> these uids move by this".
// Chains are followed through intermediate triggers (spawn -> move -> geometry)
// because that is how the levels are actually built.
// --touch-from-anchor: keep the 32 touch triggers AHEAD of the anchor instead
// of the first 32 in the level. Off by default -- see the note at the cap.
inline bool g_touchFromAnchor = false;

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
inline std::vector<TouchTrig> loadTouchTriggers(const std::string& trigPath,
                                                const std::string& grpPath,
                                                double fromX = -1e18) {
    std::vector<TouchTrig> out;
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
        if (!T.touch || T.target == 0) continue;
        TouchTrig tt{T.cx, T.cy, T.w * 0.5, T.h * 0.5, {}};
        struct Item {
            int group; float dx, dy; double dur; int ease; double erate;
            double lock, lockY;
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
        const bool rootMoves = (T.ox != 0.0 || T.oy != 0.0);
        std::vector<Item> stack{{T.target, (float)T.ox, (float)T.oy,
                                 rootMoves ? T.dur * 240.0 : 0.0,
                                 rootMoves ? T.ease : 0,
                                 rootMoves ? T.erate : 2.0,
                                 T.lockx ? T.dur * 240.0 : 0.0,
                                 T.locky ? T.dur * 240.0 : 0.0}};
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
                    if (g_touchFromAnchor && !t2->second.spawn) continue;
                    // Duration belongs to the hop that actually MOVES something.
                    // The spawn trigger in front of lv19's door carries 0.5 s of
                    // its own, and adding that would stretch the "still opening"
                    // window past the tick the player arrives -- i.e. it would
                    // keep the door shut and undo the whole fix. Measured: the
                    // move starts 5 ticks after the box is touched, not 120.
                    // The easing rides along with the duration: it belongs to
                    // the same hop, and a curve read off a hop that does not
                    // move is meaningless.
                    const bool moves = (t2->second.ox != 0.0 || t2->second.oy != 0.0);
                    const double d2 = t2->second.dur * 240.0;
                    const bool longer = moves && d2 >= it.dur;
                    stack.push_back({t2->second.target,
                                     it.dx + (float)t2->second.ox,
                                     it.dy + (float)t2->second.oy,
                                     longer ? d2 : it.dur,
                                     longer ? t2->second.ease : it.ease,
                                     longer ? t2->second.erate : it.erate,
                                     std::max(it.lock, t2->second.lockx
                                         ? t2->second.dur * 240.0 : 0.0),
                                     std::max(it.lockY, t2->second.locky
                                         ? t2->second.dur * 240.0 : 0.0)});
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
                    tt.ctl.push_back({uid, it.dx, it.dy, it.dur, it.ease,
                                      it.erate, it.lock, it.lockY});
                }
            }
        }
        if (!tt.ctl.empty()) out.push_back(std::move(tt));
    }
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
    if (out.size() > 32) {
        size_t first = 0;
        if (fromX > -1e17) {
            // 200 px of slack behind the anchor: a box the player is standing
            // in when the tail is anchored still has to be enterable.
            const double lo = fromX - 200.0;
            while (first < out.size() && out[first].cx < lo) ++first;
            // Never leave fewer than 32 in hand (an anchor near the end of the
            // level would otherwise keep only a handful).
            if (out.size() - first < 32) first = out.size() - 32;
        }
        const size_t total = out.size();
        out = std::vector<TouchTrig>(out.begin() + (long long)first,
                                     out.begin() + (long long)first + 32);
        std::printf("triggers: %zu touch triggers, keeping 32 from x=%.0f "
                    "(dropped %zu behind, %zu ahead)\n",
                    total, out.front().cx, first, total - first - 32);
    }
    for (const TouchTrig& t : out)
        std::printf("triggers: box (%.0f,%.0f) %.0fx%.0f moves %zu objects\n",
                    t.cx, t.cy, t.hw * 2, t.hh * 2, t.ctl.size());
    // the ownTouch proximity gate's table (see g_touchBoxU)
    for (size_t b = 0; b < 32 && b < out.size(); ++b)
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
                    const bool moves = (t2->second.ox != 0.0 || t2->second.oy != 0.0);
                    const double d2 = t2->second.dur * 240.0;
                    const bool longer = moves && d2 >= it.dur;
                    stack.push_back({t2->second.target,
                                     it.dx + (float)t2->second.ox,
                                     it.dy + (float)t2->second.oy,
                                     longer ? d2 : it.dur,
                                     longer ? t2->second.ease : it.ease,
                                     longer ? t2->second.erate : it.erate,
                                     std::max(it.lock, t2->second.lockx
                                         ? t2->second.dur * 240.0 : 0.0),
                                     std::max(it.lockY, t2->second.locky
                                         ? t2->second.dur * 240.0 : 0.0)});
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
    for (const auto& kv : trig) {
        const TrigRow& T = kv.second;
        if (T.id != 1346 || T.target == 0) continue;
        AutoTrig rot;
        rot.uid = T.uid;
        rot.cx = T.cx;
        rot.delay = 1;            // measured, see above
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
    return out;
}

}  // namespace dp
