#pragma once
#include "dp/modifiers.hpp"

namespace dp {

constexpr double kFloorY = 105.0;   // cube rest height on the ground line
// ...which is a REST HEIGHT, not the ground. The ground plane is at 90 and the
// rest height is 90 + the player's half, so a mini player rests at 99, not 105.
// Measured on lv11 t=9793: an inverted mini ship descending at vy = -3.424 is
// stopped dead at y = 99.000 and held there. The code below therefore uses
// kGroundY + pHalf everywhere it used to use kFloorY.
constexpr double kGroundY = kFloorY - 15.0;   // 90.0
constexpr double kCubeHalf = 15.0;  // outer box (landing)
// Mini. Measured on lv11: the cube crosses the mini portal (rect x[4409.5,
// 4440.5], so the boxes touch at x = 4394.5) at t=3384 and immediately starts
// falling from its resting y=165, then settles at **y=159.000** on the same
// continuous platform whose top is 150. 159 - 150 = 9, i.e. the half size goes
// 15 -> 9 (0.6x). Without this the model kept standing at 165 on a surface GD
// had already dropped away from.
constexpr double kMiniHalf = 9.0;
constexpr double kMiniScale = 0.6;
// Mini jumps LOWER. Measured on lv11 t=3528: same tick, same x, both grounded
// at y=159, GD leaves with vy = 8.9440 and the model left with 11.1800.
// 8.944 / 11.18 = 0.800 exactly. Gravity is NOT scaled -- both sides step
// -0.2160 per tick from there, and the traces stay exactly 2.2360 apart in vy
// until the landing. Without this the model floats 40 px above GD through the
// whole mini section and every hazard test after it is answered for the wrong
// player.
// UNVERIFIED for mini: terminal velocity, and the orb/pad impulses. Measure
// them the same way when a plan first depends on one.
constexpr double kCubeJumpMini = 8.944;
// ...and the same 0.800 applies to the launch impulses. Measured by injecting
// the mini cube onto lv11's yellow pad (x[7888.5,7913.5], t=6076): GD returned
// vy = 12.800, and the full-size pad is 16.0 -- 12.800 / 16.0 = 0.800 exactly,
// the same factor the jump showed. Two independent measurements agreeing to
// four digits is a rule, so it is applied to the orbs too.
// NOTE the orb side is INFERRED from those two, not measured directly. lv11's
// yellow orb at (8055,255) is the first plan that will depend on it: if the
// factor is wrong there, the GD replay diverges at that orb and says so.
// (This is separate from the per-MODE ratios -- ball is 0.600/0.700 -- which
// really do differ per constant and must stay measured one by one.)
constexpr double kMiniImpulse = 0.800;
constexpr double kCubeInner = 4.5;  // side/ceiling kill box
// ---- ROBOT (mode 5) -------------------------------------------------------
// Read out of PlayerObject::updateJump (win RVA 0x38b900), not fitted. The
// robot is a cube with three differences, all of which are literal branches in
// that function:
//
//  1. GRAVITY x0.9. updateJump picks a per-mode scale (xmm7) right after the
//     mode flags: ball 0.6, spider 0.6, ROBOT 0.9, everything else 1.0, and it
//     multiplies the `dt * gravity` term that is subtracted from m_yVelocity.
//     It is NOT applied to the jump.
//  2. JUMP x0.5. `if (m_isRobot) jumpHeight *= 0.5` sits immediately after the
//     jump height is read (m_gravity's sibling double at this+0x7c0), before
//     the mini factor. So a robot tap leaves at half a cube's jump for the
//     same speed, and mini still multiplies by 0.800 on top.
//  3. HOLD = HOVER, not thrust. While the button stays held after the jump,
//     updateJump ADDS exactly the same `sign * dt * gravity * 0.9` it is about
//     to subtract, so vy is constant -- the robot floats. The budget is a
//     double at this+0x830 that is zeroed when the jump fires and grows by
//     `dt * 0.1` per tick while the hover runs; the hover stops once it
//     reaches 1.5. With dt = kCubeG / 0.9581990 = 0.225422 (the same dt that
//     makes the cube's gravity -0.216) that is 1.5 / (0.1 * 0.225422) = 66.5,
//     i.e. 67 ticks.
//     Releasing the button sets a flag (this+0x99c, cleared only by the next
//     jump), so a release ENDS the hover permanently -- re-pressing in mid-air
//     does not resume it.
//
// Also from the same function: the robot's jump needs m_jumpBuffered as well as
// the hold, i.e. a fresh press. Holding through a landing does NOT re-jump.
// The model's cube already tests the rising edge (`input && !s.action`), so
// that comes for free.
// CONFIRMED WITH THE GAME (2026-09-03), after step.hpp had spent two weeks
// asserting the opposite on the strength of a citation that no longer resolves:
// on calib_holdjump_robot (flat ground, one press at t=30, never released) the
// robot jumps at t=31 and is still standing on the floor at t=600, while the
// cube on the identical rig bounces five times in the same window. The gate is
// +0x986 next to m_jumpBuffered (updateJump 0x38ba3c..0x38ba51), cleared by the
// arm that jumps (0x38c728) and restored only by releaseButton (0x398260).
constexpr double kRobotGScale = 0.9;
constexpr double kRobotJumpScale = 0.5;
constexpr int kRobotHoverTicks = 67;
// ---- SPIDER (mode 6) ------------------------------------------------------
// Same shape as the robot: the mode table in updateJump gives it the BALL's
// gravity scale (0.6), and its action is not a jump at all -- it is
// PlayerObject::spiderTestJump, a teleport to the surface on the other side.
//
// Measured on lv22 (injected onto the spider section at x~1,010, full size):
//   gravity  -0.129 / tick, i.e. 0.216 x 0.600 -- the ball's number to three
//            digits, on 13 consecutive airborne ticks
//   rest     y = 163.500 on a floor whose top is 150, and y = 286.500 under a
//            ceiling whose underside is 300. Both are 13.5 off the surface,
//            not the 15 every other ground mode uses.
//   teleport t=262 y=163.5 upright -> t=263 y=286.5 upsideDown, one tick, and
//            the gravity is flipped. The target was the block at (1035,315),
//            whose x range starts 9.5 px to the RIGHT of the player, so the
//            search is at least as wide as the player's own box.
//   gate     a tap in MID-AIR does nothing (13 ticks of undisturbed free fall
//            after one). GD only calls spiderTestJump from the grounded branch
//            of updateJump, same as the cube's jump.
// A/B switches for bisecting a regression. Not knobs: both default to OFF and
// exist so that "which of today's changes broke lv18" can be answered by two
// runs instead of two rebuilds.
inline bool g_noMiniWave = false;   // --no-miniwave
inline bool g_oldLatency = false;   // --old-latency
// --no-ringmode: restore "a held button fires a ring in every mode". GD closes
// the CONTACT path in ship/UFO/wave/swing (playerTouchedRing's last gate), so
// in those four a ring only fires from the press itself. This exists because a
// change that only ever REMOVES firings cannot be told from "no change" by a
// suite that reports the same numbers either way -- the two arms have to be one
// build apart, not one rebuild apart.
inline bool g_noRingMode = false;   // --no-ringmode
// --no-pressspent: restore "a ring only asks whether a RING already spent this
// hold" (the pre-2026-09-05 gate, `!s.ringHold`). GD's latch is +0x986, which
// every consumer clears, so the grounded jump spends the press too. Exists for
// the same reason as --no-ringmode: the change only ever REMOVES firings, and
// on this corpus it removes exactly one, so the two arms have to be one build
// apart rather than one rebuild apart to be told from "no change" at all.
inline bool g_noPressSpent = false; // --no-pressspent
// Deaths during the PLAN RECONSTRUCTION, which is a different question from
// deaths during the search. 2026-09-08, lv22@5400 at cap 8000: the search never
// put a state on the spike uid 5897 (0 of ~1,478 evaluations), the reconstructed
// plan walked into it (16 hits), the sub-step loop exited on `!dead` so DIE ran
// -- and the run still reported SOLVED and wrote a trace past the death. So a
// SOLVED verdict does not mean the plan survives its own replay, and nothing
// counted how often that happens.
//
// ONLY the reconstruction is counted, deliberately. The search runs on eight
// threads and a plain counter there would be a data race; `g_inRecon` is written
// once, between the search and the reconstruction, and only read during the
// search, so the branch below costs a predictable read and no synchronisation.
inline bool g_inRecon = false;      // set after "reconstructing plan"
inline long long g_dieRecon = 0;    // DIEs while g_inRecon
// ...and split by cause, because the total is not a number about physics.
// `out-of-play` (step.hpp:8211) is a SEARCH PRUNE -- the upright escapee bound,
// not a kill -- so a run reporting n=666 last=out-of-play may be 666 prunes and
// zero kills, or 665 and one. The total plus the last cause cannot tell those
// apart, and reading a whole census off the last entry is the mistake this file
// records elsewhere as "the last witness is not the population".
inline std::vector<std::pair<const char*, long long>> g_dieReconWhy;
// The same question asked where it can be answered: the witness resim's own
// `rdead`, which is the final plan's single walk. g_dieRecon counts every DIE
// the reconstruction runs; these count only the walk, so they say whether the
// PLAN dies. Contiguity (last - first + 1 == dead) separates "one corpse
// re-dying every tick" from "several deaths", which share a total.
inline long long g_resimDead = 0;
// ...and what killed it, taken at the FIRST dying tick. Without this, "the
// model died and GD died" can only be matched on the fact of a death, and two
// deaths at different places in the level read as agreement -- the model would
// be credited with knowing something it did not know.
inline const char* g_resimWhy = nullptr;
// ...and WHO, taken at the same tick. A cause string says `cube/hazard`; it does
// not say which object, and on lv20 the answer decides an open question: the
// model-visible hazards near the death (type 2/47 -- level_loader.hpp:923 emits
// nothing else) are all static, all `groups=0`, and none of them overlaps the
// player's box, so the killer has to be moving geometry. Naming it turns that
// from an elimination into an identification.
// The search's side of the same question the resim's `trig=` answers. The two
// walks apply triggers with different masks -- the group's (cli.hpp:3026) and
// the state's own (cli.hpp:4066) -- so a moving object can be in one place for
// the search and another for the resim, and on lv20 that is the whole
// disagreement. -1 = off; set to the tick to look at.
inline long long g_trigDbgT = -1;
inline int g_resimUid = -1;
inline float g_resimObjX = 0.f, g_resimObjY = 0.f;
inline uint32_t g_resimTrig = 0;   // the walk's own trigger mask at that tick
inline long long g_resimTicks = 0;
inline int g_resimFirst = -1;
inline int g_resimLast = -1;
inline void noteReconDeath(const char* why) {
    ++g_dieRecon;
    for (auto& e : g_dieReconWhy)
        if (e.first == why || (e.first && why && !std::strcmp(e.first, why))) {
            ++e.second;
            return;
        }
    g_dieReconWhy.emplace_back(why, 1LL);
}
// --hazdbg <uid>: trace ONE object down the ground-mode hazard loop in
// step.hpp, gate by gate. -1 = off, which is the default, so the shipped search
// is untouched and quick_regress stays byte-identical. A uid rather than a
// blanket switch because the search evaluates many states per tick and an
// unconditional print buries the answer.
inline int g_hazDbgUid = -1;        // --hazdbg
// --no-portalseat: restore "a mode portal never seats on its own tick" (the
// pre-2026-09-05 behaviour). GD runs the activation pass before the solid
// pass, so a body that becomes a cube inside a flat solid is resolved on that
// tick; the model picks its landing branch from the mode ENTERING the tick
// (step.hpp:1521) and so could not. A/B switch, one build apart.
inline bool g_noPortalSeat = false; // --no-portalseat
// --no-forceorder: fold the force field/box contribution into the acceleration
// BEFORE the terminal clamp (the pre-2026-09-05 order). GD adds it after, in
// PlayerObject::update rather than updateJump, and does not clamp it.
inline bool g_noForceOrder = false; // --no-forceorder
// --no-rot2900halve: a same-frame 2900 turn that changes the polarity does not
// halve vy (the pre-2026-09-05 behaviour). GD reaches it through flipGravity,
// whose mulsd 0.5 @0x39a2dc runs on any real polarity change.
inline bool g_noRot2900Halve = false; // --no-rot2900halve
// --no-portallatch: a gravity portal can fire on every pass (the
// pre-2026-09-05 behaviour). GD latches it on the first OVERLAP, so the second
// pass finds hasBeenActivated already up and does nothing.
inline bool g_noPortalLatch = false; // --no-portallatch
// WHICH HALF stepOne is running, for the --slopedbg prints only. Nothing reads
// it as physics. A per-tick diagnostic line cannot be counted on a dual level
// without it: stepOne runs twice with swapHalves between, so a portal each half
// passes ONCE looks exactly like one the same body passed twice, and reading it
// the second way is what put a retracted witness into 283e8a4 (see 87049e5).
inline int g_halfNow = 0;
// --no-ceilseat: a ceiling ramp only ever pushes the player OUT of itself (the
// pre-2026-09-05 behaviour), never acquires it from below. GD's underside gate
// takes it while the player is still under the line, within tol_u, and lifts it
// on. See the site in step.hpp for the band and why the press is the whole gate.
inline bool g_noCeilSeat = false; // --no-ceilseat

// --no-slopeveto: solids are resolved even where a ramp beside them is already
// governing the surface (the pre-2026-09-05 behaviour). GD's own scan sits in
// collidedWithObjectInternal ahead of the solid; see slopeVetoesSolid for the
// predicate, which scores 1,673 of 1,673 overlapping ticks on the rigs
// calib_slopeveto/2 and matches all 14 corpus ticks the hit-flag census found.
inline bool g_noSlopeVeto = false; // --no-slopeveto

// --no-slopefreshrect: a FRESH ramp contact is acquired without GD's inset-rect
// reach test (the pre-2026-09-05 behaviour). collidedWithSlopeInternal splits on
// m_wasOnSlope at 0x38fc0e and, for a new contact, intersects the player's rect
// with the OBJECT rect inset 1 px top and bottom (0x38fc3c-0x38fc7b) before
// anything else can run. See the site in step.hpp for the witness and the phase
// rule; the same test is already spelled out in slopeWouldAcquire.
inline bool g_noSlopeFreshRect = false; // --no-slopefreshrect

// --no-slopeseat: a ramp seats the player at the surface sampled at an x
// CLAMPED into the ramp's span, plus/minus a flat player half (the
// pre-2026-09-06 behaviour). GD's own seat is
// `slopeYPos(centreX) -/+ playerRect.h/(2 cos t)` -- the line EXTRAPOLATED past
// the span, with the bounds applied to the TARGET y instead
// (0x38fd42-0x38fdf0, re-evaluated on every acquiring tick 0x39072c) -- see
// slopeSeatTarget in slopes.hpp for the formula, the RVAs and the lv16 t=9,241
// witness. The two agree everywhere except on the ENTRY side of a ramp.
inline bool g_noSlopeSeat = false; // --no-slopeseat

// --no-ufolandtol: the UFO keeps kShipLandTol (6.0) as its floor acquisition
// allowance, the pre-2026-09-06 behaviour. That 6.0 was never measured -- the
// site's own comment said so ("kShipLandTol (6.0) stays for the UFO, whose own
// acquisition has NOT BEEN MEASURED this way"), the ship having been measured
// down to 0.001 while the UFO was left alone.
//
// GD's floor gate is `acquire <=> s*targetY > s*y - (extraTol ? tol : 0)`
// (@0x38fea5) and its only widths are 0 / 1 fresh / 2 continuing, selected by
// extraTol and m_wasOnSlope. 6.0 is not among them at any setting.
//
// Witness lv19 t=14,587 (UFO, uid 9522): targetY 315.928 against a free y of
// 317.298, so gap = 1.370. GD refuses -- `315.928 > 316.298` is false -- and
// seats a tick later once the line rises to meet the player. The model admits
// it through the main test (1.370 <= 6.0) and pulls the player 1.370 px DOWN
// onto a line it has not reached, one tick early. The 1.370 also settles which
// width is live: 2.0 would admit it, so fresh/continuing is load-bearing here.
//
// The corpus reach was counted before the change: of 3,424 UFO rows, 1,612
// carry the 6.0, and the rows this narrows are 4 -- of which exactly ONE is
// actually seated (the witness). No changed row is admitted by another
// disjunct, so nothing keeps its seat by a second path, and no UFO row anywhere
// has a gap in (0, 1.0], which is the only interval where "assume the band
// applies" and "clamp to zero" could have disagreed.
inline bool g_noUfoLandTol = false; // --no-ufolandtol

// A NORMAL-SIZE UFO that flaps while seated on a floor ramp leaves at this
// value instead of its plain 6.871. Measured on the calibration rig
// `calib_ufojump` (14 cells, Wine worker 99): the two control cells, pressing
// on the flat run-up, return 6.8710 and 6.6480 exactly, and every one of the
// six normal sweep cells returns 8.0000 -- at |m| 0.5, 1 and 2, and at both 1
// and 3 ramps. So it is an ASSIGNMENT, not a bonus added to the jump:
//   * gradient does not enter it (three gradients, one value),
//   * ride time does not enter it (ramp factor 0.674 vs 1.000, same value),
//     which refutes every slopeRampFactor-shaped hypothesis outright,
//   * and it is not the cube's on-ramp bonus (kSlopeJumpBonus), whose |m| ratio
//     is 1.4192 where the measured ratio is 1.0000.
// The bonus this implies, 1.129, is well under the 0.4x cap (2.748), so the cap
// is not what flattens it.
//
// MINI IS DELIBERATELY NOT FIXED and stays wrong. The same rig gives a mini
// 9.3680 at |m| 1 and 2 and 8.1470 at |m| 0.5, against the model's unchanged
// 6.6480. Two rules fit those three points equally well -- a threshold at
// |m| >= 1, and the ramp's width -- and they cannot be told apart, because the
// 22 levels contain exactly three slope shapes ((30,30), (30,60), (60,30)), in
// which |m| >= 1 <=> w = 30 without exception. Separating them needs a 60x60 or
// a 30x15 slope, i.e. an object these levels never use. Guessing between the
// two would put a width rule into the model under a gradient's name.
inline bool g_noUfoRampFlap = false; // --no-uforampflap

// --no-ridelandlaunch: the slope-exit launch fires off any CONTACT, the
// pre-2026-09-06 behaviour, instead of only off a ride that became a landing.
// GD's launch comes out of the ride; a contact that never landed has no ride to
// launch from. Witness lv19 t=14,633: the UFO meets the ramp at svy +6.323,
// fails the |vy| <= 5.0 hitGround gate (which this model already implements at
// the seat, keeping vy and leaving grounded clear), GD flies straight on, and
// the model launches anyway on leaving at 14,643 -- -2.156 that then persists
// exactly, the signature of one velocity overwrite.
//
// The gate is State::rideLanded and NOT `grounded`: the seat sets grounded only
// `if (rideLands && !(flipForRide && ridesTop))`, so a flipped rider on a floor
// ramp's top lands without it. Gating on grounded would change 2 rows and break
// lv16 t=8,913, whose launch GD makes and whose model value already matches.
// With rideLanded the reach is one row, and lv16 8,913 and lv17 18,573 both
// keep theirs.
inline bool g_noRideLandLaunch = false; // --no-ridelandlaunch
constexpr double kUfoRampFlap = 8.0;

// --no-rampfirst: keep the model's own resolution order -- every solid
// resolved before the ramp block runs, and never revisited -- instead of GD's
// ramp-then-solid. GJBaseGameLayer::checkCollisions (0x2137f0) is two passes:
// the bucket scan resolves slopes (type 0x19) and teleports (0x1c) in place,
// while solids (type 0 / 0x15) and hazards are only pushed onto a list and
// resolved afterwards through PlayerObject::collidedWithObject (0x214687), so
// in GD a solid always sees the seat the ramp already wrote. See the rule at
// the end of the slope block in step.hpp for the witness (lv16 t=4,142) and
// for what it deliberately does not cover.
inline bool g_noRampFirst = false; // --no-rampfirst

// --no-slopenudge: a ramp contact keeps the pre-2026-09-06 velocity rules --
// the gradient-signed +-2.0 writes at the three ceiling/ride sites, the ship's
// kShipRampG ladder and the swing's walkIn0 one -- instead of GD's own
// V3/V4/V5 (see slopeNudge in slopes.hpp and the site in step.hpp). Kept as the
// A/B arm: the whole 22-level replay suite has to be byte-identical with it.
inline bool g_noSlopeNudge = false; // --no-slopenudge

// --no-mpushreach: the flight loop's mover-catch (`fly/mpush`) seats at ANY
// penetration, the pre-2026-09-06 behaviour, instead of GD's reach-back
// `kShipLandTol + |dcy|/0.25` (see the site in step.hpp). Kept as the A/B arm:
// the whole 22-level replay suite has to be byte-identical with it.
inline bool g_noMpushReach = false; // --no-mpushreach

// --no-boostlatch: GD's velocity-limit exemption (State::boost) is carried by
// the SWING ONLY, as it was before 2026-09-06. The byte [player+0x952] has no
// mode test at any of its ten writers and is read by the ship's acceleration
// selector (0x38c5be / 0x38c5d8) and by the terminal clamp shared by ship, UFO
// and swing (0x38ca9f), so the real scope is those three -- see boostLatchMode
// in slopes.hpp for the reading and the sites in step.hpp for the consumers.
// Kept as the A/B arm: with it the whole 22-level replay suite has to be
// byte-identical to the build before the change.
inline bool g_noBoostLatch = false; // --no-boostlatch

// --no-ringfirsttouch: when several rings touch on the same tick the model
// picks the LOWEST UID, the pre-2026-09-06 behaviour, instead of preferring the
// one that was already in contact on the previous tick. GD's container
// `PlayerObject +0xa38` (m_touchedRings) is appended to only on a ring's FIRST
// contact (playerTouchedRing 0x217e40, containsObject 0x217ea1 / addObject
// 0x217eb5) and is pruned in place, order preserved, at every tick's head
// (resetTouchedRings(p,0) 0x3982e0), so pushButton's forward walk (0x3980b7)
// fires the FIRST-TOUCHED ring; ascending uid is only the same-tick tie-break
// (the bucket sort 0x2143a6 / comparator 0x205210). See the site in step.hpp.
// Kept as the A/B arm: with it the whole 22-level replay suite has to be
// byte-identical to the build before the change.
inline bool g_noRingFirstTouch = false; // --no-ringfirsttouch

// --no-padobb: a rotated PAD is judged against the raw player rotation, with no
// override at all (the pre-2026-09-06 behaviour). It is the OUTER arm of the
// three; g_noPadPlayerRot below wins over it, so passing both reproduces the
// build that shipped the axis-aligned player square. See the site in step.hpp.
inline bool g_noPadObb = false;         // --no-padobb

// --no-ballcorng: the BALL's hang window keeps the constant 1.5 grace on its
// exit end (the pre-2026-09-06 behaviour) instead of one tick's movement
// |useDx|, which is what the ship (r68/r74) and the swing (r71c/r74) already
// use. Two witnesses bracket the grace from opposite sides and leave no
// constant available:
//   lv17 t=18,571/18,572  full ball,  m=-1, x1=24,120, xoff 6.213, dx 1.29825
//                         last ride cx 24,113.838 / first drop 24,115.137
//                         => 0.051 <= G < 1.350
//   lv16 t=13,116/13,117  mini ball,  m=-1, x1=19,872, xoff 3.728, dx 1.614258
//                         last ride cx 19,869.660 / first drop 19,871.273
//                         => 1.388 <= G < 3.001
// The two intervals are disjoint, and |useDx| lands inside both, so these two
// ticks do not merely permit a dx-proportional grace, they require one. The
// reverse-travel mirror (okLo) is NOT converted: it has zero witnesses in the
// whole corpus, and an untested arm is better left where it was measured to be
// harmless. See the site in step.hpp.
inline bool g_noBallCornG = false;      // --no-ballcorng

// --no-padspinpre: a pad's same-tick rotation step turns toward the gravity at
// the END of the tick (the pre-2026-09-06 behaviour) instead of the gravity at
// the moment GD called runNormalRotation. Only a GRAVITY pad can tell the two
// apart, since nothing else moves the gravity within the tick. See the site in
// step.hpp for the 116 witnesses and the two samples in the comment there.
inline bool g_noPadSpinPre = false;     // --no-padspinpre

// --no-satrotraw: the four oriented-contact sites in step.hpp advance `s.rot`
// by one spin step before handing it to the SAT (the pre-2026-09-06 behaviour)
// instead of passing the state's rotation as it stands. The sites are the
// gravity-portal pass, the portal pass, the pad loop and the speed portals;
// they all carried the same expression
//     pRot* += (s.rotNeg ? 1.0 : -1.0) * (s.mini ? 2.25 : 1.7307692);
//
// IT WAS A SIGN INVERSION AGAINST THE MODEL'S OWN ROTATION LAW. The cube's law
// (step.hpp, the `spinNow && !c.grounded` branch) writes
//     c.rot = c.rot + (spinSign ? -rate : rate)
// with `spinSign` = `rotSignNow` = `s.rotNeg` on a plain airborne tick -- THE
// SAME FIELD, THE OPPOSITE MAPPING. The advance therefore subtracted the step
// the law was about to add. Census over the whole corpus, 240,271 cube ticks,
// each level cut at its own first divergence so both sides stay on one
// world-line; median |error| against GD's own `rot` column at tick t, mod 90:
//     s.rot, no advance          1.731    (corpus 1.720 .. 2.247)
//     s.rot - step (as shipped)  3.462    (corpus 3.450 .. 4.473)
//     s.rot + step (law's sign)  0.023    (corpus 0.002 .. 0.664)
//     the model's own rot[t]     0.000    (corpus 0.000 .. 0.028)
// The shipped arm is EXACTLY TWICE the no-advance arm on every level -- 3.462
// against 1.731 full size, 4.473 against 2.247 for lv11's mini -- which is the
// arithmetic signature of a sign inversion and of nothing else. Independently,
// the column's own step carries `rotNeg` with the law's sign on 180,093 of
// 180,248 airborne cube ticks.
//
// AND THE REMEDY IS NOT TO FLIP THE SIGN. The census scores against GD's
// END-OF-TICK column; the SAT is fed an angle INSIDE the tick. Asked at GD's
// own positions for lv20's uid 7030 (Q1 -- the rule, not a trajectory), margins
// in px at the two ticks that decide it:
//     arm                        7,298            7,299        fires
//     GD's own rot[t]            31.220 -> -0.585 32.950 -> +0.642  7,299
//     s.rot, no advance          32.950 -> -0.157 31.220 -> +0.214  7,299
//     s.rot + step (law's sign)  31.220 -> -0.585 29.489 -> -0.228  never
//     s.rot - step (as shipped)  34.681 -> +0.257 32.950 -> +0.642  7,298
// The law-signed arm fires NOWHERE, because GD's rotation reverses on the
// activation tick -- the pad's own runNormalRotation turns the column from
// -1.7308 to +1.7308 at 7,299 -- so extrapolating from the pre-pad sign lands
// at 29.489, inside the only refusal notch at that position (swept at 0.001 deg
// over the whole circle: refuses on 27.051 .. 30.375 and accepts everywhere
// else). Choosing on the census median would have deleted lv20's pad.
//
// `s.rot` unmodified is the only candidate the model can compute before the pad
// loop that reproduces GD's answer, and it has margin on both sides: it refuses
// 7,298 by 0.157 px and takes 7,299 by 0.214 px, against a column drift of
// 0.023 deg which is 0.006 px at the local slope (0.243 px/deg) -- a factor of
// 25 of slack. All 28 cases of that table were re-evaluated through the SHIPPED
// predicate (`leveldp --eval-padgate`): 0 disagreements, 5 hits and 23 misses,
// so the agreement is not the trivial agreement of two functions that say no.
//
// REACH, measured at GD's own positions over the whole corpus: 7,765 oriented
// objects, 46 of them in the families that reach these four sites (lv16 2,
// lv18 4, lv19 4, lv20 23, lv21 13, lv22 none at all), of which a cube's AABB
// window touches 13. The two arms differ on 5 objects and change a FIRST FIRE
// on exactly one -- lv20's pad uid 7030, 7,298 -> 7,299, which is GD's tick.
// The other four are lv16 uid 3450's contact tail (8,072 vs 8,073) and three
// that agree. It does NOT fix lv20: the model still dies at 15,125.
inline bool g_noSatRotRaw = false;      // --no-satrotraw

// --no-padplayerrot: a rotated PAD's activation is judged with the player square
// forced AXIS-ALIGNED (the 2026-09-06 behaviour, commit a874728), instead of
// turned by the player's own rotation.
//
// GD's gate, read out of `GJBaseGameLayer::collisionCheckObjects` (2.2081 win
// 0x214960; lab note measure-pad-activation-shape-2026-09-06), reaches
// `activatedByPlayer` (vt +0x558, via bumpPlayer 0x2179d0, its only call site)
// for a type-8 pad iff BOTH of
//    inclusive AABB overlap of player->getObjectRect() and obj->getObjectRect()
//                                                            0x214b09-0x214b56
//    and, when obj->m_isOriented [+0x2e8],
//        overlaps1Way(objOBB, plOBB) && overlaps1Way(plOBB, objOBB)
//                                          0x214b5e / 0x214bb0 / 0x214bbf
// where the player's OBB is a 30 x 30 square TURNED BY `player->getRotation()`
// (PlayerObject::getObjectRotation 0x3a0670 = CCNode vt+0x158), rebuilt from the
// live position inside the object loop (0x214b95/0x214ba1; setPosition dirties
// it every tick at 0x39c676). There is no radius, tolerance, depth or velocity
// term in the gate: the player's angle is the only input to it that moves.
//
// a874728 forced that angle to 0 because the mod's `ccl:` hook prints an
// axis-aligned 30 x 30 `prect`. That rect is real, but it is the AABB conjunct
// (`player->getObjectRect()`, taken once at 0x2149b8) -- NOT the shape the SAT
// uses. `PlayerObject::getOrientedBox` (0x3a0650) never sets the player's own
// m_isOriented, which is exactly why `prect` reads 30 x 30 while the cube spins
// and why every instrument that prints only the rect is blind to the rotation.
//
// The witness is lv20's uid 7030 (id 35, (10835.9,133.5), rot 29, ohw 18.1253 /
// ohh 2.9000). Re-scoring the two-way SAT at GD's OWN x,y and rotation column
// for dump ticks 7,295..7,301 (margin, px, > 0 = contact):
//     t       7295    7296    7297    7298    7299    7300    7301
//   pRot=0   -1.176  +0.672  +2.570  +4.237  +5.036  +0.984  -3.025
//   GD rot   -1.486  -1.213  -0.913  -0.585  +0.642  -2.996  -6.606
// The turned square first passes at **7,299, which is GD's own activation
// tick**; the axis-aligned one passes at 7,296, three ticks early, and that is
// lv20's whole-run first divergence before this change. After it, at the
// MODEL's own angle rather than GD's, the firing landed on 7,298 -- one tick
// still early, and that last tick was the ROTATION-SIGN leaf and not this
// shape. [2026-09-06] That leaf is taken too: with g_noSatRotRaw's advance
// deleted the model's angle is GD's to 0.023 deg and this pad fires on 7,299.
//
// SCOPED TO THE MODES WHOSE ANGLE THE MODEL TRACKS -- see padPlayerRotMode.
// Kept as the A/B arm: with it the whole 22-level replay suite has to be
// byte-identical to the build before the change.
inline bool g_noPadPlayerRot = false;   // --no-padplayerrot

// Which modes may hand the pad gate the player's real rotation.
//
// GD turns the player's square in EVERY mode; this list is a hedge against the
// MODEL's own rotation, not against GD's. Handing the SAT an angle the model
// does not track would make the test worse than the axis-aligned square it
// replaces, so the list is derived, not chosen:
//
//   * `State::rot` is written in exactly four places -- the wave's easing
//     (step.hpp, `kWaveRotK`), the mode-portal edge table, the cube's spin and
//     the ship's bank, and the ball's `rotStep`. The rotation block itself is
//     `if (c.mode == 0) ... else if (c.mode == 1) ... else if (c.mode == 2)`,
//     plus the wave inside its own branch. So modes 3 (ufo), 5 (robot), 6
//     (spider) and 7 (swing) carry rot == 0 for their whole life (the edge
//     table writes 0 on entry for every mode but cube and ball, and nothing
//     advances it), and for them this list cannot change a single digit
//     whichever way it is written.
//   * Of the four modes that DO carry an angle, the rate census over the whole
//     corpus (415,581 ticks, |d rot/tick| error > 0.01 deg) reads cube 3.7% /
//     ship 13.8% / wave 21.1% / **ball 70.3%** -- and the ball's number is
//     categorical rather than a coefficient error: its air rate is a stake only
//     `flipGravity` and `ringJump` write and the model does not carry one, so a
//     ball that left the ground off a step or a pad is turning at a rate that
//     is simply not GD's. Sweeping the threshold from 0.001 to 1.0 leaves the
//     ball at 70.3% (categorical) while ship and wave collapse (boundary).
//
// So the only load-bearing exclusion is the BALL. UNWITNESSED EITHER WAY: all
// 21 corpus contact rows against a rotated pad are mode 0, full size (census
// below), so nothing here is measured -- it is the shape of the hedge.
inline bool padPlayerRotMode(uint8_t mode) {
    return mode == 0 || mode == 1 || mode == 4;   // cube / ship / wave
}

// --no-dualflip: the partner is not fired (the pre-2026-09-05 behaviour, where
// each half re-derived the flip inside its own stepOne, one integration late).
inline bool g_noDualFlip = false; // --no-dualflip
// GD's gate for firing the partner: the two bodies' SIX mode bytes must match.
// Written as GD writes it rather than as  == b, because the difference is
// real -- wave is not among the six, so a cube and a wave both read false on
// every compared flag and therefore COUNT AS MATCHING. Collapsing this to
// equality would refuse a pair GD accepts.
inline bool sameModeFlags(uint8_t a, uint8_t b) {
    static const uint8_t kCmp[6] = {1, 2, 3, 5, 6, 7};  // ship ball ufo robot spider swing
    for (int i = 0; i < 6; ++i)
        if ((a == kCmp[i]) != (b == kCmp[i])) return false;
    return true;
}
// --no-r52gravhold: drop r52 (a gravity portal right after a rotation-frame
// change does not fire if the player was already inside it). The arm exists to
// ask whether the portal latch has made r52 a fossil: r52 was measured on
// lv22 t=6,323 uid 13833, which the latch now spends at t=6,300. It is not
// SUBSUMED, though -- r52 also covers type 3, which the latch does not -- so
// the question is empirical.
inline bool g_noR52GravHold = false; // --no-r52gravhold
// The gravity-frame speed at or below which a ramp CONTACT becomes a LANDING.
// GD's own: hitGround sets m_isOnGround only when s*v <= this (comisd against
// the double @0x622E98); above it the caller restores the old vy (0x3907dd).
// Not fitted -- read from the binary.
constexpr double kSlopeLandV = 5.0;
// --no-slopeland5: a ramp contact always lands, whatever the speed (the
// pre-2026-09-05 behaviour).
inline bool g_noSlopeLand5 = false; // --no-slopeland5
// --old-slope: A/B escape hatch (same convention as --old-latency). Restores
// the pre-2026-08-04 slope exit: ball = tap-anchored line / mini x0.625, and
// NO ride-time ramp. See slopeExitVy / slopeRampFactor for why the new form
// is the disassembled truth.
inline bool g_oldSlope = false;
// A jump taken while still ON a ramp adds this FRACTION of the ramp's own exit
// velocity (slopeExitVy's cube row), on top of the mode's jump. Measured on the
// `rampjump` calibration map -- see the jump branch for the 12-unit table.
constexpr double kSlopeJumpBonus = 0.25;
// The bonus is capped at 0.4 x that mode's bare jump value (rampjump rig 12/12,
// measured in the note at the jump branch).
constexpr double kSlopeJumpBonusCap = 0.4;
constexpr double kSpiderGScale = 0.6;
constexpr double kSpiderHalf = 13.5;
constexpr double kSpiderHalfMini = 8.1;   // 13.5 * 0.6, UNVERIFIED
// ...and the teleport SEARCH is 1.0 px wider in x than that. The lv22 note above
// could only say "at least as wide as the player's own box"; lv21 pins it.
// Measured 2026-08-10 on lv21's flipped spider resting at y=406, tapping next to
// the block (11791,375) whose top is 390 (so the near target is y=403.5 and the
// far one is y=223.5, 180 px apart -- the level's whole route turns on which).
// Seven injections of the player's x through the MCP, reading GD's landing:
//   |x-cx| = 28.799  29.299  29.499 -> 403.5 (near)
//   |x-cx| = 29.510  29.650  29.799  29.999 -> 223.5 (far)
// so the boundary is 29.5 to within 0.011 px, i.e. hw + 14.5 for a 30-wide
// block. The model was using hw + kSpiderHalf = 28.5 and took the far target
// on every tick in the 28.5..29.5 band.
// NOT SEPARABLE from this one site: "the spider's x half is 14.5" and "the box
// is 15 wide and GD wants more than 0.5 px of overlap" are the same formula
// (hw + 14.5) for every object. Only the vertical half is independently pinned
// at 13.5, by the rest positions (403.5 = 390 + 13.5, 223.5 = 210 + 13.5).
// Applied to the teleport search ONLY, which is where it was measured. The
// support test a few lines up has the same shape and may well want the same
// number, but that is a second variable and lv1-20 cannot referee it (nothing
// before lv21 has a spider).
constexpr double kSpiderSearchHalfX = 14.5;
constexpr double kSpiderSearchHalfXMini = 8.7;   // 14.5 * 0.6, UNVERIFIED
// The wave has TWO different sizes, and they are not the same number.
//
// The BAND clamp keeps its centre 10 off the surface: measured on lv17 t=3565,
// it comes down the diagonal and stops dead at y=100.000 (vy=0, onGround=1)
// on a band whose floor is 90, then slides for 30+ ticks. That 10 is the
// clamp's own constant (the same slot that holds 15 for everyone else), not a
// hitbox.
//
// Its HITBOX is far smaller -- about 5. Two independent brackets on lv17:
//   * the ball portal at (11055,197) w=34 fires at x=11033.47, and the tick
//     before is 11032.17, so half is in [4.53, 5.83)
//   * the ship portal at (8865,195) h=86 spans y=[152,238] and the wave rides
//     past it at y=140.25/141.54 WITHOUT switching, so half < 10.46
// 5.0 is the only round value in both. This half is what portal, pad and orb
// contact use as well -- with 15 the model entered that ship portal four ticks
// early and came out in the wrong mode while GD was still a wave.
// The mini values carry the established 0.600 ratio and are UNVERIFIED.
// CONFIRMED against GD 2026-08-06: injecting the player at y=600 in lv20's wave
// section and reading where GD puts it back gives y=380.000 at three x well
// apart (4,550 / 5,121 / 5,199). The band there is the x=867 wave portal's
// (cy=239 -> floor 90, ceil 390) -- the second wave portal at x=4,445 is INERT
// because the player is already a wave, so it never rewrites the band. 390-10
// = 380 exactly. 10 it is; pHalf (5) would give 385 and 15*vsize would give 375.
constexpr double kWaveClamp = 10.0;
constexpr double kWaveClampMini = 6.0;
// The side/crush kill's grace after a gravity flip: GD stamps the flip time in
// flipGravity (player+0x800) and collidedWithObjectInternal (:1180-1226) takes
// the kill arm only when `now - stamp >= 0.1` s, otherwise re-seating the
// player on the face and calling hitGround.
//
// MEASURED IN THE GAME, lv18's blue pad at (24375,273) and the two 30x30 blocks
// under and beside it (2026-09-01, worker 98). The player is injected 2.5 px
// into a block's top face -- the inner box crosses it, the outer box does not
// resolve it -- and the only thing varied is how long ago gravity flipped:
//     no flip at all                         DEATH
//     flip on the same tick                  LIVES, re-seated to top+15, og=1
//     flip 12 ticks earlier                  LIVES, og=1
//     flip 24 ticks earlier                  LIVES
//     flip 25 ticks earlier                  DEATH
//     flip 36 ticks earlier                  DEATH
// so the window is 25 ticks counted from the tick the dump first shows the flip
// -- 0.1 s x 240 Hz = 24, plus one because GD stamps the time after that tick's
// row is written. Counted the model's way (flipT = 0 on the tick gdUpOf
// changes), the grace holds while flipT < 25.
//
// SIMPLIFICATION: GD measures TIME and this counts TICKS. They agree everywhere
// the corpus goes except inside a timewarp zone (id 1935), where the substep
// density changes; lv22's is the only one.
constexpr int kFlipGraceTicks = 25;
// How long an id-1859 touch keeps the ceiling resolution armed. GD sets its
// counter (player+0xb7c) to 2 on the touch and decays it once per tick, so the
// arm covers the touch tick and the one after it. See modifiers.hpp's
// armBoxTouch for the three measurements that pin the object as the real
// discriminant.
constexpr int kArmTicks = 2;
constexpr double kWaveHalf = 5.0;
// ...and the MINI one is NOT 0.600 of it. That carry-over was flagged
// UNVERIFIED above and this session refuted the same 0.600 twice already (the
// kill box does not scale at all, the ramp box scales by ~0.70). GD brackets it
// on lv20's 1.1x speed portal uid4303 (rot 41, real box 51 x 56 -- obb and
// w0,h0 agree, so the oriented shape is not in question). Writing the fire test
// on the portal's own long axis, the threshold is `25.5 + 1.41077 * half`:
//   x=6128.008 y=298.316  |lx| = 28.979  GD does NOT fire  -> half <  2.466
//   x=6131.903 y=300.912  |lx| = 27.743  GD DOES fire      -> half >= 1.590
// (both read off a plain replay's dump, the ticks GD's `speed` column steps
// from 0.9 to 1.1; the model and GD are bit-identical up to the first of them).
// 3.0 fires three ticks early, and after that the run carries 0.316 px of extra
// x per tick for the rest of the level -- the `dy is 0 and only dx grows`
// signature. 2.0 sits in the middle of the bracket; it is BRACKETED, not
// measured, so tighten it before deriving anything from it.
constexpr double kWaveHalfMini = 2.0;
// ...and the HAZARD stage-1 box is a third one again: GD tests the AABB of
// `player->getObjectRect()`, which cfg `hitboxtrace=1` reads as 10x10 full and
// **6x6 mini** -- half 5.0 and 3.0. The 2.0 above is a bracket off a speed
// portal's firing, a different code path, and it is 1 px too small here.
// Bisected live in GD on both faces, two levels, two speeds; see the table at
// `wHazHalf` in stepOne. 3.000 exactly, touching counts, not speed-dependent.
constexpr double kWaveHazHalfMini = 3.0;
// ...and the PORTAL contact box is that same 6x6 rect, so it is 3.0 too.
// Kept as its own name because the two are measured separately.
// [2026-09-01] **The speed portal uses this one as well.** It was the last
// portal path still on kWaveHalfMini, on the strength of the lv20 bracket
// above -- and that bracket does not constrain it: its site (uid4303) is a
// FULL-SIZE wave in the reference, and it was computed with an axis-aligned
// player square (`25.5 + 1.41077*half`) where GD tests the sprite-rotated box.
// The model fires that same site on GD's exact tick with the full-size 5.0.
// What the 2.0 did cost: lv21's 0.9 portal at (19587.1,239.18) fired THIRTY
// TICKS and 48 px late, running that stretch 24% fast (census family
// `m4/mini1/g0/gdg0/sp1.1/air/in1`, which the fix removes).
constexpr double kWaveContactHalfMini = 3.0;
// [2026-08-19 night 3] **The mini's portal contact half-width is smaller than the
// collision's 9.** Lining up the ramps calibration rig tick by tick between GD and
// the model (py/calib_diff.py), **only the mini->normal size portal has the model
// 1 tick early, 8/8** (norm->mini is the same tick 8/8, i.e. the normal size's 15
// is right). Bracket: against portal id99 (cx=4470, w=31 -> hw 15.5)
//   GD does not fire at |x-cx| = 23.342  -> half < 7.842
//   GD fires at        |x-cx| = 22.044  -> half >= 6.544
// Same family as the wave's mini being 3.0 (6x6 box). 7.5 is 12.5x0.6, the middle
// of the bracket. **All 8 units share the same geometry, so it cannot be narrowed
// further** -- tighten it with a rig that varies the phase (shifts the portal x).
constexpr double kMiniContactHalf = 7.5;
// ...and SQUARE. The `portwave` rig (2026-08-18) read b ~= 11.5 on the y side
// at portal rots 20/43 and the tall box was implemented and REVERTED the next
// day: the rig put every portal centre at GROUND_TOP+15 while the zero-input
// wave slides at ~+6, and that constant dy of ~-9 enters the SAT's u-axis
// inequality with exactly the |sin| weight the y half has -- (a,b)=(3,11.5)
// at dy=0 and (3,3) at dy=-9 predict the SAME brackets at both angles, so the
// rig cannot tell them apart. A rig for b must VARY the portal heights.
// The square box is the one with in-level evidence: lv20's rot-53 stack
// (uid527, census t0=600) rejects at |ly|=50.89 (t=849) and fires at 49.07
// (t=850) against ohh 45 -- a y half of 2.77..5.80, which is 3.0 and rules
// out 11.5 (that fires at t=846, and the census following shrank 400->247).
// It also agrees with GD's own getObjectRect (6x6 mini).
// ...and the box that KILLS the wave on a solid is a different, much smaller
// one. The 5.0/3.0 above are the CONTACT half (portal, pad, orb) and match
// GD's own getObjectRect for the wave exactly -- 10x10 full, 6x6 mini
// (cfg `hitboxtrace=1`, lv20 t=645/t=654). The rect is not what a solid is
// tested against.
//
// Bisected live (gd_run + inject, boundary to 0.01 px), two levels, two object
// ids, both sizes, and BOTH faces:
//   lv17 t=7040 FULL, block id173 (9135,165) top 180: y=181.510 lives,
//                                                     y=181.500 dies
//   lv17 t=7040 FULL, same wall's left face x=9090:   x=9088.4  lives,
//                                                     x=9088.5  dies
//   lv20 t=839  MINI, block id94  (1093,125) top 140: y=141.510 lives,
//                                                     y=141.500 dies
// Every one of those brackets closes on 1.5, and the exactly-1.5 cases die, so
// touching counts as a hit. The value does NOT scale with vsize -- the mini and
// the full wave share it -- which is why deriving it from the rect (5.0, and
// 0.6x of that) was wrong in both sizes.
//
// This was costing whole routes, not pixels: replaying lv20's own best plan,
// the model tracked GD to 0.3 px for 786 ticks and then killed the player at
// t=786 on id94 (1005,195) at a corner overlap of 1.1 x 3.4 px that GD flies
// straight through.
// SOLIDS ONLY. The hazard test keeps the contact half -- measured, see the
// note at the wave's hazard branch.
constexpr double kWaveKillHalf = 1.5;
// The dart's sprite rotation eases toward its travel angle by this fraction of
// the remaining gap per tick. Measured, see the wave branch in stepOne.
constexpr double kWaveRotK = 0.0625;
constexpr double kWaveRotKMini = 0.10;
constexpr double kRadToDeg = 57.29577951308232;
// ...but the RAMP test uses its own box, and it is NOT centred on the player.
// Point-probed on lv20's zig-zag wave corridor (t=4042, one tick, x and y both
// injected, boundary bisected to 0.004 px) against the two ceiling ramps
// (5625,295) 310->280 and (5655,295) 280->310 and the two floor ramps under
// them. Every one of the 26 boundaries is reproduced by ONE rectangle:
//   corner sampled at x + 3.072 when the wedge thickens to the RIGHT
//                     x - 4.068 when it thickens to the LEFT
//   surface offset by 4.0 vertically, and the sample x clamped to the ramp box
// e.g. ceiling line - 7.072 in the far field, flat at corner - 4.0 across the
// 3 px left / 4 px right of the two ceilings' shared corner (that flat is the
// clamp), floor line + 8.072 on the mirrored side -- and the floor pair has NO
// flat, because there the sample points away from their shared corner.
// The old single 8.0 is that rectangle read at 45 degrees from
// one side only (lv18's downhill ramp at (19125,195): kill at line + 8.0 +-
// 0.4, and this box says 8.070 there).
// Only |m| = 1 is measured; the split into "sample x" and "offset y" is what
// separates it from a plain half extent, and it is what makes the corner flat.
// The mini values carry the established 0.600 ratio and are UNVERIFIED.
constexpr double kWaveSlopeDxR = 3.072;
constexpr double kWaveSlopeDxL = 4.068;
constexpr double kWaveSlopeDy = 4.0;
// ...and the mini ratio is BRACKETED, not carried over. The old 0.600 was
// flagged UNVERIFIED and lv20 refutes it; 1.000 is refuted the other way. Two
// GD verdicts in the same corridor pin it, writing the box as `s` x the
// measured full-size one (3.072 / 4.068 / 4.0):
//   LIVES  lv20 t=3691, mini wave (4790.70, 236.210), ramp uid3321 (4785,225)
//          sy0=210 sy1=240 (m=+1, floor half, sample to the RIGHT)
//          survival needs 236.210 >= 230.70 + s*(3.072 + 4.0)  ->  s <= 0.779
//   DIES   lv20 t=3712, mini wave (4819.28, 225.825), ramp uid3383 (4815,225)
//          sy0=240 sy1=210 (m=-1, floor half, sample to the LEFT)
//          the kill needs 225.825 <  220.72 + s*(4.068 + 4.0)  ->  s >  0.633
// so s is in (0.633, 0.779] and 0.700 sits in the middle of it. 0.600 misses
// GD's kill by 0.26 px -- and that one corridor cost the cold run ~40
// iterations of 5 px each, because every tick GD killed and the model did not
// came back as a `kill` fixup instead of as a rule.
// Two brackets, one constant: TIGHTEN IT with py/hitbox_sweep.py before
// deriving anything from the value.
constexpr double kWaveSlopeMiniScale = 0.700;
constexpr double kWaveSlopeDxRMini = kWaveSlopeDxR * kWaveSlopeMiniScale;
constexpr double kWaveSlopeDxLMini = kWaveSlopeDxL * kWaveSlopeMiniScale;
constexpr double kWaveSlopeDyMini = kWaveSlopeDy * kWaveSlopeMiniScale;
constexpr double kShipHalf = 15.0;  // ship outer box (any contact excludes)
// player-vs-hazard half: the OUTER 30x30 box. Measured on lv1: spike (525,105)
// hitbox 6x12 kills at |dx|=17.4 -> 3+15. (rectcorridor's 10 was a safe
// under-estimate for blocking, not the true kill range.)
constexpr double kHazHalf = 15.0;
// Planning margin: GD checks collisions at sub-tick positions and sampling only
// the tick endpoints can miss a corner graze, so plans are kept this far off
// hazard edges.
//
// 1.5 with 4 samples was too blunt for tight corridors. Measured on lv10's ball
// zone: GD carried the ball ALIVE past a spike at a gap of 15.107, while the
// model kills at kHazHalf + margin = 16.5 -- so every branch through that
// corridor died in the model and the frontier collapsed to a single state by
// x=3244. The margin exists to cover the gap BETWEEN samples, so pay for it
// with samples instead: 8 sub-steps put consecutive samples ~0.4 px apart at
// cube speed, and 0.5 covers that with room to spare.
// Even 0.5 was too much. The lv10 measurement pins it: GD ran the ball past a
// spike of half-width 2.79 at a horizontal distance of 17.90 and it LIVED --
// 17.90 is barely outside the exact box contact at 2.79 + 15 = 17.79, so any
// margin at all kills a case GD survives. With 8 sub-steps the sampling gap is
// ~0.4 px and the endpoints are exact, so the margin has nothing left to buy.
// A plan that grazes now shows up as a death in the driver's GD replay, which
// re-anchors -- that is the right place to catch it, not a blanket inflation of
// every hazard in the level.
// Hazards are sampled at this many points between the previous and the new
// position. A tick IS one GD physics step, so GD only ever tests the END of the
// tick -- every intermediate sample is a position the player never occupied,
// and killing on one would be killing on a fiction, which is what makes the
// model miss frame-perfect gaps like lv11's three spikes at x=24,883.
// TRIED AND REVERTED (2026-07-31): kSubSteps = 1, endpoints only. lv1-10 all
// still cleared, but lv11 went the WRONG way -- the GD replay fell from 24,874
// back to 11,914, i.e. the model started emitting plans GD kills. So the
// intermediate samples are catching something real; GD's collision pass is
// evidently finer than the per-tick state dump exposes. The under-kill is the
// dangerous direction, so this stays at 8 until it can be measured directly
// instead of argued from the tick rate.
constexpr int kSubSteps = 8;
constexpr double kHazMargin = 0.0;
// Cube: how far below a surface the foot may already be and still be pushed up.
// 3.0 was a guess and it was too small: lv6 t=13606 has GD landing the cube on
// the ledge at x=17700 (top y=180) with the foot 3.29 px below it, and the model
// missed by 0.29 px and fell past.
// Swept at that ledge (inject y in 0.5 steps, read land/die):
//   penetration <= 10.50 -> lands,  >= 11.00 -> dies
// so the true limit is ~10.5-11. 6.0 is used instead: it is the ship's exactly
// measured boundary (kShipLandTol) and it covers everything observed in play,
// while a 10 px upward teleport would be a large licence to hand the planner on
// the strength of one sweep whose contact had already begun at injection time.
// Redo as a contact-onset sweep (like the ship's) before raising it further.
// TRIED AND REVERTED (2026-08-03): 10.5 (the swept value quoted above), to see
// whether lv18's frontier could then reach the ledge 269 (24,675,255) top 270
// that it tops out 8.35 px below. It changed NOTHING -- the frontier's y range,
// alive counts and death tick were bit-identical. kLandTol only forgives a foot
// that is ALREADY within N px BELOW the face; at y=262 the foot is at 253, i.e.
// 17 px below 270, which no tolerance in this range reaches. lv18's wall is not
// the landing tolerance.
// [2026-08-18] 6.0 -> 10.5 (the measured value the sweep above produced, as is).
// The landing at lv19 t=9,451 has a 6.05px penetration and fell **0.05px outside
// the threshold**. Once the robot's g was corrected to the measured -0.194 the
// player's y matched GD, and as a result the penetration went from 5.958 to 6.047
// and the landing was dropped (GD does land). Restoring the constant to the
// measured value passes both. --landtol A/Bs it.
// [2026-08-18, revised the same day] 10.5 -> 10.0. 10.5 made lv9 STUCK in the same
// day's cold run: the gravity-flipped ball at x=11,265 "landed" from **10.2125px**
// past the underside (180) of the slab (11285,187) and burrowed in, whereas GD does
// not land it and it dies on the wall with an x penetration of 10.9
// (gd_death_context names it). So the top face of the same slab was re-swept finely
// with worker-98 injections (x centred / entering sideways from non-overlapping x,
// vy -2 / 0):
//   d=10.0 (centred)       lands
//   d~10.05 (sideways)     refused -> floats 7 ticks, then side death at xpen~11
//                          (same as the ball)
//   d=10.2125 (real run)   refused (lv9's ball itself)
//   d=10.5 (centred)       dies on the injection tick (the crush side fires first)
//   d=6.047 (real run)     lands (lv19 t=9,451 -- the reason for raising to 10.5;
//                          holds at 10.0 too)
// The boundary is a single one at [10.0, 10.05]; no sideways/centred distinction is
// needed. Yesterday's 10.5 was the value of lv6's 0.5px-step sweep, whose own note
// warned of contamination with "contact had already begun at injection time"
// ([[gd-injection-resolution]]). The side where the penetration keeps deepening
// while refused is killed by cube/solid-side at the same xpen ~11 as GD (lv9's
// replay is confirmed to match on all 8,672 ticks).
inline double kLandTol = 10.0;
// The "gap between the extrapolated seat and the player's y" that ride continuation
// (stickHere) allows. **A constant in px** -- measured on the seam rig at 3 speeds
// (see the stickHere note). --stickgap A/Bs it.
// The value is the midpoint of the range [4.0307, 4.0371) that satisfies **130** of
// the 132 units (seam/seam07/seam11 x cube/ball x |m|{1,0.5} x 12 phases). The 2
// outliers are one-offs (seam unit8 wants >= 4.087, seam11 unit43 wants < 4.026),
// 1 tick at the edge of the phase -- the remaining systematic error is under 0.05px.
inline double kStickGap = 4.034;
// How far off a rotation trigger (id 2900) the player may be on the PERPENDICULAR axis and still
// fire it. Bracketed by GD's own gframe changes across two full lv22 runs -- fires at |dv| up to
// 116.7, skips at 274.4 and 657.8 -- so anything in that gap fits every measurement; 150 sits in
// it with margin either side. The reasoning, and why the old 30 was falsified, is at the gate in
// step.hpp's applyRotation. --rotperp A/Bs it.
inline double kRotPerpWin = 150.0;
// How deep a top face may sit (how far below the line) and still be grabbed as a
// "step" while riding. Midpoint of the measured boundary (3.100, 3.200] on the
// ridestep calibration rig. --stepdepth A/Bs it.
// [2026-08-21 diagnostic] A/B switch that cuts the ship ladder of hanging rides
// (r88). Default false = rule on. Exists only to measure where a cold run's cost
// is attributed.
inline bool g_noHangLadder = false;
// [2026-08-22 r106] Inner-box half-size for the crush (squeeze). Measured on the
// calib_crush calibration rig: "instant death when the inner box (centre +-4.5)
// touches the interior of a solid". The boundaries are
// cube (19.5,19.6] / robot (19.5,19.75] / ball (19.5,20.0] /
// mini cube (13.5,13.6] -- all consistent with "dies when ceiling underside -
// centre <= 4.5", and **still 4.5 for mini** (the same size-independent family as
// the wave's solid=1.5). Injecting into the interior of a corridor also dies on
// the injection tick = an interior intersection, not an entry condition.
// Independent corroboration: the kLandTol sweep's "d=10.5 (centred) dies on the
// injection tick (the crush side fires first)" = the centre is the same 4.5 from the
// slab's top face. The existing cube/solid-side killing at xpen ~11 (= 15 - 4.5 +
// alpha) is the same box seen from the side. --nocrush A/Bs it.
inline bool g_noCrush = false;
inline double kCrushHalf = 4.5;
inline double kStepDepth = 3.15;
// Ship: MEASURED, not guessed. State-injection sweep at lv4's ledge x=17250
// (top y=150), stepping the injected y by 0.1 and reading the outcome:
//   penetration <= 5.95  -> lands (GD pushes the ship up onto the ledge)
//   6.05 .. 9.15         -> passes straight through, no landing and no death
//   >= 10.15             -> dies
// so the landing limit is exactly 6.0, twice the cube's 3.0. Using the cube's
// value here made the model fall past ledges GD had landed on (lv4 t=13257,
// penetration 5.67 -- it missed by 0.02 px and cost the whole level).
// The 6..10 "passes through" band is GD having a smaller inner box for the
// kill test than the outer box it rides on; the model kills there instead,
// which is conservative (it never plans a route that GD would end).
constexpr double kShipLandTol = 6.0;
// How deep the horizontal overlap must be before GD resolves a solid it did not
// see the ship fly into, as a multiple of the player's half width. Bracketed by
// the two 0->1 transitions of the hooked collidedWithObject return on lv11:
// mini (half 9) 14.670 -> 15.970, normal (half 15) 25.930 -> 27.230, i.e.
// 1.630..1.774 and 1.729..1.815. 1.75 is the only clean value in both.
constexpr double kSolidResolveX = 1.75;
// The plain (unarmed) cube CEILING STOP is OFF. [2026-08-16]
// It was fitted to two samples: lv1's false stop (xPen 6.67, full size) and
// lv22 t=2,749 (xPen 19.78, mini) -- but t=2,749 is the 2866 FLIPHEAD event,
// which `acquireFlip` handles on its own. A third sample, measured 2026-08-16
// by sweeping GD directly (gd_run inject at lv22 t=11,385, mini), has GD NOT
// pinning at xPen 15.957 -- and not at the block's dead centre either
// (xPen 24.0, the deepest possible), so no threshold on xPen can separate it
// from t=2,749. GD's unarmed didHitHead does nothing: the player keeps moving
// into the block and dies of the overlap, which is exactly what it did.
// --ceilpin restores it for A/B. `held` reads ceilPin, which only this branch
// sets, so it is gone too.
inline bool g_ceilPin = false;
// Ship zones have an invisible ceiling. It was modelled as "ship portal cy +
// 120" from a single lv1 measurement (portal cy 255, ceiling 375.000) -- but
// that was a coincidence. lv4's ship portal sits at cy = 233, so the rule
// predicts 353, and GD's measured ceiling in that zone is again 375.00 -- the
// model clamped the ship 22 px low and it could never reach the route above.
// Two zones with different portal heights, same ceiling: it is ABSOLUTE.
// ...and 375 is not universal either. lv10 t=17836 has GD flying the ship at
// y=427 and still climbing, so a hard clamp at 375 blocked that route outright.
// maxY does not explain it (lv1 maxY=540 -> 375, lv2 maxY=390 -> 375,
// lv10 maxY=630 -> above 427), so there is no level-derived rule in hand.
// TRIED AND REVERTED: disabling the clamp entirely (relying on g_yBound alone).
// It let lv10 through to t=18,743 but **lv2 went from cleared to stuck** at
// t=10,569 and lv1 needed 5 iterations instead of 2 (2.7 min vs 0.3). The clamp
// is load-bearing where it is right, so it stays at the value that is right for
// lv1/lv2/lv4.
// What it actually is: a SEARCH BOUND, not physics. GD exceeds it (lv10 y=427)
// and no level-derived rule reproduces it -- neither maxY nor the ship zone's
// own highest surface (lv1 zone 450 -> 375, lv2 zone 390 -> 375). But lv1/lv2
// need it, because without it the DP spends its frontier on high routes that
// GD then refuses. So it is a knob with a default, not a constant: `--shipceil`
// raises it for levels whose ship really does fly higher.
inline double g_shipCeil = 375.0;
// ...and the flying modes have a hard FLOOR too, which is not the world's.
// Measured on lv12's UFO section through the gd MCP: injecting the UFO at y=60
// at t=5400 / 5500 / 5640 (x = 7010 .. 7321) has GD put it back at y=195.000
// with onGround=1 every time, and injecting at y=700 clamps it down to 465.
// So the band is [180, 480] for the player's surfaces -- exactly 300 tall --
// while there is NOT ONE object under it (checked: zero type-0 in y[140,220]
// across x[7000,7160]). A cube injected the same way at t=5300 just dies, so
// the band belongs to the flying modes, not to the level's geometry.
// The model had the ground pinned at 90 and simply fell through, which is why
// the DP planned a route GD never lets it take.
// Same status as g_shipCeil: measured per level, not derived. 0 = no floor.
// The band is per SECTION, not per level: forcing lv12's UFO band (180/465)
// onto its ship sections killed the search inside the first one immediately.
// So these two apply to the UFO portal (type 19) only, and a cube or ball
// portal clears them again.
// ...and for the UFO the band is DERIVED from the portal, not a knob at all.
// lv12 has two UFO sections at wildly different heights and one number cannot
// serve both, so the portal's own cy was checked against the measurements:
//   portal (6945, 345)   -> floor 180 measured, 345 - 165 = 180   ceiling 480
//                           measured, 345 + 135 = 480
//   portal (16665, 1095) -> floor 930 measured, 1095 - 165 = 930
// Two sections, three bounds, exact on all of them. The same arithmetic also
// reproduces the ship bound that was tuned by hand long ago: lv1's ship portal
// sits at cy = 255, and 255 + 135 - 15 = 375 -- exactly g_shipCeil's default.
// The knobs below stay as overrides (0 = derive).
constexpr double kFlyBandBelow = 165.0;
constexpr double kFlyBandAbove = 135.0;
inline double g_flyFloor = 0.0;
inline double g_ufoCeil = 0.0;   // 0 = fall back to g_shipCeil

}  // namespace dp
