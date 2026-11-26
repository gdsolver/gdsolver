#pragma once
// The live part of what step.hpp held: the definitions the replay walker, the shadow step and the model still use after the old tick (stepOne, stepBoth) is gone.
// Moved out of step.hpp unchanged (t63_move.py): the nodes no old-only code owns, in their original order, with the macros they use.
#include "dp/slopes.hpp"
#include "dp/bornseat.hpp"
#include "dp/cam_y_obs.hpp"   // --camy (observation only)

namespace dp {

inline float dashSlopeOf(const Obj* ob, int frame);   // defined with the dash code below (--dashframe3 (removed) re-reads the slope here)
inline bool touchOwnOn(const TouchTrig& T, long long t, const State* st = nullptr);

// ---- WHICH LINE LAST WROTE c.vy ------------------------------------------
//
// Twice now a value read off a fixup record has been blamed on a site chosen
// for having the right shape, and twice the arithmetic killed it (the downhill
// release at :8255 and the robot jump at :3209). Choosing is the weak step, so
// stop choosing: let the writer name itself.
//
// AS AN LVALUE WRAPPER, NOT AN EXTRA STATEMENT. Appending `VYW();` after each
// assignment would silently change every braceless `if (cond) c.vy = X;` into a
// conditional assignment followed by an unconditional call -- and it would
// still compile, still leave c.vy correct, and still pass a byte-identical
// record comparison. C++ keeps the comma operator's right operand an lvalue, so
//     VYSET(c.vy) = expr;
// is the same single statement it always was, whatever surrounds it and however
// many lines the right-hand side spans.
//
// __LINE__ rather than a hand-kept id table: a table is a second copy of the
// truth and drifts from it. Line numbers are resolved by quoting the commit.
//
// THREAD_LOCAL, this and every other record below that a step writes. The
// search steps on every thread of its pool, and a global each of them wrote on
// every step was a cache line passed between cores all search long -- one
// which, depending only on where the linker put it, could also hold a
// read-mostly flag such as g_slopeDbg and slow every read of that too (the same
// source measured 15% apart between two builds). The one reader is the
// replay's per-tick line, which runs the step on its own thread, so it reads
// exactly what it read before.
inline thread_local int g_vyWriter = 0;      // __LINE__ of the last write this tick

inline thread_local int g_vyWrites = 0;      // how many writes this tick -- see below

inline long long g_vyWatchT = -1;   // --vywriter <t>: report only this tick

#define VYSET(x) (::dp::g_vyWriter = __LINE__, ++::dp::g_vyWrites, (x))

// ...and the same for c.y, because the two are written two lines apart from the
// SAME vpNew (:2637 and :2640) while a record reports edy = 0.000 with
// edvy = -6.3952. One value agreeing to the digit and its neighbour disagreeing
// by 6.4 cannot both come from that pair untouched -- so either y is overwritten
// afterwards by something else, or the reading is wrong. The y writer says
// which, and neither answer needs a guess about the terms.
inline thread_local int g_yWriter = 0;

inline thread_local int g_yWrites = 0;

// ...and the RAW values the step began with, so a report can show the sequence
// (in -> out) rather than only naming the last writer. With one write each the
// pair IS the sequence; with several it still bounds it.


// ---- the slope seat's exemption gate, recorded rather than printed ---------
//
// Printing at the gate itself named no state: it lives in the removed old tick's stepOne, which the
// search runs for many states at one tick, and at lv22 t=18,568 ninety-four of
// those carried the record's vy in three different flag combinations. Reading
// the first match would have picked one of three by luck.
//
// Recorded into globals instead and emitted from the replay's own per-tick
// line. That reporting point fires once per tick, and a --replay invocation
// does no searching, so the values it reads belong to the replayed state and
// to nothing else.





// ...and WHICH of the eight sites raised impulsedThisTick, since that flag is
// half of the exemption's condition and the comment justifying the exemption
// names a jump or a flip while the measured vy writer was the integrator.
// Eight candidates, and picking one is what failed twice, so the site says so
// itself -- as a comma expression, which keeps `impulsedThisTick = IMPULSE();`
// the single statement `impulsedThisTick = true;` already was.
inline thread_local int g_impulseSite = 0;

// ...with a count and the sites in order, since the flag only needs to be
// raised once and the FIRST site is the one that means something; the last one
// alone cannot say whether a second site raised it again.
inline thread_local int g_impTrail[8] = {};

inline thread_local int g_impCount = 0;

#define IMPULSE() (::dp::g_impTrail[::dp::g_impCount & 7] = __LINE__, ++::dp::g_impCount, ::dp::g_impulseSite = __LINE__, true)

#define YSET(x) (::dp::g_yWriter = __LINE__, ++::dp::g_yWrites, (x))

// ...and the lines that built vpNew, IN ORDER. c.vy is written once, after the
// whole branch chain, from vpNew -- so the vy writer names that shared line and
// not the branch that produced the value. The last eight writes are kept, so
// a jump, its robot scale and a slope bonus read as the sequence they are.


inline long long g_vyWatchT2 = -1;  // --vywriter2 <t>: a second (control) tick

// --coins: where one coin is THIS tick, and the bit it owns in State::coins.
// A coin can be group-controlled like any other object -- lv20's first sits in a
// toggled group with the platform under it, lv21's third and lv22's third are
// MOVED into reach -- so the collect test cannot read the load-time row. A coin
// its group has switched off simply has no entry: GD does not credit one.
struct CoinLive {
    double cx, cy, hw, hh;
    int bit;
};

struct StepCtx {
    double x, xPrev;
    float dxF;         // this layer's per-tick x advance (see advanceX)
    long long t;
    // (the flying band lives in State now -- see State::bandFloor)
    const std::vector<const Obj*>* near;
    const std::vector<const Obj*>* ports;
    const std::vector<const Obj*>* pads;
    const std::vector<const Obj*>* orbs;
    const std::vector<const Obj*>* slopes;
    const std::vector<const Obj*>* speeds;
    // Both sizes. The ship used ShipParams::normal() unconditionally, so a mini
    // ship climbed at the full-size 0.086/tick where GD gives 0.101 -- measured
    // on lv11 t=8395..8411, a steady 0.0150/tick of drift that put the model
    // 3.6 px below GD within 17 ticks and killed the whole ship section.
    // ShipParams::mini() already held the right numbers; nothing selected them.
    const gdapprox::ShipParams* SP;
    const gdapprox::ShipParams* SPmini;
    const gdapprox::UfoParams* UP;
    const gdapprox::UfoParams* UPmini;
    // Touch-trigger boxes overlapping this group's x window, with the bit each
    // one owns in State::trig. Null on every level that has none.
    const std::vector<std::pair<const TouchTrig*, TouchMask>>* trigs = nullptr;
    // The coins where they are this tick (model.hpp prepareGroup's coinLive), which the route's
    // afterStep collects from (search_dp.hpp CurrentRules). Null when the group did not ask for
    // the list (GroupOpts::coins); empty without --coins. The step itself does not read it.
    const std::vector<CoinLive>* coins = nullptr;
    // The search's per-group SlopeVetoIndex over `near` and `slopes`. Null in
    // every other caller; the veto then walks `slopes` whole (see vetoOf).
    const SlopeVetoIndex* veto = nullptr;
    // --solidorder (removed): `near` in GD's solid order (descending uid), built once where `near` is
    // built. Null elsewhere -- the collision loops then sort a copy themselves.
    const std::vector<const Obj*>* nearSolid = nullptr;
    // --followy: the objects of a Follow Player Y group in the windows above (model.hpp
    // prepareGroup). Null when there are none -- the step then places nothing per state.
    const std::vector<FollowRef>* fol = nullptr;
    // --spiderrectall (removed): `near` / `slopes` as a rect query sees them -- --followsection's far-cell copies
    // at their place, not sent away. Null outside that pass (the windows are then the same).
    const std::vector<const Obj*>* nearRect = nullptr;
    const std::vector<const Obj*>* slopesRect = nullptr;
    // --shadowstep: which prepareGroup call built this group (model.hpp GroupEnv::sgen; 0 = none). The shadow keeps its thread's movers placed per (group, tick): a counter read when
    // the step runs was another thread's bump as often as this group's, and two groups of one tick then shared a placement.
    uint64_t sgen = 0;
};

// Why the last `dead = true` fired, and on what. "The frontier died at x=N" on
// its own never says whether the model is over-killing or under-killing, and
// every wall so far has needed that answer before anything else -- lv16's ramp
// ridge and lv18's spiked tunnel both read as "the search ran out" until the
// object was named. Written unconditionally (a store of two words per kill) and
// only ever READ by --dbg, so the search itself is unaffected.
// thread_local because the layer expansion runs on several threads (see
// ThreadPool): every worker kills its own children and would otherwise stamp on
// the others' reason. --dbg reads them on the same thread that stepped the
// child, so the reason is still the right one there.
inline thread_local const char* g_deadWhy = "";

inline thread_local const Obj* g_deadObj = nullptr;

// Kills by the box GD's own random numbers can put an Area Move's object in (the hazard twins,
// uid >= kEnvTwinUidBase), over the life of the process. The box is a conservative model --
// it also closes routes the seed of any one game would leave open -- so a level whose count
// moves had branches pruned by uncertainty rather than by the level.
// Never reset: a reader takes the difference across what it wants to count.
inline std::atomic<long long> g_envKills{0};

// Whether something that can deliver an impulse (orb / pad) was within reach on this
// tick. **"Was present", not "fired"**. An axis for diagnosing missed impulses; it is
// written to the trace and goes into the driver's cause signature.
inline thread_local uint8_t g_nearOrb = 0;

// The vy that GD's dump prints on the tick a dash ring **engages**. The model's
// internal value is already 0 there (no vy is held while dashing), but GD leaves
// "the value with gravity added once more" in that tick's dump, and it only becomes
// 0 from the next tick on.
// Measured (lv21; y matches exactly on both sides = same trajectory):
//   t=1,894  type37  -10.120 -> GD -10.336 (= -10.120 + g), model 0
//   t=18,955 type37   -5.452 -> GD  -5.668 (= -5.452 + g)
//   t=4,511  type38  -14.256 -> GD  -7.236 (= (-14.256 + g) / 2)
//   t=5,555  type38   -8.908 -> GD  -4.562 (= (-8.908 + g) / 2)
// So **37 passes it through, 38 (with gravity flip) halves it**. y is already frozen
// on the engage tick, so it never shows in the trajectory, and GD also restarts the
// fall from 0 after the dash ends ([[gd-dash-stop-1829]]) -- a purely
// **display-only** difference.
// It is fixed anyway because fixcensus / quick_regress / the driver's fixup all look
// at the trace: left alone, a non-real divergence gets recorded once every time, and
// at worst a fixup gets written back on top of correct physics.
// Not kept in the state (a non-zero vy would split the dedupe key per branch).


// Why vy was set to 0 on this tick. **Kept the same way as the cause of death.**
// A wrong clamp shows up as "only the model loses velocity", but which clamp fired
// cannot be worked back from outside (it was happening where there is not a single
// static object). "" = not clamped.
inline thread_local const char* g_clampWhy = "";

// What the clamp hit. Chasing a false firing needs "which object was taken as the
// ceiling / floor" (measured: not a single static object at that coordinate = it was
// looking at a moving object).
inline thread_local int g_clampUid = -1;

inline thread_local float g_clampCx = 0.f, g_clampCy = 0.f;

// ...and WHERE the killer was AT THE MOMENT it killed. Reading cx/cy off the
// pointer after the run is not the same number for a moving object, and that
// difference is exactly what a stale-geometry bug looks like.
inline thread_local float g_deadCx = 0.f, g_deadCy = 0.f;

// ...and WHAT KIND of death it was: the physics (GD kills it too), a rule of the route the search is
// asked for (a glitch it avoids, a hairline it will not trust), or a prune of the search itself (a
// branch that can never come back). Only the first is the game's; the other two are choices, which
// a different route or a different search may make differently (search_dp.hpp CurrentRules).
enum : uint8_t { kDeathPhysics = 0, kDeathRule = 1, kDeathPrune = 2 };

inline thread_local uint8_t g_deadKind = kDeathPhysics;

// The ship's accel-switch threshold is per SPEED (see ShipParams::withSpeed).
// The base sets are built once at start-up and never carried a speed, so a
// 0.7 section ran with the 0.9/1.1 threshold and switched to the weak accel one
// tick late -- 0.026 vy, which on lv18 grew to 43 px and a different route.
// Only the slow variant differs, so it is two extra statics rather than a copy
// per step.
// The Options (2899) triggers of the body's own channel that this tick's move crossed, as State::ctrlT (refused from the next tick on): GD fires an x-crossed Options trigger from the crossing queue whatever
// the level is made of (frames.hpp ctrlChanCrossed). A level that turns its gameplay takes the queue's last word, raise or lower. Any other level takes the LOWERING only: the anchor's ctrlOff = 1 seeds ctrlT = 1
// (cli.hpp, "seed: ctrlOff=1 at the anchor") and the enabling trigger the player then crossed left it up for good (the touch boxes' block is the only other writer, and a crossing-only level has none), while GD's
// ctrlOff went 1 -> 0 on that tick: custom level A (5,492; the robot's press at 5,639 was refused and GD jumped), custom level B (504; the press at 540), custom level C (9,153; the press at 9,342). The raise there is the
// window gate's (ctrlTrigInputOff). [M] 222 of the 256 ctrlOff 1 -> 0 ticks of the 63 custom flights' dumps are a tick that crossed a non-gated enabling 2899; the rest are touch / spawn-fired or not x-crossed.
// Called by the old tick and by the shadow's tick (shadow_step.hpp): one place.
inline void ctrlCrossedApply(State& c, const State& s, bool dead) {
    if (dead || g_ctrlTrigs.empty() || c.frame != 0 || c.dual || s.dual) return;
    const int cr = ctrlChanCrossed((double)s.xAbs, (double)c.xAbs, (int)s.rotChan, s.rev != 0);
    if (cr < 0) return;
    if (!g_rotTrig.empty()) c.ctrlT = (uint8_t)cr;
    else if (cr == 0 && !s.rev && !c.rev) c.ctrlT = 0;
}

// GD's spider surface search: "the face on the other side, travelling against
// the current gravity". Used by the SPIDER's own tap (inline in the removed old tick's stepOne, which
// is where every constant here was measured) and by the SPIDER ORB (type 43),
// which runs the same search from any mode and in mid-air.
// Returns false when there is nothing to land on -- GD's spiderTestJump finds
// no surface then, and the caller must do nothing rather than guess.
// `outHaz` (when given) comes back true if the thing the search stopped on is a
// HAZARD. GD's search does not skip them: measured on lv22 t=1,737 with a hook
// on PlayerObject::spiderTestJump itself (the raw accumulator fields are reset
// every tick, so only a hook inside the call sees the answer) -- the spider taps
// off the ceiling at y=316.500 and GD puts it at **213.250**, which is exactly
// the cy of the id-392 spike row (uid 18201..18206 / 17571, 2.6x4.8, all at
// cy=213.25), and kills it there. The model skipped `type != 0`, walked past the
// row and landed on the block below at y=163.500 -- 50 px lower, alive, and the
// DP then planned a route out of a section GD does not let you leave.
// Eight teleports were captured in that one replay; the other seven land on
// blocks at block-derived heights (223.5 / 163.5 / 316.5 / ~286-291), so the
// hazard case is the only one whose landing is the object's OWN centre. That
// offset is ONE sample -- but the player dies on the same tick, so only the
// stopping matters, not the height.
// `frame` is the gameplay frame the search runs in; it only matters for the
// transverse window (see the note at the window test).
// `bandWall` / `bandLo` / `bandHi`: the band, when it is a wall (bandIsWall).
// spiderTestJumpInternal extends its search rect only as far as
// getMin/MaxPortalY, so nothing beyond the band is a candidate, and GD puts a
// spider that finds nothing inside it on the band's own face. Measured on
// SubZero 4003 t=16,276 (spider band 1354.5/1651.5, Free Mode off, no solid
// between): the tap off the ceiling at y=1,395 lands at 1,368.000 = 1354.5 +
// 13.5, where the model, with no band, fell to the world floor at 103.5.
// GD's own search in frame 0 at normal size: spiderTestJumpInternal (2.2081,
// 0x3943f0) read step by step and checked against the rects GD itself queried
// (hitboxtrace srect:/drect: lines) on SubZero 4002.
//
//  1. Two queries over the same reach along the travel axis -- from 2 px inside
//     the player's leading face to the band. SOLIDS in a strip hung off the
//     centre travelling right, [x, x + w/2 + 1], but off the LEFT EDGE
//     travelling left, [x - w - 1, x - w/2] (0x3947cd tests m_isGoingLeft and
//     moves the origin to minX - (w/2 + 1) instead of mirroring it); HAZARDS in
//     an 8 px strip centred on the player either way. t=21,695: GD's srect is
//     o=30,760.467 w=14.5 for x=30,788.467 running left.
//  2. Solids nearest first. A landing may be up to 10 px BEHIND the player
//     (0x394bcf..0x394c20); t=21,695 the spider on a ceiling at 1,696.5 takes
//     the slat whose top is its own bottom and flips in place. A solid wholly
//     AHEAD of the centre (min x past it right, max x short of it left) is
//     taken only if the player's rect at the landing height intersects no
//     other solid of the same list, each shrunk 2 px top and bottom
//     (0x394d21..0x394dff); t=21,484 the slat uid17831 is blocked by uid17886
//     stacked on it and GD lands on the floor at 1,483.5. A solid reaching back
//     under the centre is taken as it is.
//  3. HAZARDS: if the solid strip came back empty and a hazard did not, the
//     solids are asked again over the player's whole width plus 4,
//     [x - w/2, x + w/2 + 4] (0x39490b..0x394947); with nothing there either,
//     the first hazard is the landing, at its own position. And when a solid
//     landing was chosen but the first hazard is NEARER, the function calls
//     itself with that wider solid query; if the hazard is still nearer, the
//     spider lands on it (0x394f5e..0x394fc7). The shape does not matter:
//     t=20,305 GD asks the strip, the hazards, then the 31-px rect, and puts
//     the spider at y=1,545, the centre of the saw uid18080.
//  Nothing accepted: the band face (the world floor when the band is not a wall).
//
// `outSolid` (when given) comes back true when the landing is a solid object's face -- not a
// hazard, not the world floor, not the band's face. A spider pad (--spiderpad) needs it: GD's
// solid pass runs after the activation pass that fires the pad, and lands the player on an
// object's face on the teleport's own tick, while the world floor lands it on the next.
// `atFromOk`: the fallback faces (world floor, band) are taken at the player's own height too,
// not only 0.5 px or more ahead. A spider pad facing against the gravity first turns the gravity
// toward the surface the player is on, and GD then "teleports" it onto that same floor.
inline bool spiderTargetGd(const StepCtx& K, double x, double fromY, bool flip,
                           double pHalf, double searchHalfX, double& outY,
                           bool worldFloor, bool* outHaz, bool rev, bool bandWall,
                           double bandLo, double bandHi, double hazStrip = 4.0,
                           bool* outSolid = nullptr, bool atFromOk = false) {
    const double fallbackAhead = atFromOk ? -0.5 : 0.5;
    const double gs = flip ? -1.0 : 1.0;
    // A CIRCLE hazard enters the damaging query through its RADIUS box, not
    // through the rect. Swept on 4002 by injecting the player's x before a tap
    // and reading the uids GD's own query returned (`dhit:`): saw uid18080
    // (radius 15.876 after the level's 1.47 scale) is in the 8 px strip at a
    // centre distance of 19.86 and out at 19.88, and uid18019 -- the same
    // radius at a different angle -- is in at 19.85 and out at 19.90. Both
    // brackets contain the radius and nothing else: the rotated rect (26.46),
    // the sprite half (18) and the raw radius (10.8) all sit outside them.
    const auto hazHalfX = [&](const Obj* o) {
        return o->type != 0 && o->radius > 0.0 ? o->radius : o->hw;
    };
    const auto hazHalfY = [&](const Obj* o) {
        return o->type != 0 && o->radius > 0.0 ? o->radius : o->hh;
    };
    const auto inReach = [&](const Obj* o) {
        const double hy = hazHalfY(o);
        if (flip ? o->cy - hy > fromY - pHalf + 2.0 : o->cy + hy < fromY + pHalf - 2.0)
            return false;
        return !(bandWall && (flip ? o->cy + hy < bandLo : o->cy - hy > bandHi));
    };
    constexpr int kN = 64;
    const Obj* strip[kN];
    const Obj* wide[kN];
    int nStrip = 0, nWide = 0;
    const Obj* haz = nullptr;
    const double s0 = rev ? x - pHalf - searchHalfX : x;
    const double s1 = rev ? x - pHalf : x + searchHalfX;
    const double w0 = x - pHalf, w1 = x + pHalf + 4.0;
    // --spiderrectall (removed): the layer's rect queries walk every section the rect covers, not the 3 x 3
    // cells around the player the collision pass walks, so --followsection's far-cell objects are in
    // reach here. A custom level t=821: an upside-down spider's pad sends it down past a 300 px slab
    // (uid 47, load-time cell row 43, the player's 45) that GD lands it on at 4,333.5; the model, with
    // the slab sent away, dropped it to the floor at 103.5.
    // (always on: the switch --spiderrectall is removed)
    // --spiderrectall (removed): the spider teleport's search reads --followsection's far-cell objects where they
    // stand (step.hpp spiderTargetGd, StepCtx::nearRect).
    const auto* nearL = (K.nearRect) ? K.nearRect : K.near;
    const auto* slopesL = (K.slopesRect) ? K.slopesRect : K.slopes;
    for (const Obj* o : *nearL) {
        if (!inReach(o)) continue;
        // --spidernoextcol (speed.hpp): the rect queries walk the section grid, which an Extended Collision
        // object is not filed in
        if (g_spiderNoExtCol && o->extCol) continue;
        const double lo = o->cx - o->hw, hi = o->cx + o->hw;
        if (o->type != 0) {
            const double hx = hazHalfX(o);
            if (o->cx + hx < x - hazStrip || o->cx - hx > x + hazStrip) continue;
            if (!haz || (o->cy - haz->cy) * gs < 0.0) haz = o;   // nearest first
            continue;
        }
        if (!(hi < s0 || lo > s1) && nStrip < kN) strip[nStrip++] = o;
        if (!(hi < w0 || lo > w1) && nWide < kN) wide[nWide++] = o;
    }
    // --spiderslopeline (constants.hpp): ...and the ramps, which the model keeps apart from `near`;
    // GD's staticObjectsInRect returns them with the blocks
    if (slopesL)
        for (const Obj* o : *slopesL) {
            if (!inReach(o)) continue;
            const double lo = o->cx - o->hw, hi = o->cx + o->hw;
            if (!(hi < s0 || lo > s1) && nStrip < kN) strip[nStrip++] = o;
            if (!(hi < w0 || lo > w1) && nWide < kN) wide[nWide++] = o;
        }
    // step 2 over one list, then the fallback faces
    bool pickedSolid = false;   // the last pick's answer came from an object's face
    const auto pick = [&](const Obj* const* L, int n, double& y) {
        bool f = false;
        pickedSolid = false;
        for (int i = 0; i < n; ++i) {
            const Obj* o = L[i];
            double faceY = flip ? o->cy + o->hh : o->cy - o->hh;
            // --spiderslopeline (constants.hpp): a ramp facing the search is met at its line
            if (o->slope && o->hw > 0.0) {
                const bool ceilLike = o->slopeDir < 7 && ((0x6au >> o->slopeDir) & 1u);
                if (ceilLike != flip)
                    faceY = o->sy0 + (o->sy1 - o->sy0) * (x - (o->cx - o->hw)) / (2.0 * o->hw);
            }
            const double rest = faceY - gs * pHalf;
            if (g_slopeDbg)
                std::printf("spidertgt x=%.3f from=%.3f flip=%d uid=%d slope=%d dir=%d face=%.3f rest=%.3f\n",
                            x, fromY, (int)flip, o->uid, (int)o->slope, (int)o->slopeDir, faceY, rest);
            if ((rest - fromY) * gs < -10.0) continue;
            if (f && (rest - fromY) * gs >= (y - fromY) * gs) continue;
            if (rev ? o->cx + o->hw < x : o->cx - o->hw > x) {
                bool blocked = false;
                for (int j = 0; j < n && !blocked; ++j) {
                    const Obj* b = L[j];
                    if (b == o) continue;
                    blocked = !(x + pHalf < b->cx - b->hw || b->cx + b->hw < x - pHalf
                                || rest + pHalf < b->cy - b->hh + 2.0
                                || b->cy + b->hh - 2.0 < rest - pHalf);
                }
                if (blocked) continue;
            }
            y = rest;
            f = true;
        }
        pickedSolid = f;
        if (!f && flip && worldFloor && !bandWall) {
            const double rest = kGroundY + pHalf;
            if (fromY - rest > fallbackAhead) { y = rest; f = true; }
        }
        if (!f && bandWall) {
            const double rest = flip ? bandLo + pHalf : bandHi - pHalf;
            if (flip ? fromY - rest > fallbackAhead : rest - fromY > fallbackAhead) { y = rest; f = true; }
            // --spiderbandback: ...and also when the spider is already past that face -- the band
            // is a clamp, not a surface found ahead. SubZero 4002 t=23,440: a flipped spider at
            // 1,453.5 (feet 3 px below the band floor 1,443) presses; GD lands at 1,456.5 = 1,443 +
            // 13.5, 3 px back up, where the model found nothing and lost the spider.
            else  y = rest; f = true; 
        }
        return f;
    };
    const auto hazNearer = [&](bool f, double y) {
        return haz && (!f || (haz->cy - y) * gs < 0.0);
    };
    double y = 0.0;
    bool f = false, onHaz = false, solid = false;
    if (nStrip == 0 && haz) {
        if (nWide == 0) onHaz = true;
        else {
            f = pick(wide, nWide, y);
            solid = pickedSolid;
            onHaz = hazNearer(f, y);
        }
    } else {
        f = pick(strip, nStrip, y);
        solid = pickedSolid;
        if (hazNearer(f, y)) {
            double y2 = 0.0;
            const bool f2 = nWide ? pick(wide, nWide, y2) : false;
            if (nWide == 0 || hazNearer(f2, y2)) onHaz = true;
            else { y = y2; f = f2; solid = pickedSolid; }
        }
    }
    if (onHaz) { y = haz->cy; f = true; solid = false; }
    if (outHaz) *outHaz = f && onHaz;
    if (outSolid) *outSolid = f && solid;
    outY = y;
    return f;
}

// `outSolid` / `atFromOk`: see spiderTargetGd. The turned-frame search below answers outSolid for
// the solids it takes; atFromOk applies to GD's search only.
inline bool spiderTargetY(const StepCtx& K, double x, double fromY, bool flip,
                          bool mini, double pHalf, double& outY,
                          bool worldFloor = true, bool* outHaz = nullptr,
                          int frame = 0, bool rev = false, bool bandWall = false,
                          double bandLo = 0.0, double bandHi = 1e9,
                          bool* outSolid = nullptr, bool atFromOk = false) {
    const double gs = flip ? -1.0 : 1.0;
    // 1.0 px wider in x than the spider's own half -- measured, see
    // kSpiderSearchHalfX. This picks WHICH surface it lands on.
    const double searchHalfX = mini ? kSpiderSearchHalfXMini : kSpiderSearchHalfX;
    // frame 0 at normal size: GD's own search, step by step (above)
    if (frame % 2 == 0 && !mini)
        return spiderTargetGd(K, x, fromY, flip, pHalf, searchHalfX, outY, worldFloor,
                              outHaz, rev, bandWall, bandLo, bandHi, 4.0, outSolid, atFromOk);
    // --spiderminigd: ...and at mini size the same search, with the widths GD queried for a mini
    // spider (hitboxtrace srect:/drect:, a custom level's dual section, a teleport at x=724.428,
    // pHalf 8.1): the solid strip o=724.428 w=9.100 (the centre to half + 1, the
    // +1 unscaled -- kSpiderSearchHalfXMini's 14.5 x 0.6 = 8.7 scaled it); the hazard strip
    // o=722.028 w=4.800 (+-4 x 0.6); with the strip empty and hazards in it, the wide query
    // o=716.328 w=20.200 (x - half .. x + half + 4, the +4 unscaled) returned block uid5429, and
    // GD landed the spider on its top (1,020 + 8.1). The legacy window below takes nothing that
    // far right of the spider, so the model teleported to the band floor 240 px lower.
    if (frame % 2 == 0 && mini)
        return spiderTargetGd(K, x, fromY, flip, pHalf, pHalf + 1.0, outY, worldFloor,
                              outHaz, rev, bandWall, bandLo, bandHi, 4.0 * 0.6, outSolid,
                              atFromOk);
    double bestY = 0.0;
    bool found = false, bestHaz = false, bestSolid = false;
    // HAZARDS are searched too, but through their OWN rect. Straight off
    // spiderTestJumpInternal (2.2081, 0x3943f0 -- note the version, see the
    // note on kSpiderSearchHalfX): the function builds a rect, extends it to
    // getMinPortalY/getMaxPortalY, narrows it in x, and then asks the layer
    // TWICE with two different rects:
    //     r15 = staticObjectsInRect (rect A)      <- solids
    //     r13 = damagingObjectsInRect (rect B)    <- hazards
    // Using ONE rect for both is what broke the first attempt at this: with the
    // solid column's half (14.5) the spike uid5356 at |dx| = 15.93 becomes a
    // candidate and the model died at t=819, where GD sails past it
    // (163.500 -> 290.860). With the player's own half (13.5) that spike is
    // 15.93 > 13.5 + 2.376 = 15.876 -- OUT by 0.05 px -- while the one that
    // does kill, uid18205 at |dx| = 0.74, is well inside 13.5 + 1.3.
    // The margin is thin, but the two rects are the binary's own structure and
    // the sign of the split is not a free parameter.
    for (const Obj* o : *K.near) {
        const bool haz = (o->type != 0);
        if (haz && o->radius > 0.0) continue;   // saws are circles, not faces
        const double halfX = haz ? pHalf : searchHalfX;
        // ...and in a TURNED frame that half is on the WRONG AXIS. The rect
        // spiderTestJumpInternal builds is narrowed in the player's own x and
        // EXTENDED to getMinPortalY/getMaxPortalY in world Y. In frame 0 the
        // search's transverse axis IS world x, so `halfX` is the narrowing and
        // everything lines up; in an odd frame the transverse axis is world Y --
        // the EXTENDED one -- and narrowing it to 14.5 throws away exactly the
        // objects GD reaches.
        // Measured on lv22's rotated section (2026-08-16, cold run, t=1,813):
        // the spider taps at world (2,266.5, 229.5) and GD lands it at world
        // x=2,173.5 = the right edge of uid1204 (2,152.5,202.5, 15x15) plus the
        // spider's 13.5. That block is 27.0 px away in world Y and the window
        // was 7.5 + 14.5 = 22.0, so the model skipped it and took uid1212/1214
        // at world x=2,115 instead -- landing 30 px further, and every tick
        // after that belongs to a different run.
        // (uid1201/1202 sit at the same world Y but have MOVED to y=84 by then,
        // so exactly one candidate is left at that height and the
        // nearest-ahead rule then picks GD's.)
        // ...and only the SOLID rect is the extended one. The two rects are
        // already known to differ (see the note above this loop); a hazard found
        // with the band's reach is one GD never sees. Measured on the same tick:
        // with the band applied to both, the model stopped on a hazard at world
        // x=2,257.5 -- nearer than uid1204 in the travel axis, so it won the
        // nearest-ahead rule -- and died, where GD sails past it to 2,173.5.
        // [2026-08-26] In a TURNED frame the window is not centred on the
        // player at all. Hooked spiderTestJumpInternal's two queries (the
        // srect:/drect: lines, cfg hitboxtrace=1) on lv22's t=1,813 tap:
        //   srect: o=(0.000,201.517) s=(2255.000,14.500)   <- solids
        //   drect: o=(0.000,225.517) s=(2255.000, 8.000)   <- hazards
        // with the player at world y 230.815 scrolling 1.298/tick. In the
        // model's progress coordinate both anchor at the POST-MOVE centre
        // xF = x + dxF:
        //   solids : [xF + pHalf, xF + 2*pHalf + 1]  (the strip hangs off the
        //            LEADING edge; 14.5 = size*0.5 + 1, the 0x3943f0 constant)
        //   hazards: [xF - 4, xF + 4]                (the +4.0 constant; the
        //            old +-(hw+pHalf) window was ~4x wider than GD's band and
        //            invented the tp-hazard kill at 2,257.5 that doomed every
        //            anchor on the cold run's prefix)
        // This one form reproduces every measurement on both worldlines: the
        // natural tap (lands 2,173.5 on uid1204, a 6px GAP from the player's
        // box -- inside the strip), the y=240/244 injection boundary (strip
        // overlap 0.6px / miss 3.2px), and the 8/16 landings at 2,143.5
        // (uid1204 out of the strip at that worldline's y, the moved-in
        // uid1212/1214 inside it).
        if (frame % 2 != 0) {
            const double xF = x + (double)K.dxF;
            if (haz) {
                if (o->cx + o->hw < xF - 4.0 || o->cx - o->hw > xF + 4.0)
                    continue;
            } else {
                if (o->cx + o->hw < xF + pHalf
                    || o->cx - o->hw > xF + 2.0 * pHalf + 1.0)
                    continue;
            }
        } else if (true) {
            if (!mini && !rev) {   // GD's srect / drect
                if (haz) {
                    if (o->cx + o->hw < x - 4.0 || o->cx - o->hw > x + 4.0) continue;
                } else {
                    if (o->cx + o->hw < x || o->cx - o->hw > x + searchHalfX) continue;
                }
            } else if (std::fabs(x - o->cx) > o->hw + halfX) {
                continue;
            }
        } else {
            // the portal band in this frame's travel coordinate. GD's own
            // limits come from the anchor's dump (--startband); with none, a
            // window wide enough for the 270 px band.
            double bLo = x - 150.0, bHi = x + 150.0;
            if (startBandUsable()) {
                const double u0 = frameU(frame, 0.0, g_startBandFloor);
                const double u1 = frameU(frame, 0.0, g_startBandCeil);
                bLo = std::min(u0, u1); bHi = std::max(u0, u1);
            }
            if (o->cx + o->hw < bLo || o->cx - o->hw > bHi) continue;
        }
        // the face that matters is the one the player comes to rest ON: the
        // UNDERSIDE of a solid when travelling up, its TOP when travelling down
        if (g_slopeDbg && frame % 2 != 0)
            std::printf("spidertgt-turned x=%.3f dxF=%.4f from=%.3f flip=%d uid=%d haz=%d cx=%.3f hw=%.3f cy=%.3f hh=%.3f\n", x, (double)K.dxF, fromY, (int)flip, o->uid, (int)haz, o->cx, o->hw, o->cy, o->hh);
        const double face = flip ? (o->cy + o->hh) : (o->cy - o->hh);
        // a hazard is not landed ON -- GD puts the player at its centre and
        // kills it there (lv22 t=1,737: 316.500 -> 213.250, the id392 row's cy)
        const double rest = haz ? o->cy : (face - gs * pHalf);
        if ((rest - fromY) * gs <= 0.5) continue;   // not ahead of us
        if (!found || (rest - fromY) * gs < (bestY - fromY) * gs) {
            bestY = rest;
            bestHaz = haz;
            bestSolid = !haz;
            found = true;
        }
    }
    if (outHaz) *outHaz = found && bestHaz;
    // ...and the world's own floor counts when travelling down with no block
    // under us. NOT in a turned frame: there `v` is a world X, so kGroundY is a
    // vertical line at world x=90 -- a floor that does not exist. Measured on
    // lv22 (2026-08-13): the tap sent the model to world x=1,459
    // and then it slid off the level, where GD lands at x=2,143.5 every time.
    // GD does have a ground in the turned frame (the spider bounces between
    // 2,143.5 and 2,266.5 with no object at the former), but its position is
    // not derived yet -- and a missing floor kills the branch, which is the
    // safe direction, while a phantom one invents a route.
    if (flip && worldFloor) {
        const double rest = kGroundY + pHalf;
        if (fromY - rest > 0.5 && (!found || rest > bestY)) {
            bestY = rest;
            bestSolid = false;
            found = true;
        }
    }
    // ...and a band that is a wall bounds the search: its face is the landing
    // for a spider that found nothing nearer, and whatever lies beyond it was
    // never in GD's rect. Frame 0 only, for the same reason as the world floor.
    if (bandWall && frame == 0) {
        const double rest = flip ? bandLo + pHalf : bandHi - pHalf;
        const bool ahead = flip ? (fromY - rest > 0.5) : (rest - fromY > 0.5);
        const bool beyond = found && (flip ? bestY < rest : bestY > rest);
        if (ahead && (!found || beyond)) {
            bestY = rest;
            bestSolid = false;
            found = true;
            if (outHaz) *outHaz = false;
        }
    }
    if (outSolid) *outSolid = found && bestSolid;
    outY = bestY;
    return found;
}

// --rotpretap: bring every OTHER built frame's moving geometry to tick t. The
// re-tap in applyRotation reads the NEW frame's rects on the tick it turns, and a
// frame the player is not in is otherwise left where it stood when the player
// last left it. Measured on lv22 t=1,817: frame 0 still had uid1214 at
// y=299.93 from t~1,745, while frame 1's own copy (the block the spider was
// standing on) had it at 225.0 -- so the re-tap landed on a block that had
// gone, 45 px short of GD's 316.5. Recording and closed-form positions only;
// the touch doors (applyTriggers) are the current frame's and stay there.
// SERIAL CALLERS ONLY: seek() writes the rects in place and the search reads
// them from the worker threads.
inline void seekOtherFrames(const Level* cur, int t) {
    if (g_rotTrig.empty()) return;
    for (int f = 0; f < 4; ++f) {
        Level* lv = levelOfFrame(f);
        if (lv && lv != cur) lv->dyn.seek(t);
    }
}

// Turn the state if this tick's advance crossed a rotation object (id 2900).
// `uPrev` is where it was BEFORE the move, in its own frame. Returns the new
// frame, or -1 when nothing changed.
//
// The world POINT carries over (it is one player in one world) and so does the
// world VELOCITY: the forward speed becomes the new perpendicular one, and the
// old perpendicular is dropped because the forward speed is the section's, not
// a free variable. Everything that names a surface or a band is cleared -- the
// band is a pair of world-Y limits and would clamp the player into a wall.
// `t` is this tick, and it is READ ONLY here: a trigger already spent on an
// earlier tick is skipped, one spent on THIS tick still fires (the whole group
// crosses together, and the marking is done once per group in the layer
// prologue -- this runs on the worker threads).
// `input` / `wasGrounded` describe the tick's BUTTON, which GD processes after
// the rotation has already turned the world (see the re-tap at the end).
// --tpjumpnext (removed) (modifiers.hpp): where a state's x crossings stand -- before a forward teleport this tick, the x it
// left from, which is also where the next tick's crossings start
inline double crossBaseX(const State& s) {
    // (always on: the switch --tpjumpnext is removed)
    // --tpjumpnext (removed): ...and a teleport that moves x FORWARD (a portal's exit, an orb's, a trigger's by its crossing or
    // its touch) leaves the x crossings it passed to the NEXT tick. Measured on a rig of its own (not shipped:
    // a cube at 1x jumped from about 578 to 720): gravity triggers inside the jump (690) and on its target (720) fire
    // on the tick after the orb's, and the same after a teleport trigger's; a 2900 there turns the frame on that tick
    // too (vy 5.193); an ordinary crossing fires on its own tick. So a tick's crossings end where the jump left from
    // and the next tick's start there (State::tpFromX, step.hpp crossBaseX). A custom level t=1,158: an orb puts a
    // robot on a 2900 at (1605, 570); the search turned a tick early and the replay never did. Gravity and time
    // warps were already right (read by position). One body in frame 0.
    return ((s.tpSkip & 4)) ? (double)s.tpFromX : (double)s.xAbs;
}

// --reversetrig, the touch-started ones (modifiers.hpp g_revTouchBoxes): the boxes this body overlaps now and has not fired each toggle the direction (two in one tick toggle twice, as two
// reverseDirection calls) and are marked fired (State::revFired); the next tick's x step runs the other way. One body in frame 0. Returns how many fired.
inline int revTouchApply(State& c, double yFree) {
    int n = 0;
    const double ph = (c.mode == 4 && c.mini) ? kWaveContactHalfMini : playerHalf(c.mode, c.mini != 0);
    for (size_t i = 0; i < g_revTouchBoxes.size() && i < 16; ++i) {
        const RevTouchBox& b = g_revTouchBoxes[i];
        if (!(c.revFired & (uint16_t)(1u << i)) && std::fabs((double)c.xAbs - b.cx) <= b.hw + ph && std::fabs(yFree - b.cy) <= b.hh + ph) {
            c.revFired |= (uint16_t)(1u << i);
            c.rev = (uint8_t)!c.rev;
            ++n;
        }
    }
    return n;
}

inline int applyRotation(State& c, double uPrev, double dxUsed, long long t,
                         int input = 0, bool wasGrounded = false,
                         double sPrevY = 0.0, bool havePrevY = false, bool freshPress = true) {
    if (g_rotTrig.empty()) return -1;
    // --rotretapfresh (constants.hpp): the re-tap is a fresh press's (freshPress: the button was not held, or a portal handed the press over)
    // (never on: the switch --rotretapfresh is removed)
    // (P9) A tick the shadow steps does not call the old tick. The State is the one before the tick, with the shadow's writes (shadowAdvanceImpl) and the registered families' derivations (field_check.hpp)
    // laid over it; a tick the shadow declines (or a mode it does not step) calls the old tick as before. What the old tick moved and nobody writes stays at its value before the tick: --fieldcheck's census lists those members.
    // --rotretapfresh: applyRotation's re-tap (a grounded spider's press redone in the frame it turns into) is for a FRESH press only -- one the old frame's tap would have spent (the button was not held,
    // or a portal handed the press over). A button already held for ticks has no tap to undo and redo: lv22 t=1,745 (a press held from t=1,689), GD stays on the ceiling it was on (x 2,266.5 in the new frame)
    // where the model re-tapped to the floor (2,143.5) and every teleport after it came out mirrored. Default off.
    const bool reTapPress = input == 1 && c.mode == 6 && wasGrounded;
    const int f0 = (int)c.frame;
    const int rev0 = (int)c.rev;   // reverse travel is a 180-degree heading = true frame +2
    int nf = -1;
    int nflip = -1;
    // --rotpretap: the perpendicular coordinate the collision pass tests, i.e.
    // before the tap. Only a grounded spider's tap moves it (the re-tap below
    // uses the same predicate); otherwise it is c.y.
    const double vColl = (reTapPress && havePrevY)
        ? sPrevY : (double)c.y;
    // ---- THE QUEUE (frames.hpp) ------------------------------------------
    // GD walks ONE channel from a cursor and stops at the first element the
    // player has not passed. That is not a nearest-match search, so none of the
    // selection below runs: no perpendicular window (the pairs 6 px apart that
    // it was invented for are in DIFFERENT CHANNELS, and the cursor picks
    // between them), no scroll gate, no last/nearest rule.
    //
    // The axis is the player's -- vertical exactly when the frame is turned,
    // since a quarter turn is what makes travel vertical -- while the DIRECTION
    // is the channel's reverse bit. GD is asymmetric that way on purpose, and
    // the comparison is against WORLD coordinates either way.
    bool qFired = false;
    float qModY = 1.0f;
    uint8_t qOvr = 0;
    uint8_t qNoSlide = 0;
    // --tpjumpnext (removed): the x this tick's crossings end at (the turn itself, below, starts from c.xAbs)
    const double cCrossX = crossBaseX(c);
    // --rottouch (frames.hpp): a touch 2900 the player has reached fires in the object pass,
    // whichever channel the queue's cursor is on; its queue slot is spent with it
    if (g_rotTouch && g_rotQueue && !g_rotQ.empty()) {
        double twx, twy;
        fromFrame(f0, cCrossX, (double)c.y, twx, twy);
        for (size_t k = 0; k < g_rotQ.size() && k < 64 && !qFired; ++k) {
            const RotQEntry& e = g_rotQ[k];
            if (e.id != 2900 || e.rotIdx < 0 || ((c.rotSpent >> k) & 1ull)) continue;
            const RotTrig& r = g_rotTrig[(size_t)e.rotIdx];
            // --shadowrotbook: on a tick the shadow stepped, the slots its pass turned the body with are the touches (a channel-only slot, which the pass leaves to the queue, keeps the old test)
            const bool hitK = (g_shadowRotBook && t_rotBookOn && !e.chanOnly) ? (((t_rotBookBits >> k) & 1ull) != 0) : rotTouchHit(r, twx, twy, playerHalf(c.mode, c.mini != 0));
            if (!hitK) continue;
            c.rotSpent |= (uint64_t)1 << k;
            if (e.swarm && e.swch >= 0 && e.swch <= 15) {
                c.rotChan = (uint8_t)e.swch;
                if ((unsigned)(r.gndDir - 2) < 2u) c.rotRev |= (uint16_t)(1u << e.swch);
                else c.rotRev &= (uint16_t)~(1u << e.swch);
            }
            if (!e.chanOnly) {
                if (r.setRev >= 0) c.rev = (uint8_t)r.setRev;
                nf = r.frame;
                nflip = r.setFlip;
                qModY = r.vmodY;
                qOvr = r.ovrVel;
                qNoSlide = r.dontSlide;
                qFired = true;
            }
        }
    }
    if (g_rotQueue && !g_rotQ.empty() && !qFired) {
        double wx, wy;
        fromFrame(f0, cCrossX, (double)c.y, wx, wy);
        // A guard rather than `while (true)`: GD's loop is bounded by the
        // bucket, and a bug here would otherwise hang the search.
        for (int guard = 0; guard < 64; ++guard) {
            const int ch = (int)c.rotChan & 15;
            const int beg = g_rotQBeg[(size_t)ch], end = g_rotQEnd[(size_t)ch];
            if (end <= beg) break;                       // no such channel
            const int done = popCount64(c.rotSpent & g_rotQChanMask[(size_t)ch]);
            int idx = beg + done;
            // --rotqtouch (removed): the cursor is the channel's first element not yet spent -- a touch
            // trigger ahead of it may already be spent by its touch, which the count above would
            // read as the cursor having passed it
            // (always on: the switch --rotqtouch is removed)
            // --rotqtouch (removed): a touch-activated 2900 in the queue does not fire on the crossing; it holds its
            // channel until the player touches it, and once touched the walk passes it without firing it again.
            // GJBaseGameLayer::checkSpawnObjects (0x21a8f0): an element with m_isTouchTriggered (+0x5d0) set
            // stops the walk unless its activated-by-player flag (+0x5b3) is set, and then is skipped with the
            // cursor moved on. The model fired it when the player's x passed it: a custom level's reversed cube
            // at x 2,324 under touch 2900 uid 314 (2,325, 105), 180 px below the player, turned forward where GD
            // kept reversing. The cursor is the channel's first element not yet spent.
            {
                idx = beg;
                while (idx < end && idx < 64 && ((c.rotSpent >> idx) & 1ull)) ++idx;
            }
            if (idx >= end) break;                       // (A) consumed to the end
            const RotQEntry& e = g_rotQ[(size_t)idx];
            // --rotqtouch (removed) (frames.hpp): a touch-activated element holds the channel until the touch
            // spends it (the --rottouch pass above); it never fires on the crossing
            // ...unless its group is switched off (below): a disabled touch element does not hold the channel -- GD's cursor on a custom level (the dump's rotidx) walks past
            // the touch Rotate uid 32505 (group 568, off by the Toggle uid 32480, never touched) at its x, and the 2900 behind it fires at 16522
            int offNet = 0;
            if (g_rotQToggle && (size_t)idx < g_rotQOff.size()) {
                offNet = popCount32((uint32_t)c.trig.word(0) & g_rotQOff[(size_t)idx])
                         - popCount32((uint32_t)c.trig.word(0) & g_rotQOn[(size_t)idx]);
                if ((size_t)idx < g_rotQAutoTog.size())
                    for (const RotQAutoTog& g : g_rotQAutoTog[(size_t)idx]) {
                        const long long F = g_autoTrig[(size_t)g.autoIdx].fireT;
                        if (F >= 0 && t >= F + (long long)std::llround(g.delay)) offNet += g.on ? -1 : 1;
                    }
            }
            if (e.rotIdx >= 0 && g_rotTrig[(size_t)e.rotIdx].touch && offNet <= 0) break;
            const bool rev = ((c.rotRev >> ch) & 1u) != 0;
            const bool vertical = (f0 & 1) != 0;
            const double ref = vertical ? wy : wx;
            const double p = vertical ? e.py : e.px;
            // (B2)-(B5). Forward fires on `ref >= p`, reverse on `ref <= p`;
            // the same value fires in both, which is GD's own asymmetry.
            if (!rev) { if (ref < p) break; }
            else      { if (p < ref) break; }
            if (idx < 64) c.rotSpent |= (uint64_t)1 << idx;
            // --rotqtoggle: the object sits in a group this state has switched
            // off by entering a touch Toggle's box. GD skips triggerObject for
            // it (checkSpawnObjects 0x21aad8 on +0x28e, which toggleGroup
            // 0x223cbc sets) and moves the cursor on anyway (0x21ab62), so it is
            // consumed here with neither the switch nor the rotation half.
            // ...and the same counter for an AUTONOMOUS (x-crossing) Toggle's off / on (frames.hpp g_rotQAutoTog), dated by the Toggle's own fireT:
            // the entry is skipped while the offs fired by tick t outnumber the ons (a custom level:16607, uid 32481 in group 568, off by the Toggle uid 32480).
            if (offNet > 0) continue;
            // The switch half: only a 2900 with `swarm` moves the active
            // channel, and the reverse it writes is the pure predicate
            // `gnddir - 2 <u 2` -- no mapping through the frame. The id check
            // is not defensive: ten of lv22's thirty queued objects are 2899
            // Options triggers, and an Options trigger never reaches
            // rotateGameplay -- it is consumed here and does nothing to the
            // channel.
            //
            // [2026-09-10] This used to say "two of them carry
            // m_changeChannel". They carry none: a 2899 is a GameOptionsTrigger
            // and has no such field. The dumper was reading the offsets off the
            // base class, so five of the ten rows came out with swarm=1 and
            // five with swarm=255 -- neither of which was a field. The id check
            // was load-bearing for a reason slightly different from the one
            // written here, and stays.
            if (e.id == 2900 && e.swarm && e.swch >= 0 && e.swch <= 15) {
                c.rotChan = (uint8_t)e.swch;
                const int gd = (e.rotIdx >= 0)
                    ? g_rotTrig[(size_t)e.rotIdx].gndDir : 0;
                if ((unsigned)(gd - 2) < 2u) c.rotRev |= (uint16_t)(1u << e.swch);
                else c.rotRev &= (uint16_t)~(1u << e.swch);
            }
            // ...and the rotation half, which a channel-only trigger skips.
            if (!e.chanOnly && e.rotIdx >= 0) {
                const RotTrig& r = g_rotTrig[(size_t)e.rotIdx];
                if (r.setRev >= 0) c.rev = (uint8_t)r.setRev;
                nf = r.frame;
                nflip = r.setFlip;
                qModY = r.vmodY;
                qOvr = r.ovrVel;
                qNoSlide = r.dontSlide;
                qFired = true;
                if (g_rotWatchLo >= 0) g_rotWatchUid = r.uid;   // --rotwatch, print only
                // ONE ROTATION PER TICK. GD's loop would keep going, and the
                // next element could be eligible in the frame this one just
                // set. It does not happen in the corpus -- lv22's 20 firings
                // are 71 to 2,884 ticks apart -- and the effect code below
                // rebases the state once, so a second turn in the same tick
                // would need it restructured. If a level ever needs it, the
                // symptom is a frame transition GD makes and the model makes
                // one tick late.
                break;
            }
        }
    }
    const bool qUsed = (g_rotQueue && !g_rotQ.empty());
    const RotTrig* best = nullptr;   // the matching trigger nearest in the perpendicular axis
    double bestDv = 1e18;
    float rotModY = 1.0f;          // vy multiplier of the adopted trigger (RotTrig::vmodY)
    uint8_t rotOvr = 0;
    uint8_t rotNoSlide = 0;        // --rotboost: the adopted trigger's Dont Slide (RotTrig::dontSlide)
    bool toggleRev = false;
    for (const RotTrig& r : g_rotTrig) {
        if (qUsed) break;   // the queue above already decided
        // --rotqtoggle, the pre-queue selection's half: a trigger whose object
        // this state has switched off (see the queue branch above) is not a
        // candidate at all.
        if (g_rotQToggle) {
            const size_t k = (size_t)(&r - g_rotTrig.data());
            if (k < g_rotTrigOff.size()
                && popCount32((uint32_t)c.trig.word(0) & g_rotTrigOff[k])
                   > popCount32((uint32_t)c.trig.word(0) & g_rotTrigOn[k]))
                continue;
        }
        // A 2900 that points to the current frame is a **reverse-travel toggle**.
        // GD measurement (2026-08-15, lv22's 14 firings tabulated by frame and
        // travel before/after):
        //   t=18,382 (22575,1755) f0=0 -> 0, dx +1.613 -> **-1.613**, player 4px
        //   from the obj. Neither ug nor the y progression changes, so it is not
        //   a rotation
        //   t=14,489 (16365,1275) f0=0 -> 0 but travel unchanged -- here the player
        //   is **543px** from the obj's cy, a firing that fails the proximity test
        // The other 12 all actually change the frame. So it is not "points to the
        // same frame = do nothing" but "reverse the travel direction".
        // In an export that has gnddir, the frame and rev this trigger sets are
        // known, so "same frame means toggle" is not needed. If the heading is
        // already the same it is naturally a no-op (lv22's uid11177 is exactly that).
        const bool same = (r.setRev < 0) ? (r.frame == f0)
                                         : (r.frame == f0
                                            && r.setRev == (int)c.rev);
        const int shot = same ? r.revT : r.firedT;
        if (shot >= 0 && shot != (int)t) continue;
        const double ru = frameU(f0, r.cx, r.cy);
        // In reverse travel (State::rev) the travel coordinate **decreases**, so a
        // forward-only crossing test can never step on it. [2026-08-16] lv22
        // t=11,965: GD turns to frame 1 on uid6307 (15,405,859), but the model only
        // saw u drop 15,405.5 -> 15,404.2, never once judged it "crossed", and
        // dropped the rotation entirely.
        // The DP side's group latch already looks at grev (the layer prologue), so
        // only this spot had been left behind.
        // --rottouch (frames.hpp): a touch trigger the player's box overlaps fires without the
        // crossing and the windows below
        bool touchHitR = false;
        if (g_rotTouch && r.touch) {
            double twx, twy;
            fromFrame(f0, cCrossX, (double)c.y, twx, twy);
            touchHitR = rotTouchHit(r, twx, twy, playerHalf(c.mode, c.mini != 0));
        }
        const bool crossed = touchHitR || (c.rev
            ? (uPrev > ru && cCrossX <= ru)
            : (uPrev < ru && cCrossX >= ru));
        if (!crossed) continue;
        // ...and the player has to be AT the trigger, not merely past its
        // travel coordinate. `frameU` is the trigger's world Y once f0 is odd,
        // so without this a trigger ANYWHERE in the level matches: lv22's
        // uid5957 sits at world (8,297,423) -- 1,548 px away -- its frame-3 u
        // is 423, and the model turned back to frame 0 at t=4,688 on it and
        // flipped its gravity, which GD never does.
        // GD's own firings (hooked GJBaseGameLayer::rotateGameplay, `rot:`):
        //   t=1,745 uid1343 obj(2265,316)   player(2265.4,316.5)
        //   t=1,816 uid1215 obj(2143.5,225) player(2143.5,224.3)
        //   t=4,664 uid4355 obj(6735,399)   player(6735.3,399.0)
        //   t=5,120 uid4467 obj(6875,885)   player(6878.3,885.3)
        //   t=5,968 uid5809 obj(8247,963)   player(8247.2,961.0)
        // In all five the player is ON the trigger in both axes within 3.3 px,
        // which is why this used to test both. **BOTH HALVES ARE WRONG.**
        // Falsified 2026-08-15 on lv22's own cold run, twice, from GD's dump
        // for the plan the DP built (model and GD bit-identical either side):
        //   t=1,746  frame 0 -> 1 on uid1343 (2,265, 316) with the player at
        //            (2,265.4, **1,306.2**) -- 990 px ABOVE it in y
        //   t=2,579  frame 1 -> 0 on uid1215 (2,143.5, 225) with the player at
        //            (**5,027.8**, 224.8) -- 2,884 px away in x
        // The travel axis is the only one that matters: in frame 0 the player
        // crossed the trigger's world X, in frame 1 it crossed its world Y, and
        // the other coordinate was arbitrary both times. Those five samples
        // were routes that happened to pass over their own trigger.
        // Without this the model plans the whole rotated section in frame 0 --
        // lv22's cold run flew 990 px over the entrance, GD turned, and it died
        // 900 ticks later at x=5,112 with the death looking like a spike.
        //
        // What DOES gate it is the level's own x window: an object is not live
        // until the level has scrolled past it. Dropping the test entirely is
        // wrong in the other direction -- the same replay then turns back to
        // frame 0 at t=1,771 on uid11177 (16,365, 1,275), whose frame-1 travel
        // coordinate the player crosses while it is still at x=2,310, i.e.
        // **14,055 px before that part of the level exists**.
        // `r.cx <= wxP` fits all seven observations at once:
        //   FIRE   uid1343 (2,265)   player x 2,265.4     (frame 0, crossing)
        //   FIRE   uid1215 (2,143.5) player x 5,027.8     (frame 1, 2,884 back)
        //   FIRE   uid4355 (6,735)   player x 6,735.3
        //   FIRE   uid4467 (6,875)   player x 6,878.3
        //   FIRE   uid5809 (8,247)   player x 8,247.2
        //   SKIP   uid11177 (16,365) player x 2,310.5     (14,055 ahead)
        //   SKIP   uid5957 (8,297)   player x ~6,735      (1,562 ahead)
        // In an even frame the crossing already implies it, so this only bites
        // in a turned one -- which is exactly where `frameU` stops meaning
        // "world x" and a trigger anywhere in the level can match by y.
        // The 30 is one tick of slack for the crossing tick itself.
        // OPEN: "has ever reached" would be the literal reading of scrolling
        // and would differ from "is at" for a player that drifted back left
        // inside a turned frame. lv22 never does, so the cheaper form stands
        // (the alternative costs a State field and a dedupe-key bit).
        double wxP, wyP;
        fromFrame(f0, cCrossX, vColl, wxP, wyP);
        if (r.cx > wxP + 30.0) continue;
        // ...and the PERPENDICULAR axis has to overlap. [2026-08-16 revived]
        // The note below says both halves are wrong; the perpendicular half is
        // NOT. GD was asked directly (cfg hitboxtrace=1 -> `rot:` lines) on the
        // x=15,656 plan and named its six firings:
        //   t=1745 (2265,316)   player(2265.4, 316.5)   du 0.4  dv 0.5
        //   t=1816 (2143.5,225) player(2143.5, 224.3)   du 0.0  dv 0.7
        //   t=4435 (6735,399)   player(6735.4, 406.5)   du 0.4  dv 7.5
        //   t=4893 (6875,885)   player(6849.6, 885.8)   du 25.4 dv 0.8
        //   t=5759 (8247,963)   player(8247.0, 961.0)   du 0.0  dv 2.0
        //   t=6093 (8297,423)   player(8275.9, 421.9)   du 21.1 dv 1.1
        // Every |dv| is under 8, and the two it did NOT fire on that same run
        // are uid6308 (15,399,615) |dv|=89.4 and uid6307 (15,405,859)
        // |dv|=330.9 -- the player crossed both travel coordinates.
        // The level's own layout settles it: lv22's 2900s past x=15,000 come in
        // PAIRS 6 px apart in x and 126-366 px apart in y, each half setting the
        // OPPOSITE direction (15,399 gnd=4 / 15,405 gnd=2; 16,005 gnd=1 /
        // 16,011 gnd=3; 16,125 gnd=1 / 16,131 gnd=3). Only the player's height
        // can pick between them, so a travel-axis-only rule cannot be right.
        // Without this the model turned at x=15,405 where GD's gframe stays 0,
        // and planned the rest of the level in a frame GD does not have.
        // The travel-axis CROSSING above stays: it is what stops a fast tick
        // from stepping over the box, and dropping it is the 2026-08-15 fix.
        //
        // [2026-08-24] **30 IS FALSIFIED, and the window was doing the pair's job.**
        // Read off GD's own gframe changes in two full lv22 runs, measured on the
        // tick BEFORE the change (once gframe moves the reported x/y are in the new
        // frame, and comparing those to the trigger's world position is nonsense --
        // that mistake is what made this look settled):
        //   FIRES  0, 0, 0.5, 1.2, 1.3, 2.0, 3.7, 5.6, 6.0, 9.3, 12.0, 14.1, 14.7,
        //          24.4, 25.6, 29.3, and **116.7**
        //   SKIPS  274.4 (uid16911), 657.8 (uid11177)
        // The 116.7 is uid11342 (20115,723) on lv22's cold route -- THE SAME TRIGGER
        // the known-good route fires at 29.3. One trigger, two heights, both fire, so
        // no window of 30 can be right. lv22's 84% wall is exactly this: the model
        // dropped that firing, planned the rest of the level in frame 0, and every
        // route it found there is in a world the game does not have.
        //
        // What the window was really for is the PAIRS (the note above): two triggers
        // 6px apart setting opposite directions, where "only the player's height can
        // pick between them". That is a CHOICE, not a range -- so the choice is made
        // explicitly below (nearest |dv| wins) and the window goes back to being what
        // it says it is, "the player is at this trigger rather than somewhere else in
        // the level". Bracketed by the measurements above at (116.7, 274.4); 150 sits
        // in that gap with ~30% margin either side. The older note's non-firings at
        // 89.4/330.9 are from a lineage this could not re-measure -- if 89.4 is real
        // and not a pair losing to its partner, this is too wide and the regression is
        // where that shows up. ([[gd-constant-justification-goes-stale]])
        const double dv = std::fabs(vColl - frameV(f0, r.cx, r.cy));
        if (dv > kRotPerpWin) continue;
        if (same) { if (r.setRev < 0) toggleRev = true; continue; }
        // NEAREST WINS. The loop used to let the last match overwrite the earlier
        // ones, which is only safe while the window is too narrow for two to match at
        // once -- i.e. the tight window was hiding this. With the pairs 6px apart in
        // travel and 126-366 apart perpendicular, both halves now match and the height
        // has to choose, which is what the note above says GD does.
        // ...and the winner is adopted **inside the loop**, exactly where the old
        // code adopted the last match. That is not a style point: `c.rev` is read
        // by the `same` test at the top of this loop, so a trigger adopted here
        // decides whether the NEXT one counts as a rotation or as a reverse-travel
        // toggle. Deferring the write until after the loop -- which is how the
        // nearest-wins choice was first written -- classifies every trigger against
        // the ORIGINAL heading, and that is a different rule even on a tick where
        // only one trigger ends up winning.
        //
        // Bisected 2026-08-25, in-process cold lv22, same binary either way:
        //   db91ab5 (adopt in the loop)      x=20,135  84%, first veto x=8,294
        //   0a1e8b6 (adopt after the loop)   x=2,266   stuck at the level's FIRST
        //                                             2900, first veto x=2,266
        //   0a1e8b6 with --rotperp 30        x=2,266   -- so it was never the
        //                                             window that did it
        // The pair choice the rewrite was for is kept: a strictly nearer match
        // still overwrites a farther one, and with one match this is the old code.
        if (dv < bestDv) {
            bestDv = dv;
            best = &r;
            if (g_rotWatchLo >= 0) g_rotWatchUid = r.uid;   // --rotwatch, print only
            if (r.setRev >= 0) c.rev = (uint8_t)r.setRev;   // absolute value
            nf = r.frame;
            nflip = r.setFlip;
            rotModY = r.vmodY;
            rotOvr = r.ovrVel;
            rotNoSlide = r.dontSlide;
        }
    }
    if (qUsed) {
        // The queue owns the answer: no toggle rule (a same-frame firing sets
        // reverse as an absolute value from gnddir) and no nearest-match.
        if (!qFired) return -1;
        rotModY = qModY;
        rotOvr = qOvr;
        rotNoSlide = qNoSlide;
    } else if (toggleRev) {
        c.rev = (uint8_t)!c.rev;
    }
    if (nf < 0) return -1;
    // A same-frame firing (one that sets reverse travel as an absolute value via
    // gnddir) changes **only the heading**. Coordinates and velocity stay as they
    // are -- running it through the re-basing below (fromFrame/toFrame and
    // kRotCarry) would replace the vy of an ongoing jump arc with the dx-derived
    // carry. Measured (uid 16659, the note at the head of this function): dx
    // +1.613 -> -1.613 with "the y progression does not change" -- GD's vy
    // simply decays normally across the turn tick. [2026-08-18]
    // The note gave t=18,382; in today's gdref that firing is t=18,467, same
    // uid, 9.1200 -> 8.9050, one gravity step. The tick moved with the base and
    // the uid did not, so match a note's evidence on the uid, not the tick.
    // It is the flip-UNCHANGED case, so it is not a counter-example to the
    // halving below -- it is that rule's negative side.
    if (nf == f0) {
        // --slopedbg: WHICH of the two 2900 branches ran, and what it did to the
        // flip. The two are measurably different in GD and the trace's own
        // columns cannot tell them apart (both leave `frame` where it was for
        // the same-frame one, and neither writes a clamp label):
        //   lv22 t=11,351  frame 0->3   vy 5.7040 -> 5.1930  ratio 0.910  the
        //                  frame-carry value (9245), NOT a halving
        //   lv22 t=20,954  frame 0      vy 15.9450 -> 7.9725 ratio 0.500  a
        //                  halving, and this branch is the one that runs
        if (g_slopeDbg)
            std::printf("rot2900 t=%lld site=samefr uid=%d nf=%d f0=%d "
                        "nflip=%d flip=%d->%d vy=%.4f\n",
                        (long long)t, best ? best->uid : -1, nf, f0, nflip,
                        (int)c.flip, (nflip >= 0 ? nflip : (int)c.flip),
                        (double)c.vy);
        // ...and the polarity change halves vy, because GD reaches this through
        // flipGravity, whose `mulsd 0.5` @0x39a2dc runs whenever the polarity
        // ACTUALLY changes. Measured on every 2900 firing in the corpus, split
        // by branch with the print above (18 firings, lv20/21/22):
        //   same-frame, flip changes    lv22 t=20,954 uid 17039
        //                               15.9450 -> 7.9725, ratio 0.500
        //   same-frame, flip unchanged  t=13,685 / 18,467 / 19,376
        //                               9.1200 -> 8.9050, ratio 0.976, i.e. one
        //                               gravity step -- no halving
        // 4 of 4 with no exception. The frame-changing branch below is a
        // different mechanism and must not get this: its 14 firings replace vy
        // with the frame-carry value (5.193 / 6.457 / 7.8) and their ratios
        // scatter from 0.000 to 3.095.
        // NO MODE GATE. This first excluded ball / robot / spider, on a reading
        // of the ledger's "the fly/bird/dart/swing toggles each halve in both
        // directions (roll/robot/spider do not)". That sentence is about the
        // MODE TOGGLES, not about flipGravity, and the function has no mode
        // branch at all -- read at 0x39a1d0:
        //   0x39a1e9  cmp byte [rcx+0x9bf], dl   ; the requested polarity
        //   0x39a1ef  je  0x39a41f               ; unchanged -> early out
        //   0x39a2cb  cmp byte [rbx+0x7e1], 0
        //   0x39a2d2  jne 0x39a32f               ; set -> skip the halving
        //   0x39a2d4  movsd xmm0, [rbx+0x9a0]
        //   0x39a2dc  mulsd xmm0, [rip+0x288ab4] ; x 0.5
        // Between the early-out and the multiply there is no test of any mode
        // byte, so the halving is unconditional on mode; the two guards are the
        // polarity actually changing and [+0x7e1], which is not modelled here
        // and is not a mode (the mode flags live at +0x9b9..+0x9c4).
        // The corpus could not have caught the mistake either way: all four
        // same-frame firings are cube.
        // No double-halving guard against the portal pass's own `gravChanged`
        // halving (step.hpp:7628): applyRotation is called from the CLI tick
        // loop after the removed old tick's stepOne, not from inside it, and the one firing that halves
        // has no portal on its tick (the objects at x=21,555 are the 2900 itself
        // and two force boxes). If a tick ever carries both, this is where to
        // look.
        if (nflip >= 0 && (uint8_t)nflip != c.flip) {
            VYSET(c.vy) = (float)((double)c.vy * 0.5);
            // (always on: the switch --robotflipnohover is removed)
            // --robotflipnohover (removed): a robot whose gravity a gameplay-rotation 2900 flips (the same-frame firing in applyRotation, the one that halves vy) while it
            // hovers keeps one more hover tick and then falls. PlayerObject::flipGravity (2.209: 0x4f8440) clears the byte at +0x9c9 and so does didHitHead (0x4f1e30), the two
            // events that end a robot's hover this way (--bonkouter (removed) is the second). A custom level, a mini robot flipped one tick after its jump (t=4,832 jump, flip at 4,833,
            // button held to 4,846): GD's vy went 4.472, 2.236 (the flip's half), 2.236 (one hover tick), then 2.430, 2.624, ... (+0.194 a tick, the full gravity step) while
            // the model hovered at 2.236 for the rest of the budget -- the loop's fixups for it crept one tick later on every pass (dy 0.765, 0.809, 0.853, ...) and never
            // converged, and the robot route ended in a search handoff. With the flag the model's trace matches GD's for the whole stretch (4,500..4,900, no row off by more
            // than 0.02). Only that site is covered: a flip by a portal, an orb or a trigger has not been measured against a hovering robot.
            // On since 2026-10-27: the custom level with the robot flip goes from 29 iterations and 4 handoffs to 21 and 0, every other level unchanged.
            if (c.mode == 5 && c.rHover > 1) c.rHover = 1;   // --robotflipnohover (removed): flipGravity ends a robot's hover
        }
        if (nflip >= 0) c.flip = (uint8_t)nflip;
        return nf;
    }
    // The point that carries over is the one GD's collision pass sees: AFTER the
    // move, BEFORE the button (see the re-tap below). the removed old tick's stepOne already applied
    // the tap, so for the one case where that moves the player discontinuously
    // -- a grounded spider -- the tap's y is undone here and redone in the new
    // frame. Measured on lv22 t=1,745: GD's dump reads y=316.500 on the rotation
    // tick, i.e. the parent's y untouched, while the model had already dropped
    // it to 313.500 with the old frame's tap.
    const bool reTap = reTapPress;
    if (reTap && havePrevY) YSET(c.y) = (float)sPrevY;
    double X, Y;
    fromFrame(f0, (double)c.xAbs, (double)c.y, X, Y);
    double nu, nv;
    toFrame(nf, X, Y, nu, nv);            // no origin correction is needed
    c.xAbs = (float)nu;
    YSET(c.y) = (float)nv;
    // ---- vy hand-over [2026-08-19 settled -- replaces the whole old kRotCarry set] --
    // Cross-checking the disassembly of PlayerObject::rotateGameplay (0x399d50)
    // against lv22's raw level data (keys 169/582/583/584 of the 2900s) closed all
    // 15 of gdref's transitions, all 15 exactly (measured 2026-08-19):
    //   The velocity swap runs **only when** the vertical/horizontal flag
    //     (mvdir 3/4 = vertical) changes: yvel := travel speed (= |dx|/0.25, always
    //     positive) x modY. The old vy is discarded (outside platformer mode the
    //     old-vy component is thrown away wholesale).
    //   modY is per trigger instance: if m_editVelocity(169) is set, then
    //   m_velocityModY(583, member default 0.0); otherwise pass-through (x1).
    //   m_overrideVelocity(584) is an absolute assignment, not a multiply (no
    //   example in lv22).
    //   vertical/horizontal = parity of the true frame. A transition that keeps the
    //   parity (true 2->0, gdref t=13,646) runs no swap at all and leaves vy as
    //   is -- in the model's representation that is same frame + rev, so the
    //   nf==f0 early return above already behaves that way.
    // Every constant and exception of the old implementation was a misreading of
    // this law:
    //   kRotCarry 0.90 = (dx/0.25)/(dx/0.225). The ball's 0.99 was modY 1.1
    //   misread (6.457x1.1 = 7.175x0.990, indistinguishable from one sample).
    //   t=4,665's "starts from rest" = editVelocity=1 with 583 missing (x0).
    //   t=6,094's "mystery of the ball's magnitude" = modY -1.21. t=16,956's
    //   "robot has no mirror" = modY +1 (the paired trigger at t=4,894 is -1 -- it
    //   was the trigger's sign, not a mode mirror).
    // Mapping onto the internal convention: the model's frame 3 has +v=-X (the
    // parser's kUpFor correspondence), opposite to GD's local up (+X), so only
    // enf==3 flips the sign. gdref measurements, 3 points:
    //   t=11,344 (0->3)                     GD +5.193 -> internal -5.192
    //   t=11,626 (true 3->2 = frame0+rev1)  GD +5.193 -> internal +5.192
    //   t=1,817  (1->0)                     GD -5.193 -> internal -5.193
    // dxUsed arrives **negative** in reverse travel, so only its magnitude is used
    // (measured lv22 t=11,965, f0=0 rev0=1 -> nf=1: with the sign included the
    // world-x direction is opposite to GD's).
    const int ef0 = (f0 + 2 * rev0) & 3;
    const int enf = (nf + 2 * (int)c.rev) & 3;
    if (((ef0 ^ enf) & 1) != 0) {
        double vyGd = (double)rotModY * std::fabs(dxUsed) / 0.25;
        if (rotOvr) vyGd = (double)rotModY;   // absolute assignment (no example in lv22)
        VYSET(c.vy) = (float)(vyGd * (enf == 3 ? -1.0 : 1.0));
        // --rotboost: the same swap block raises the velocity-limit exemption (0x399faa); only
        // Dont Slide (585) lowers it again (0x39a00d). Measured on official level 22 t=16,496
        // (frame 0 -> 3, ship, vy := 7.8): GD's vy runs 7.886 / 7.972 / 8.058 / ... / 8.55, past
        // the 8.0 clamp, with +0.086 released and +0.064 held -- the latched rungs (+0.086 /
        // +0.108) plus the force box's -0.172. The model, unlatched, sat at 6.4 + 0.172.
        // Both writes are unconditional stores, so Dont Slide does not leave the byte as it was:
        // 0x399faa stores 1 and 0x39a00d then stores 0, and a body that arrived latched leaves
        // the swap unlatched.
        if (g_rotBoost) c.boost = rotNoSlide ? 0 : 1;
    }
    // [2026-08-21 r52] Carry "the tick the frame changed" one tick forward
    // (see the note on State::frameChg. The rotation hits **after** the removed old tick's stepOne, so an
    //  assignment on the the removed old tick's stepOne side alone never sets it -- that misfired once).
    if ((uint8_t)nf != c.frame) c.frameChg = 1;
    c.frame = (uint8_t)nf;
    // --dashframe3 (removed): the rotation hits after the removed old tick's stepOne, so a dash ring that fired on this tick took its slope in the old frame (a ring at rot -90 on frame 0 is the +70 clamp);
    // GD reads the frame at the ring's own activation, which comes after the spawned rotation turned it. A dash going on when the frame turns to 3 is given the slope
    // of the ring that started it, from the new frame.
    // (always on: the switch --dashframe3 is removed)
    // --dashframe3 (removed): a dash ring met on GD's frame 3 (the player travelling up) takes its slope from the same clamp as on frame 0, with the frame's
    // -90 (+0x9c3) put around it, so a ring whose rot is -90 -- along the travel axis -- dashes straight on. Without it the slope is the raw tan(-rot), which the clamp
    // turns into tan 70 = 2.747 for a ring at -90 (or infinity): a custom level's UFO-less cube at t=4,542 takes the dash ring uid 8358 (rot -90) with the gameplay
    // rotation uid 8364 on the same spot (GD frame 3 from that tick): GD moves y +2.4 a tick for two ticks and x not at all; the model moved y the same and x -6.594 a tick
    // (2.4 x 2.747) and was 13 px off the cross axis, then died at t=4,583 where GD dies at 4,624. Only frame 3 is measured; frame 2 (+180) is not.
    if (g_slopeDbg)
        std::printf("dashframe3 t=%lld nf=%d f0=%d dashing=%d usedOrb=%d type=%d slope=%.4f\n", (long long)t, nf, f0, (int)c.dashing,
                    c.usedOrb ? c.usedOrb->uid : -1, c.usedOrb ? (int)c.usedOrb->type : -1, (double)c.dashSlope);
    if (nf == 3 && f0 != 3 && c.dashing && c.usedOrb
        && (c.usedOrb->type == 37 || c.usedOrb->type == 38))
        c.dashSlope = dashSlopeOf(c.usedOrb, 3);
    // ...and the trigger SETS the gravity direction too (m_moveDirection, the
    // `mvdir` column). The model used to carry `flip` across a rotation, which
    // is only right when the trigger happens to agree. Measured 2026-08-15 on
    // lv22's cold run, GD's dump against the trace's own `flip` column:
    //   t=1,746  uid1343 mvdir=4 (right), nf=1: GD upsideDown stays 1
    //   t=2,579  uid1215 mvdir=2 (down),  nf=0: GD upsideDown **1 -> 0**
    // The carry got the first one right by luck and the second one wrong, and
    // from that tick the model's gravity stepped +0.216/tick where GD stepped
    // -0.216 -- the whole post-rotation section solved upside down.
    if (g_slopeDbg)
        std::printf("rot2900 t=%lld site=newframe uid=%d nf=%d f0=%d "
                    "nflip=%d flip=%d->%d vy=%.4f\n",
                    (long long)t, best ? best->uid : -1, nf, f0, nflip,
                    (int)c.flip, (nflip >= 0 ? nflip : (int)c.flip),
                    (double)c.vy);
    if (nflip >= 0) c.flip = (uint8_t)nflip;
    // Entering frame 3 flips `upsideDown`. Measured directly with the trace's
    // own `flip`/`frame` columns against GD's dump: the two agree on every tick
    // up to t=4,664 and disagree from t=4,665 -- the rotation tick itself --
    // where GD goes up 0 -> 1 and the model stays 0. The frame-1 rotation
    // (t=1,745) does NOT flip it, which is the odd frames' opposite handedness
    // showing up (`toFrame` gives frame 1 v = +X, frame 3 v = -X).
    // Without this a type-4 portal asks `c.flip != 0`, reads 0, and is skipped
    // as inert -- GD fires it at t=5,110 and halves vy (12.824 -> 6.413).
    // (the frame-3 mirror is handled by gdUpOf, where GD's statement is read --
    // flipping it HERE puts the sign in the physics and makes the player fall
    // the wrong way in world; tried and reverted)
    // (the ground flag is kept across the turn, as GD's rotateGameplay does)
    c.snapObj = nullptr;
    c.snapDist = 0.f;
    c.onSlope = 0;
    c.slopeM = 0.f;
    // --ringmemrot (removed) (speed.hpp): GD does not offer a ring it has fired again while the body is still in contact with it, whatever the frame does after the
    // fire, so the rings the state has used are kept across the turn -- BY UID: usedOrb is a pointer into the old frame's objects and the new frame's
    // candidates are other objects, so it is moved into usedOrbOld (the history takes what that held), which the ring gates read by uid.
    // (always on: the switch --ringmemrot is removed)
    // --ringmemrot (removed): applyRotation forgets the rings the body has used (usedOrb / usedOrbOld / usedOrbHist) on every frame change; with the flag they stay (by uid: usedOrb moves into usedOrbOld, as a pointer into the old frame's objects means nothing in the new one), as GD's
    // once-per-attempt activation mark does. A dash ring (uid 8358, rot -90) fires at t=4,541 and the body turns into frame 3 on the next tick; the model wipes the ring's
    // record there and the press at t=4,560, with the body back over the ring's top edge, fires it again (the dash line from 4,562: vy 0 and the cross axis frozen). Measured
    // in GD with the plan flown with and without that press: the rows are identical (vy -3.456, -3.672, -3.888 ... on 4,560.. with no second dash), and the ring is never offered
    // to the body again -- no ringJump call for it after the fire although the body overlaps its rect until t~4,560 (orbtrace: calls every tick from 4,530 to 4,541, none after).
    if (c.usedOrb) {
        if (c.usedOrb->uid >= 0) {
            if (c.usedOrbOld >= 0 && c.usedOrbOld != c.usedOrb->uid) {
                c.usedOrbHist[2] = c.usedOrbHist[1];
                c.usedOrbHist[1] = c.usedOrbHist[0];
                c.usedOrbHist[0] = c.usedOrbOld;
            }
            c.usedOrbOld = c.usedOrb->uid;
        }
        c.usedOrb = nullptr;
    }
    for (int i = 0; i < 4; ++i) c.touchRing[i] = -1;
    c.touchRingT = 0;
    // The flying band is the WORLD's and a rotation does not take it away: the turn used to put it back to "none" ([0, 1e9], bandRefY 0), which made the shadow run every flier of the turned frame as Free Mode
    // (shadow_step.hpp: `!banded` -> `L.freeMode`) and skip the recorded band it reads in frames 1 and 3. [M] GD, 11-18, lv22 rotation 11342 at (20115, 723), a ship entering frame 3 (tick 17014, y 735): pmin / pmax
    // keep following the camera ([594.3, 1022.8] on the turn tick, camscale 0.7) and, once the static camera at (19905, 945) has fired (t=17030), stand at [415.714, 844.286]; the ship sits on 844.286 - 15 =
    // 829.286 for 14,000 ticks, and the model, with no band, flew on up the shaft (E10_coins_a589807d it=15 / 16, wedge at (20115, 829); the same phenomenon the 11-12 4880fb9c found for an anchor in a turned frame).
    // The band, its reference y and the camera Offset Y it carries (bandOffY) all stay with the body across the turn -- for a SHIP, the one mode this is measured for. Any other mode keeps the old reset: the state's band
    // is written by a mode portal (or an anchor) and is only cleared by the old reset, so a ball / cube that has left a ship section would carry that section's band through the turn, and the shadow would clamp it
    // against the recorded row GD wrote for another mode: [M] quick lv22 (base de614ba9 vs the all-modes carry), the sections t=5800 / 6200: a ball at y 439.5 on the tick GD's row reads [90, 387] was pushed to 387 - 15 = 372
    // (GD does not clamp it), tracking 3420 -> 670 and 3020 -> 270 ticks. And only INTO a turned frame (1 or 3), where the shadow reads the recorded band and there is no writer to wait for: a turn back to an upright frame rewrites
    // the band from the camera on that very tick, which the model does not do. [M] 604 no recording, a custom level:4600 (gd-06): frame 3 -> 0 on tick 4799, a ship; GD's pmin / pmax jump [2657.6, 3462.4] -> [3110, 3610]
    // (camscale 0.4 -> 0.6 on the same tick, freemode 0, bandmode 1: GD clamps it), the carried band [2734.6, 3245.1] is stale and the window, tracked until then, splits at 4800.
    if (!(c.mode == 1 && !c.dual && (nf == 1 || nf == 3))) {
        c.bandFloor = 0.f;
        c.bandCeil = 1e9f;
        c.bandRefY = 0.f;
    }
    // ---- the tick's BUTTON, run again in the NEW frame -----------------------
    //
    // GD's per-tick order is update -> checkCollisions -> buttons. The rotation
    // trigger fires in the collision pass, so a tap on the SAME tick is served
    // by a world that has already turned. the removed old tick's stepOne applied it in the old frame
    // and everything above then overwrote x, y and vy, so the tap vanished.
    //
    // Measured on lv22's cold run (2026-08-16), the entrance to the first
    // rotated section, model and GD bit-identical up to the tick:
    //   t=1,744  both (2,264.850, 316.500) vy=0.000 frame 0
    //   t=1,745  GD (**2,143.500**, 316.500) vy=**-1.000** frame 1
    //            model (2,266.149, 313.500) vy=+5.193 frame 1
    // -1.000 is the teleport's own signature (see the spider tap in the removed old tick's stepOne), and
    // 2,143.5 is a landing along the NEW frame's vertical (world X). The model
    // kept the rotation's carry and never teleported, and every tick after that
    // belongs to a different run -- the whole reason lv22's cold stalled at
    // x=2,275 with a frontier that otherwise reaches x=23,513 of 24,085.
    //
    // Only the SPIDER needs this: it is the one mode whose button moves the
    // player discontinuously along the gameplay vertical, which is the axis the
    // rotation just redefined. A cube/ball/ship impulse is a velocity, and the
    // rotation sets the velocity itself from the carry.
    if (reTap) {
        Level* nlv = levelOfFrame(nf);
        if (nlv) {
            const double pH = c.mini ? kSpiderHalfMini : kSpiderHalf;
            // the new frame's near list around the landing column
            std::vector<const Obj*> nr;
            const double lo = (double)c.xAbs - 40.0, hi = (double)c.xAbs + 40.0;
            auto lb = std::lower_bound(
                nlv->objs.begin(), nlv->objs.end(), lo - 80.0,
                [](const Obj& o, double v) { return o.cx < v; });
            for (auto it = lb; it != nlv->objs.end() && it->cx <= hi + 80.0; ++it)
                nr.push_back(&*it);
            nlv->dyn.collect(Dynamics::NEAR, lo, hi, nr);
            StepCtx NK{};
            NK.near = &nr;
            NK.t = t;
            double bestY = 0.0;
            bool hazTgt = false;
            if (spiderTargetY(NK, (double)c.xAbs, (double)c.y, c.flip != 0,
                              c.mini != 0, pH, bestY, nf == 0, &hazTgt, nf,
                              c.rev != 0)
                && !hazTgt) {
                const double gs = c.flip ? -1.0 : 1.0;
                YSET(c.y) = (float)bestY;
                c.flip = c.flip ? 0 : 1;
                VYSET(c.vy) = (float)(1.0 * gs);   // the teleport's own +-1.000
                c.spiderJumpT = 0;   // GD's +0x820 (State::spiderJumpT)
                c.grounded = 1;
                c.snapObj = nullptr;
            }
        }
    }
    return nf;
}

// Fly-rings (speed.hpp): the rings a ship or UFO takes through pushButton. Only the impulse rings
// -- yellow 11, pink 12, gravity 13, green 29, red 35, drop 32 -- take this path; dash, spider,
// teleport and custom rings keep the old one.
inline bool flyEarlyRingType(int type) {
    // (always on: the switch --flydashring is removed)
    // --flydashring (removed): ...the dash rings (37, and 38 that also turns gravity) take that path too. A custom level's
    // gravity dash ring uid 68437 (id 1751, rot -70) and a UFO, the plan's press swept one tick at a time in GD:
    // it fires for presses on 10,460..10,464 and not on 10,459 or 10,465, and the row after the press already
    // carries the turned gravity at the y the press found -- the buttons phase of that tick, as the impulse
    // rings. The model fired it through the late loop a tick later, from the row the flap had moved, for
    // presses on 10,458..10,461.
    return type == 11 || type == 12 || type == 13 || type == 29 || type == 35 || type == 32
        || (type == 37 || type == 38);   // (speed.hpp)
}

// Was `uid` fired before usedOrbOld? (speed.hpp, the note that was --ringonce (removed)'s)
inline bool ringFiredBefore(const State& s, int uid, bool second = false) {   // second: the second body's map (the first body's by default)
    if (uid < 0) return false;
    // --spentglobal (speed.hpp): a ring the anchor was told GD fired before t0 stays spent, whatever the five slots below still hold
    if (g_spentGlobal) {
        const std::vector<uint8_t>& m = second ? g_spentMap2 : g_spentMap1;
        if ((size_t)uid < m.size() && m[(size_t)uid]) return true;
    }
    for (int i = 0; i < 3; ++i)
        if (s.usedOrbHist[i] == uid) return true;
    return false;
}

// --hsshadow (constants.hpp g_hsShadow): GD's press bytes, written by the table's writers only (notes/pushbutton-state-table-2091-1027.md, section 4). Each helper is
// a no-op without the flag. State::gdHS bits: H 1, S 2, Hm 4, Sm 8, RF 16, ring slots A / B / C 32 / 64 / 128.
//
// THE MODEL'S STEP IS HALF A GAME STEP OUT OF PHASE with the game's: the model's step N runs the game step N-1's update, tail and collision pass with the button as it
// stood after the game's button pass N-1, and then takes the button pass N. The shadow therefore keeps TWO registers while a step runs:
//     Old = State::gdHSb  the bytes the update / collision part sees (the previous row's), spent by the held-button consumers, mirrored at the update's tail, and rung
//                         by the collision pass's rings;
//     New = State::gdHS   the bytes after this step's button pass -- the row the game's hstrace.csv takes: the press or release edge, then what a fresh press does
//                         at once (pushButton's updateJump(0) jump, its ring scan, the spider's tap).
// With no edge the two are one byte; a release edge zeroes New's H, S but leaves the mirror to Old's tail; a press edge resets New whole (pushButton sets H, S, the
// mirrors, clears the slots and RF) and nothing the old part did to the mirrors or slots survives it. State::gdAux holds, per body (the low nibble the one being stepped, the high nibble the other; swapHalves swaps them): bit 0 the no-control window held the button back,
// bits 1-2 this step's edge (1 press, 2 release), bit 3 the early fly ring (below) has taken the press, bits 4 / 5 a flap / flip (S) or a dash stop (H) spent a press
// this very step's button pass made, which the game does one step later (hsTop applies them).
enum : uint8_t { kHsH = 1, kHsS = 2, kHsHm = 4, kHsSm = 8, kHsRF = 16, kHsSlotA = 32, kHsSlotB = 64, kHsSlotC = 128 };

// bit 4 of the Old register is RF, which only the New register needs: it carries "this step's button pass pressed" while the step runs
inline int hsEdge(const State& c) { return (c.gdHSb & kHsRF) ? 1 : 0; }

// A ring fired: usedOrb takes it and the one it displaces moves to usedOrbOld -- and the one
// usedOrbOld held moves into the history instead of being forgotten.
inline void noteRingFired(State& c, const Obj* ob) {
    if (g_histStatOn && ob) {
        ++g_hist.orbFired;
        if (std::find(g_histFired.begin(), g_histFired.end(), ob->uid) != g_histFired.end())
            ++g_hist.orbRefire;
        else
            g_histFired.push_back(ob->uid);
        if (c.usedOrb && c.usedOrb != ob && c.usedOrbOld >= 0 && c.usedOrbHist[2] >= 0)
            ++g_hist.orbDrop;
    }
    if (c.usedOrb && c.usedOrb != ob) {
        if (c.usedOrbOld >= 0) {
            c.usedOrbHist[2] = c.usedOrbHist[1];
            c.usedOrbHist[1] = c.usedOrbHist[0];
            c.usedOrbHist[0] = c.usedOrbOld;
        }
        c.usedOrbOld = c.usedOrb->uid;
    }
    c.usedOrb = ob;
}

// --ringpresseat (removed) (speed.hpp): a toggle ring fired. It is no Obj of the ring passes, so it does not take
// usedOrb: its uid goes into usedOrbOld, the one there moving into the history, and usedOrb keeps the
// last ring that is one. Per body, as the rest of the memory (swapHalves).
inline void notePressRingFired(State& c, int uid, bool second = false) {
    if (uid < 0 || c.usedOrbOld == uid || ringFiredBefore(c, uid, second)) return;
    if (c.usedOrbOld >= 0) {
        c.usedOrbHist[2] = c.usedOrbHist[1];
        c.usedOrbHist[1] = c.usedOrbHist[0];
        c.usedOrbHist[0] = c.usedOrbOld;
    }
    c.usedOrbOld = uid;
}

// --tporder (constants.hpp): does GD's collision pass reach the object at (ox, oy) AFTER the teleport
// at (tcx, tcy) on the tick the teleport fires -- so that it is tested at the landing site? The pass
// walks the 3x3 section cells around where the player was when it began (px, py: before the teleport
// moved it), x-major then y, a cell's objects in ascending uid, and teleportPlayer runs inside that
// walk with the rect re-read after it. A cell is 100 units in x; in y it is 100 units only when the
// level has dynamicLevelHeight (or platformer mode) on, and otherwise the y factor is 0 and every
// object sits in row 0 (loadLevelSettings 0x23ae17; measured from the running game: lv20 x=0.01 y=0,
// a custom level with dynamicLevelHeight y=0.01). A coordinate <= 0 is cell 0 (addToSection 0x226500).
// (never on: the switch --sectionround is removed)
// --sectionround (off): the section cell of an object is its x / 100 ROUNDED, not truncated (step.hpp, gdSectionCell). A custom level's mini wave
// at t=29,517: a teleport portal (uid 158635, x 56,907) moves it from y 771 to 730 and GD fires a cube portal (uid 158644, x 56,895, y 674..760) on the
// same tick, which it can only do if the pass reaches the portal after the teleport; truncated, the cells are 568 and 569 and the model walks the
// cube portal first (at the old y, no contact), then never meets it (the next tick the wave is 20.9 px off in x): it stays a wave where GD is a
// cube. Rounded, both are cell 569 and the uid order puts the cube portal after the teleport.
// SUPERSEDED by --sectionuid (removed) (below): rounded cells put the same plan's next teleport wrong (t=30,399, a speed portal whose cell becomes the one after the
// teleport's: the model steps 1.35 px short from t=30,400). Kept for the record; do not turn it on.
inline int gdSectionCell(double v) { return v <= 0.0 ? 0 : (int)(v * 0.01); }

// --solidorder (removed) (speed.hpp): the near list in the order GD resolves solids, descending uid. A local
// copy per call, not a shared buffer: a step can run another step inside its collision loop.
inline void solidOrderInto(const std::vector<const Obj*>& near, std::vector<const Obj*>& out) {
    out.assign(near.begin(), near.end());
    std::stable_sort(out.begin(), out.end(),
                     [](const Obj* a, const Obj* b) { return a->uid > b->uid; });
}

// --revcarry (speed.hpp): PlayerObject::reversePlayer's book-keeping. A reverse ring or pad at x `objX` is met with the player at `xNow`:
// the mirror image of where it stands is owed, revOff += 2 * (objX - (xNow + revOff)), and update pays it back (the removed old tick's stepOne's advance).
// A turned frame reads the y difference instead (+0x9c3); those stay as they were.
// Only a level with kA42 (g_reverseSync) pays it: without, GD keeps the plain step after the flip
// (measured on a level without kA42: four reversals, step ratio 1.0000 against the plain step at each).
inline void revReflect(State& c, double objX, double xNow) {
    if (c.frame != 0 || g_reverseSync == 0) return;
    const double R = (double)c.revOff;
    c.revOff = (float)(R + 2.0 * (objX - (xNow + R)));
}

// --ringorder (removed) (speed.hpp): a ring's place in m_touchedRings (State::touchRing), 4 when absent.
inline int touchRank(const State& s, int uid) {
    for (int i = 0; i < 4; ++i)
        if (s.touchRing[i] == uid && uid >= 0) return i;
    return 4;
}

// `foot` (--earlyring, speed.hpp): the pick for a press on foot, which leaves the teleport orb to its
// contact path.
inline const Obj* flyEarlyRing(const State& s, const StepCtx& K, double stepDx, bool foot = false) {
    (void)stepDx;
    const Obj* pick = nullptr;
    int pickRank = 4;
    for (const Obj* ob : *K.orbs) {
        // --teleportorb: a ship's or UFO's teleport orb goes this way too -- calib_teleportorb's
        // ship and UFO arrive on the tick after the press, at the target with none of its advance,
        // which is the buttons phase of that tick (the note above)
        // --flyspiderring: ...and a spider orb (the caller runs its search, applyFlySpiderRing)
        if (!flyEarlyRingType(ob->type) && (foot || (ob->type != 46 && !(ob->type == 43))))
            continue;
        const bool reuse = ob->multiAct;   // --multiorb (the late loop's note)
        if (!reuse && (ob == s.usedOrb || (ob->uid >= 0 && ob->uid == s.usedOrbOld))) continue;
        if (!reuse && ringFiredBefore(s, ob->uid)) continue;
        // the array pushButton walks is the previous tick's end state's (State::touchRing)
        const int rk = touchRank(s, ob->uid);
        // --flydashring (removed): a dash ring fires only while that end state is in contact with it, not from
        // the array's carry-over of the tick before -- the sweep's press on 10,465 found the ring
        // touched on its own row (10,465) and not on the next, and GD flapped; lv22's 1704 (press
        // 9,426: contact 36.30 on that row, 39.52 on the next, threshold 39.21) did not dash either.
        if ((ob->type == 37 || ob->type == 38)
            && (rk >= 4 || !((s.touchRingT >> rk) & 1)))
            continue;
        // --earlyring: ...and on foot every ring does. A custom level, t=349: a falling cube presses with
        // moving red ring uid 298 last in contact two ticks before (|dy| 32.18 against 33, then 35.66),
        // and GD's vy runs on at -14.981 / -15; fired from the array's carry-over, the model went +15.76.
        if (foot && (rk >= 4 || !((s.touchRingT >> rk) & 1))) continue;
        if (rk < pickRank) { pick = ob; pickRank = rk; }
    }
    return pick;
}

// --robothoverwarp (removed) (constants.hpp): the robot's hover budget in ticks at time warp `tScale`
inline uint8_t robotHoverTicks(double tScale) {
    // (always on: the switch --robothoverwarp is removed)
    // --robothoverwarp (removed): the dt that grows the budget is the time warp's (updateJump's own argument), so under a warp
    // the hover lasts 1.5 / (0.1 x 0.225 x warp) ticks: 75 at 0.9. A custom level: an upside-down robot jumps at
    // t=8,578 under a 0.9 warp; GD's +0x830 climbs by 0.02025 a tick and the hover runs 75 ticks, the model's 67.
    if (tScale == 1.0 || tScale <= 0.0) return (uint8_t)kRobotHoverTicks;
    const double inc = (double)(float)(0.225f * (float)tScale) * 0.1;   // updateJump: +0x830 += dt * 0.1
    const int n = (int)std::ceil(1.5 / inc - 1e-9);
    return (uint8_t)std::clamp(n, 1, 255);
}

// --tpexitmove (modifiers.hpp): how far the Moves that carry teleport `uid`'s exit have taken it by tick t
inline double tpExitShift(int uid, long long t, bool yAxis) {
    if (!g_tpExitMove || g_tpExitMoves.empty()) return 0.0;
    const auto it = g_tpExitMoves.find(uid);
    if (it == g_tpExitMoves.end()) return 0.0;
    double d = 0.0;
    for (const TpExitMove& m : it->second) {
        const long long F = g_autoTrig[(size_t)m.a].fireT;
        if (F < 0 || t < F) continue;
        const double u = m.durT > 0.0 ? std::min(1.0, (double)(t - F) / m.durT) : 1.0;
        d += (double)(yAxis ? m.dy : m.dx) * gdEase(m.ease, m.erate, u);
    }
    return d;
}

inline double tpExitTurn(int uid, long long t) {
    if (!g_tpExitRot) return 0.0;
    const auto it = g_tpExitRots.find(uid);
    if (it == g_tpExitRots.end()) return 0.0;
    double turn = 0.0;
    for (const TpExitRot& r : it->second) {
        const long long F = g_autoTrig[(size_t)r.a].fireT;
        if (F < 0 || t <= F) continue;
        const double u = r.durT > 0.0 ? std::min(1.0, (double)(t - F) / r.durT) : 1.0;
        turn += r.total * gdEase(r.ease, r.erate, u);
    }
    return turn;
}

inline const bool kTpExitHookSet = (gdcol::g_tpExitShiftHook = &tpExitShift, gdcol::g_tpExitTurnHook = &tpExitTurn, true);

// The dash line's slope for ring `ob` (dy per px of x). --dashclamp (speed.hpp): startDashing's
// angle, in its float arithmetic; otherwise, and on a turned frame, tan(-rot).
inline float dashSlopeOf(double rot, bool flipX, int frame) {
    // --dashframe3 (removed) (speed.hpp): GD's frame 3 adds -90 around the clamp (+0x9c3), so the ring's angle is measured from the frame's own travel axis
    const bool f3 = frame == 3;
    if (frame != 0 && !f3) return (float)std::tan(-rot * 3.14159265358979 / 180.0);
    float a0 = -((float)rot + (flipX ? 180.f : 0.f));
    if (f3) a0 += 90.f;   // (added only here: `+ 0.f` would turn a -0 into +0 and move every flag-off dash trace by a sign of zero)
    float a = std::fmod(a0, 360.f);
    if (a > 180.f) a -= 360.f;
    else if (a < -180.f) a += 360.f;
    if (std::fabs(a) > 90.f) {
        a = (a > 0.f ? 180.f : -180.f) - a;
        if (a > 180.f) a -= 360.f;
        else if (a < -180.f) a += 360.f;
    }
    if (a > 70.f) a = 70.f;
    else if (a < -70.f) a = -70.f;
    const float r = a * 0.017453292f;
    return std::sin(r) / std::fabs(std::cos(r));
}

inline float dashSlopeOf(const Obj* ob, int frame) {
    return dashSlopeOf(ob->rot, ob->flipX != 0, frame);
}

// ...and a slope already built as tan(-rot) (the anchor's): held inside the +-70 line. A ring that
// points back is not told apart here (speed.hpp).
inline float dashSlopeClamped(float slope, int frame) {
    if (frame != 0) return slope;
    const float lim = std::sin(70.f * 0.017453292f) / std::cos(70.f * 0.017453292f);
    return slope > lim ? lim : slope < -lim ? -lim : slope;
}

// ringJump's writes for those types, on a ship or UFO, applied to the state it fires from. The
// values are the late loop's (applyOrb) with the flying modes' own ratios.
inline void applyFlyEarlyRing(State& c, const Obj* ob, float useDx) {
    if (ob->type == 46) {
        // --teleportorb: the move the late loop puts at the end of the step (the removed old tick's stepOne's tpOrbFired
        // block), here on the state the next tick runs from -- vy kept, the ground under it kept,
        // the press spent (the UFO does not flap on it: calib_teleportorb)
        if (!(ob->tpKeep & 2)) c.y = (float)ob->tpEy;
        if (!(ob->tpKeep & 1)) {
            // --tpback (modifiers.hpp), as the late loop's
            if (g_tpBack && ob->tpEx < (double)c.xAbs) c.tpBack = tpBackAfter(c.tpBack, tpBackObjIdx(ob->uid));
            // --tpjumpnext (removed): this move is in the buttons phase, before the tick's update, so the crossings it
            // passes are that same tick's: the step this state runs starts its crossings from here
            if (ob->tpEx > (double)c.xAbs && c.frame == 0 && !c.dual && !c.rev
                && !(c.tpSkip & 4)) {
                c.tpSkip |= 4;
                c.tpFromX = c.xAbs;
            }
            c.xAbs = (float)ob->tpEx;
        }
        int wantUp = -1;
        if (ob->tpGrav == 1) wantUp = 0;
        else if (ob->tpGrav == 2) wantUp = 1;
        else if (ob->tpGrav == 3) wantUp = c.flip ? 0 : 1;
        if (wantUp >= 0 && c.flip != (uint8_t)wantUp) {
            c.flip = (uint8_t)wantUp;
            c.vy = c.vy * 0.5f;
            c.grounded = 0;
        }
        c.snapObj = nullptr;
        c.snapDist = 0.f;
        c.onSlope = 0;
        c.slopeM = 0.f;
        noteRingFired(c, ob);
        c.ringHold = 1;
        c.pressSpent = 1;
        c.action = 1;
        c.jumpBuf = 0;
        return;
    }
    if (ob->rev) { c.rev = c.rev ? 0 : 1; revReflect(c, ob->cx, (double)c.xAbs); }
    const double ms = (c.mini ? kMiniImpulse : 1.0) * ringScaleFor(useDx);
    if (ob->type == 37 || ob->type == 38) {
        // --flydashring (removed) (speed.hpp): the late loop's dash branch, on the state the next tick runs from
        c.dashing = 1;
        c.dashSlope = dashSlopeOf(ob, (int)c.frame);   // --dashclamp
        c.vy = 0.f;
        if (ob->type == 38) c.flip = c.flip ? 0 : 1;
    } else if (ob->type == 13) {
        const double gv = g_rawValues ? rawQ(kCubeJump * 0.8 * ms) * 0.5 : kOrbGravity * ms;
        c.flip = c.flip ? 0 : 1;
        c.vy = (float)(gv * (c.flip ? 1.0 : -1.0));
    } else if (ob->type == 32) {
        const double dv = (c.mode == 3) ? kRingDropUfo : kRingDropFly;
        if ((double)c.vy * (c.flip ? -1.0 : 1.0) >= 2.0) c.pNoTerm = 1;
        c.vy = (float)(c.flip ? dv : -dv);
        // --boostcarry: ringJump's type-0x20 branch ends by raising the velocity-limit exemption
        // (+0x952 = 1 after the setYVelocity, every mode), so a UFO slammed at 11.2 is not clamped
        // back to 6.4 on the next tick, and falls on unclamped until the head of the flying branch
        // finds it inside the band again. A custom level, the 09-30 cold run's attempt 23: a
        // flipped dual UFO takes a drop ring on t=1,383 (vy 11.2) and GD's rows run 11.286,
        // 11.372, 11.458 ...; the model clamped at 6.4 from 1,384.
        c.boost = 1;
    } else if (ob->type == 35 || ob->type == 29) {
        const bool green = (ob->type == 29);
        const double r = green ? ((c.mode == 1) ? kRingGreenShip : kRingGreen)
                         : (c.mode == 1) ? (c.mini ? kRingRedShipMini : kRingRedShip)
                                         : (c.mini ? kRingRedUfoMini : kRingRedUfo);
        if (green) c.flip = c.flip ? 0 : 1;
        const double ov = g_rawValues ? rawQ(kOrbYellow * r * ms) : kOrbYellow * r * ms;
        c.vy = (float)(c.flip ? -ov : ov);
        if (!green && true) c.boost = 1;
    } else {
        const double ratio = (ob->type == 12) ? pinkRingRatio(c.mode) : 1.0;
        const double ov = g_rawValues ? rawQ(kCubeJump * ratio * ms) : kOrbYellow * ratio * ms;
        c.vy = (float)(c.flip ? -ov : ov);
    }
    c.grounded = 0;
    c.rHover = 0;
    // --a1clatch: ringJump raises +0xa1c for every ring but the drop ring (0x3991f5-0x3991fe).
    // The ring fired at the end of the previous tick, so this tick's updateJump may lower it again.
    if (ob->type != 32) c.a1cLatch = (uint8_t)(c.a1cLatch | 1u);
    noteRingFired(c, ob);
    c.ringHold = 1;
    c.pressSpent = 1;
    // The press is GD's previous tick's: it is no longer an edge on this one, so the UFO's flap
    // gate (`input && !s.action`) stays shut, as ringJump's spent +0x986 keeps GD's shut.
    c.action = 1;
    c.jumpBuf = 0;
}

// --flyspiderring (speed.hpp): the spider orb's ringJump on a ship or UFO, applied to the state it fires from -- the
// late loop's branch (gravity from the orb's facing under --spiderorbface, the spider's surface search, vy +-1 the
// way it went), from the position the press's tick ended at. False when the search finds no surface: the late loop
// keeps it then, as before.
inline bool applyFlySpiderRing(State& c, const Obj* ob, const StepCtx& K) {
    if (c.frame != 0) return false;
    const bool sfl = objFacingDown(*ob);
    double tgt = 0.0;
    if (!spiderTargetY(K, (double)c.xAbs, (double)c.y, sfl, c.mini != 0, playerHalf(c.mode, c.mini != 0), tgt,
                       true, nullptr, 0, c.rev != 0))
        return false;
    c.y = (float)tgt;
    c.flip = sfl ? 0 : 1;
    c.vy = (float)(sfl ? -1.0 : 1.0);
    c.spiderJumpT = 0;
    c.snapObj = nullptr;
    c.snapDist = 0.f;
    c.onSlope = 0;
    c.slopeM = 0.f;
    c.grounded = 0;
    c.rHover = 0;
    c.a1cLatch = (uint8_t)(c.a1cLatch | 1u);   // --a1clatch: every ring but the drop ring
    noteRingFired(c, ob);
    c.ringHold = 1;
    c.pressSpent = 1;
    c.action = 1;
    c.jumpBuf = 0;
    return true;
}

// (--shadowsupply's lock-offset mail, t_lockAdvApplied / t_lockAdvX, stood here: only the removed old tick set it, so the supply arms that waited for it never ran. Removed 2026-11-20;
// the shadow's own lockOff rule is in shadow_step.hpp.)


// --shadowsupply, Follow Player Y: the objects the follow pass of this tick put somewhere else for this state (placed by a running command, or sent away because their cell is far), as the shadow's world takes them. The shadow's tick sets it from followPassPlan (shadow_step.hpp).
struct FollowPlaced { int uid; double cx, cy, dcy; bool far, slope; bool placed = false, enabled = true; double windowY = 0.0; };   // cy/dcy describe the placement; windowY is the pre-overlay position

inline thread_local std::vector<FollowPlaced> t_followPlaced;

// --shadowsupply, gravTouch: the touch-box test the old tick ran, set now by the shadow's tick before its supply (shadow_step.hpp) (it ran, the value before it, the box it ended on, the y it tested), so the new model can run it at its own body.
inline thread_local bool t_gravLoopRan = false;

inline thread_local uint8_t t_gravPreBox = 0, t_gravBoxApplied = 0;


// --shadowsupply, Event triggers (--eventtrig (removed)): the touch Teleport box / gravity value the old tick's event listeners set from the orbs and pads it fired (index + 1), taken as they are.
inline thread_local size_t t_tpEvent = 0;

inline thread_local uint8_t t_gravEvent = 0;

// --shadowsupply, Options: the shadow's tick sets these from its button gate (shadow_step.hpp): the button is refused this tick (a no-control window, a crossed Options trigger, a touched one: State::ctrlT).
inline thread_local bool t_ctrlGateNow = false;

inline thread_local bool t_ctrlGateP2 = false;   // --shadowdualctrl: player 2's half of the same

inline thread_local bool t_ctrlGateP1 = false;   // --shadowdualctrl: the gate player 1's half of a dual tick met (t_ctrlGateNow is the last half's: player 2's)

inline thread_local bool t_inputEffValid = false;   // the button the physics read after every gate (pcRel, the no-control windows, State::ctrlT)

inline thread_local int t_inputEff = 0;

// ...and its touch-box test: it ran, the value before it, the box value it ended on, the y it tested
inline thread_local bool t_ctrlLoopRan = false;

inline thread_local uint8_t t_ctrlPreBox = 0;

inline thread_local bool t_ctrlBoxApplied = false;


// The modes whose press reaches the body two ticks after the plan's press tick (cli.hpp, latOf in
// --replay): the ship and the UFO. Every other mode takes it one tick after. GD's button pass is
// one tick after the press tick in both cases, so for these two a step's input is the button pass
// that ended the PARENT's tick, and for the rest the one that ends this step's own.
inline bool pressLatTwo(uint8_t mode) { return mode == 1 || mode == 3; }

// --pressfresh (triggers.hpp): does a press that begins on this step fire toggle block T in the
// button pass that ends the parent's tick? Only in a latency-2 mode (pressLatTwo, the parent's
// mode), against the parent's box -- the boxes the parent's collision pass touched -- and not while
// the box's own group is off (--touchoff). The caller has checked that the press is fresh and the
// box not yet fired. markTouched dates such a fire to the parent's tick; the portal pass lets a
// teleport it switches on (Obj::pendBox) act on this step.
inline bool pressFreshFires(const TouchTrig& T, const State& par, long long t) {
    if (!T.press || T.offSwitch || T.spawnRing) return false;
    if (!pressLatTwo(par.mode)) return false;
    double bx = T.cx, by = T.cy;
    if (T.followIdx >= 0 && g_touchFollowObjs
        && (size_t)T.followIdx < g_touchFollowObjs->size()) {
        const Obj& fo = (*g_touchFollowObjs)[(size_t)T.followIdx];
        bx += (double)fo.cx - T.followX0;
        by += (double)fo.cy - T.followY0;
    }
    const double h0 = playerHalf(par.mode, par.mini != 0);
    if (!(std::fabs((double)par.xAbs - bx) < T.hw + h0 && std::fabs((double)par.y - by) < T.hh + h0))
        return false;
    return !(g_touchOwnOff && !T.ownTog.empty() && !touchOwnOn(T, t));
}

// --shadowfollow (constants.hpp, shadow_step.hpp): the Follow Player Y pass (the slot loop, the group objects' places) as a function of the State before the tick and the tick's context only -- no tick is stepped -- so the shadow's tick can
// put the objects where the pass puts them and write State::followF / followOk. `out` is t_followPlaced's entries, in the order the pass pushes them; `fNew` / `okNew` are what the pass leaves in the State
// before the touch-start update (followTouchStart, which needs the tick's end). Its one caller is shadow_step.hpp's tick; the removed old tick had its own copy of this pass, and the shadow's counters once compared the two.
inline void followPassPlan(const State& s, const StepCtx& K, bool half2, float fNew[kFollowYSlots], uint8_t& okNew, std::vector<FollowPlaced>& out) {
    out.clear();
    fNew[0] = s.followF[0]; fNew[1] = s.followF[1];
    okNew = s.followOk;
    if (g_followYs.empty() && g_followSecObjs.empty() && g_bigSecObjs.empty() && g_bigSecDyn.empty()) return;
    double fac[kFollowYSlots] = {0.0, 0.0}, facPrev[kFollowYSlots] = {0.0, 0.0};
    int placeCmd[kFollowYSlots] = {-1, -1};
    for (int sl = 0; sl < kFollowYSlots; ++sl) {
        const int own = followOwner(sl, K.x);
        if (own < 0) continue;
        const FollowY& f = g_followYs[(size_t)own];
        const uint8_t bit = (uint8_t)(1u << sl);
        if (!f.touch && K.xPrev < f.cx) {
            fNew[sl] = (float)f.y0ref;
            okNew = (uint8_t)(okNew | bit);
            continue;
        }
        if (f.touch ? (s.followOwn[sl] != (uint8_t)(own + 1)) : !(s.followOk & bit)) continue;
        const long long n = f.touch ? (long long)s.followN[sl] + 1 : (f.tAct >= 0 ? K.t - f.tAct : -1);
        if (n < 0 || (double)n <= f.durTicks + 1.0) {
            const float p1y = half2 ? s.y2 : s.y;
            float d = (p1y + (float)f.offset - s.followF[sl]) * (float)f.k;
            if (f.maxStep > 0.0)
                d = std::max(-(float)f.maxStep, std::min((float)f.maxStep, d));
            fNew[sl] = (float)((double)s.followF[sl] + (double)d);
        }
        placeCmd[sl] = own;
        if (n >= 0) {
            fac[sl] = std::pow(1.0 - f.k, (double)std::min<long long>(n, 1000000));
            facPrev[sl] = std::pow(1.0 - f.k, (double)std::min<long long>(n - 1, 1000000));
        }
    }
    if (!K.fol) return;
    const std::vector<const Obj*>* src[6] = {K.near, K.ports, K.pads, K.orbs, K.slopes, K.speeds};
    const bool secAll = g_dynamicLevelHeight;
    const double secX = K.x;
    for (const FollowRef& r : *K.fol) {
        if (!src[r.win]) continue;
        const bool bigDyn = r.cmd == kBigSecDynCmd;
        const bool big = r.cmd == kBigSecCmd || bigDyn;
        const int sl = (r.cmd == kFollowSecCmd || big) ? -1 : g_followYs[r.cmd].slot;
        const bool placed = sl >= 0 && placeCmd[sl] == (int)r.cmd;
        const Obj* orig = (*src[r.win])[r.idx];
        const bool enabled = orig->recOn != 0 && orig->onNew == 0;
        const double yFiled = bigDyn ? orig->cy : r.y0;
        const bool farCell = big
            ? (std::abs(gdSectionCell(orig->cx) - gdSectionCell(secX)) > 1
               || (g_dynamicLevelHeight
                   && std::abs(gdSectionCell(yFiled) - gdSectionCell((double)s.y)) > 1))
            : secAll
            && (std::abs(gdSectionCell(orig->cx) - gdSectionCell(secX)) > 1
                || std::abs(gdSectionCell(r.y0) - gdSectionCell((double)s.y)) > 1);
        if (!placed && !farCell) {
            out.push_back({orig->uid, orig->cx, orig->cy, orig->dcy, false, orig->slope != 0, false, enabled, orig->cy});
            continue;
        }
        double cy = orig->cy, dcy = orig->dcy;
        if (placed) {
            const FollowY& f = g_followYs[r.cmd];
            const double y = (double)fNew[sl] + (r.y0 - f.y0ref) * fac[sl];
            const double yPrev = (double)s.followF[sl] + (r.y0 - f.y0ref) * facPrev[sl];
            cy = y;
            dcy = y - yPrev;
        }
        out.push_back({orig->uid, orig->cx, cy, dcy, farCell, orig->slope != 0, placed, enabled, orig->cy});
    }
}

// ...and the touch-start update that follows the tick (--followtouch): a trigger the state's box has not touched yet starts when it does, a started one counts its ticks. `n` is the state the tick left (the pass's `n`).
inline void followTouchStart(const State& s, const StepCtx& K, State& n, float fNew[kFollowYSlots], uint8_t& okNew) {
    if (!g_followTouch) return;
    for (int sl = 0; sl < kFollowYSlots; ++sl) {
        const int own = followOwner(sl, K.x);
        if (own < 0) continue;
        const FollowY& f = g_followYs[(size_t)own];
        if (!f.touch) continue;
        if (s.followOwn[sl] == (uint8_t)(own + 1)) {
            n.followN[sl] = (uint16_t)std::min<int>(65535, (int)s.followN[sl] + 1);
            continue;
        }
        if ((n.frame & 3) != 0) continue;
        const double h0 = playerHalf(n.mode, n.mini != 0);
        if (std::fabs((double)n.xAbs - f.tcx) < f.thw + h0 && std::fabs((double)n.y - f.tcy) < f.thh + h0) {
            n.followOwn[sl] = (uint8_t)(own + 1);
            n.followN[sl] = 0;
            fNew[sl] = (float)f.y0ref;
            okNew = (uint8_t)(okNew | (1u << sl));
        }
    }
}

// Swap the primary player's fields with the second one's. The removed old tick ran its stepOne twice (once per half) with it, which kept the dual honest: the two halves
// see different geometry, so nothing about the second can be derived from the first once either of them touches anything. Today it is called where the second body's seed is built from the model (shadowDualLoad) and by cli.hpp's swapHalves(init).
inline void swapHalves(State& s) {
    std::swap(s.halfPrev, s.halfPrev2);   // --portprevhalf
    std::swap(s.y, s.y2);
    std::swap(s.vy, s.vy2);
    std::swap(s.slopeM, s.slopeM2);
    std::swap(s.snapDist, s.snapDist2);
    std::swap(s.grounded, s.grounded2);
    std::swap(s.flip, s.flip2);
    std::swap(s.ringHold, s.ringHold2);
    std::swap(s.pressSpent, s.pressSpent2);
    std::swap(s.gdHS, s.gdHS2);   // --hsshadow: per-body press bytes
    s.gdAux = (uint8_t)(((s.gdAux & 0x0F) << 4) | (s.gdAux >> 4));
    // (always on: the switch --dualslide is removed)
    // --dualslide (removed): each dual body keeps its own DART SLIDE arm (State::slideT2 beside
    // slideT; modifiers.hpp slideBoxTouch). GD's is per player: collisionCheckObjects sets THAT body's
    // +0xb78 to 2 when it touches an id-1755 box (0x215ab7), PlayerObject::update steps it down, and
    // collidedWithObjectInternal gives a wave the solid push-out only while it is >= 1. The model had one
    // arm for the pair, so the second body's step read the first's. Measured on custom level C
    // t=10,677-10,680 (hitboxtrace): p1, inside the lower lane's 1755 boxes, is pushed out of block
    // 56076's top at 0.01 px; p2, in the upper lane with no 1755, sinks 0.25-3.48 px into block 56066's
    // top with collidedWithObject returning 0 and dies at 5.1 px -- where the model, on p1's arm, seated
    // p2 at 575 (step_shared.hpp swapHalves; the removed old tick's stepBoth in fixup.hpp, the dual entry).
    // Acceptance for the default: kitref on 69 kits (the 13 levels with a dual portal and 1755 boxes, plus
    // one more dual-ball level), 1,028 episodes: no change; lv16/21/22 one-session [fp] identical; no positive replay of the
    // scene above (its level is not in the lab).
    std::swap(s.slideT, s.slideT2);   // per-body dart slide arm
    std::swap(s.rHover, s.rHover2);   // --dualhover: per-body robot hover budget
    // --dualdash: per-body dash. GD's m_isDashing is the player's own; with one shared field the
    // second body ran the first body's dash. Custom level C t=2,763: p1 (robot) takes the
    // dash ring uid 24826 and dashes, while GD's p2 (ball) keeps falling (p2vy -3.741); the model
    // dashed both.
    {
        std::swap(s.dashing, s.dashing2);
        std::swap(s.dashSlope, s.dashSlope2);
    }
    // ...and the velocity-limit exemption, which became per-half on 2026-09-06
    // (State::boost2). lv16 t=8,913 is a DUAL ship whose two halves both carry
    // it; left shared, p2's step would read p1's byte and its own answer would
    // be dropped by the merge in fixup.hpp.
    std::swap(s.boost, s.boost2);
    std::swap(s.onSlope, s.onSlope2);
    std::swap(s.slopeT, s.slopeT2);
    std::swap(s.rideLanded, s.rideLanded2);
    // --dualseatt: ...and the law seat's count (State::seatT), the next tick's m_wasOnSlope for a
    // law seat. Shared, the first body's step wrote it over the second's every tick.
    std::swap(s.seatT, s.seatT2);
    std::swap(s.mode, s.mode2);
    std::swap(s.mini, s.mini2);
    // ...and WHICH RAMP each half is riding. slopeUid0/slopeUidNow were left behind, so the
    // second body's step read the FIRST body's ramp uid -- which decides the seam hand-over
    // clamp (rTopIsSeam ... sp->uid != s.slopeUid0) and whether a re-acquisition counts as the
    // same ride. Two halves on mirrored ramps never share a uid, so the comparison was answering
    // about the wrong body every time.
    std::swap(s.slopeUid0, s.slopeUid02);
    std::swap(s.slopeUidNow, s.slopeUidNow2);
    std::swap(s.ceilT, s.ceilT2);
    std::swap(s.ceilM4, s.ceilM42);
    std::swap(s.snapObj, s.snapObj2);
    std::swap(s.usedOrb, s.usedOrb2);
    std::swap(s.usedOrbOld, s.usedOrbOld2);
    for (int i = 0; i < 3; ++i) std::swap(s.usedOrbHist[i], s.usedOrbHist2[i]);
    for (int i = 0; i < 2; ++i) std::swap(s.spdFired[i], s.spdFired2[i]);
    for (int i = 0; i < 2; ++i) std::swap(s.spdMore[i], s.spdMore2[i]);   // --spdsameonce
    std::swap(s.holdDead, s.holdDead2);
    for (int i = 0; i < 3; ++i) std::swap(s.portSeen[i], s.portSeen2[i]);
    for (int i = 0; i < 4; ++i) std::swap(s.touchRing[i], s.touchRing2[i]);
    std::swap(s.touchRingT, s.touchRingT2);
    std::swap(s.portalLatch, s.portalLatch2);
    for (int i = 0; i < 4; ++i) std::swap(s.usedPad[i], s.usedPad2[i]);
    std::swap(s.hitG, s.hitG2);   // --repela0c
    // --a1clatch: the two bodies' +0xa1c are bits 0 and 1 of one byte
    s.a1cLatch = (uint8_t)(((s.a1cLatch & 1u) << 1) | ((s.a1cLatch >> 1) & 1u));
    // --ballscaleair: the two bodies' +0x728 are bits 0-1 and 2-3 of one byte
    s.ball728 = (uint8_t)(((s.ball728 & 3u) << 2) | ((s.ball728 >> 2) & 3u));
    {                 // --rot2
        std::swap(s.rot, s.rot2);
        std::swap(s.rotStep, s.rotStep2);
        std::swap(s.rotNeg, s.rotNeg2);
    }
    {        // --dualflipclock
        std::swap(s.flipT, s.flipT2);
        std::swap(s.modeT, s.modeT2);
    }
}

// Touch-trigger boxes (see TouchTrig): plain rect overlap of the player's box
// with the trigger's, which is what GD was measured to do. Once a bit is set it
// stays set -- the doors do not close again, and a state that could un-touch a
// trigger would be a different world line, not the same one.
// The dual's second body is deliberately not tested: no level has put a touch
// trigger inside a dual section, and firing from the mirrored half would have to
// be threaded through swapHalves as well.
// `preY` is where the player was BEFORE this tick's button effects. GD's tick
// is update -> collisions (triggers) -> buttons, so a SPIDER that teleports on
// this tick has already been counted as touching whatever its old position was
// inside. Measured on lv22 t=1,753 (first rotated section, frame 1): the player
// is at v=2,266.5 and the box uid1417 (world 2277,273 -> frame u=-273,v=2277)
// is du=34.41 / dv=10.50 away, inside 21.165+13.5 and 15+13.5 -- it fires in GD
// and the spawn cascade under it moves the spike uid17571 by -150 from t=1,815.
// The model tested the POST-teleport v=2,143.5 (dv=133.5), never fired, and so
// kept that spike frozen at its t=1 position 150 px away from where GD has it --
// a phantom hazard sitting in the only lane out of the section.
// THIS READS p1's POSITION ONLY. In a dual section a box that only the second
// player enters is one the model can never fire -- the DP then plans against a
// world where that trigger does not exist, with or without an anchor, and a
// fire tick carried in from GD would be a value the step function cannot
// reproduce going forward.
//
// COUNTED 2026-09-04, and the corpus does not exercise it. Only lv20 has both
// a dual portal and touch boxes at all (lv16 and lv17 have portals and no
// boxes; lv19 and lv22 have boxes and NO dual portal, so their 3 and 155 can
// never be reached by a second player). On lv20, GD's own trajectory puts the
// two players against its six boxes for 805 dual ticks and p2 enters none of
// them (oneoff/py/p2boxes.py).
//
// So this is a real limit on what the model can express, with zero witnesses.
// It is recorded rather than fixed because adding the p2 arm increases forward
// firing -- geometry and deaths change -- and there is nothing to measure the
// change against. Whoever gives it a witness should read fullreplay and
// deathref, not the seeding check.
//
// AND NOTHING WAS WATCHING FOR IT. The anchor-seeding check compares a seeded
// state against the model's own whole run, and lv20's whole run died at
// t=1,157 while its dual section starts near t=17,119 -- so that check could
// not reach the one place in the corpus where this could bite. There was no
// detector that would name this the day it started to matter; the condition
// for one existing was lv20's replay reaching t~17,119.
//
// [2026-09-21] **THAT CONDITION IS MET.** With the three fidelity fixes on by
// default (0e5bd3a) lv20's whole-run replay completes the level, so the dual
// section is on the model's own trajectory rather than only on GD's. Re-taken
// there with --p2touch: 805 dual ticks, and p2 enters a box on NONE of them --
// the same 805 and the same zero the 2026-09-04 count got off GD's dump. So
// the reading is unchanged and it is now a reading of the run the corpus
// actually makes, which is what "nothing is watching" was about.
//
// The reason to leave it alone is therefore NOT "something will catch it". It
// is that only lv20 can be affected at all, it measures zero there over 805
// dual ticks, and lv22 -- the 155-box level -- has no dual portal, so it is
// excluded by the level data rather than by a measurement that might age.
// --touchoff (g_touchOwnOff): is box T's own trigger switched on at tick t? The Toggle that fired
// last decides (by fire tick, then by x); none fired yet = on, as the level placed it.
// A Toggle that fires ON tick t is not seen by t's own collision pass. Measured on SubZero 4003
// (worker 99, cold solution + injections): with the player held inside the Move box uid 1382
// (group 4) while it crosses the ON Toggle at x=2,535 on t=1,634, the box fires on 1,635 (its
// group's first motion 1,638, three ticks after the fire as below); jumping from x~1,808 into the
// same box crosses the OFF Toggle at x=1,815 in the same transition (t=1,190), and the box fires on
// 1,190 anyway (first motion 1,193).
inline bool touchOwnOn(const TouchTrig& T, long long t, const State* st) {
    int lastT = -1;
    double lastX = -1e18;
    uint8_t on = 1;
    for (const TouchTrig::OwnTog& e : T.ownTog) {
        const int ft = g_autoTrig[(size_t)e.autoIdx].fireT;
        if (ft < 0 || ft >= t) continue;
        if (ft > lastT || (ft == lastT && e.cx >= lastX)) {
            lastT = ft;
            lastX = e.cx;
            on = e.on;
        }
    }
    // --touchrearm (constants.hpp): a ring this state pressed after the last Toggle fired switches the group on again.
    // Like a Toggle, a ring that fires ON tick t is not seen by t's own pass.
    if (g_touchReArm && st && !on)
        for (const int rb : T.ringArm)
            if ((st->trig & touchBit(rb)) && (int)st->fireB[rb] > lastT && (long long)st->fireB[rb] < t) {
                on = 1;
                break;
            }
    return on != 0;
}

// `btnHeld`: the button as the game's collision pass sees it on this tick -- the PARENT's input,
// since a tick runs update, collisions, then buttons (a press on tick t is first seen by t+1's
// collisions). Read only by a toggle block (TouchTrig::press): measured on SubZero 4003's uid
// 4001, a tap at t=7,776 fires it on 7,777 and a press held from before it fires it on the first
// tick the boxes overlap. (--pressfresh reads those ticks as the plan's and the recording's --
// the cube takes a press a tick after the plan's, so 7,777 is the press's own step -- and adds
// `btnNow`, this step's input; a ship or UFO fires a fresh press from the parent's box --
// triggers.hpp.)
// `btnFresh` (--boxoff (removed)): the button goes down on THIS step (the step's input is a press and the
// parent's was not), and `par` the parent: the game takes a press in the button pass that ends the
// parent's tick, against the boxes the parent's collision pass touched. Read only by a toggle
// block that switches its group off (TouchTrig::offSwitch): measured on a custom level's box, a
// ship holding the button into it does not fire it (t=1,722, the hazards stay) and one that
// releases and presses again inside it does (past the hazards to t=1,922). The held form above was
// measured on a cube in the air; a flying mode spends its press every tick.
// --boxarm: an axis-aligned square (half h) against a box of half sides hw, hh turned rotDeg degrees clockwise --
// GD's sense, so the box's axes are at -rotDeg -- by separating axes. Touching counts.
inline bool squareMeetsTurnedBox(double px, double py, double h, double cx, double cy, double hw, double hh,
                                 double rotDeg) {
    const double th = -rotDeg * 3.14159265358979 / 180.0;
    const double co = std::cos(th), si = std::sin(th);
    const double dx = px - cx, dy = py - cy;
    const double axes[4][2] = {{1.0, 0.0}, {0.0, 1.0}, {co, si}, {-si, co}};
    for (const auto& ax : axes) {
        const double d = std::fabs(dx * ax[0] + dy * ax[1]);
        const double rp = h * (std::fabs(ax[0]) + std::fabs(ax[1]));
        const double rq = hw * std::fabs(co * ax[0] + si * ax[1]) + hh * std::fabs(-si * ax[0] + co * ax[1]);
        if (d > rp + rq) return false;
    }
    return true;
}

// The toggle blocks (1594, not spawn-only) the shadow's body fired in this pass, and whether the shadow's world holds a block at all (shadow_step.hpp sets both around its call of markTouched).
// A block the body knows fires for player 1 when the body's own pushButton / ringJump fired it and not otherwise: the press latch, the box and the row tested are the game's there.
// Measured in the game on a custom level (the mini ship, a 41.4 x 41.4 block at (4231,1899), the press at plan tick 2830, y injected at 2829 in two scans, one keeping vy 3.675 and one with vy 0):
// the block fires up to y 1928.643 / 1928.627 on row 2830 and not from 1928.743 / 1928.727 - one threshold, the half sum 20.7 + 9 from the block's centre, on the row of the press tick; the rows
// before and after give two different ones. The box test below reads the row after it in a latency-2 mode (the parent of the step that takes the press) and missed that press by 0.11 px.
inline thread_local const std::vector<int>* t_bodyRingFired = nullptr;
inline thread_local bool (*t_bodyRingKnown)(int uid) = nullptr;
// ...and the rings the body fired in the step this thread made last, for the world's tog update that follows the step (model.hpp togWorldStep takes them once; not valid = no shadow step was made)
struct BodyFiredOut { bool valid = false; std::vector<int> uids; };
inline thread_local BodyFiredOut t_bodyFiredOut;

inline void markTouched(State& c, const StepCtx& K, double preY, bool btnHeld,
                        bool btnFresh = true, const State* par = nullptr, bool btnNow = false) {
    if (!K.trigs || K.trigs->empty()) return;
    const double half = playerHalf(c.mode, c.mini != 0);
    for (const auto& tb : *K.trigs) {
        if (c.trig & tb.second) continue;
        const TouchTrig* T = tb.first;
        // A COUNT trigger has no box to enter: the item counter fires it, from
        // wherever the player happens to be (cli.hpp's item test). Nor does a
        // TAP trigger: the press fires that one, once its x is behind.
        if (T->count >= 0 || T->tap) continue;
        // ...and a toggle block needs the button down while the boxes overlap. --pressfresh
        // (triggers.hpp), a spawn ring excepted: in a latency-2 mode (pressLatTwo) this step's
        // button (`btnNow`) -- or a press that begins on this step while the PARENT is inside the
        // block, which the game takes in the button pass ending the parent's tick: it fires
        // wherever this step goes, dated to the parent's tick (`fireAt`). In every other mode this
        // step's button pass ends this step, so a press of this step or the parent's (`btnNow ||
        // btnHeld`) fires against this step's box, on this step.
        const bool byBody = T->press && !T->spawnRing && t_bodyRingFired && t_bodyRingKnown && t_bodyRingKnown(T->uid);
        const bool bodyFire = byBody && std::find(t_bodyRingFired->begin(), t_bodyRingFired->end(), T->uid) != t_bodyRingFired->end();
        const bool freshFire = !byBody && btnFresh && par && pressFreshFires(*T, *par, K.t);
        // (the body's fire in a latency-2 mode is dated like the fresh press it stands for: the game's rows have the block's group switched on the row before this step's)
        long long fireAt = (freshFire || (bodyFire && par && pressLatTwo(par->mode))) ? K.t - 1 : K.t;
        // --flyheldbox (triggers.hpp): ...except that a ship or UFO holding the button into the block
        // does not fire it; only the fresh press above does.
        // ...and only a press nothing has spent yet: the block is a ring, and ringJump tests the press latch (GD's +0x98a, the tick's copy of +0x986; State::pressSpent
        // is its complement and is read from the PARENT, as every ring does - state.hpp). Measured in the game on a custom level (the cube, a 1594 at (1907,1920)
        // sharing its spot with a gravity ring, four flights that differ only in the button between t=1307 and 1348): the press at 1307 fires a gravity ring and is
        // held through the block (rows overlap from 1326) - the block never fires; released at 1312 and pressed again in the air at 1320, still held at 1326 - it
        // fires on 1326; a press at 1335 inside the block, after a release at 1312 or at 1334 - it fires on that press.
        const bool held = (!T->spawnRing)
                              ? (!pressLatTwo(c.mode) && (btnNow || btnHeld) && !(par && par->pressSpent))
                              : btnHeld;
        if (T->press && !T->offSwitch && !held && !freshFire && !bodyFire) continue;
        // --spawnringon (speed.hpp): ...and a spawn ring takes a held press only with the button down on this
        // step, while the recording has it on (the row before this tick's) and nothing has spent the press -- a
        // ring fires once per press, and the ring the press went to switches the next one on after it
        // (always on: the switch --spawnringon is removed)
        // --spawnringon (default on since 2026-10): the toggle rings (1594) take presses the way the recording says GD's did.
        // pushButton hands a press to the rings the previous tick's collisions touched, and a fired ring's Toggles switch
        // the next ring on after that, in the row the fire is recorded in (the press step's own K.t). So:
        //  - a ring the recording has OFF on row K.t-1 takes no press, as a spawn ring (--orbspawn, --acthazoff (removed)) or under
        //    --ringpresseat (removed) (which read its "on" from row K.t+1);
        //  - a spawn ring takes a held press (markTouched) only with the button down on this step and the press not spent
        //    (it took the parent's button, a tick after a release, and every overlapping box at once);
        //  - its jump (--spawnringjump) is the held press's own, a tick late: given only to a cube that pressed from the
        //    ground, and not while another ring the recording has on in the fire's row holds the body.
        // A custom level x 41,355 .. 41,520: three rings stacked per spot, A ("A off, B on"), B ("B off, C on") and C (opens
        // the spike wall at x 41,685). GD's cube fires A on the press at 21,165 in the air (row 21,166: A off, B on) and B on
        // the press at 21,175 on a rising block (row 21,176: B off, C on), never leaves its line, presses no more and dies on
        // the wall at 21,225. The model fired all three boxes when it entered them at 21,162, a tick after the release, then C
        // on the second press, jumped both times and lived. With the flag the anchored replay agrees with GD on every column
        // to the death.
        if (T->spawnRing
            && (!btnNow || c.pressSpent || !pressRingOnAt(T->uid, (long long)K.t - 1)))
            continue;
        // ...and an off one, a press that begins (btnFresh) while the parent already overlaps it:
        // a ship whose press began a pixel before its box did not fire it. (An off Toggle that
        // fires on contact, 1049 with touch, is not pressed: it enters like any box.)
        // --boxoffown: in a latency-2 mode that press is the button pass ending the parent's tick,
        // so it fires there whether or not this step is still inside (`offFresh`, dated like
        // freshFire). A custom level's 1594 shares its spot with a green orb: the ship's press
        // takes both on t=1,651 and GD's recording has the group off on that row, but the orb's
        // jump carries the body 0.25 px clear of the box on the press's own step (y 7,989.75), so
        // the box never fired here and --boxoffown kept the hazard it switches off.
        bool offFresh = false;
        if (T->offSwitch && T->press && !byBody) {
            if (!btnFresh || !par) continue;
            const double h0 = playerHalf(par->mode, par->mini != 0);
            if (!(std::fabs((double)par->xAbs - T->cx) < T->hw + h0
                  && std::fabs((double)par->y - T->cy) < T->hh + h0))
                continue;
            if (pressLatTwo(par->mode)) {
                offFresh = true;
                fireAt = K.t - 1;
            }
        }
        // --p2touch: WOULD THE SECOND PLAYER HAVE ENTERED THIS BOX.
        //
        // This function reads c.xAbs and c.y and nothing else, so in a dual
        // section only p1 can ever enter a box: a trigger p2 alone reaches is
        // one the DP plans against as if it never fires, anchor or no anchor.
        // That is a hole in what the model can express rather than in how an
        // anchor is seeded, so it is counted before anything is carried from
        // GD -- seeding a bit the step function can never set forward would
        // make an anchor claim what a whole run of the same plan would not.
        //
        // Diagnostic only: it prints and changes nothing. Both players share
        // an x (the portal makes the second at the same point), so the test is
        // p2's own half against y2.
        if (g_p2Touch && c.dual) {
            const double half2 = playerHalf(c.mode2, c.mini2 != 0);
            // --p2xoff: at the second body's own x
            const double x2 = (double)c.xAbs + ((c.frame == 0) ? (double)c.dx2 : 0.0);
            const bool p2in = std::fabs(x2 - T->cx) < T->hw + half2
                              && std::fabs((double)c.y2 - T->cy) < T->hh + half2;
            const bool p1in = std::fabs((double)c.xAbs - T->cx) < T->hw + half
                              && (std::fabs((double)c.y - T->cy) < T->hh + half
                                  || std::fabs(preY - T->cy) < T->hh + half);
            if (p2in)
                std::printf("p2touch: t=%lld box=0x%llx p1=%d p2=1\n",
                            (long long)K.t, (unsigned long long)tb.second.word(0),
                            p1in ? 1 : 0);
        }
        // A box that moves with its group (TouchTrig::followIdx) is tested where
        // it is now, not where it was placed.
        double bx = T->cx, by = T->cy;
        if (T->followIdx >= 0 && g_touchFollowObjs
            && (size_t)T->followIdx < g_touchFollowObjs->size()) {
            const Obj& fo = (*g_touchFollowObjs)[(size_t)T->followIdx];
            bx += (double)fo.cx - T->followX0;
            by += (double)fo.cy - T->followY0;
        }
        // --boxarm (constants.hpp): in a dual, the second body enters a toggle block (a press box that switches
        // its group on) at its own x and y, the test --p2touch prints. A custom level's dual-wave corridor has a
        // block for each body, the second one's at the second body's height, and four of its seven switched-off
        // groups come back on only from those.
        // ...and a block with rows of its own (TouchTrig::recBox) is where the recording has it on this tick, turned.
        const DynSample* rb = nullptr;
        if (T->recBox && c.frame == 0) {
            const std::vector<DynSample>& sm = *T->recBox;
            size_t lo = 0, hi = sm.size();
            while (lo + 1 < hi) {
                const size_t md = (lo + hi) / 2;
                if (sm[md].t <= K.t) lo = md; else hi = md;
            }
            if (sm[lo].t <= K.t) rb = &sm[lo];
        }
        bool p2In = false;
        // --p2touchbox (removed) (speed.hpp): ...and every other touch box too, not only a toggle block's
        // (always on: the switch --p2touchbox is removed)
        // --p2touchbox (removed): in a dual, the second body enters EVERY touch box at its own x and y (step.hpp markTouched), as --boxarm already had it enter a toggle block. Until now a box only
        // the second body reaches was one the model could never fire (the --p2touch diagnostic's note). A weekly custom level: a pair of slabs that close on the corridor, 6 px over 24 ticks,
        // are switched by a touch box that sits on the lower one (the mirror pair above has no box of its own); the second body, mini and upright at y=160 with its head 3 px into the slab's
        // underside, touches it at t=8,457 and GD's recording has both slabs moving from t=8,458. The model's p1 hangs at y=290, never touches that box, so the slabs stood still; GD kills
        // both bodies at t=8,461 (the lower slab's underside 164.447 against the second body's kill box top 164.478), the model flew on 44 ticks.
        if (c.dual) {
            const double half2 = playerHalf(c.mode2, c.mini2 != 0);
            const double x2 = (double)c.xAbs + ((c.frame == 0) ? (double)c.dx2 : 0.0);
            p2In = rb ? squareMeetsTurnedBox(x2, (double)c.y2, half2, rb->cx, rb->cy, T->hw, T->hh, rb->rot)
                 : T->obb ? squareMeetsTurnedBox(x2, (double)c.y2, half2, bx, by, T->obbHw, T->obbHh, T->obbRot)
                      : std::fabs(x2 - bx) < T->hw + half2 && std::fabs((double)c.y2 - by) < T->hh + half2;
        }
        // --touchobb (constants.hpp): a turned box by its own oriented box, where the box is now
        // --spidertptouch (constants.hpp): ...and after a fresh press's spider teleport, the box meets the body
        // the collision pass had, before the teleport
        // (--spidertptouch, removed: nothing sets the teleport tick any more -- the old tick that did is gone -- so yNow below is always the body after the pass.)
        // --spidertptouch: a spider teleport made by a fresh press happens in the buttons phase, after the collision pass
        // that meets touch triggers, so on that tick the boxes meet the pre-teleport body only (step.hpp markTouched,
        // frames.hpp t_spiderFreshWarp). The model also tested the teleported body and fired a box one tick early.
        // Official level 22 t=1,679: a spider teleports up into Move box uid 1337 (ease-in, -120); GD's Move starts a
        // tick later than the model's (the recorded floor rows run one tick behind the model's formula), and the
        // spider's next teleport lands 0.15 px off on the descending floor at 1,685. On by default; --no-spidertptouch is
        // the off arm.
        const double yNow = (double)c.y;
        // --touchobbrect (constants.hpp): the turned box only past the rect test, as collisionCheckObjects does
        // (always on: the switch --touchobbrect is removed)
        // --touchobbrect (on by default since 2026-10-14; --no-touchobbrect is the off arm): ...and only past the rect test, which collisionCheckObjects makes first: a turned box is
        // met where the trigger's rect (triggers.txt w/h) AND its oriented box both meet the player. SubZero 4003's box
        // has a rect around the turn (149 x 149), so there the oriented box decides alone; SubZero 4001's toggle uid13410
        // has an unturned 63 x 63 rect under a 62.7 square turned -26, whose corners stick out of it -- the model fired the
        // box from those corners, and the iteration-2 search walled at x=24,775 where the rect alone lets it through.
        const bool rectGate = !T->obb || rb
            || (std::fabs((double)c.xAbs - bx) < T->hw + half
                && (std::fabs(yNow - by) < T->hh + half || std::fabs(preY - by) < T->hh + half));
        const bool p1In = rectGate
            && (rb
            ? (squareMeetsTurnedBox((double)c.xAbs, yNow, half, rb->cx, rb->cy, T->hw, T->hh, rb->rot)
               || squareMeetsTurnedBox((double)c.xAbs, preY, half, rb->cx, rb->cy, T->hw, T->hh, rb->rot))
            : T->obb
            ? (squareMeetsTurnedBox((double)c.xAbs, yNow, half, bx, by, T->obbHw, T->obbHh, T->obbRot)
               || squareMeetsTurnedBox((double)c.xAbs, preY, half, bx, by, T->obbHw, T->obbHh, T->obbRot))
            : (std::fabs((double)c.xAbs - bx) < T->hw + half
               && (std::fabs(yNow - by) < T->hh + half || std::fabs(preY - by) < T->hh + half)));
        // (constants.hpp, above g_touchReArm) a toggle block a FRESH press is made inside is fired by where the body stood when the press
        // was handed out - the parent's position, the previous tick's - and not only by where the step then takes it.
        const bool parIn = btnFresh && par && T->press && !T->spawnRing && !rb && !T->obb
            && std::fabs((double)par->xAbs - bx) < T->hw + playerHalf(par->mode, par->mini != 0)
            && std::fabs((double)par->y - by) < T->hh + playerHalf(par->mode, par->mini != 0);
        if (byBody ? (bodyFire || p2In) : (freshFire || offFresh || p2In || p1In || parIn)) {
            // --touchoff: a trigger in a group a Toggle has switched off is not touched.
            if (g_touchOwnOff && !T->ownTog.empty() && !touchOwnOn(*T, K.t, &c)) continue;
            c.trig |= tb.second;
            c.trigT = (int32_t)fireAt;
            {
                // Level-wide first-entry tick per box (see g_touchFireT):
                // the state only remembers its LAST box, and the switch
                // band's per-box delay needs each punch's own tick.
                const int b = touchBitIndex(tb.second);
                if (g_touchFireT[b] < 0) {
                    g_touchFireT[b] = (int)fireAt;
                    // met by preY only: the body the pass left is outside the box (the plain rect test; a turned box or a fresh / parent fire keeps the old reading)
                    g_touchFireEntryOnly[b] = (!bodyFire && !freshFire && !offFresh && !p2In && !parIn && !rb && !T->obb
                        && !(std::fabs((double)c.xAbs - bx) < T->hw + half && std::fabs(yNow - by) < T->hh + half)) ? 1 : 0;
                }
                // ...and this state's own tick for this box. The line above is
                // level-wide and first-entry-only, so it cannot tell two
                // worldlines apart; this can.
                c.fireB[b] = (uint16_t)fireAt;
                // --touchcensus: which test admitted this entry. "y" = the
                // player's y this tick overlaps; "preY" = only the y passed in
                // as preY does (the callers pass the parent state's y).
                if (g_touchCensus) {
                    const bool yHit = std::fabs((double)c.y - T->cy) < T->hh + half;
                    std::printf("tcensus: t=%lld box=%d uid=%d hit=%s mode=%d mini=%d "
                                "x=%.3f y=%.3f preY=%.3f vy=%.3f\n",
                                (long long)K.t, b, T->uid,
                                bodyFire ? "body" : (freshFire || offFresh) ? "fresh" : yHit ? "y" : "preY",
                                (int)c.mode, (int)c.mini, (double)c.xAbs, (double)c.y,
                                preY, (double)c.vy);
                }
            }
            if (g_slopeDbg)
                std::printf("trigfire t=%lld box(%.1f,%.1f) %.0fx%.0f "
                            "player(%.1f,%.1f) half=%.1f mask=0x%llx\n",
                            (long long)K.t, T->cx, T->cy, 2 * T->hw, 2 * T->hh,
                            (double)c.xAbs, (double)c.y, half,
                            (unsigned long long)c.trig.word(0));
        }
    }
}

// The search's wall rules (search_dp.hpp: --needsize, --needspeed) kill a state with it. The old tick's other kill macros (DIE, CLAMP0, CLAMP0O, VPSET) went with it, and so did what only it
// needed here: --noprune (removed) (a kDeathPrune kill is the only kind it dropped; the search passes kDeathRule) and the Area Move twins' kill count (g_envKills: only an object-carrying kill raised it, and the search
// passes none).
#define DIE_AS(kind, why, obj) do { dead = true; g_deadKind = (kind); g_deadWhy = (why); \
    if (g_inRecon) noteReconDeath(why); \
    const Obj* dobj_ = (obj); g_deadObj = dobj_; \
    if (dobj_) { g_deadCx = (float)dobj_->cx; g_deadCy = (float)dobj_->cy; } \
    else { g_deadCx = g_deadCy = 0.f; } \
    } while (0)

}  // namespace dp
