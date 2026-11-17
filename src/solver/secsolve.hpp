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
// How state is held: each frontier node keeps its own restorable state -- a
// CheckpointObject, or under psnap a snapshot of a few KB -- released as soon as
// the layer moves on, so an expansion is one restore and one step. (The first
// design kept one checkpoint for the section entry and replayed each node's
// prefix from it; that cost grew with the square of the depth.) A node's
// identity is its parent and the input it took; walking the parents gives the
// plan.
//
// The search is per-layer breadth-first + quantised dedupe + cap truncation.
// It is shaped like the DP on purpose, to keep the truncation properties the
// same as known ones.
//
// Ways to start it:
//   * as a rung of the repair loop (cfg `dpsecauto`, on by default; or the
//     runtime command `secrung <startTick> <targetX> [horizon] [cap]`): the
//     loop suspends, its deepest VERIFIED plan replays to the section head (the
//     handoff in dpsolve::poll, repair.hpp), and a leaf the plain replay
//     reproduces is spliced back into the plan and the loop resumes.
//   * cfg keys at session open (the served path, py/secsolve_run.py) -- a plan
//     in the cfg reaches the section head, `checkpointat`/`secstart` take over.
//   * the runtime command `secsolve <startTick> <targetX> [horizon] [cap]`
//     (cmd.txt) DURING an in-process solve session: the same handoff, but
//     one-way -- the search ends the session itself.
// ============================================================================

