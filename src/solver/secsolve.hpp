#pragma once
// ============================================================================
// Section-limited GD solver (cfg `secsolve=1`)
//
// Why it is needed: the approach of closing the approximate model's divergences
// as rules got to the point of taking tens of minutes per case even on lv20,
// which has no new elements (9 cases in one night on 2026-08-07, 18.8%->26%).
// On unknown custom levels this would happen every time, so USE THE REAL GD AS
// THE TRANSITION FUNCTION FOR JUST THE STUCK SECTION. A partial return to the
// original policy (the spec ('use GD itself as the simulator')); the reasons the
// beam-era failure will not repeat are
//   - the entry is fixed   (the section's start state is the checkpoint itself)
//   - the exit is binary   (crossed x >= target alive, or not)
//   - the horizon is short (default 300 ticks)
// and therefore NO FITNESS FUNCTION IS NEEDED.
//
// Feasibility is measured: GD's practice-mode checkpoint restore is 2.02 ms per
// restore (lv20/worker-99, n=200, measured with `restoreloop`). At 100 states
// per layer x 300 layers = 60,000 expansions that is 2-3 minutes, practical as
// long as it stays section-limited.
//
// How state is held: only ONE CheckpointObject is kept, for the section entry.
// Each node is represented as "the input sequence from the section start" and
// is rebuilt on every expansion by
//   restore -> replay the prefix -> branch
// Holding one checkpoint per layer would mean holding "all object state" in
// batches of 100 — what caused the old implementation's OOM — so that is not
// done. Replaying the prefix is about 0.007ms per tick, so even at depth 300 it
// adds only about 2ms.
//
// The search is per-layer breadth-first + quantised dedupe + cap truncation.
// It is shaped like the DP on purpose, to keep the truncation properties the
// same as known ones (docs/HANDOFF.md update 28).
//
// Two ways to start it:
//   * cfg keys at session open (the served path, py/secsolve_run.py) -- a plan
//     in the cfg reaches the section head, `checkpointat`/`secstart` take over.
//   * the runtime command `secsolve <startTick> <targetX> [horizon] [cap]`
//     (cmd.txt) DURING an in-process solve session: the repair loop retires and
//     its deepest VERIFIED plan replays to the section head (the handoff in
//     dpsolve::poll, repair.hpp). One-way -- the search ends the session itself.
// ============================================================================