namespace secsolve {

inline bool g_on = false;
inline long long g_startTick = -1;   // create the checkpoint at this tick and search from it
inline double g_targetX = 0.0;       // success when crossed alive (<=0 disables)
inline int g_horizon = 300;          // section length (ticks)
inline size_t g_cap = 100;           // states kept per layer
// ---- how the cap chooses (both off = the even spacing in y order) ----------
// cfg `seccover`: inside a family, keep the nodes that cover the (y, vy) plane -- one per
// cell of the finest grid with no more occupied cells than the family's share, both y edges
// always in -- rather than the evenly spaced ones of the list sorted by y, then vy. The
// sorted list samples vy at random within a y: at a custom level's UFO section (a 48 px gap
// between two spikes, a normal-size UFO's centre free over 18 px for about 34 ticks) every
// branch that reached the gap had flapped within the last few ticks, the coasting ones the
// gap needs were all cut, and the search died there at cap 2,000 and passed at cap 6,000.
// Chosen by cover (with the normal-size route forced) it passed at cap 400 and at cap 100.
// On by default since 2026-09-28, with secsizefam, secp2key and dpseccaptiers=2: the three fix
// what a cap keeps; without them the custom level's normal-size route never got through the
// UFO window even at cap 2,000.
inline bool g_cover = true;
// cfg `secsizefam`: player 1's size is a family of its own. When both sizes are in a layer
// each gets half the cap (what one cannot fill goes to the other), so a route that skipped
// an optional size portal is not crowded out by the one that took it. At the same level the
// normal-size route was about a twentieth of the frontier under seccover alone, and it died
// out at a column it has to climb over, at cap 100 and at cap 400. On by default (see seccover).
inline bool g_sizeFam = true;
// cfg `secp2key`: in a dual section player 2's state is part of the dedupe key, and its y an
// axis of seccover's grid. Both players take the same input, so two branches can put player 1
// in the same place with player 2 in different ones -- riding under a block or in the air --
// and past a spike hung under that block only the one in the air lives. With player 1 alone in
// the key the first of the two in won and the other was dropped as a duplicate: at the custom
// level's dual section the ten normal-size branches that reached that spike all died on it,
// and with player 2 in the key eight normal-size branches got past it. On by default (see
// seccover).
inline bool g_p2Key = true;
// cfg `secforcesize` (a probe, off = -1): player 1 starts the search at this size, 0 normal or 1
// mini, whatever the plan left it at -- set again every time the search goes back to its head.
// The state is one no route may reach, so what the search finds is not a plan: it only says
// whether the stretch can be crossed at that size. What a wall a plan cannot get past would
// need, asked of the game rather than of the model.
inline int g_forceP1Size = -1;
// cfg `seckeepsize` (off = -1): every branch whose player 1 is not this size (0 normal, 1 mini)
// is dropped, the spine included -- a search for the route that keeps the size.
inline int g_keepP1Size = -1;
inline long long g_keepDropped = 0;   // branches dropped for it, this search
// ---- exit conditions (other than x) -----------------------------------------
// The axis of travel is NOT necessarily x. lv22's x≈2,266 is a 90-degree
// rotated section: travel is -y and pressing moves x. "Crossing x" is not a valid goal there, so the exit can also be written
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
// said once: "secsolve=1 but no checkpoint, so nothing ran"
inline bool g_warnedNoCkpt = false;

// ---- the reference spine ----------------------------------------------------
//
// One rollout is pinned: the verified solution's own inputs, followed from the
// section head, exempt from BOTH the dedupe and the cap. Without it a window
// the solution demonstrably crosses can still come back EXHAUSTED, because
// dedupe is representative selection and the representative it keeps for the
// solution's class may be a state that dies.
//
// MEASURED on lv22's top window (t0=5,387, 712 ticks): the search dies at depth
// 233 at cap 100 AND at cap 600 -- the same depth to the tick, with six times
// the allowance -- while the same snapshot replayed with the solution's own
// inputs survives 300 ticks bit-identically (snapverify 300/300, worst 0.0).
// So it is not the cap: at 600 the frontier only ever reached 138, meaning
// dedupe was the binding constraint, and the class the solution lives in was
// being merged into one that dies.
//
// This does not put a seed into a solve. This is a measurement pass -- a
// reference replay is allowed as an INSTRUMENT, and what the spine
// buys is that the window is always crossed, so the diff tables are always
// complete around the path rather than absent whenever the search loses it.
inline bool g_spineOn = true;        // cfg `secspine=0` turns it off for A/B
inline int g_spine = -1;             // node index of the spine at this layer
inline int g_spineNext = -1;         // ...and the child that continues it
// THE SPINE AS A CHECK ON THE SNAPSHOT. The spine flies a plan the game has flown, so it lives as
// long as that plan did -- g_spineUntil, the plan's verified death, when the caller knows it. On
// the player snapshot a spine that dies earlier is the search disagreeing with the game, and every
// other branch's death is suspect with it: in official level 22's switch band (the 1x lane) a
// plan that lived to t=2,938 lost its own branch within 9 ticks of a window's head at t=2,740, and
// from t=2,212 the snapshot search came back EXHAUSTED at depth 294 while the same window on
// checkpoints (secsnap=0) crossed past t=2,938 -- the snapshot keeps only the player, and the
// switches' doors and groups are not the player's. Caught that way (g_psnapLied), the search goes
// on -- what it finds is replayed from the head before it counts. A rung whose search then finds
// nothing goes back to the loop; only with cfg seccpfallback=1 (a diagnostic) is the same window
// asked again on checkpoints (hooks_gamelayer).
inline long long g_spineUntil = -1;  // the tick the spine should live to, -1 = not known
constexpr long long kSpineSlack = 3; // ...less this many ticks before it counts
inline bool g_psnapLied = false;     // this search's spine died early on the snapshot
inline long long g_spineLostT = -1;  // ...at this tick
// cfg `secspineoff`: which tick's plan input a layer applies. SWEPT, not
// derived -- see the note at the lookup.
inline int g_spineOff = -1;   // MEASURED: -1 tracks all 60 layers to 0.0499 px;
                              // 0 and +1 lose it at depth 10 and 9, +2 at 25 px
// cfg `secdeadline=<seconds>`: give the search its own clock and let it STOP
// ITSELF, reporting what it reached. 0 = no deadline (the old behaviour).
//
// Why the search needs one when the caller already has a timeout: a caller's
// timeout kills the session, and a killed session writes no verdict line -- the
// depth reached, the frontier, the spine's tracking, all of it is lost. Measured
// on the first night of the 017 queue: 5 of 7 windows hit the 90-minute session
// timeout and came back NO-VERDICT, so the night's most common outcome carried
// no information at all beyond "not within 90 minutes".
//
// With a deadline the search stops at a layer boundary and reports normally.
// It also projects: at layer L, "elapsed per layer x the layers still needed"
// says whether the horizon can be reached at all, and a run that cannot make it
// is stopped THERE rather than at the deadline. Same information, minutes
// instead of an hour.
inline double g_deadlineSec = 0.0;
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
// [2026-08-27] SWEEP IT, never read one run. On a custom level at ckptTick=300 the
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
// The wave trail is not in a player snapshot (cfg `sectrail=0` leaves it alone). In wave mode
// every step appends a point to HardStreak::m_pointArray and nothing takes one away short of a
// checkpoint restore, while GJBaseGameLayer::update walks the whole array (updateStroke) four
// times a step. lv4003: +173 points a layer, ~0.34 us a point -- 49 us a step at 166 points,
// 512 at 1,544. The trail is only drawn, so every snapshot restore empties it: the three wave
// windows of the bench went 33/38/28 s -> 6.7/6.8/8.6 s, and all eight windows kept every
// layer's fingerprint. (A checkpoint restore every 200 snapshot restores had got the first part
// of that for the wrong reason, and moved the world's phase with it.)
inline bool g_trailClear = true;
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
// cfg `secwsnapauto` (on; `=0` is the old snapshot): without it, under `secsnap=2` with
// `secverifyevery` set (a rung), psnap runs in a
// section with moving objects and leaves them where the last stepped node put them, trusting the
// cross-checks to reject what that breaks: the snapshot puts back the player, the effect manager
// and the game state, not the objects, so a node is expanded in a world the node stepped before
// left behind. Official level 19 from t=19,700 (a mini
// UFO; a touch trigger at t~19,695 opens a two-group jaw over ~101 ticks): the plan's own branch,
// which the game flies to t=20,100, died at t=19,878 on the snapshot (the loop's lie at 19,879),
// and the loop's window came back EXHAUSTED eight times before a checkpoint search solved it in
// 1,568 s. With the section's moving set (movingObjectsInSection: what moved under three input
// patterns, and the grouped objects in reach) restored as `secsnap=3` does, the same branch lives
// to t=20,100. The set is capped by `secmaxmov`, as secsnap=3's is not.
// Fourteen snapshot windows on custom levels where the plan's own branch died on the snapshot before
// it did in the game (2026-10-08): with the set (secwsnapset=5) the branch lived exactly as long as
// in the game on 6, against 0 without it. Where secsolve never runs (official levels 1-22) nothing
// changes.
inline bool g_wsnapAuto = true;
// cfg `secwsnapset` (with secwsnapauto, default 5): which objects go into that set. 0 = what moved under the
// three input patterns and every grouped object in reach (secsnap=3's set); 1 = only what moved.
// The grouped-in-reach part covers objects a branch sets moving that the three patterns do not
// (a touch trigger only some branches reach), and it is most of the set's size -- decorations
// carry groups too. Measured at the lv19 window above (cap 100): the full set of 537 costs 0.948 ms
// a restore against psnap's 0.155. And 1 found NOTHING there -- the three patterns are compared end
// to start, and none of them left an object displaced -- so the jaw was only ever covered by the
// grouped-in-reach part. 2 = what moved, plus the objects of the groups the window's own position
// triggers aim at (hooks_gamelayer wsnapTriggerSet). Both lv19 windows (heads 19,700 / 19,300, depth
// goals 454 / 854, cap 100) come back SOLVED on psnap+wsnap with 0 and with 2, the spine living to
// the plan's own death: 0 holds 537 / 504 objects at 0.84 / 0.80 ms a restore, 2 holds 407 / 381 at
// 0.71 / 0.66. The jaw's two groups alone (172 objects, secwsnapgroups=82,83) keep the spine but
// leave the 19,700 window EXHAUSTED.
// 3 = as 2, with every trigger kind in reach (not only the six movers) and their center groups -- the
// Area effects and the rest that move or reshape objects (hooks_gamelayer wsnapTriggerSet).
// 4 = what moved or changed pose, plus the grouped objects in reach that are not decoration (object type
// 7): 0's set without the decoration, which is most of it on a heavy level (a custom window held 10,116
// grouped objects in reach, over secmaxmov) and cannot touch the player.
// 5 = 4 with the reach window's far end carried to the window's horizon: the probe runs min(400,
// horizon) ticks, and the far end goes on by the ticks it did not run at the fastest x step it saw.
// Whatever the set, the search says what it did with it on a `secsolve: wsnap set` line: put back,
// empty, or over secmaxmov and dropped (then the search runs on psnap alone).
inline int g_wsnapSet = 5;
// cfg `secwsnapany` (with secwsnapauto, on by default there): an object counts as moving when ANY of
// what wsnap puts back changes at a sample during the three input patterns -- position, rotation,
// scale, visibility and the area effects' offsets -- not only when its position differs between the
// start and the end of a pattern. An area-animated hazard that rotates and scales in place, or that
// comes back to where it started, never moved by the old test. Measured on a custom level's window
// (2026-10-08): two 170-212 px rotated hazards that an Area effect scales and turns killed the spine
// on the snapshot from the second layer, and a psnap restore left their position, rotation and scale
// offsets where the last stepped node had put them (secworld). secwsnapany=0 is the old test.
inline bool g_wsnapAny = true;
// cfg `secwsnapgate` (on; `=0` puts the set back on every ask): a rung puts the wsnap set back only
// when it has to. Its first ask holds the set back and searches on the player snapshot alone, as
// secwsnapauto=0 does; when that search finds nothing, the same window is asked again at once with the
// set put back, and the ladder counts that answer. A window the plain snapshot solves pays nothing for
// the set. Measured on two custom levels whose windows the plain snapshot solves (2026-10-08): the
// searches' restores and steps are the same with the set and without it, and putting back 622-1,742
// objects made each restore 2-4x dearer -- 770 s of section search against 155 s with the gate, the
// same as with no set at all (and 228 s against 75 s). A rung's SOLVED is always a leaf the plain replay
// reproduced, so a lie on the snapshot can only cost a window its leaf, which is when the re-ask comes.
// A one-way search (secsolve by hand) is not gated.
inline bool g_wsnapGate = true;
inline bool g_wsnapForce = false;    // the rung in flight is that re-ask: the set goes back in
inline size_t g_wsnapHeld = 0;       // the size of the set this search held back (0: none)
// cfg `seclieskippsnap` (off): a window whose plain snapshot search was caught parting from the game
// (g_psnapLied: "the snapshot parts from the game in this window") and found nothing is not searched on
// the player snapshot alone again: a later ask of it puts the wsnap set back from the start, where
// secwsnapgate would hold it back first. It skips only searches of a kind that already found nothing
// there, so it cannot cost a leaf. Measured over two days of runs' logs (2026-10-09): 218 of 468 plain
// snapshot searches were caught that way, and the 53 later asked of a window already caught found
// nothing, at 2,699 s.
inline bool g_lieSkipPsnap = false;
inline std::vector<long long> g_liedWindows;   // the starts (g_startTick) of the windows caught so
inline bool liedWindow(long long start) {
    for (long long s : g_liedWindows)
        if (s == start) return true;
    return false;
}
// cfg `secwsnapgroups=a,b,...` (diagnosis): the set is exactly these groups' objects.
inline std::vector<int> g_wsnapGroups;
// cfg `secworld=N`: compare the world object-by-object after checkpoint restore
// vs after psnap restore
inline int g_worldDiff = 0;
// WHAT THIS SEARCH CANNOT SEE [2026-08-27, measured on a custom level]
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
// ...and in a mode that steers in the air (ship, UFO, wave, swing), which the test above passes
// on its first tick, how many ticks one of the lines has to live (cfg `secgracefly`, 0 = none).
// A rung's leaf only has to be alive at its depth: on coin-off SubZero lv4001 30 of 43 rungs
// were fired by a death 2-9 ticks past the previous splice, a ship the model and the game both
// killed on the same tick from the spliced state.
inline int g_graceFly = 0;
// cfg `secgracedash` (on by default since 2026-10; 0 = off): a body riding a dash ring's dash
// counts as controllable in the test above -- releasing is an input that changes the path, and none
// of the test's lines (never press, hold, press every N ticks) holds a dash for a while and then
// lets go. A custom level's dual section: both bodies take pink dash rings at x~1,695 (t~1,300);
// holding to t=1,420 lives to t=1,439, and every earlier release dies about 9 ticks after it. A
// window whose target (t=1,326) fell inside that dash came back DOOMED at caps 100 to 400 and from
// two heads: the hold line dashes on into the wall at ~1,439 and every releasing line dies soon
// after its release.
inline bool g_graceDash = true;
// Which leaf a depth goal takes first (cfg `secleafmid`, on: the middle of the band; 0 = the
// old order). The frontier comes out of the cap sorted by y, and the layer that reaches the goal
// depth took the first child alive there -- the band's lowest edge. Coin-off SubZero lv4001: in
// every one of the rungs that were fired 2-9 ticks past the previous splice, the leaf was the
// frontier's lowest y (y 564 over a band 564..840, the floor's killer at 546), whatever the
// depth (254 or 632). Loop A/B with dpsecauto (same geode and pricing, coin off): 4001
// 66 rounds / 1,867 s -> 23 / 247, 4002 30 / 514 -> 25 / 461, 4003 68 / 918 -> 71 / 872.
inline bool g_leafMid = true;
// cfg `secleaflife=N` (print only, 0 = off): an accepted exit's life under the best of the grace
// lines, up to N ticks (`secleaf:` lines).
inline int g_leafLife = 0;
// cfg `secdrift=1` (print only): per layer, the object furthest from its start position
// (`secdrift:` lines).
inline bool g_driftLog = false;
// cfg `seccoins` (on; 0 = off), under cfg coinroute: the search takes the coins itself. GD
// collects nothing in practice mode -- collisionCheckObjects skips the whole coin branch
// (destroyObject, pickupItem) while PlayLayer+0x31f0, which togglePracticeMode writes, is set --
// and a section search runs in practice mode, so until now it simply ignored coins: a rung whose
// window held a coin could splice a route past it, and the pin then kept the ladder from ever
// taking it back. Each node now carries the coins its path has touched (the player's box
// overlapping the coin's, where GD credits: offsets up to 34.8 px in one axis, measured on the
// 22 stored solutions), and a branch past a coin's far edge + 15 without it is dead -- the loop's
// own coinmiss bound. Coins already taken or already passed at the section head do not count, nor
// do disabled ones, nor any while the player runs left or sideways (as coinmiss).
inline bool g_secCoins = true;
// cfg dpseccoinrung: the coin this rung was fired for (-1 = none). Leaving it behind is final and
// a leaf without it is no answer. Set by the handoff, cleared when the rung ends.
inline int g_rungCoin = -1;
// cfg `secrungcoinoff` (on by default since 2026-10; 0 = off), under seccoins: the coin a rung was
// fired for (g_rungCoin) is not counted taken for being passed while its group is off -- neither
// per step nor at the head. A coin whose group is off is skipped as "not there, so passing it is no
// miss", and the bit that records that is the one the rung's leaf test reads as "has the coin":
// SubZero 4003's third coin (group 152 off until a Count sees four presses) was "taken"
// (bits 3 -> 7) by every leaf of six spliced rungs, one of them at a head it was already passed at
// (7 -> 7), and GD credited none.
inline bool g_rungCoinOff = true;
// cfg `secbookfirst` (on by default since 2026-10; 0 = off): a section search that hands back to
// the loop puts the loop's coin books (pickup ticks, GD's credits, coinlive latches, item
// counters, the miss latch, the last
// x) back BEFORE its resetLevel starts the next attempt, instead of when the search's coroutine
// ends -- which is after that reset, so the next attempt began with the searched attempt's books
// (hooks_gamelayer.cpp, runSectionSolve's hand-back).
inline bool g_bookFirst = true;
// cfg `secbound` (on; 0 = off): inside a search, an object outside the level's box (every
// object's position at the section head, plus kBoundMargin px) is not re-bucketed into GD's
// sections. A player snapshot puts the in-progress moves back but not the objects they move, so
// a move re-applies on top of where the last step left it, and on lv4001 x~27,000 objects under
// a player-locked move, a follow of it, and later others ran off by millions of px within a layer
// (id 1011 uid 17884 to x +3.3M / y -10.6M). Every section crossed on the way stays allocated,
// and every checkpoint restore's sortSectionVector walks them all: secPhys 4,551 -> 1.5M over a
// run, a restore 1.3 -> 5.8 ms, and a fresh process at the same place still at 1.5-1.6.
// Out there an object touches nothing the player can reach, and the checkpoint restore that puts
// it back re-buckets it as usual. Bench: all 11 windows (lv4003 x8, lv4001 x3) keep every layer's
// fingerprint and the leaf; lv4001's go 25/29/30 s -> 17/20/19 s (~430 re-buckets a step skipped).
inline bool g_boundOn = true;
constexpr double kBoundMargin = 3000.0;
inline double g_boxX0 = 0.0, g_boxX1 = 0.0, g_boxY0 = 0.0, g_boxY1 = 0.0;
inline long long g_boundSkips = 0;
// ...and, bound or not, an object at a position that is not a finite number is never handed to the
// game's re-bucketing inside a search. GJBaseGameLayer::addToSection (0x226500) turns the position
// into a cell index and grows the grid to it: a NaN made the index negative and the grow threw
// "vector too long" (updateObjectSection 0x227f50 -> addToSection -> 0x252df7 -> the vector's
// grow; a custom level's ladder window at t=9,628, depth 66, every run). Counted, and the first one kept
// (uid, id, position) for the search to report and give up on: its world is no longer the game's.
// A position this far out counts too: the cell index is an int (x or y times 0.01), and no level's
// object comes near it.
constexpr double kPosLimit = 1e8;
inline long long g_nonFinite = 0;
inline int g_nonFiniteUid = -1, g_nonFiniteId = -1;
inline double g_nonFiniteX = 0.0, g_nonFiniteY = 0.0;
// cfg secthrow: the exceptions thrown on purpose so far in this process (Config::secThrowKind).
inline int g_secThrowsDone = 0;
// cfg `secshaderskip` (on; 0 = off): inside a search, leave the shader layer as it is. resetLevel
// calls GJBaseGameLayer::updateShaderLayer twice per restore (once through resetLevelVariables),
// and it moves the layers that hold every object's sprite in and out of the shader's container;
// each move walks the whole subtree through onExit / onEnter, Geode's hooks on them included. A
// late restore of a long search (a heavy custom level's slice, 09-26) spent 87% of its time there,
// and the cost grew with the number of restores.
// On by default since it was measured to change nothing but the time: a heavy custom level solved
// cold with and without it made the same 50 rounds ([fp] line for line) through the same 8,707
// search layers (every layer's fingerprint), in 328 s against 470 s -- the restore 74 -> 12 us.
inline bool g_shaderSkip = true;
inline long long g_shaderSkips = 0;

// cfg `secshadersig=1` (print only): where the shader layer's hierarchy stands -- which parent
// each sprite batch and object layer hangs from, the part updateShaderLayer moves -- at a
// search's start and end, at the first real reset after it, and at each level's first reset
// (`shadersig:` lines). Run with and without secshaderskip, the lines after the search say whether
// the reset the search did not do is made good by the first one the game does.
inline bool g_shaderSig = false;
inline bool g_shaderSigPending = false;   // a search ended; the next real reset prints
inline std::string shaderSig(GJBaseGameLayer* l) {
    if (!l) return "no layer";
    auto kind = [l](cocos2d::CCNode* p) -> char {
        if (!p) return '0';
        if (p == l->m_inShaderParent) return 'I';
        if (p == l->m_aboveShaderParent) return 'A';
        if (p == l->m_objectLayer) return 'O';
        if (p == l->m_inShaderObjectLayer) return 'i';
        if (p == l->m_aboveShaderObjectLayer) return 'a';
        if (p == l) return 'L';
        return '?';
    };
    std::string seq;
    size_t in = 0, above = 0, other = 0;
    if (l->m_batchNodes) {
        for (auto* n : geode::cocos::CCArrayExt<cocos2d::CCNode*>(l->m_batchNodes)) {
            const char k = kind(n ? n->getParent() : nullptr);
            seq += k;
            seq += std::to_string(n ? n->getZOrder() : 0);
            seq += ',';
            if (k == 'I' || k == 'i') ++in;
            else if (k == 'A' || k == 'a') ++above;
            else ++other;
        }
    }
    std::string layers;
    for (cocos2d::CCNode* n : {static_cast<cocos2d::CCNode*>(l->m_objectLayer),
                               static_cast<cocos2d::CCNode*>(l->m_inShaderObjectLayer),
                               static_cast<cocos2d::CCNode*>(l->m_aboveShaderObjectLayer),
                               l->m_inShaderParent, l->m_aboveShaderParent})
        layers += kind(n ? n->getParent() : nullptr);
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char ch : seq + "|" + layers) { h ^= ch; h *= 1099511628211ULL; }
    char b[160];
    snprintf(b, sizeof(b), "batches=%zu in=%zu above=%zu other=%zu layers=%s shader=%s sig=%08x",
             l->m_batchNodes ? (size_t)l->m_batchNodes->count() : (size_t)0, in, above, other,
             layers.c_str(), (l->m_shaderLayer && l->m_shaderLayer->isVisible()) ? "on" : "off",
             (unsigned)(h & 0xffffffffULL));
    return b;
}
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
// Every write of g_active goes through setSearching, so the section hook that acts only during a
// search (updateObjectSection, hooks_gamelayer.cpp) is enabled before the search's first update
// and disabled after its last. The hook installs g_onActive.
inline void (*g_onActive)() = nullptr;
inline void setSearching(bool v) {
    g_active = v;
    if (g_onActive) g_onActive();
}
inline int g_feed = 0;              // button state given at this tick (0/1)
inline int g_held = 0;               // currently pressing? (for handleButton deltas)
// Can be turned off with cfg `secjumpbuf=0` (default ON): when restoring a held
// press, also set m_jumpBuffered. WITHOUT THIS A HELD BRANCH BECOMES A
// DIFFERENT THING FROM THE PLAIN REPLAY — history and measurements are in the
// note on secArmHold in phase1.cpp. It is made switchable for A/B testing.
inline bool g_jumpBuf = true;
// cfg `secjbkeep` (on by default since 2026-10; 0 = off): after a held restore has raised
// m_jumpBuffered, put back each body's own value as the point had it -- the head's from the plain
// flight (g_headJb), a node's from its capture (DashState::jumpBuf). Raising it on BOTH bodies is a
// buffered press for a body whose press was spent long ago: a custom level's dual section (a search
// from t=741), a flipped UFO holds a gravity dash ring's press from t=746 while the spider partner,
// flipped by the ring the same press fired, falls to the floor; the plain flight lands it at t=802
// and holds on to t=944, while every restored branch had the spider teleport on landing, into a saw
// -- the spine died on that tick. With the node's own value the spine lives on.
inline bool g_jbKeep = true;
inline int8_t g_headJb[2] = {-1, -1};   // m_jumpBuffered of p1 / p2 in the plain flight, -1 = none

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
    int8_t jumpBuf = -1;   // m_jumpBuffered at the capture (cfg secjbkeep), -1 = not taken
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
    uint8_t mini;    // player 1 mini? (a family of the cap under cfg secsizefam)
    uint8_t dual;    // in a dual section? (then y2 is player 2's y; see g_p2Key)
    float y2;
};
inline std::vector<Node> g_nodes;
// THE CHECKPOINT DOES NOT CARRY EVERY PLAYER MEMBER (cfg `seccpplayer`, on). A load puts back
// what GD's checkpoint saves and leaves the rest of the player as the last run left it. On the
// section search's checkpoint paths -- the head, a cross-check's replay origins, a leaf's replay,
// an exit's survival lines -- that "last run" is whichever branch was stepped before, so the replay
// the psnap nodes are checked against started from a player no run of its inputs would have.
// Measured at a custom level's dual section (a search from t=24,680): loading the head's
// checkpoint again left player 2's m_padRingRelated, m_stateJumpBuffered, m_wasRobotJump,
// m_blackOrbRelated and its slope members at the values the branch before had given them, where
// the first load had left the plain flight's; and every load, the first included, cleared player
// 1's m_stateRingJump where the plain flight had it set. After
// each such load the search now writes back its own snapshot of that point: at the head, the
// player as the plain flight brought it there (taken before the head's checkpoint is made), and
// at a cross-check anchor, the snapshot retaken from the replay that made it.
inline bool g_cpPlayer = true;
inline std::vector<uint8_t> g_headPlain;
// Per-node dash state (same index as g_nodes). Held because the checkpoint's
// RESTORE does not read it back -- see the note above DashState.
inline std::vector<DashState> g_dash;
// Per-node exact y velocity. The checkpoint restore re-rounds it onto the 0.001
// grid (hole 3), and a search restores once per tick, so the rounding
// is applied once per step instead of once per section.
inline std::vector<double> g_vy;
// Per-node boost accumulator (m_accelerationOrSpeed) and pad-touch flag. The
// SAME KIND OF HOLE as the dash: the field survives a plain replay because
// nothing rewinds it, and a search reads whatever the previously stepped node
// left behind.
//
// MEASURED, lv22 window t=5,387, spine against the head run (`robodbg`):
// the accumulator differs from the FIRST layer -- head 1.2150, spine 1.5000 --
// and is LATENT for 158 of them, y and vy agreeing to the last digit. It bites
// at the pad: the pad fires on both sides at t=5,545 (both accel 0, both
// vy 5.71), and on the next tick the head ramps 0.0225 per tick and holds
// vy 5.71 for the whole 16-tick boost, while the spine, sitting at 1.5, gets no
// continuation and decays under gravity (5.710, 5.516, 5.322, ...). 1.5 is the
// cutoff the field is compared against, so a search restores every node into a
// state that reads as "this boost is already over".
//
// This is why the spine came apart THREE TICKS INTO A SUSTAINED HOLD rather
// than at its first tick, which no amount of reasoning about button events
// explained: the first tick of a boost is the same either way.
inline std::vector<double> g_accel;
inline std::vector<uint8_t> g_pad;
// cfg `secheadaccel` (on; `=0` reads them from the live player): the head node's accumulator and
// pad flag, both bodies, are the plain
// flight's (g_headPlain, taken before the head's checkpoint is made), not the live player's when
// the search sets up. By then the qualify probes have loaded the head's checkpoint, and a load
// leaves a robot at 1.5 with the pad flag of the run before -- and every expansion of the head
// writes the node's pair back, so the head's own snapshot (which has the plain flight's values)
// never reaches a child. MEASURED on a custom level's window (2026-10-08, `robodbg`): a mini robot
// holding a jump through the head, the game at vy 4.492 with the accumulator at 0.09 and climbing
// 0.0225 a tick; the spine had 1.5 and pad 1 from the first layer, lost 0.195 of vy a tick, and
// died at depth 12 where the game flew on for 197 ticks. Every search from that head started from
// the same wrong pair, so no cap or cross-check could recover it. Over fifteen uncleared custom
// levels with a 600 s budget, it on against off: one more cleared, ten unchanged, four moved by a
// search that returned a different solution or trimmed differently, the plan's own branch the same.
inline bool g_headAccel = true;
// ...and all four for PLAYER 2 (cfg `secp2extras`, on). Every one of them was written back to
// m_player1 only, so in a dual section p2 took its boost accumulator and pad flag from whichever
// node was stepped last, and on the checkpoint path its y velocity off the 0.001 grid as well.
// MEASURED on a custom level (dual robots mirrored about y=285 in the game, their y summing to
// 570.000 on every tick): a checkpoint-branched search from t=7,985 kept player 1 on the plan
// (spine y 267.1 at x=13,372.6, the game 267.12) while player 2 came up 25 px high with vy 7.45
// against the game's 5.695 at x=13,374.6, and died on a saw there -- the plan it was replaying
// flew on to x=14,736. With p2's four written back, the loop's window from t=8,585 was solved and
// the run went on past x=14,736; without, that window and the three wider ones behind it came back
// EXHAUSTED, as before (same build, cfg secp2extras=1/0). The psnap path gave the same rounds
// either way on that level: psnap copies p2's bytes, the velocity among them.
inline bool g_p2Extras = true;
inline std::vector<DashState> g_dash2;
inline std::vector<double> g_vy2;
inline std::vector<double> g_accel2;
inline std::vector<uint8_t> g_pad2;
// ...and the same thing for the plain checkpoint/restore path (hole 2),
// which the section runs use and which had no dash handling at
// all. MEASURED without injection: lv22 checkpoint at t=2,112 mid-dash,
// restore at t=2,232, and from two ticks on the restored run FALLS (vy 0.324,
// -0.272, -0.570, ...) while the run from the head holds y=241.7341 at
// vy=0.000.
inline DashState g_ckptDash;
// cfg `secheaddash` (on by default since 2026-10; 0 = off): the section search's head takes its
// dash from the plain flight (g_ckptDash, and g_ckptDash2 for player 2) instead of from what the
// head's checkpoint load left, which never has one. Without it a head inside a dash put every node
// back undashed: the node's psnap restore brings the plain flight's m_isDashing back and the head's
// DashState, taken after the load, sets it to 0 again. A custom level's dual section: the loop's
// window from t=812, inside a flipped UFO's dash held from t=746, lost its spine 7 ticks in (t=819)
// on the snapshot and on checkpoints alike, where the plan flew on to t=889.
inline bool g_headDash = true;
inline DashState g_ckptDash2;
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