namespace secsolve {

inline bool g_on = false;
inline long long g_startTick = -1;   // create the checkpoint at this tick and search from it
inline double g_targetX = 0.0;       // success when crossed alive (<=0 disables)
inline int g_horizon = 300;          // section length (ticks)
inline size_t g_cap = 100;           // states kept per layer
// ---- exit conditions (other than x) -----------------------------------------
// The axis of travel is NOT necessarily x. lv22's x≈2,266 is a 90-degree
// rotated section: travel is -y and pressing moves x (docs/HANDOFF.md update
// 71). "Crossing x" is not a valid goal there, so the exit can also be written
// in y and in depth.
// The test is OR — meeting any one of the enabled ones is success. Default is
// both disabled, the old behaviour of looking at x only.
inline double g_targetY = 0.0;       // cfg `sectargety`
inline int g_targetYDir = 0;         // cfg `sectargetydir`: +1 = y>=target / -1 = y<=target
                                     // / 0 = disabled
inline int g_targetDepth = 0;        // cfg `sectargetdepth`: success after surviving this
                                     // many ticks
// Dedupe granularity. Do NOT coarsen. A bucket's representative is the first
// one in, and insertion order puts the lower branches first, so a coarse grid
// silently discards the top of the band. Measured at lv20 x=6342:
//   1.0 / 0.5, cap 400 (cap unused)      -> band 54px, wiped out
//   0.25 / 0.1, cap 200 (evenly spaced)  -> band 149px, passes with dead=0
inline double g_yq = 0.25;           // dedupe y granularity
inline double g_vq = 0.1;            // dedupe vy granularity
// x granularity (cfg `secxq`). x IS IN THE KEY BECAUSE OF ROTATED SECTIONS.
// In a normal section x is a function of the tick (all branches of one layer
// share the same x), so adding it to the key splits no bucket = mostly
// harmless. In a rotated section, conversely, x IS THE AXIS THAT JUMPS, y is
// just a clock advancing at constant speed and vy stays 0, so with a (y,vy)
// key only 2 buckets per layer survive (held 0/1) and the search becomes
// effectively a single path.
inline double g_xq = 0.25;
inline bool g_done = false;          // once per session
inline bool g_verify = false;        // cfg `secverify=1`: no search, only check
                                     // restore fidelity
// cfg `seclog=1`: emit the per-layer breakdown. Whether THE CAP IS BINDING OR
// THERE REALLY IS NO CONTINUATION cannot be told apart without this (in update
// 32 the depth only going 137->153 for cap 40->200 was read as "the wall is
// real", but that was never corroborated).
inline bool g_log = false;
// dt passed per step (cfg `secdt`). Default 1/240 = one physics tick.
// The plain replay passes 1/60 (fastdt of BASE_CFG), so one update call
// advances 4 ticks. If the ticks per call differ, the phase of any processing
// tied to the call (if there is any) rather than to the substep shifts.
// Kept as a knob so this is the first suspect when restore fidelity breaks.
inline double g_dt = 1.0 / 240.0;
// cfg `secoff`: how many ticks to shift the input sequence by in secverify. The
// mapping is decided by MEASUREMENT, not derivation — where within the tick the
// button takes effect (before or after update) enters into it, so it is not
// necessarily the value that falls straight out of "the restore lands at tick
// ckptTick+1".
//
// [2026-08-27] SWEEP IT, never read one run. On 1474319 at ckptTick=300 the
// series -2/-1/0/1/2 gave diedAt 125/118/114/110/106, and only -2 reproduced the
// plain run's death (x=553.055, t=426) to three decimals. Read at the default 0
// alone, the same section says "the restore is unfaithful, the replay dies 11
// ticks early" -- which is a misdiagnosis of a knob that exists precisely
// because the answer is not derivable.
inline int g_off = 0;
// cfg `secpsnap=N`: measure over N ticks whether a player-only snapshot restore
// can replace a full checkpoint restore (no search). `secpsnapreps=R` sets the
// repetition count. In sections where this passes, one branch goes from
// 2.02ms/0.95MB to a single memcpy.
inline int g_snapCmp = 0;
inline int g_snapReps = 3;
// cfg `secsnap`: choice of branch primitive. 0=checkpoint only / 1=force psnap /
// 2=sweep at the section start and decide automatically. Default 0 (no
// behaviour change).
inline int g_snapMode = 0;
// After how many consecutive psnap branches to re-phase the world with a
// checkpoint (cfg `secrephase`). Phase-independent sections bit-matched even
// shifted by 10,000 ticks, but a real search expands 100k times per section,
// so this is a safety valve to stay inside the measured range.
inline int g_rephase = 5000;
inline bool g_snapOn = false;        // is psnap actually used in this section
// Whether to restore moving objects too (`wsnap`). psnap splits in moving
// sections because it does not restore GameObject positions, so restore ONLY
// THE MOVING OBJECTS in the same way.
// `secsnap=2` sets this automatically (when there are moving objects and the
// set fits within `secmaxmov`). `secsnap=3` forces it.
inline bool g_worldOn = false;
inline size_t g_maxMoving = 4000;    // cfg `secmaxmov`: beyond this fall back
                                     // to the checkpoint
inline std::vector<GameObject*> g_movSet;   // decided once at the section start
// cfg `secworld=N`: compare the world object-by-object after checkpoint restore
// vs after psnap restore
inline int g_worldDiff = 0;
// WHAT THIS SEARCH CANNOT SEE [2026-08-27, measured on 1474319]
// A branch is one restore plus one step, so any death GD only reaches by
// ACCUMULATING state over consecutive ticks -- the out-of-bounds latch that
// wants two of them, a one-shot trigger the restore puts back -- is reset
// before it can ever fire. The continuous replay of the same inputs does
// accumulate it and dies, so the search believes a lethal corridor is passable
// and its own leaf cross-check then refuses the answer.
// Measured shape: the section from t=300 reached x=701.1 with two taps in 240
// ticks; replaying those two taps from the same checkpoint dies at x=524.5 at
// EVERY input phase (secoff -3..+1), and x=524.5 is where the loop's own plans
// die, with `killer: obj=NULL` -- an object-less kill. UNVERIFIED here is the
// guard working, not a bug to chase: on a section whose deaths are all decided
// within one tick (lv22's switch band) the same machinery verifies clean.
//
// Do NOT kill during search. Death is only caught in destroyPlayer and turned
// into a flag.
//
// Why: actually letting it die puts the level into an "attempt over" state and
// no amount of restoring afterwards makes the physics advance. Re-waking needs
// a checkpoint restore (2ms), and each one moves the world's phase. Measurement
// narrowed it down to "in sections where no branch dies psnap bit-matches the
// checkpoint; only sections with deaths split", so the clean fix is to NOT LET
// THE DEATH HAPPEN AT ALL. Collision remains, so portals and triggers fire
// normally (same as cfg nodeath).
inline bool g_noKill = false;   // set only by the search proper (not by the sweep)
// cfg `secnokill`: a knob EXCLUSIVELY FOR ISOLATION, to align the handling of
// death across both paths.
// -1 = auto (swallow only under psnap, default) / 0 = always really kill /
//  1 = always swallow. When adding more world restore never moves the way the
// split behaves, use this to check whether the two runs being compared differ
// in how they treat death.
inline int g_killOverride = -1;
// cfg `seckilllog=1`: emit WHAT KILLED the branch every time one dies.
// When chasing "the checkpoint kills but psnap does not", look at this before
// adding members by guesswork (indeed, all three hypotheses — position and the
// section windows — missed).
inline bool g_killLog = false;
inline int g_depth = 0;          // the layer currently being expanded (for logging)
// cfg `secvis=1`: rebuild the collision candidate lists after a psnap restore.
// GJBaseGameLayer reassembles m_solidCollisionObjects /
// m_hazardCollisionObjects every frame from "the sections around the player".
// The checkpoint path gets 2 frozen steps after restore, where they are
// reassembled, but the psnap path has no freeze, so it takes its first step
// with THE CANDIDATE LIST OF WHERE THE WORLD USED TO BE.
inline bool g_visRefresh = false;
// cfg `secoverlay=N`: a check that STACKS a psnap restore ON TOP OF the
// checkpoint path. The same node's snapshot is applied right after the
// checkpoint restore, so if the psnap restore is faithful it MUST be the
// identity. If a difference shows, psnap is not "missing something" but
// "breaking something", and the direction of the search flips.
// Bits isolate the part: 1=player bytes / 2=EM pulse /
// 4=GJGameState / 8=touch ledgers. 15 for all.
inline int g_overlay = 0;
// cfg `secmemat=N`: print by name the members around offset N.
// Bisection yields a byte range; without this it cannot be mapped to a name.
inline long long g_memAt = -1;
// cfg `secwinshift=N`: POSITIVE CONTROL. Right after restore, shift the
// collision window (m_left/rightSectionIndex) by N. If the shift makes the
// checkpoint path's deaths disappear, then "a stale window misses hazards"
// holds as a mechanism and explains the psnap split.
// If they do not disappear, the window is re-derived every frame and that lead
// is dead.
inline int g_winShift = 0;
// cfg `seccollog=1`: periodically emit the element counts of m_collisionLog*
// (whether they are used)
inline bool g_colLog = false;
// ---------------------------------------------------------------------------
// Verify and repair (cfg `secverifyevery=V`, user proposal 2026-08-07)
//
// psnap's errors are ONE-DIRECTIONAL — they only appear on the side of missing
// a death (measured: on lv20 ship, cp dead=4 / psnap dead=0; the reverse has
// never appeared). So fidelity need not be nailed down completely — CROSS-CHECK
// AGAINST THE CORRECT SIDE AND DROP.
//
// The correct side is "plainly replay the input sequence from the section
// checkpoint" = method A itself. Cross-checking one node costs 1 checkpoint
// restore + depth steps, and applied to the frontier only, once every V layers
// is cheap enough. A frontier that survives this is guaranteed REACHABLE ON THE
// REAL GAME. V=1 degenerates to method A (slow but complete).
//
// Dropped branches are counted and reported, so PSNAP'S ERROR RATE BECOMES A
// DIRECT MEASUREMENT.
inline int g_verifyEvery = 0;
inline double g_verifyTol = 1e-4;
// Place a checkpoint at the cross-checked point (cfg `secanchor=1`, default on).
//
// Without this the cost of cross-checking is proportional to depth. Re-running
// from the section start every time makes one pass cost cap x depth steps, and
// on a depth-1,249 section (lv20's wave) cross-checking ate 9/10 of all the
// work and became SLOWER THAN THE CHECKPOINT PATH (measured 60 min vs 21 min).
// With a checkpoint at the previous cross-check point, the next cross-check
// only replays V LAYERS FROM THERE, and one pass becomes cap x V, independent
// of depth. Checkpoint restores too: the checkpoint path does 2*cap per layer,
// vs cap per V layers here, i.e. 1/(2V).
inline bool g_anchor = true;
// A found solution is ALWAYS confirmed with a plain replay (unconditional).
// Once, before saying SOLVED.
inline bool g_leafVerified = false;
inline int g_leafDeadAt = -1;
// Survivability of the exit (cfg `secgrace`, default 600 ticks).
//
// Reaching the target is NOT enough. A branch flung onto an arc keeps advancing
// x at 1.2982/tick even though input has no effect, so with x as the goal, "a
// corpse in mid-fall" is returned as the solution. Measured (lv22, 2026-08-11):
// a branch where the spider teleported where there was no landing surface,
// WENT THROUGH THE WALL AND OUT OF BOUNDS, "advanced" to x=3,228 and died 188
// ticks later. The driver's deepest-first accepted that as progress and kept
// choosing the same dead end.
//
// Run 2 lines from the leaf (no press / held) for N ticks; IF BOTH DIE, THERE
// IS NO CONTINUATION FROM THAT EXIT (DOOMED). In a state where input works,
// normally one of them lives.
inline int g_grace = 600;
// After how many doomed exits to give up with "nothing from this entry"
// (cfg `secmaxdoomed`). Each one costs a replay + 10 grace lines ≈ 1,300 steps,
// measured 344 of them in 104 seconds. That is plenty as evidence, so stop there.
inline long long g_maxDoomed = 500;
// Out-of-bounds ceiling (cfg `secmaxy`). 0 = auto (topmost object of the level
// + margin).
//
// Branches flung onto an arc eat the frontier. In lv22's rotated section, a
// spider teleporting where there is no surface flies to y=3,200+ and then takes
// 190 ticks to die out of bounds. All that time it occupies half the cap, so
// the correct family drops as capped (measured: capped=59 on a cap-120 layer).
// It is a height GD KILLS ANYWAY, so it may be declared dead early. Take it
// well above the top of the level.
inline double g_maxY = 0.0;
inline double g_maxYAuto = 0.0;   // computed once at the section start
inline bool g_died = false;     // did a death verdict come in the previous step

// During search the plan's input feed is stopped and we take over tick by tick.
// A REFERENCE to the flag config.hpp declares, so that the audio predicate -- which is compiled
// long before this header -- can ask the same question rather than a copy of it (two flags that
// mean the same thing drift, and the one that drifts is always the one nobody is looking at).
inline bool& g_active = g_secSearching;
inline int g_feed = 0;               // button state given at this tick (0/1)
inline int g_held = 0;               // currently pressing? (for handleButton deltas)
// Can be turned off with cfg `secjumpbuf=0` (default ON): when restoring a held
// press, also set m_jumpBuffered. WITHOUT THIS A HELD BRANCH BECOMES A
// DIFFERENT THING FROM THE PLAIN REPLAY — history and measurements are in the
// note on secArmHold in phase1.cpp. It is made switchable for A/B testing.
inline bool g_jumpBuf = true;

// Dash (dash ring type 37/38) state.
//
// CheckpointObject does NOT save this. Measured (lv22, 2026-08-12, `secverify`):
// placing a checkpoint mid-dash at t=2100 and restoring, the position
// (2569.99, 237.258) comes back but it starts falling at vy=-0.275 right after
// the restore. The plain run continues from there for 31 ticks at y=237.2577 /
// vy=0. So THE SECTION SOLVER CANNOT REPRESENT A DASH. Same as the held press
// ([[gd-restore-repress-changes-the-input]]): we restore it ourselves.
// While at it, carry ALL of PlayerObject's pointer members. psnap's mask has
// the policy "copy not a single pointer" (against stale pointers), but INSIDE
// THE SECTION SOLVER THE LEVEL'S OBJECTS ARE NEVER RE-CREATED, so these stay
// valid to the end. The ground object, slopes, stair snap and the last portal
// passed each change the next tick's behaviour, so dropping them makes psnap
// drift silently (measured 150.7px).
struct DashState {
    bool on = false;
    GameObject* ring = nullptr;
    double x = 0, y = 0, angle = 0, startTime = 0;
    // Below: pointers whose omission changes behaviour on restore (12 of them)
    GameObject* lastGround = nullptr;
    GameObject* maybeLastGround = nullptr;
    GameObject* preLastGround = nullptr;
    GameObject* collided = nullptr;
    GameObject* collideLeft = nullptr;
    GameObject* collideRight = nullptr;
    GameObject* slope = nullptr;
    GameObject* slope2 = nullptr;
    GameObject* potentialSlope = nullptr;
    GameObject* snappedTo = nullptr;
    GameObject* lastPortal = nullptr;
};

// 1 node = "the input sequence from the section start". Walking the parents
// recovers the sequence. y/vy/x are used for the sort when narrowing a layer to
// the cap, and for MATCHING THE SPLICE POINT of the solution.
//
// x is also held IN ORDER TO SAY AT WHICH DEPTH the search's transition and the
// plain replay first split. In lv22's spider section y is a constant-speed
// clock and vy stays 0, so `dy=dvy=0` is no evidence of agreement (measured:
// dy=dvy=0 while the exit x differs by 963px).
struct Node {
    int parent;      // -1 = the section start
    uint8_t in;      // input given at this tick
    uint8_t dash;    // dashing? (held so the cap does not squash the kinds)
    float y;
    float vy;
    float x;
    // COUNTER DELTA from the section start (sum of GJEffectManager's items).
    //
    // lv22's x≈3,260..3,900 is a contraption where "hitting a hanging block
    // spawns counter +1 and simultaneously the spike row (group 265) rises
    // +30"; the branch that hit it and the branch that passed by keep nearly
    // the same (y,vy,x) while ONLY THE WORLD DIFFERS. Without it in the key,
    // dedupe identifies them and erases one, and even with the cap's even y
    // spacing the whole bucket drops — the same family of defect as before
    // dash entered the key (wiped out at x=2,684). Measured 2026-08-12: adding
    // just one bonk to the current plan moves the death point x=3,495 → 3,569
    // (the whole spike row is +30).
    // On levels where the counter never moves it is always 0 and affects
    // neither the key nor the cap.
    uint16_t cnt;
};
inline std::vector<Node> g_nodes;
// Per-node dash state (same index as g_nodes). Held because the checkpoint
// does not save it.
inline std::vector<DashState> g_dash;
// Per-node checkpoints. Kept alive ONLY FOR THE FRONTIER and released as the
// layer advances (holding hundreds to thousands returns to the old
// implementation's OOM). Index is the same as g_nodes.
inline std::vector<CheckpointObject*> g_cps;
// Checkpoints at cross-check points (`secanchor`). ONE GENERATION only (cap of
// them). Swapped out on every pass.
inline std::unordered_map<int, CheckpointObject*> g_anchors;

inline void releaseAnchors() {
    for (auto& kv : g_anchors) if (kv.second) kv.second->release();
    g_anchors.clear();
}

// ---- deadband (cfg `secdeadband`) ------------------------------------------
// Applies the same rule as leveldp's --deadband to the search's expansion.
// Branches inside the band [x0,x1] are considered dead — those not in the given
// mode if a mode is specified, all of them if unspecified (mode<0).
// With y0,y1, only branches whose y is also in that range (an arc passes the
// same x as the floor path at y=600+).
// History is in the cfg-parsing note in phase1.cpp (secgrace let an arc rising
// out of bounds pass through).
struct SecDeadBand { double x0, x1; int mode; double y0, y1; };
inline std::vector<SecDeadBand> g_secBands;

// ---- counter observation (see the note on Node::cnt) ------------------------
// Sum of GJEffectManager's item counters + THE NUMBER OF DISABLED OBJECTS. The
// baseline for the delta (g_cntBase) is taken at the section start. psnap does
// not carry this map (OPAQUE in po_members.inc), but a section where counters
// move also has moving geometry, so it falls back to the checkpoint anyway
// (verify-and-repair guards it).
//
// Why disabled was added (2026-08-12, late night): the contraption at lv22
// x=12,100 is "when the spider lands hanging on the ceiling, the touch Toggle
// (uid 11545) toggles group 356 (the 293px-wide platform pair of 1888)".
// A TOGGLE DOES NOT MOVE THE ITEM COUNTERS, so the branch that touched it and
// the branch that passed by are identified by the key and one is erased — the
// same family as the switch band (Pickup) but a different observable. A
// Toggle's activation changes the count of m_disabledObjects, so adding it
// makes both families split by the same mechanism. An autonomous toggle that
// moves uniformly within a layer has the same value on every node and so
// splits no dedupe (harmless).
inline long long g_cntBase = 0;
inline long long countSum(GJBaseGameLayer* l) {
    if (!l) return 0;
    long long s = 0;
    if (auto* em = l->m_effectManager)
        for (auto& kv : em->m_itemCountMap) s += (long long)kv.second;
    s += (long long)l->m_disabledObjects.size() * 131LL;
    return s;
}
inline int cntNow(GJBaseGameLayer* l) {
    long long d = countSum(l) - g_cntBase;
    if (d < 0) d = -d;
    if (d > 65535) d = 65535;
    return (int)d;
}

// ---- the search yields the frame back (cfg `secslicems`) --------------------
// The search used to own the frame it started in, start to finish. Measured on lv22's switch
// band (2026-08-26): one frame held for 19 minutes, and Windows draws a window that has not
// pumped its messages as a ghost -- so the only evidence that anything was happening was the log
// file, and every notification already on screen froze with it.
//
// A search that stops at a LAYER boundary and comes back next frame costs nothing in fidelity:
// game time does not advance between slices (the driver returns from update() at exactly the
// point the DP solve's own freeze does, which has held a level still for minutes a run since
// Stage C), and every expansion begins by restoring a checkpoint regardless. What it buys is a
// live window, a HUD that counts the layers, and the hotkeys.
//
// The budget is per slice, in milliseconds of wall clock. One layer of lv22's band is ~2.5 s at
// cap 120, so most slices are a single layer -- the frame is handed back as soon as one is done,
// never in the middle of one. 0 disables slicing entirely (the old single-frame behaviour, kept
// so the two can be compared).
inline int g_sliceMs = 12;
inline std::chrono::steady_clock::time_point g_sliceStart;

inline bool sliceExpired() {
    if (g_sliceMs <= 0) return false;
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - g_sliceStart).count() >= (double)g_sliceMs;
}

// The search in flight, as a coroutine: suspending at a layer boundary keeps every local and
// every lambda of the search body exactly where they were, which is the whole reason for the
// coroutine -- a hand-rolled state machine would have had to hoist thirty locals into globals,
// and the search's behaviour is the one thing that must not change.
struct SecTask {
    struct promise_type {
        SecTask get_return_object() {
            return SecTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        // Nothing runs until the driver resumes it: the search is created at a frame boundary
        // and takes its first slice on the next one, through the same path as every later slice.
        std::suspend_always initial_suspend() noexcept { return {}; }
        // Kept alive after the body returns so done() can be asked; the driver destroys it.
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() { std::terminate(); }
    };
    using handle_t = std::coroutine_handle<promise_type>;
    handle_t h{};

    SecTask() = default;
    explicit SecTask(handle_t hh) : h(hh) {}
    SecTask(const SecTask&) = delete;
    SecTask& operator=(const SecTask&) = delete;
    SecTask(SecTask&& o) noexcept : h(o.h) { o.h = {}; }
    SecTask& operator=(SecTask&& o) noexcept {
        if (this != &o) { destroy(); h = o.h; o.h = {}; }
        return *this;
    }
    ~SecTask() { destroy(); }

    bool valid() const { return (bool)h; }
    bool done() const { return h && h.done(); }
    void resume() { if (h && !h.done()) h.resume(); }
    void destroy() { if (h) { h.destroy(); h = {}; } }
};

// The search in flight (empty when none). Owned by the game layer's update.
inline SecTask g_task;
// The level the suspended search belongs to. Its frame holds `pl` and `this` across every
// suspension, so resuming it against a level that has since been rebuilt would step through
// freed objects. The driver checks this before every resume and drops the search if the level
// underneath it has changed (a search cannot survive its level in any case).
inline void* g_taskLayer = nullptr;
// Is a search suspended between slices right now? While this holds, the game's own update must
// not run: the world belongs to the search.
inline bool inFlight() { return g_task.valid() && !g_task.done(); }
// Frontier size of the last completed layer, for the HUD.
inline size_t g_frontierNow = 0;

inline void reset() {
    g_task.destroy();
    g_taskLayer = nullptr;
    g_frontierNow = 0;
    g_done = false;
    g_active = false;
    g_feed = 0;
    g_held = 0;
    g_nodes.clear();
    g_dash.clear();
    g_cps.clear();
    releaseAnchors();
    g_movSet.clear();
    g_worldOn = false;
    g_cntBase = 0;
}

// Quantisation key: (mode, mini, flip, held, y, vy, x).
//
// Dropping held erases surviving branches in ship sections. The first version
// used only (mode,mini,flip,y,vy), and on lv20 the frontier was wiped out 41
// ticks after tick=4700 — even though the original plan survives there. In
// ship the acceleration depends on "is it pressed", so the same (y,vy) with a
// different held is a different state. Wave and robot likewise.
// Even if GD is exact, mistaking state identity makes the search lie.
//
// x was added for the same reason, the ROTATED-SECTION VERSION of it (see the
// note on g_xq). Bit-packing ran out of digits, so it became a mixing hash.
// A different packing never creates different buckets (barring collisions) —
// the representative is "the first one in", so as long as the key is injective
// the behaviour is the same.
// dash: dashing? (`m_isDashing`). Dropping it makes dash rings unusable.
//
// During a dash, y is fixed and vy stays 0 while only x advances, so without
// it in the key "the player mid-dash" and "the player passing the same height
// plainly" become THE SAME STATE and only the earlier one (insertion order)
// survives. Measured (lv22, 2026-08-12): the branch using the dash ring
// (type 37) at x=2,517 was erased every time, and the frontier's y band was
// wiped out at x=2,684 without ever reaching the 239 of the then-current run.
// Same family of defect as ship disappearing when `held` is dropped.
// cnt: counter delta (see the note on Node::cnt). Hit vs not-hit is a
// difference in the world, hence different states. On levels where it never
// moves, every node is 0 and the key splits exactly as before.
inline long long keyOf(double y, double vy, int mode, int mini, int flip,
                       int held, double x, int dash = 0, int cnt = 0) {
    const long long qy = (long long)std::llround(y / (g_yq > 0 ? g_yq : 1.0));
    const long long qv = (long long)std::llround(vy / (g_vq > 0 ? g_vq : 1.0));
    const long long qx = g_xq > 0
        ? (long long)std::llround(x / g_xq) : 0LL;
    uint64_t h = 1469598103934665603ull;
    auto mix = [&h](long long v) {
        const uint8_t* b = (const uint8_t*)&v;
        for (size_t i = 0; i < sizeof(v); ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };
    mix(((((long long)mode * 3 + mini) * 2 + flip) * 2 + held) * 2 + dash);
    mix(qy);
    mix(qv);
    mix(qx);
    mix((long long)cnt);
    return (long long)h;
}

// Exit condition. AND OF THE ENABLED ONES. With only one passed it equals OR,
// so the old calls that pass x only change nothing.
//
// Why AND: with "depth alone" as the sole goal, a BRANCH THAT MERELY SURVIVED
// WITHOUT ADVANCING is returned as the solution. Measured 2026-08-11 (lv22's
// x=3,117): the leaf SOLVED by `sectargetdepth=270` alone was at x=2,334 —
// 780px short of the wall (even though the frontier had branches as far as
// x=3,148). "Cross the wall and then survive K ticks from there" can only be
// written as an AND.
inline bool reachedGoal(double px, double py, int depth) {
    bool any = false;
    if (g_targetX > 0.0) { if (px < g_targetX) return false; any = true; }
    if (g_targetYDir > 0) { if (py < g_targetY) return false; any = true; }
    if (g_targetYDir < 0) { if (py > g_targetY) return false; any = true; }
    if (g_targetDepth > 0) { if (depth < g_targetDepth) return false; any = true; }
    return any;      // with no condition at all, never succeed (a misconfig
                     // would make every branch a success)
}

// The press sequence from the section start. The root (nodes[0]) is NOT
// included. The root's `in` is a dummy 0 and is no tick's input. Mixing it in
// delays the whole sequence by 1 tick, and additionally a spurious "release at
// the section start" appears at the front (if the prefix is held, the hand
// lets go there). On lv20 this showed up as the search escaping alive while
// the solution died in plain replay exactly at the wall's x. The input at
// depth d belongs to tick ckptTick + 1 + d of the plain run (the restore
// lands on the state at tick ckptTick+1).
inline std::string inputsOf(int leaf) {
    std::vector<uint8_t> seq;
    for (int i = leaf; i > 0; i = g_nodes[(size_t)i].parent)
        seq.push_back(g_nodes[(size_t)i].in);
    std::reverse(seq.begin(), seq.end());
    std::string s;
    for (size_t i = 0; i < seq.size(); ++i) {
        if (i) s += ',';
        s += (char)('0' + seq[i]);
    }
    return s;
}

}  // namespace secsolve