// The bytes the per-node tables hold allocated (their capacities, not their sizes), for the line that
// says what freeNodeTables gave back.
inline size_t nodeTableBytes() {
    return g_cps.capacity() * sizeof(CheckpointObject*) + g_nodes.capacity() * sizeof(Node)
           + (g_dash.capacity() + g_dash2.capacity()) * sizeof(DashState)
           + (g_vy.capacity() + g_accel.capacity() + g_vy2.capacity() + g_accel2.capacity()) * sizeof(double)
           + g_pad.capacity() + g_pad2.capacity();
}

// The per-node tables, let go with their buffers: clear() keeps a vector's allocation, and one
// search of a custom level holds ~620k nodes (hooks_gamelayer.cpp's note at its `worlds`), about
// 360 bytes each across these. The node checkpoints still held are released first -- a search
// releases each as its layer is passed (releaseCp), so what is left is a search that never got
// to the end -- except node 0's, which is the section checkpoint (g_ckpt) and has its own owner.
// A search starts by filling all of these afresh, so emptying them changes nothing it reads.
// Swapped with an empty vector: `v = {}` is the initializer-list assignment, which empties the
// vector and keeps its buffer, the same as clear().
inline void freeNodeTables() {
    for (size_t i = 1; i < g_cps.size(); ++i)
        if (g_cps[i]) g_cps[i]->release();
    { decltype(g_cps) e; g_cps.swap(e); }
    { decltype(g_nodes) e; g_nodes.swap(e); }
    { decltype(g_dash) e; g_dash.swap(e); }
    { decltype(g_dash2) e; g_dash2.swap(e); }
    { decltype(g_vy) e; g_vy.swap(e); }
    { decltype(g_accel) e; g_accel.swap(e); }
    { decltype(g_pad) e; g_pad.swap(e); }
    { decltype(g_vy2) e; g_vy2.swap(e); }
    { decltype(g_accel2) e; g_accel2.swap(e); }
    { decltype(g_pad2) e; g_pad2.swap(e); }
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
//
// 50 ms, not the 12 it was: every frame between two slices costs the game's own frame work (~4 ms
// measured, nearly all of it outside the game layer's update -- `secsolve: frames=`), and at 12 ms
// that was a fifth of a search's wall time. A heavy custom level solved cold at both, side by side:
// the same 8,707 search layers and 50 rounds, 2,794 frames between slices (34 s) against 737
// (3.5 s), the search 166.5 s against 136.8 s, the restore unchanged. The window still takes about
// twenty frames a second while a search runs.
inline int g_sliceMs = 50;
inline std::chrono::steady_clock::time_point g_sliceStart;
// What the frames between two slices cost (print only, `secsolve: frames=`): the wall time from
// the end of one slice to the start of the next -- everything the game does in a frame that is
// not the search -- and the part of it the overlays took.
inline long long g_frames = 0;
inline double g_frameGapMs = 0.0, g_overlayMs = 0.0;
inline double g_outsideMs = 0.0;   // ...of which outside the game layer's update (the frame)
inline std::chrono::steady_clock::time_point g_sliceEnd;
inline bool g_sliceEnded = false;

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
    g_keepDropped = 0;   // cfg seckeepsize's count (a search also starts it at 0)
    g_psnapLied = false;
    g_liedWindows.clear();   // cfg seclieskippsnap: the windows are this layer's
    g_spineLostT = -1;
    g_done = false;
    setSearching(false);
    // A search dropped in the middle -- the player left the level -- also leaves these raised,
    // and the next session inherited them: with g_noKill up the destroyPlayer hook swallowed
    // every death, so the next solve "cleared" at once and replayed (lv16, lv20), or never got
    // anywhere (lv22) (user report on the panel, 2026-09-25).
    g_on = false;
    g_noKill = false;
    g_died = false;
    g_rungCoin = -1;
    g_feed = 0;
    g_held = 0;
    freeNodeTables();   // after the task: its frame indexes them
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
inline std::vector<uint8_t> inputVecOf(int leaf) {
    std::vector<uint8_t> seq;
    for (int i = leaf; i > 0; i = g_nodes[(size_t)i].parent)
        seq.push_back(g_nodes[(size_t)i].in);
    std::reverse(seq.begin(), seq.end());
    return seq;
}

inline std::string inputsOf(int leaf) {
    const std::vector<uint8_t> seq = inputVecOf(leaf);
    std::string s;
    for (size_t i = 0; i < seq.size(); ++i) {
        if (i) s += ',';
        s += (char)('0' + seq[i]);
    }
    return s;
}

}  // namespace secsolve
