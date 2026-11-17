#pragma once
#include "dp/object.hpp"
#include <atomic>

namespace dp {

// ---- MOVING GEOMETRY ------------------------------------------------------
//
// Everything above is a STATIC grid: the objrects dump is one snapshot taken at
// level entry, and the whole model treats it as the truth for all 30,000 ticks.
// That holds for lv1-18, which contain exactly ZERO moving objects
// (scripts/mechanic_census.ps1, "movable" column), and it collapses at lv19,
// where the same column reads 820 / 1,015 / 1,731 / 761 for lv19-22.
//
// The failure is not subtle. lv19's cold DP dies at x=1,459 because the static
// grid has nothing to stand on there -- while GD, replaying a working plan, has
// the player GROUNDED at (1,352, 131.6). The thing it is standing on is
// `id 470 type 0 groups=1`, which the dump lists at (1,365, **45**), i.e. below
// the floor: it rises into place before the player arrives. lv22 is the same
// shape at x=525 and it is what stops that level at x=604.
//
// Rather than implement GD's trigger system (move / rotate / toggle / spawn,
// with easing), the MOD records what actually happened: `grouptrace=1` writes
// `tick,uid,cx,cy,w,h` for every grouped object, every tick it changes. That
// makes rotation and scaling fall out for free -- they change the rect, and the
// rect is all the model ever asks for.
//
// Between two samples an object holds its last rect, and past the final sample
// it holds it forever. That is exactly right for a platform that has finished
// moving, which is the common case: the trigger fires when the object comes on
// SCREEN, so a platform is normally already in place by the time the player
// reaches it.
// `on` is GD's own toggle state (GameObject::m_isGroupDisabled). A toggle does
// NOT move the object, so a recorder that watched positions alone would report
// a wall that never changes -- which is exactly what lv19 x=27,705 looked like:
// two 30x30 blocks filling the only 60 px gap in a wall the player flies
// straight through.
// ---- ROTATED GAMEPLAY (id 2900) --------------------------------------------
//
// GD 2.2 turns the whole gameplay frame: the world does not move, the player's
// travel direction and gravity do. lv22 has 20 of these (no other level in the
// suite has any, so all of this is inert there).
//
// Measured on lv22 with the reference replay (2026-08-13):
//   - `rot` is the ABSOLUTE screen angle, not a delta. Eight of lv22's twenty
//     are rot=0, i.e. "turn it back".
//   - travel = R(-rot) * xhat:  0 -> +X,  90 -> -Y,  180 -> -X,  270 -> +Y.
//     uid 1343 (2265,316, rot=90): from t=1,747 x freezes at 2,266.5 and y
//     falls 1.2982/tick (= the section's dx). uid 6286 (16125,489, rot=-90):
//     x freezes at 16,131 and y RISES 1.298/tick.
//   - it fires on the tick AFTER the player's forward coordinate crosses the
//     object's -- the same crossing+1 rule every autonomous trigger uses. The
//     boxes overlap for ~15 ticks before that and nothing happens.
//   - the world VELOCITY carries over: the forward speed becomes the new
//     perpendicular one, and the old perpendicular is dropped (the forward
//     speed is the section's, not a free variable).
//
// The model keeps its state in the CURRENT frame's coordinates, so every
// physics rule stays written as "x is the clock, y is height"; what changes is
// the geometry handed to it. (u,v) = R(-rot)*(X,Y), which for 90-degree
// multiples is a pure index swap with signs -- an AABB stays an AABB with
// hw/hh swapped.
struct RotTrig {
    double cx, cy;     // WORLD position of the trigger
    int frame;         // rot/90 mod 4
    // ONE SHOT, like every other trigger GD activates from the level: the tick
    // it fired, -1 while it is still live. Measured on lv22 (2026-08-13): the
    // player leaves the first rotated section at world x=2,143.5 heading +X and
    // re-crosses uid 1343 (2265,316) at t=1,911 -- injected back up to y=315.9,
    // i.e. INSIDE the trigger's own box -- and GD does nothing at all.
    // Without this the model turns again, exits at 2,143.5, walks back to
    // 2,265, turns again: a closed loop the frontier can never leave (x pinned
    // at <=2,266 for 1,000 ticks). It only looked like progress before because
    // the broken frame floor dropped the player 700 px out of the section.
    int firedT = -1;
    // A separate one-shot for the reverse run (a 2900 pointing at the same
    // frame). **Must NOT be shared with firedT**: if shared, a trigger consumed
    // at the entrance as a reverse-run toggle can no longer be used later as its
    // real frame change. Measured 2026-08-15: lv22's cold run could no longer
    // get out of x=2,343 (the old exe was at 2,441 by iter 11).
    int revT = -1;
    // The gravity direction this trigger sets (from mvdir), in the model's local
    // convention. -1 = an old export without the column, in which case flip is
    // carried over as before.
    int setFlip = -1;
    // The reverse run this trigger sets (from gnddir). -1 = an old export
    // without the column; only then does it fall back to the old "toggle if it
    // is the same frame" rule.
    int setRev = -1;
    // objrects' uid. Used only to match --spentrot (the driver naming the 2900s
    // that had already fired before the anchor).
    int uid = -1;
    // Effective multiplier on vy [2026-08-19]: m_velocityModY(583, member
    // default 0.0) if m_editVelocity(169) is set, 1.0 (pass-through) if not.
    // Folded at parse time. An old export (no column) falls back to 1.0 -- lv22
    // is the only level with a 2900, and its objrects was re-dumped the same day.
    float vmodY = 1.0f;
    // m_overrideVelocity(584). When set, vmodY is an absolute assignment, not a
    // multiplier. lv22 has no instance (carried only).
    uint8_t ovrVel = 0;
    // m_dontSlide(585): rotateGameplay lowers the velocity-limit exemption its axis swap raised
    // (--rotboost). Read by name from the objrects `dontslide` column; 0 when the dump has none.
    uint8_t dontSlide = 0;
    // The object's RAW rotation and flipX, kept because the queue's sort order
    // is a function of them and of nothing else. `frame` above cannot serve:
    // it is overridden from gnddir, and GD's own sort reads the rotation.
    // `determineSlopeDirection` truncates the rotation to an int before its
    // exact comparisons against 0 / +-180 / 90 / 270, so a non-cardinal
    // rotation matches no branch and the direction stays x-ascending.
    double rawRot = 0.0;
    uint8_t flipX = 0;
    // The raw gnddir, for the reverse predicate. The frame table above folds
    // this into (frame, setRev) through a FITTED mapping; the queue needs the
    // value itself, because GD's reverse flag is the pure predicate
    // `gnddir - 2 <u 2` (i.e. gnddir is 2 or 3) and no mapping is involved.
    int gndDir = 0;
    // A touch-activated trigger (objrects `touch`) and its half-box, for --rottouch.
    uint8_t touch = 0;
    double hw = 15.0, hh = 15.0;
};
inline std::vector<RotTrig> g_rotTrig;
// --rottouch (on by default since 2026-10-14): a touch-activated 2900 also fires when the player's box overlaps its own,
// not only when the travel coordinate crosses it. The game activates such a trigger in the collision
// pass's object loop, so a teleport that lands the player on it turns it on that very tick. SubZero
// 4003 t=12,468: teleport uid 4687 lands a reversed cube on touch 2900 uid 4928 (9363, 1561), which
// sets forward travel; GD moves right from the next tick, the model only once the cube had walked
// back across the trigger's x four ticks later.
inline bool g_rotTouch = true;
// --rotqreplay: the plan replay (cli.hpp) asks the rotation queue every tick, as the search does.
// It used to call applyRotation only on a tick its own crossing scan found a trigger within
// kRotPerpWin, so a queue element the player reaches from far away on the perpendicular axis
// never fired there. lv22 t=1817: the spider warps from world x 2143.5 to 2926.5 in frame 1 and
// crosses world y 225, where GD fires uid 1215 (2143.5, 225) back to frame 0; the replay stayed
// in frame 1 and parted from GD the next tick.
inline bool g_rotQReplay = true;
// --rotqtouch: a touch-activated 2900 in the queue does not fire on the crossing; it holds its
// channel until the player touches it, and once touched the walk passes it without firing it again.
// GJBaseGameLayer::checkSpawnObjects (0x21a8f0): an element with m_isTouchTriggered (+0x5d0) set
// stops the walk unless its activated-by-player flag (+0x5b3) is set, and then is skipped with the
// cursor moved on. The model fired it when the player's x passed it: a custom level's reversed cube
// at x 2,324 under touch 2900 uid 314 (2,325, 105), 180 px below the player, turned forward where GD
// kept reversing. The cursor is the channel's first element not yet spent.
inline bool g_rotQTouch = true;
// --waveramprot: a wave riding a ramp (a dart slide) turns toward the ramp's angle by the cube's
// --sloperot law -- two steps of the half-angle slerp, w = 0.04375 x the speed, toward
// convertToClosestRotation(-atan(m)) -- instead of easing toward its travel angle. updateRotation
// (0x391020) takes its on-ramp branch (+0x9b0) before the mode test, so the wave never reaches
// updateShipRotation there, and collidedWithSlopeInternal has already run updateSlopeRotation for
// the contact; updateRotation's weight is m_playerSpeed * 0.175 * dt, halved for the ship only.
// SubZero 4003 t=10,960..10,993 (Worker 99, pobb): on ramp uid 8922 (m 1) the sprite closes on
// -45 by 0.1105 of the gap a tick, and from ramp uid 8892 (m 2) on -63.43, the same ratio:
// 1 - (1 - 0.056875)^2 at speed 1.3. The model eased at 0.0625 toward +45 and its rotated box
// met the tilted block uid 8882 two ticks early.
inline bool g_waveRampRot = true;
// --portslopepre: a turned portal whose uid is below the ramp the body rides this tick is judged at
// the y the update left (yFree), not at the y the ramp then pushed the body to. Portals and ramps
// share collisionCheckObjects' one ascending-uid loop (--portalslopeorder's note at the end of
// stepOne), so the game reaches such a portal before the ramp moves the body. SubZero 4003
// t=10,993: an unheld wave climbs ramp uid 9044 (m 2) into teleport uid 8882 (747, 50 x 180 at
// -45); at the free y 431.754 the boxes stay 0.58 px apart (SAT with GD's own pobb corners),
// at the pushed y 437.604 they overlap by 3.55, and GD teleports a tick later, from 437.604.
// The turned portals only: the axis-aligned test already reads yFree.
inline bool g_portSlopePre = true;
// --growramporder: a mode portal that grows a body riding a ramp does not lift it on the portal's
// tick when the ramp's uid is below the portal's: the loop seated the old body on that ramp before it
// reached the portal, and the new body is seated on the next tick (the ramp branch's uid gate after
// the lift says the same for a body in the air). SubZero 4002 t=11,695: a wave riding ramp uid 8633
// (m 2) takes ball portal uid 8646; GD keeps y 813.165 on the portal tick and seats the ball at
// 839.424 the next, where the model lifted it to 818.165 (--wavegrowclamp's +5).
inline bool g_growRampOrder = true;
// --spdtapwarp: on the tick a spider's tap warps it, the speed portals are judged at the y the
// warp left (g_preBtnY). The tap is the button pass's, after collisionCheckObjects, so a speed
// portal at the landing is met on the next tick -- the rule the portal pass already keeps for
// mode, gravity and teleport portals (the spiderWarpedThisTick break). SubZero 4002 coin
// t=23,770: a flipped spider taps from the ceiling 1,753.5 down to 1,543.5, inside 1.1x portal
// uid 17380; GD's next step is still 1.298 and the speed changes on 23,771, where the model's
// next step was already 1.614 and it ran 0.316 px ahead of GD from there.
inline bool g_spdTapWarp = true;
// --ballfreshend: an upright ball whose first contact with a ceiling ramp's underside comes just
// past the ramp's low end is pushed to the corner flat (line(x1) - half), as the flight modes and
// the spider are (step.hpp freshEndU); --ballunderv3 had added the ball to the continuing window
// only. SubZero 4002 coin t=20,987: a ball rising at vy -0.87 meets uid 15998 (sdir 5, m -2) with
// its contact point 0.23 px past x1; GD puts it at 1,635.000 = 1,650 - 15 and releases it the
// next tick at -4.734, where the model flew on and was 2.36 px above GD from there.
inline bool g_ballFreshEnd = true;
// --tapwarpnocrush: the inner-box (crush) test is not run at the landing on the tick a spider's tap
// warps it. The tap is the button pass's, after collisionCheckObjects, so that tick's inner-box test
// saw the body where the warp found it, and the landing is first tested on the next tick, one step
// further on. SubZero 4002 coin t=23,015: a reversed spider taps up to 1,696.5 with the 3 x 30 wall
// uid 18167 (31,560..31,563) 0.17 px inside its inner box's right edge; GD lives (the next tick's
// inner box ends at 31,558.9), the model killed it on `crush`.
inline bool g_tapWarpNoCrush = true;
// The test is the player's centre inside the trigger's box, not the two boxes overlapping. The
// overlap fired a custom level's touch 2900 (15,645, 1,755) at x 15,625 with the player 23 px above
// its centre, where GD never fires along that pass (dy 23..34); the 4003 landing fires at (4.9, 8.0).
// So the bracket for the perpendicular reach is (8, 23), and the trigger's own half (15) sits in it.
inline bool rotTouchHit(const RotTrig& r, double wx, double wy) {
    return r.touch && std::fabs(wx - r.cx) <= r.hw && std::fabs(wy - r.cy) <= r.hh;
}
// --rotwatch <lo>,<hi>: print, per search tick in [lo, hi], the rotations the
// search's stepKid actually applied (uid, frame before/after, children). Print
// only: applyRotation writes the uid it adopted into the thread-local below
// when the window is set, and nothing reads it for a decision. -1 = off.
inline long long g_rotWatchLo = -1, g_rotWatchHi = -1;
inline thread_local int g_rotWatchUid = -1;
// --qfoldwatch <lo>,<hi>: print, per search tick in [lo, hi], how wide the
// frontier is and how many rotation-queue states (rotSpent, rotChan, rotRev)
// the dedupe merged away. keyOf does not include the queue, so two children
// that differ only in it land in one cell. Print only. -1 = off.
inline long long g_qfoldLo = -1, g_qfoldHi = -1;
// --rotqtoggle: a queue entry whose object sits in a group a touched Toggle
// (1049, togon=0) has switched off is consumed without firing -- the cursor
// advances, no channel switch, no rotation. GD does exactly this: toggleGroup
// (0x223bc0) sets the object's +0x28e when its toggle counter goes negative,
// and checkSpawnObjects (0x21aad8) then skips triggerObject while still moving
// the cursor on (0x21ab62). Per state, because the touch is: the off/on
// masks below are over State::trig. Off by default.
inline bool g_rotQToggle = false;
// ...its masks, built once the queue and the touch boxes are both loaded
// (cli.hpp): for queue entry k (and pre-queue trigger k), the touch-box bits
// whose Toggle switches that object's group off (togOn 0) or on (togOn 1). An
// entry counts as disabled for a state when it has entered more "off" boxes
// than "on" ones -- GD's per-object toggle counter below zero.
inline std::vector<uint32_t> g_rotQOff, g_rotQOn, g_rotTrigOff, g_rotTrigOn;
// --touchseed uid:tick,...: touch boxes the anchored attempt had already
// entered by t0, from the mod's geometric test on its own recorded positions.
// OR-ed into the anchor state's trig/fireB without claiming ownership, so the
// recording-derived seeding still runs. Exists because GD's touch recorder
// (activatedByPlayer) never sees a touch Toggle: 0 of lv22's three.
inline std::string g_touchSeedArg;
// --touchcensus: print every touch-box entry markTouched makes (step.hpp),
// labelled by which test admitted it -- the player's y this tick, or only the
// `preY` its callers pass (the parent state's y, which the comment there calls
// "before this tick's button effects"). For --replay: a search would print once
// per child, from the worker threads. Print only. Off by default.
inline bool g_touchCensus = false;
// ...and which of stepOne's y-moving branches this tick took, so the census can
// name an entry by the model's own path instead of a threshold on dy: 1 = the
// spider's tap warp, 2 = a spider orb, 4 = a teleport portal. Written only under
// --touchcensus; thread_local because phase 1 steps the layer in parallel.
// g_tcBranchP1 is p1's value, saved by stepBoth before p2's stepOne overwrites it.
inline thread_local int g_tcBranch = 0;
inline thread_local int g_tcBranchP1 = 0;
// --touchprey=button: the y markTouched's preY test reads is p1's y before this
// tick's button effects, carried out of stepOne -- the spider tap warp's
// pre-warp y -- or this tick's own y when no button effect moved it
// (g_preBtnSet/g_preBtnY, written by the warp branch). The old reading, the
// parent state's y (--touchprey=parent), is a tick too early: the cross of
// "this tick's x" with "last tick's y" fired lv20's uid5625 on a position the
// player never occupied, moving a saw 113.7 px onto a lane GD leaves open.
// On since 2026-09-21; always on since the flag
// clean-up, which removed --touchprey.
// --rotpretap: the same tick order for a 2900. GD fires a rotation
// in the collision pass, BEFORE the tick's button, so the trigger's
// perpendicular-window test (applyRotation, step.hpp) sees the spider where it
// stood before its tap warp. The model ran the tap first and tested the warped
// position. Measured on lv22 (2026-09-21), a plan both sides replay alike up to
// t=1,816: GD fires uid1215 (2143.5,225) with the player at (2143.5, 224.3) and
// then warps it in frame 0 to y=316.5; the model had already warped it in frame
// 1 to world x=2,926.5, read |dv| = 783 > kRotPerpWin, dropped the rotation and
// planned on in a frame GD had left. applyRotation already restores the pre-tap
// y once a rotation has fired (its re-tap); this makes the DECISION use it too.
// ON by default since 2026-09-21, as ONE unit with
// --spiderstrip: this alone still left a wrong landing, the pair made the
// target plan match the game for 400 ticks. Both always on since the flag
// clean-up.
// --spiderstrip: the spider's teleport search in frame 0 uses
// GD's own rects instead of a window centred on the player. Read off
// spiderTestJumpInternal's two queries (cfg hitboxtrace=1, `srect:`/`drect:`)
// on lv22, both frame-0 taps of one replay (t=1,816 on the rotation tick and
// t=1,854), with x the player's position on the tap's tick:
//   srect o.x = x        s.x = 14.5   -> solids  [x, x + 14.5]
//   drect o.x = x - 4    s.x = 8.0    -> hazards [x - 4, x + 4]
// The centred window was |x - cx| <= hw + 14.5 for solids (lv21's seven
// injections pin 29.5 -- on the LEADING side, where the two agree) and
// hw + 13.5 for hazards. Behind the player it reached 14.5 px too far: at
// t=1,816 it took uid1212 (x 2,100..2,130, the player at 2,143.5) and landed
// the spider at 226.5 where GD, which never looks behind, lands at 316.5.
// Forward travel at normal size only: reverse travel and the mini are not
// measured, and keep the old window.
// ON by default since 2026-09-21, paired with
// --rotpretap (see there).
// --ceilpush (default off): seat a body that overlaps a ceiling ramp's rect at
// GD's ceiling seat even where the ramp window has dropped the ramp. See the
// branch beside the push-out gate in step.hpp.
inline bool g_ceilPush = false;
// ...its two per-step channels, thread_local because phase 1 steps the layer in parallel.
// g_bandPortalWrote: set by the portal band write in stepOne (step.hpp), so stepBoth can tell a
// band the second half WROTE from the one it only carried -- the band's tween (bandAnim) moves
// every tick, so comparing fields cannot. g_dualOtherMode: the first body's FINISHED mode, handed
// to the second half's height (GD runs p1's collisions first); -1 = read s.mode2 as before.
inline thread_local bool g_bandPortalWrote = false;
inline thread_local int g_dualOtherMode = -1;
// --p2solo's two per-step channels, thread_local for the same reason.
// g_p2SoloHalf: stepBoth is running the SECOND body's step. g_p2SoloFired: that step's portal pass
// took a solo portal, so stepBoth hands player 1 the second body's attributes.
inline thread_local bool g_p2SoloHalf = false;
inline thread_local bool g_p2SoloFired = false;
// --dualslide (on by default since 2026-10-07; --no-dualslide is the off arm): each dual body keeps its own DART SLIDE arm (State::slideT2 beside
// slideT; modifiers.hpp slideBoxTouch). GD's is per player: collisionCheckObjects sets THAT body's
// +0xb78 to 2 when it touches an id-1755 box (0x215ab7), PlayerObject::update steps it down, and
// collidedWithObjectInternal gives a wave the solid push-out only while it is >= 1. The model had one
// arm for the pair, so the second body's step read the first's. Measured on custom level C
// t=10,677-10,680 (hitboxtrace): p1, inside the lower lane's 1755 boxes, is pushed out of block
// 56076's top at 0.01 px; p2, in the upper lane with no 1755, sinks 0.25-3.48 px into block 56066's
// top with collidedWithObject returning 0 and dies at 5.1 px -- where the model, on p1's arm, seated
// p2 at 575 (step.hpp swapHalves, fixup.hpp stepBoth, the dual entry).
// Acceptance for the default: kitref on 69 kits (the 13 levels with a dual portal and 1755 boxes, plus
// one more dual-ball level), 1,028 episodes: no change; lv16/21/22 one-session [fp] identical; no positive replay of the
// scene above (its level is not in the lab).
inline bool g_dualSlide = true;
// --dualdash: each dual body keeps its own dash (State::dashing2 / dashSlope2): GD's m_isDashing is
// per player, and with one shared field the second body ran the first body's dash (step.hpp
// swapHalves, fixup.hpp stepBoth, the dual entry, search_key.hpp).
// On by default since 2026-10 (--no-dualdash turns it off).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --hangseatrepeat (always on since 2026-10): a flipped cube's or robot's hang ends on the tick
// its seat repeats, as a floor ride does under --rideseatrepeat (step.hpp, the hang branch of
// sampleAt).
// --sloperot (always on since 2026-10): a cube seated on a ramp turns toward the ramp's own
// angle, and the ramp's launch spins it the other way at half the take-off size (step.hpp, the
// cube's spin).
// --portalpressorder (always on since 2026-10): a mode portal met while a ramp presses the body
// keeps the press when the ramp comes first in uid order (step.hpp, the free step after a mode
// portal).
// --ceilreleaseball (always on since 2026-10): a ball leaving a ceiling ramp's press gets the
// release's launch as the flying modes do (step.hpp, ceil/release).
// --undercrush (always on since 2026-10): a cube, robot or spider that takes a ramp's underside
// outside the 0.1 s grace is not seated -- it dies if it is more than 2 px past the seat, and is
// left where it is otherwise (step.hpp, the upright ceiling push and the --slopelaw seat).
// --crushhithead: ...except while the id-1859 arm is up (State::armT < kArmTicks). The underside path
// in collidedWithSlopeInternal is entered only with m_stateHitHead (+0xb7c) < 1, the counter an id-1859
// touch sets to 2 (0x215ac6); with it up the body goes on to the seat. lv22 coin t=8790: an upright
// spider inside 1859 box uid 17791 (hitHead pinned at 1 from t=8753) rises into ceiling ramp uid 17789
// 1.85 px past the seat; GD puts it on the seat (441.866, vy 0) and slides it down the underside, where
// the model left it and killed it 2 px past the next tick's seat.
inline bool g_crushHitHead = true;
// --ufoslopekill (always on since 2026-10): a UFO dies on the ship's measured outline around a
// spiked ramp (the sloped side at ph, and with --shipslopecap its caps and flat side), instead of
// by the box overlap the unmeasured modes keep (step.hpp, the spiked-ramp kill).
// --spikebottom (always on since 2026-10): a spiked ramp's flat-edge
// strip and wall are the same 1-px solid a plain ramp's are (preSlopeCollision does not read
// m_slopeIsHazard); the ramp still never seats the body (step.hpp, the preslopegate block and the
// support scan).
// --balltapexit (always on since 2026-10): a ball's fresh press on the tick it leaves a ramp
// with the uphill launch does not tap -- the launch has taken the body off the ground before the
// buttons run, so it keeps its gravity and the launch's velocity (step.hpp, the top of stepOne).
// --extseatrepeat (always on since 2026-10): an upright cube riding a downhill ramp's line past
// its low end keeps the ride while the seat moves and leaves on the tick the clamped seat repeats,
// the --rideseatrepeat test, instead of on the first tick past the extC window (step.hpp, the extC
// branch of sampleAt).
// --tponce (on by default since 2026-10): a teleport portal fires once per contact -- not again
// while the body is still inside the box it was inside at the previous tick (GD's activatedByPlayer
// mark; step.hpp, the portal pass).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --tplatch (on by default since 2026-10): ...and once per ATTEMPT, as GD's mark is: a teleport
// portal takes a bit of State::portalLatch beside the gravity portals (level_loader.hpp numbers
// them, static and dyn) and does not fire again once set, however the body comes back into its box
// (step.hpp, the portal pass).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --teleoldpos: on a teleport tick, a portal with a smaller uid than the teleport is judged (and
// fires) at the position the teleport found the player, as GD's ascending-uid pass reaches it first,
// instead of being skipped (step.hpp, the portal pass).
// On by default since 2026-10 (--no-teleoldpos turns it off).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --stripvetoorder (OFF by default): a ramp's 1-px top / bottom strip (--preslopesolid) is vetoed
// only by the ramps GD's slope map holds at that point -- the one last ridden and those with a
// smaller uid -- not by every ramp in the level (step.hpp, thinSolid). Off because its model-only check A/B is
// negative: on its own it broke ten custom level A episodes (a ramp that takes the body must veto its own
// strip, --stripownacq), and with --stripownacq it still breaks two of custom level F (t=6,513, a falling
// ball GD puts on a ceiling ramp's top strip) that nothing told apart from the witness; with it off
// the rest of the branch fixes the same 22 and breaks none, the witness (custom level B, iteration 33) included.
inline bool g_stripVetoOrder = false;
// --ballungroundramp (on by default; --no-ballungroundramp is the off arm): a ship / UFO that a ramp
// with a smaller uid than the ball portal seated is not on the ground on the tick it becomes a ball
// (step.hpp, the mode change in the portal pass).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --contseatclamp (on by default; --no-contseatclamp is the off arm): a ship's CONTINUING ride along a
// ceiling ramp sits one tick on the corner where it passes onto the next same-sign ramp, as the old
// ramp's clamped seat puts it in GD (step.hpp, the flipped ceiling pin).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --bandfloorunstick (on by default; --no-bandfloorunstick is the off arm): a grounded ship the flying
// band's floor puts back on it is off the ramp it was riding -- the slope stick does not pull it
// back down the line on the same tick (step.hpp, fly/bandcarry and stickHere).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --ceilgracerider (on by default; --no-ceilgracerider is the off arm): the flipped ceiling push's
// exit-side grace (|dx| past the ramp's end) is for a body that rode the ramp last tick only
// (step.hpp, the flipped ceiling pin).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --lawuplaunch (on by default; --no-lawuplaunch is the off arm): a --slopelaw seat dropped as a
// repeat on a ramp that rises in the travel launches the body as a ride's uphill exit does
// (step.hpp, the --lawcontact release).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --dualseatt (on by default; --no-dualseatt is the off arm): each body of a dual keeps its own law-
// seat count (State::seatT2, swapHalves, the merge in fixup.hpp); shared, the first body's step wrote
// it over the second's.
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --ceilfreshrect (on by default; --no-ceilfreshrect is the off arm): the flipped ceiling push takes
// a fresh contact only through GD's inset-rect test, as the ramp window does (step.hpp, the flipped
// ceiling pin).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --ceilpushreach (on by default; --no-ceilpushreach is the off arm): a fresh flipped ceiling push
// reaches a ramp by box overlap, as GD's acquisition does, not by the rotated contact point
// (step.hpp, the flipped ceiling pin).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --stripownacq (on by default; --no-stripownacq is the off arm): under --stripvetoorder a ramp's
// strip is also vetoed by its own ramp when that ramp takes the body on this tick (step.hpp,
// thinSolid's veto).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --escufo (on by default since 2026-10): escapee-prune leaves the UFO alone, as it does the ship
// and the wave (step.hpp, where the measurement is).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --spentdyn (on by default since 2026-10): --spentorb also names rings in dyn (cli.hpp, the
// seeding).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --radreach (on by default since 2026-10): the cube branch's hazard x window reaches a circle's
// radius when it is wider than the object's rect (step.hpp, the hazard walk).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --uforot (on by default since 2026-10): the UFO's tilt follows GD's updateShipRotation UFO branch
// (step.hpp, the rotation update, where the constants and the measurement are).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --spdonce (on by default since 2026-10): a speed portal does not fire again while the body stays
// in its box (step.hpp, the speed portal pass).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --dualorbflip (on by default since 2026-10): a ring that flips a dual body's gravity couples to
// the partner the way a gravity portal does (step.hpp, after the ring pass; fixup.hpp stepBoth).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --ufolawflap (always on since 2026-10): a UFO the --slopelaw seat held on a ramp rising in
// its travel the tick before flaps at GD's ramp flap, min(flap + v/2, 1.4 x flap), v the ramp's
// exit velocity (step.hpp, the seat and the UFO flap; State::lawSv; UfoModel::stepVy).
// --spiderminigd (on by default since 2026-10): a MINI spider's teleport search in frame 0 is GD's
// own too (spiderTargetGd), with the strip hung off the centre at the mini's half + 1 and the
// hazard strip at +-4 x 0.6 (step.hpp, spiderTargetY).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --spawnringjump (on by default since 2026-10, with --orbspawn): the spawn ring's own jump, given
// on the move of the tick after the press (step.hpp, the end of stepOne).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --ballstake (on by default since 2026-10): the ball's spin rate is also written where GD writes
// it -- 0 on entering the ball (toggleRollMode), the ground rate off a pad (propellPlayer), on a
// speed portal (updateTimeMod) and on a size portal over a ground stake (togglePlayerScale), the
// air rate on a gravity portal (flipGravity), airborne or not -- and a ring's stake turns from the
// next tick (step.hpp, the ball branch and the portal pass).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --boostcarry (on by default since 2026-10): the velocity-limit exemption (State::boost) is kept
// through every mode and set by its writers in every mode, as GD's byte is, and a pad other than
// the red one clears it (step.hpp, where the per-tick drop was).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --dualflyring (on by default since 2026-10): a dual ship or UFO takes its fly-ring on the press's
// tick through the early path, as a single one does (step.hpp, the head of stepOne).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --dyngravseen (on by default since 2026-10): a gravity portal without a latch bit (a grouped one)
// does not fire again while the body stays in its box, through the mode portals' portSeen slots
// (step.hpp, the portal pass).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --dropfall (on by default since 2026-10): a drop ring skips the next tick's terminal clamp when
// GD's playerIsFallingBugged says "not falling" on the firing tick, flipped arm and all (step.hpp,
// the late ring loop's drop ring).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --repelland (on by default since 2026-10): the dual balls' repel is also decided after BOTH
// bodies' collisions and before either tap, as GD's checkRepellPlayer is -- which a body that lands
// on the tick its partner closes in needs (fixup.hpp stepBoth, after the second half).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --repela0c (on by default since 2026-10): the dual balls' repel decided once per tick in stepBoth
// with GD's own choice of body -- p1's +0xa0c up flips p2, else p2's up flips p1 -- from
// State::hitG, in place of the rule inside each half (and of --repelland's one case).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --spentaction (on by default since 2026-10): an --start anchor whose hist payload says the press
// is held and spent seeds action = 1 for a UFO or swing, so the first tick does not read a consumed
// press as a fresh edge (cli.hpp, the hist seeding).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --rot2 (on by default since 2026-10): each dual body keeps its own sprite angle and spin
// (State::rot2 / rotStep2 / rotNeg2), so the second body's tests against turned objects read its
// own angle (step.hpp swapHalves, fixup.hpp stepBoth).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --cubeentryspin: a cube entered in the air from the robot or the spider does not turn until a
// spin is staked (updateJump's fall stake or the usual ones); see the cube's spin block in step.hpp.
// On by default since 2026-10 (--no-cubeentryspin turns it off).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --ringrotsign: a rotated ring's firing box is tested at the ring's own angle (-rot in the math
// sense), not its mirror; see the late ring loop in step.hpp.
// On by default since 2026-10 (--no-ringrotsign turns it off).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --a1clatch (on by default since 2026-10): GD's +0xa1c per body (State::a1cLatch) -- raised by the
// jump, a ring, a pad, a ramp's launch, the head-hit flip and a gameplay rotation, lowered by
// updateJump the first tick playerIsFallingBugged holds -- and a ramp's exit does not launch a
// cube, robot or spider, nor release any mode, while it is up (step.hpp, the ramp exit and the
// ceiling release).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --lawcontact (always on since 2026-10): the tick after the slope law seats an upright body on
// a floor ramp is GD's continuing contact (m_wasOnSlope): no solid's face the ramp vetoes holds the
// body, the law re-seats it within the 4 px band and drops a target equal to the last one, and a
// dropped downhill contact releases at the face's velocity (step.hpp, the law seat and the ramp
// exit).
// --padentry (on by default since 2026-10): with all four pad slots taken, a pad the body did not
// overlap at the previous tick's position fires instead of being skipped (step.hpp, the pad loop).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --forceboxdir (on by default since 2026-10): a force box (2069) pushes along its own direction --
// rotation and flipY, from calculateForceToTarget -- instead of always up (level_loader.hpp).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --ceilwallnopush (always on since 2026-10): an upright ship on a rising ceiling ramp's wall
// side (dirs {2,3,4,6} with the centre left of the box; the others travelling left, right of it) is
// not pushed by that ramp -- preSlopeCollision returns the wall first -- so neither the V-valley
// corner (ceilLimSeat's min with the neighbour) nor cornerReg applies there (step.hpp).
// --heldtolhead (always on since 2026-10): the spiked-ramp kill's head-side band. A flight mode
// holding the button, against a ramp bVar22 leaves clear and while not on a slope, dies 1 px (2
// when it was on a slope last tick) short of the line and cap (step.hpp, the spiked-ramp kill;
// 0x38ff71..).
// --heldnodown (always on since 2026-10): the descending floor ramp's +1 of the spiked-ramp
// kill is the feet side's band, which a ship holding the button does not get (step.hpp;
// 0x38fe6b-0x38fe7d).
// --capinsetgate (always on since 2026-10): the spiked-ramp kill's caps sit at the box's edge
// plus ph; the pixel the rigs measured short of that is the fresh-contact 1-px inset, which a body
// that was on a slope last tick does not face (step.hpp; 0x38fc0e-0x38fc7b).
// --ballslopekill (always on since 2026-10): a ball dies on the ship's
// outline around a spiked ramp instead of by the box overlap (step.hpp, the spiked-ramp kill).
// --flipcldbg N: print the first N states where the --flipceilland rule acts (print
// only; it says where in a search the rule takes effect).
inline std::atomic<int> g_flipLandDbgLeft{0};
// slope law: the acquisition GD actually performs, in place of --ceilpush's
// one-sided push-out. See step.hpp's site for what is measured and what is not.
// On since v0.1.4; always on since the flag clean-up.
// ship slope kill: the ship's spiked/plain ramp kill on the measured outline
// (perpendicular d = playerHalf, flat side +kCubeInner) instead of the interval
// overlap. See the note at the ramp kill in step.hpp. On since v0.1.4; always on
// since the flag clean-up.
// --killslopeside: the slope KILL test decides which half of the
// box is solid with GD's own set, {1,3,5,6} (slopeIsCeiling, read off
// collidedWithObjectInternal at 0x3921B5), for every mode -- not just the ship.
// The kill test is the last place in the tree still on the {1,3} subset: the
// ride path dropped the mode whitelist in r63 (step.hpp, 2026-08-21) on the
// grounds that "the reason was the same every time and has nothing to do with
// the player's mode", and step.hpp's ceiling-ramp branch reads ceilRampSide.
// 447 of the corpus's 3,091 type-25 slopes sit in the difference set {5,6}, so
// this is NOT a one-object change and it moves kills in BOTH directions -- a
// slope GD calls ceiling is currently judged as floor, which empties one
// interval and fills the other.
// ON by default since 2026-09-21. The enumeration came
// back with one row changed -- lv21's t=14,999 over-kill, the same rule and the
// same direction as lv20's -- and nothing moved the other way over 22 whole-run
// replays. deathref 41/47 -> 42/47 with nothing lost, cold 22/22. Always on
// since the flag clean-up.
// flipped wave slope kill: a flipped wave's ceiling-type spiked ramp kill at the
// measured perpendicular distance kWaveFlipSlopeKillD instead of playerHalf. See
// the note at the ramp kill in step.hpp. On since v0.1.4; always on since the
// flag clean-up.
// --latgap: the search does not change the input on a tick no button edge can reach --
// the tick after a mode portal from a latency-1 mode (cube/ball/wave/...) into a
// latency-2 one (ship/UFO). The emitter maps such an edge to the press one tick earlier
// (cli.hpp, the plan writer's two-pass latency), which GD and --replay apply one tick
// LATE. Measured in cold runs: lv12 t=19,034 (ball -> ship at 19,033) and lv14 t=19,638
// (cube -> ship at 19,637), both SOLVED plans whose own witness lived while --replay of
// the emitted plan followed GD to its death. On by default since 2026-10 (was opt-in).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --waveslopeside: the wave's floor-ramp kill with the two things the plain rig
// measured on 2026-09-17 (calib_slopespike_wave and _wave_mini, bisected to 0.006 px,
// positions converted to the kill tick): a MINI wave's perpendicular distance is 3.0
// (its rect's half), not the 2.0 the model used, and on a DESCENDING floor ramp the
// boundary sits exactly 1.0 px higher than on an ascending one, for both sizes and both
// slopes (normal 5.000 up / +1.000 down, mini 3.000 up / +1.000 down, on m=1 and m=0.5).
// lv21 t=14,674 is the cold run's case: a mini wave 3.54 px (perpendicular) above
// descending plain ramp 1338, killed by GD and not by the model.
// On since 2026-09-20, always on since the flag clean-up. The high-end cap was
// re-confirmed on a third object -- id1339 rotated 90, m=2 -- by a coin route on lv21 that GD
// kills at t=16,725 and the model flew 95 ticks past; with the flag the model kills on the same
// uid at the same place. Gates: the replay suite moves 38 of 1,116 traces with the reference
// tracking unchanged, a cold run clears 22/22 for +8 on lv20 and +1 on lv17, deathref keeps
// 41/47 with no reference lost, and refaudit adds no over-kill (lv19 improves from 153
// differing rows to 3).
constexpr double kWaveSlopeDescendDy = 1.0;
// ...and past the high end the kill is flat at objMaxY + half - 1.0 (both directions).
// The downhill +1.0 is the source's tolerance xmm10 = (m_slopeUphill == 0), x4 while
// m_wasOnSlope, plus a velocity term for a moving ramp (0x38fab3-0x38fb94); only the
// static, not-sliding case is modelled here. (That 1.0 was kWaveSlopeCapInset; since
// --waveslopecap and --capinsetgate lost their switches the upright wave's cap is the
// capped outline's, whose inset is the fresh contact's 1 px -- insetK in step.hpp.)
// The overlap of the two flipped-wave brackets (m=0.5: 5.83-5.97, m=1: 5.69-5.86).
constexpr double kWaveFlipSlopeKillD = 5.845;
// (--nofreeside, which dropped --slopelaw's one empirical conjunct, is gone since the flag clean-up.)
// --escrotahead: do not escapee-prune a body in a turned frame while a rotation
// is still ahead on the active queue channel. See the prune in step.hpp. Always
// on; the switch is gone since the flag clean-up.
inline thread_local bool g_preBtnSet = false;
inline thread_local double g_preBtnY = 0.0;
// --spidertptouch (constants.hpp): this tick's spider teleport came from a fresh press (the buttons phase,
// after the collision pass), so the touch boxes met this tick are the pre-teleport body's (step.hpp,
// markTouched). Set by stepOne's tap branch; cleared and restored with g_preBtnSet.
inline thread_local bool t_spiderFreshWarp = false;
// Did the last stepOne's ball tap? Written by stepOne (cleared on entry, set from its own
// ballFlippedThisTick near the end), read by stepBoth right after each half (--repelland).
inline thread_local bool g_ballTapped = false;
// ...and the body's +0xa0c as the repel reads it: after this tick's collisions, before its tap
// (--repela0c; stepOne writes it next to g_ballTapped).
inline thread_local uint8_t g_hitGRepel = 0;

// ---- THE 2.2 TRIGGER QUEUE (channel / ord) ---------------------------------
//
// GD does not fire these on proximity. Every trigger with m_objectType==1 sits
// in a per-channel bucket, sorted once at load, and `checkSpawnObjects` walks
// ONE channel per tick from a per-channel cursor, stopping at the first element
// whose firing point the player has not passed. So an unconsumed element blocks
// everything behind it, and a channel switch can release several at once -- not
// because bulk firing is a rule, but because the loop keeps going until it
// breaks.
//
// The model had none of this: it tested "did the player cross this trigger's
// axis coordinate" against every 2900, with a perpendicular window standing in
// for channel separation. That is why lv22's t=6,315 is missing -- uid5957 is
// released by channel 3's REVERSE flag, which uid5809 set 335 ticks earlier.
struct RotQEntry {
    int uid = -1;
    // 2900 or 2899. Both are queue residents -- they carry m_channelValue and
    // m_ordValue and are consumed the same way -- but ONLY a 2900 touches the
    // channel machinery: EffectGameObject::triggerObject sends 2900 to
    // rotateGameplay and 2899 to processOptionsTrigger, and the writers of the
    // active channel (+0x33c) are rotateGameplay, resetSpawnChannelIndex,
    // loadUpToPosition, createCheckpoint and init -- no Options path. Ten of
    // lv22's thirty are 2899, so reading that field without checking the id
    // would switch the channel on objects that cannot switch it.
    //
    // [2026-09-10] This used to say "two of those carry m_changeChannel". None
    // of them do -- a 2899 is a GameOptionsTrigger, which has no such field.
    // The dumper read the offset off EffectGameObject, the common base, so the
    // column held whatever memory happened to sit past the end of a 2899: five
    // rows of 1/0/1 and five of 255/255/-1, on objects spread over five
    // channels. The id check was right; the number in the comment was reading
    // that garbage back.
    int id = 0;
    int chan = 0;     // m_channelValue: the bucket this object lives in
    int ord = 0;      // m_ordValue: the first sort key, stronger than position
    double px = 0.0, py = 0.0;   // the firing point: the LOAD position, frozen
    int swarm = 0;    // m_changeChannel: only these switch the active channel
    int swch = 0;     // m_targetChannelID
    // 1 = switches the channel WITHOUT rotating the player.
    //
    // 0 MEANS "ROTATES", NOT "ABSENT". A 2899 has no such field at all (it is a
    // GameOptionsTrigger), and the loader gives it 0 here because there is no
    // third value to give -- unlike swarm, whose 0, and swch, whose -1, are
    // refused by every consumer on their own. What actually keeps a 2899 out of
    // the rotation branch is `rotIdx >= 0`, and the authoritative predicate is
    // `id != 2900`; chanOnly is not carrying that and cannot.
    //
    // So a NEW consumer of chanOnly has to sit behind `id == 2900` or
    // `rotIdx >= 0`. Reading it alone would take "does not switch the channel
    // only" from an object that has no opinion on the question.
    int chanOnly = 0;
    int rotIdx = -1;  // index into g_rotTrig
};
inline std::vector<RotQEntry> g_rotQ;      // grouped by channel, sorted in it
// channel -> [beg, end) in g_rotQ. Two arrays rather than one of size 17,
// because the channels a level uses are not consecutive (lv22 uses 15 of them
// but not 0..14), so "the next channel's begin" is not this one's end.
inline std::array<int, 16> g_rotQBeg{};
inline std::array<int, 16> g_rotQEnd{};
// Bits of State::rotSpent that belong to each channel, so "how many of this
// channel have been consumed" is one popcount.
inline std::array<uint32_t, 16> g_rotQChanMask{};
// --rotqueue: consume rotations from the queue instead of the pre-queue
// selection (the travel-axis crossing, the perpendicular window, the
// last/nearest rule). OPT-IN, and the reason is measured rather than cautious:
//
// the queue is right from t=0 and WRONG AT AN ANCHOR. State::rotSpent /
// rotChan / rotRev are built up tick by tick, so a state handed to --start
// mid-level begins on channel 0 with nothing consumed and re-fires everything
// the run had already passed. Measured: from t=0 the queue fixes the
// transition it was built for (lv22 t=6,315, frame AND gravity, 191 of 192
// transitions agreeing), while quick_regress -- which is anchored sections
// throughout -- loses tracking in 12 of lv22's, worst 400 -> 17 at t=1,800.
//
// This is the third instance today of the same hole: a per-state value the
// anchor scan does not seed (State::fireB, State::lockOff, and now these).
// The old path had --spentrot for exactly it, fed from the GD dump's frame
// transitions before t0; the queue needs the equivalent before it can be the
// default, and until then it is what the flag turns on.
inline bool g_rotQueue = false;
// --startrotq <chan>,<revHex>[,<uid>...]: the queue's equivalent of --spentrot,
// which is the thing the paragraph above says is missing. It seeds the three
// per-state values the anchor scan does not: the active channel, the per-channel
// reverse bits, and WHICH QUEUE ENTRIES the run had already consumed before t0.
//
// The consumed set is given as UIDs, not as a raw rotSpent mask, for the reason
// this campaign spent a day learning: a bit index is a proxy that depends on the
// order buildRotQueue happened to produce, while a uid is the identity of the
// object itself. A mask handed to a differently-ordered queue is wrong in a way
// nothing can detect; a uid that is not in the queue can be reported, and is.
// --spentrot names uids for the same reason.
//
// -1 = not given, which is not the same as "given as channel 0": the anchor
// starting on channel 0 with nothing consumed is precisely the broken state
// described above, so it must be distinguishable from an explicit seed.
inline int g_startRotChan = -1;
inline unsigned g_startRotRev = 0;
inline std::vector<int> g_startRotSpent;
// --seeddump <t>: print the state's accumulated fields at tick t. The
// self-check for anchor seeding -- see the print site in cli.hpp.
inline int g_seedDump = -1;
// --seedevery <n>: print them every n ticks instead. The check needs the truth
// at MANY ticks and the seed at one each, and the truth comes from a single
// whole run -- so this turns the expensive half into one run rather than one
// per tick.
inline int g_seedEvery = 0;
// --p2touch: count the touch boxes the SECOND player enters. Diagnostic only.
// markTouched reads p1's position alone, so a box only p2 reaches is one the
// model can never fire -- see the print site in step.hpp.
inline bool g_p2Touch = false;

// ---- THE ANCHOR PAYLOAD ----------------------------------------------------
//
// `--start` carries the physics state. It does NOT carry the values whose
// worth at tick t depends on the ticks before t, so an anchor begins those at
// their defaults and unmakes what the run had already done: State::fireB read
// as "fired at tick 0", State::lockOff as "never rode anything", the rotation
// queue's three as "channel 0, nothing consumed". Three defects on 2026-09-04,
// one shape.
//
// Seeding them from the recording goes as far as the recording goes and no
// further -- 768 unexpected differences on lv22 became 163, and the remainder
// is boxes whose objects have no recording at all. Those have to come from GD,
// which knows.
//
// NAMED, NOT POSITIONAL. The positional --start fields (30 today, 26 when this was written) cannot take another
// one without every reader changing, and appending after an optional field is
// its own trap. So: `--anchor-state key=value;key=value`.
//
// TRIGGERS ARE NAMED BY UID, NOT BY BIT. Bit numbering is a property of this
// build's 32-box window; a bit index crossing the boundary would mean a
// different box whenever the window moved.
//
// AND THE TOUCH IS p1's ONLY. markTouched reads p1's position, so a bit set
// from "either player entered it" would be one the step function can never set
// going forward -- an anchor claiming what a whole run of the same plan would
// not. The payload's meaning is fixed as "what GD observed under the model's
// own convention", which on today's corpus is the same set (p2 enters no box)
// and is the safe side if that ever stops being true.
// EVERY VALUE IS THE STATE AT t0, like every other --start field. The first
// simulated tick is t0+1, so a payload written at t0+1 is one tick of motion
// too far along -- measured while testing this: lockOff handed in at t0+1 came
// out a whole dx (1.615 px) high at the first compared tick.
inline std::string g_anchorState;
// Which keys this build understands. A payload naming anything else is a
// payload from a different build, and the run stops rather than quietly
// dropping it -- see the refusal in cliMain.
//
// NO VERSION FIELD: the key set IS the version. Printing both sides' keys
// diagnoses the mismatch and says which is older (the shorter one), with no
// build-stamp plumbing to keep in step. If a human-readable identity is ever
// wanted, the mod version and the exe's mtime are already free.
// A PAYLOAD DECLARES THE SUBSYSTEMS IT OWNS, not a bag of keys. `owns=touch`
// means the touch seeding (trig + fireB) comes from GD and everything else is
// left to the path that already seeds it.
//
// The reason is a property this code's own first test established: a payload
// REPLACES the recording-derived seed rather than topping it up, so a key it
// omits is set by nobody -- three fire ticks perfect, the ride at zero, dead
// 45 ticks later. If the payload were just a key list, dropping a key would
// mean either that death by default, or a silent fall back to the recording
// for that one value -- a hybrid seed, and the opposite of what the refusal
// says. Declaring ownership keeps "replaces wholesale" true WITHIN a
// subsystem and leaves the others honestly alone.
inline const char* const kAnchorKeys[] = {"owns", "touch", "portal", "portal2",
                                          "hist"};
// `owns=hist` -> the per-body history values that --start does not carry, as
// ONE versioned value: `hist=<version>|<count>|name:value,name:value`. The
// version and the count are checked, and a name this build does not know is
// refused like an unknown key (every new history value goes
// through this one transport, never another positional --start field).
// Version 1 knows pressSpent and pressSpent2 (GD's +0x986 negated, per body).
inline bool g_ownsHist = false;
inline const char* const kHistNames[] = {"pressSpent", "pressSpent2"};
// Version 2 adds `freeMode`: GD's [layer+0x311], the Free Mode byte each mode
// portal copies in and checkCollisions reads before it clamps anything to the
// band (see kBandFreeKnown in bands.hpp). A version carries every name it knows,
// so a version-1 payload is still complete and leaves the byte unknown.
// nullptr pads the shorter version.
// Version 3 adds `spiderJumpT`: ticks since GD's spider teleport stamp
// (player+0x820 against the clock at +0xaa0), State::spiderJumpT. An anchor
// taken inside that window otherwise starts saturated and kills on the solid
// side GD spares.
// Version 4 adds p1's slope ride as GD's raw facts: `slopeOn` (+0x9b0),
// `slopeUnder` (+0x9b8), `slopeUid` (the ramp at +0x678), `slopeAge` (ticks
// since the ride's clock +0x598 was stamped, read against +0xaa0) and
// `slopeLanded` (a grounded row since then). -1 = not said. Parsed always,
// seeded only under --anchorride (cli.hpp maps them onto the ride fields).
// Version 5 adds the second body's dash: `dashing2` (p2's m_isDashing) and
// `dashRing2` (the uid of the ring p2 dashes from, -1 for none). --start carries
// p1's dash (its 17th/18th fields) and nothing carried p2's, so an anchor taken
// while both bodies of a dual dash started State::dashing2 at 0 and p2 fell. The
// slope is rebuilt from the ring's angle as the dash's own start does (step.hpp),
// so the value stays an integer.
// Version 6 adds the press latch's first byte itself, per body: `jumpBuf` (GD's
// m_jumpBuffered, +0x985) and `jumpBuf2` (-1 = no second body). pressSpent is
// +0x985 && !+0x986, which cannot tell "the button is up" from "the button is down
// and a ball's tap cleared +0x985", and the second is State::holdDead (the hold
// latch): a ship entered on that hold does not thrust. Seeded under --holdseed.
inline std::array<const char*, 13> histNamesFor(int version) {
    if (version == 6)
        return {"pressSpent", "pressSpent2", "freeMode", "spiderJumpT", "slopeOn",
                "slopeUnder", "slopeUid", "slopeAge", "slopeLanded", "dashing2", "dashRing2",
                "jumpBuf", "jumpBuf2"};
    if (version == 5)
        return {"pressSpent", "pressSpent2", "freeMode", "spiderJumpT", "slopeOn",
                "slopeUnder", "slopeUid", "slopeAge", "slopeLanded", "dashing2", "dashRing2",
                nullptr, nullptr};
    if (version == 4)
        return {"pressSpent", "pressSpent2", "freeMode", "spiderJumpT", "slopeOn",
                "slopeUnder", "slopeUid", "slopeAge", "slopeLanded", nullptr, nullptr,
                nullptr, nullptr};
    if (version == 3)
        return {"pressSpent", "pressSpent2", "freeMode", "spiderJumpT", nullptr,
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    if (version == 2)
        return {"pressSpent", "pressSpent2", "freeMode", nullptr, nullptr,
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
    return {kHistNames[0], kHistNames[1], nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr};
}
// --anchorride / --no-anchorride (default on since 2026-09-30): seed the anchor's
// slope-ride fields from the hist payload's version-4 values (cli.hpp, after the
// geometric guess rideAtAnchor). A payload before version 4 seeds nothing, so a
// run without the recording keeps the guess. On together with the mod's cfg
// histride, which writes version 4 and passes one of the two flags on every
// anchored call (config.hpp).
inline bool g_anchorRide = true;
// Which subsystems this payload claims. The lock and the rotation queue keep
// their own seeding until someone measures a reason to move them.
inline bool g_ownsTouch = false;
// `owns=portal` -> State::portalLatch / portalLatch2 come from GD. The mask is
// per player, so the subsystem takes TWO keys and claiming it requires both:
// a single-player level writes `portal2=` empty, which says "p2 spent nothing"
// out loud instead of leaving it to a default that means the same thing by
// accident. Uids rather than bit indices, for the reason the touch payload
// gives -- a bit index is this build's ordinal and a uid is the level's.
inline bool g_ownsPortal = false;
// --seed-partial-ok: run anyway when a key this build wants is absent, and
// stamp the outcome so the result carries it. A warning on stderr does not
// survive into the place results are compared.
inline bool g_seedPartialOk = false;
inline std::string g_seedPartial;   // what was missing, for the outcome line

// Split `a=1;b=2` into pairs, rejecting a key this build does not know. The
// rejection is the point: a payload naming an unknown key was written by a
// build that carries something this one would silently drop, and dropping it
// is how a run degrades without saying so.
inline bool parseAnchorState(const std::string& s,
                             std::vector<std::pair<std::string, std::string>>& out,
                             std::string& unknown) {
    size_t i = 0;
    while (i < s.size()) {
        size_t semi = s.find(';', i);
        if (semi == std::string::npos) semi = s.size();
        const std::string tok = s.substr(i, semi - i);
        i = semi + 1;
        if (tok.empty()) continue;
        const size_t eq = tok.find('=');
        if (eq == std::string::npos) { unknown = tok; return false; }
        const std::string k = tok.substr(0, eq);
        bool known = false;
        for (const char* kk : kAnchorKeys) if (k == kk) { known = true; break; }
        if (!known) { unknown = k; return false; }
        out.emplace_back(k, tok.substr(eq + 1));
    }
    return true;
}
// `uid:tick,uid:tick` -> pairs. Uids, not bit indices: see g_anchorState.
inline std::vector<std::pair<int, int>> parseTouchPayload(const std::string& v) {
    std::vector<std::pair<int, int>> out;
    size_t i = 0;
    while (i < v.size()) {
        size_t comma = v.find(',', i);
        if (comma == std::string::npos) comma = v.size();
        const std::string tok = v.substr(i, comma - i);
        i = comma + 1;
        const size_t colon = tok.find(':');
        if (colon == std::string::npos) continue;
        out.emplace_back(std::atoi(tok.substr(0, colon).c_str()),
                         std::atoi(tok.substr(colon + 1).c_str()));
    }
    return out;
}
// `uid,uid,uid` -> uids. The portal payload carries no tick: a spent portal is
// spent, and unlike a touch box nothing downstream asks WHEN.
inline std::vector<int> parseUidList(const std::string& v) {
    std::vector<int> out;
    size_t i = 0;
    while (i < v.size()) {
        size_t comma = v.find(',', i);
        if (comma == std::string::npos) comma = v.size();
        const std::string tok = v.substr(i, comma - i);
        i = comma + 1;
        if (!tok.empty()) out.push_back(std::atoi(tok.c_str()));
    }
    return out;
}
// std::popcount is C++20 and this tree builds as C++17.
inline int popCount32(uint32_t v) {
    int n = 0;
    while (v) { v &= v - 1; ++n; }
    return n;
}
inline int g_rotQChans = 0;                // how many channels actually appear

// GD's `getObjectDirection`, which is what decides a bucket's sort axis:
// 1 = y ascending, 2 = y descending, 3 = x descending, 4/default = x ascending.
// flipY does not reach the result and is not read here. The rotation is
// truncated to an int and then compared for exact equality, so 89.5 degrees
// matches nothing and falls through to x-ascending -- deliberately, that is
// GD's own behaviour rather than a tolerance this code chose.
inline int rotQDirection(double rot, bool flipX) {
    const int r = ((int)rot) % 360;
    if (r == 0)                  return flipX ? 3 : 4;   // x desc : x asc
    if (r == 180 || r == -180)   return flipX ? 4 : 3;
    if (r == 90 || r == -270)    return flipX ? 1 : 2;   // y asc : y desc
    if (r == 270 || r == -90)    return flipX ? 2 : 1;
    return 4;
}

// The signed coordinate a bucket is ordered along.
inline double rotQAxis(int dir, double px, double py) {
    switch (dir) {
        case 1:  return  py;    // y ascending
        case 2:  return -py;    // y descending
        case 3:  return -px;    // x descending
        default: return  px;    // x ascending
    }
}

// Sort each channel's bucket the way LevelTools::sortChannelOrderObjects does:
// ord first, then the axis coordinate TRUNCATED TO AN INT (so two objects in
// the same integer bucket are not separated by position at all), then uid.
// The direction comes from the FIRST 2900 in uid order that switches TO this
// channel -- not from the objects in it, and not from the level's rotation.
inline void buildRotQueue() {
    g_rotQBeg.fill(0);
    g_rotQEnd.fill(0);
    g_rotQChans = 0;
    if (g_rotQ.empty()) return;
    // Pass 1: the direction of each channel, from the first switcher that
    // names it. "First" is array order, which is uid order.
    std::array<int, 17> dir{};
    dir.fill(0);
    std::vector<const RotQEntry*> byUid;
    byUid.reserve(g_rotQ.size());
    for (const RotQEntry& e : g_rotQ) byUid.push_back(&e);
    std::sort(byUid.begin(), byUid.end(),
              [](const RotQEntry* a, const RotQEntry* b) { return a->uid < b->uid; });
    for (const RotQEntry* e : byUid) {
        if (!e->swarm || e->swch < 0 || e->swch > 15) continue;
        if (dir[e->swch]) continue;                       // first one wins
        if (e->rotIdx < 0 || (size_t)e->rotIdx >= g_rotTrig.size()) continue;
        const RotTrig& rt = g_rotTrig[(size_t)e->rotIdx];
        dir[e->swch] = rotQDirection(rt.rawRot, rt.flipX != 0);
    }
    // Pass 2 + 3: bucket by channel, then sort inside each bucket.
    std::stable_sort(g_rotQ.begin(), g_rotQ.end(),
                     [](const RotQEntry& a, const RotQEntry& b) {
                         return a.chan < b.chan;
                     });
    size_t i = 0;
    while (i < g_rotQ.size()) {
        size_t j = i;
        const int ch = g_rotQ[i].chan;
        while (j < g_rotQ.size() && g_rotQ[j].chan == ch) ++j;
        const int d = (ch >= 0 && ch <= 15 && dir[ch]) ? dir[ch] : 4;
        std::sort(g_rotQ.begin() + (long long)i, g_rotQ.begin() + (long long)j,
                  [d](const RotQEntry& a, const RotQEntry& b) {
                      if (a.ord != b.ord) return a.ord < b.ord;
                      const int ka = (int)((float)a.ord
                                           + (float)rotQAxis(d, a.px, a.py));
                      const int kb = (int)((float)b.ord
                                           + (float)rotQAxis(d, b.px, b.py));
                      if (ka != kb) return ka < kb;
                      return a.uid < b.uid;
                  });
        if (ch >= 0 && ch <= 15) {
            g_rotQBeg[(size_t)ch] = (int)i;
            g_rotQEnd[(size_t)ch] = (int)j;
            // `k < 32` is the same 32-bit cursor limit step.hpp's `idx < 32`
            // obeys. A queue longer than that is REFUSED before it is used
            // (cli.hpp, right after loadRotQueue), so the truncation here is
            // unreachable rather than merely unlikely.
            uint32_t m = 0;
            for (size_t k = i; k < j && k < 32; ++k) m |= (uint32_t)1 << k;
            g_rotQChanMask[(size_t)ch] = m;
            ++g_rotQChans;
        }
        i = j;
    }
}

// Read the MOD's rotgameplay.txt (solver.hpp has written it for a while; until
// now nothing read it). Joins to g_rotTrig by uid, so it runs AFTER the level.
inline bool loadRotQueue(const std::string& path) {
    std::ifstream in(path);
    if (!in) return false;
    std::unordered_map<int, int> byUid;
    for (size_t k = 0; k < g_rotTrig.size(); ++k)
        byUid[g_rotTrig[k].uid] = (int)k;
    std::string line;
    std::getline(in, line);                       // header
    g_rotQ.clear();
    while (std::getline(in, line)) {
        RotQEntry e{};
        int sord = 0, sordd = 0, spx = 0, target = 0, chanChanged = 0;
        const int n = std::sscanf(
            line.c_str(), "%d,%d,%lf,%lf,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
            &e.uid, &e.id, &e.px, &e.py, &e.chan, &e.ord, &sord, &sordd, &spx,
            &target, &chanChanged, &e.swarm, &e.chanOnly, &e.swch);
        // The last three columns are "-,-,-" for an object that HAS no such
        // fields -- a 2899 is a GameOptionsTrigger and carries none of
        // m_changeChannel / m_channelOnly / m_targetChannelID (the dumper wrote
        // whatever memory sat at those offsets until 2026-09-10). Such a row is
        // still a queue resident and must not be dropped: `n < 14` alone would
        // silently remove all ten of lv22's 2899s from the queue, which is a
        // behaviour change and not the hygiene this was.
        //
        // The sentinels are the values every consumer already refuses --
        // swarm 0 (frames.hpp:384, cli.hpp:1825), swch -1 (step.hpp:390's
        // `swch >= 0`) -- so an absent field cannot be read as a present one.
        static const std::string kAbsent = ",-,-,-";
        const bool absent = line.size() >= kAbsent.size()
                            && line.compare(line.size() - kAbsent.size(),
                                            kAbsent.size(), kAbsent) == 0;
        if (n == 11 && absent) {
            e.swarm = 0;
            e.chanOnly = 0;
            e.swch = -1;
        } else if (n < 14) {
            continue;
        }
        const auto it = byUid.find(e.uid);
        e.rotIdx = (it == byUid.end()) ? -1 : it->second;
        g_rotQ.push_back(e);
    }
    buildRotQueue();
    return true;
}
// --spentrot uid,uid,...: the uids of the 2900s this run had already fired
// before the anchor (--start's t0). The one-shot (firedT) cannot be carried
// through --start, so re-anchoring behind the maze re-fires a spent trigger at
// the next crossing and only the model turns (lv22 t=14,321 uid6337: GD had
// consumed it at t=12,795 and passes straight through; the model turned into
// frame 3 and the whole world broke. A fixup cannot carry the frame, so the
// driver re-stacked the same fixup for 30 iterations and marked time). The
// driver derives it from the gframe transitions (t<t0) in the GD dump and
// passes it in.
inline std::vector<int> g_spentRot;
// --spentpad uid,uid,...: the uids of the PADS this run had already fired
// before the anchor. The same hole one shape down: GD's activatedByPlayer latch
// is permanent for the attempt (collisionCheckObjects drops a latched object at
// the vf560 test, before any shape test), the model keeps it in
// State::usedPad, and --start cannot carry a pointer table. The seeding that
// was there read the boxes the player OVERLAPS at t0, which catches a contact
// still in progress and nothing else; a pad fired earlier and stepped off comes
// back live, and the anchored arm fires it a second time where GD never does.
//
// Measured on lv18 x~25,000, a six blue-pad zig-zag the player re-enters about
// 32 ticks after each first contact: anchoring at t0=18,200 lands between uid
// 12805's two contact runs (18,184 and 18,216-217), the fresh anchor re-fires
// it at 18,216 and edvy closes exactly on kPadBlueVy (0.215 - (-15.595) =
// +15.810). Corpus-wide: 502 static pads, 18 re-entries (lv13/14/18/21, all id
// 67 / type 10), GD fires the second run 0 of 18 and the WHOLE-RUN model 0 of
// 18 -- so this is an instrument defect and not a physics one, and the only
// thing that may move is an anchored comparison.
//
// The producer walks the recording rather than the geometry (py/gdtas/
// padhistory.py). Overlap is the portal latch's rule and NOT a pad's: the blue
// pad's polarity gate in step.hpp runs BEFORE `c.usedPad[slot] = pd`, so a
// gravity pad met with the wrong gravity is neither fired nor consumed, and
// seeding it would suppress a firing GD performs.
inline std::vector<int> g_spentPad;
// A/B switch (--no-spentpad): ignore the list above, i.e. seed only from the
// boxes overlapped at t0, which is what every build before 2026-09-06 did.
inline bool g_spentPadSeed = true;
// (--norevtoggle, which turned off the reverse-run toggle of a same-frame 2900,
// is gone since the flag clean-up.)

// ---- CONTROL-DISABLED WINDOWS (--ctrlwin t0:t1,...) -------------------------
//
// **id 2899 is not a reverse run but an Options trigger** (GD's
// GameOptionsTrigger). All 10 of lv22's raise/lower m_disableP1Controls, and
// while it is raised **the button is ignored entirely** -- neither a press nor
// the cube's held-button re-jump happens. The windows measured in GD
// (2026-08-18, cfg endtrace=1) are four:
//   t=13,461..14,224 / 18,259..18,612 / 19,152..19,576 / 20,110..20,487
// 1,921 ticks in total. The model thought it could jump there, and this was the
// identity of fixcensus's `m0/mini0/g1/gdg1/sp0.9/air/in1` family (edy=0.000 /
// edvy=-11.180, only the model jumps).
//
// **Why not derive it from the triggers ourselves**: which x crossing makes GD
// raise it is still unsolved. Crossing the same x of the same trigger many times
// fires it only once, and that once is not necessarily the first crossing
// (uid 18316 on the 5th, uid 18086 on the 3rd). Nor is it the distance to the
// player's y (no fire at Δ=1,236, fires at Δ=1,324). The gate is GD's camera or
// something like it, which the current model does not have. **Firing on a guess
// does more harm than good** (burning uid 18315 at its first crossing t=10,673
// creates 393 ticks of control loss that GD does not have, and on top of that
// misses the real window). So the windows are passed only "when GD knows them":
// section-anchor replays (fixcensus / quick_regress / the driver's re-anchor)
// already receive gframe and pmin/pmax from the GD dump, so this fits the same
// scheme. A cold run from the head has no such information and the model jumps
// as before (an unresolved hole).
inline std::vector<std::pair<long long, long long>> g_ctrlWin;

inline bool ctrlOffAt(long long t) {
    for (const auto& w : g_ctrlWin)
        if (t >= w.first && t <= w.second) return true;
    return false;
}
// --ctrlwinlag: the window's first tick still takes the button as it is; the window holds it at 0
// from the second tick on. A custom level's ship, its window 7,271..7,331 (the ctrlOff column), five
// plans in GD that differ only in the press:
//   held from 7,270, released by the plan on 7,272  thrust 7,270-7,271, falls from 7,272
//   held from 7,270, released by the plan on 7,292  the same rows: the window released it on 7,272
//   held from 7,269                                  thrust 7,269-7,271, falls from 7,272
//   a fresh press reaching 7,271                     thrust on 7,271, falls from 7,272
//   a fresh press reaching 7,272                     no thrust at all
// So the flag the column shows is raised after the tick's own buttons. The model zeroed 7,271 too.
// `heldBefore` is kept for the callers; the measurement does not need it.
// On by default since 2026-10-06; --no-ctrlwinlag is the off arm.
inline bool g_ctrlWinLag = true;
// --ctrlliftlag (default on): ...and the lift lags the same tick: the button the model's input at t stands for is
// GD's at the end of t - 1, which a flag still raised then refuses, so t is refused while t - 1 is in the window,
// whatever t is. The custom level of --ctrlwintrig: a ship held across its window's lift (window 13,404..13,551)
// thrusts from 13,553 in GD (vy -4.732 on 13,552, -4.624 on 13,553); the model, refusing only inside the window,
// thrust from 13,552.
inline bool g_ctrlLiftLag = true;
inline bool ctrlInputOffAt(long long t, bool heldBefore) {
    (void)heldBefore;
    if (g_ctrlLiftLag && g_ctrlWinLag) return ctrlOffAt(t - 1);
    if (!ctrlOffAt(t)) return false;
    return !g_ctrlWinLag || ctrlOffAt(t - 1);
}
// --ctrlwin2 / --ctrlwinp2 (--ctrlwinp2 on by default since 2026-10): the windows above are player 1's m_controlsDisabled, and
// the Options trigger switches the two players separately (processOptionsTrigger 0x223d50: +0x754
// calls disable/enablePlayerControls on player 1, +0x758 on player 2). --ctrlwin2 carries the second
// body's own windows (the dump's p2ctrlOff). A custom level's dual robot: an Options trigger with
// only the player-1 box set raises ctrlOff at 7,012, and on the plan's press at 7,035 GD's second
// body jumps (vy +5.615) while the first, held on its band, does not; the model held both. With the
// flag the second half of a dual tick (stepBoth's second stepOne) reads --ctrlwin2 instead of
// --ctrlwin. The replay's window re-push (cli.hpp) is still built from --ctrlwin alone: a second-body
// window that lifts under a held button is not measured.
inline std::vector<std::pair<long long, long long>> g_ctrlWin2;
inline bool ctrlOff2At(long long t) {
    for (const auto& w : g_ctrlWin2)
        if (t >= w.first && t <= w.second) return true;
    return false;
}
// Whether the body being stepped takes no button at t: the second half reads its own windows
// under --ctrlwinp2.
inline bool ctrlBodyInputOffAt(long long t, bool secondHalf) {
    if (!secondHalf) return !g_ctrlWin.empty() && ctrlInputOffAt(t, false);
    if (g_ctrlLiftLag && g_ctrlWinLag) return ctrlOff2At(t - 1);   // --ctrlliftlag
    if (!ctrlOff2At(t)) return false;
    return !g_ctrlWinLag || ctrlOff2At(t - 1);
}
// --ctrlwintrig (default on): past what the recorded windows cover (g_ctrlTrigFrom, the recording's reach), player
// 1's controls follow the Options triggers the player's x has crossed, on a level with no gameplay rotation (lv22's
// rotated crossings are the unresolved gate above). A custom level's five windows all come out of its ten optp1
// 2899s that way: from the tick x reaches a raising one's cx to the tick before it reaches the lowering one's
// (2,737..3,094, 4,264..4,494, 10,910..11,032, 13,404..13,551); the fifth, raised by a spawn-triggered 2899, starts
// 11 ticks after its crossing, so a touch- or spawn-triggered row makes the state unknown (no window). The window
// at 13,404 began past the 12,483 the search call's recording reached, and the model's wave turned on a press GD
// ignored. The column's raise-after-buttons lag (--ctrlwinlag) is the test at both x and the previous tick's x.
struct CtrlTrig {
    double cx;      // world x
    int8_t v;       // 1 disables player 1's controls, -1 enables them
    uint8_t gated;  // touch- or spawn-triggered: not fired by the crossing
};
inline bool g_ctrlWinTrig = true;
inline std::vector<CtrlTrig> g_ctrlTrigs;   // sorted by cx at load
inline long long g_ctrlTrigFrom = -1;        // the tick past which they answer (cli.hpp)
// 1 off, 0 on, -1 unknown (the last trigger at or behind x is gated)
inline int ctrlTrigStateAt(double x) {
    int st = 0;
    for (const CtrlTrig& t : g_ctrlTrigs) {
        if (t.cx > x) break;
        st = t.gated ? -1 : (t.v == 1 ? 1 : 0);
    }
    return st;
}
inline bool ctrlTrigInputOff(long long t, double x, double xPrev) {
    if (!g_ctrlWinTrig || g_ctrlTrigs.empty() || t <= g_ctrlTrigFrom) return false;
    if (g_ctrlLiftLag && g_ctrlWinLag) return ctrlTrigStateAt(xPrev) == 1;   // --ctrlliftlag
    return ctrlTrigStateAt(x) == 1 && (!g_ctrlWinLag || ctrlTrigStateAt(xPrev) == 1);
}
// --ctrlwavenow (default on): ...and a wave's button with no lag. A tick in GD runs update, collisions, the crossing
// queue (where an Options trigger raises the flag) and then the buttons, so a button on the tick the flag goes up
// finds it raised. The model's wave turns on the button the state came in with (the tick after), i.e. its input at
// t is GD's button at the end of t; the ship's thrust, which --ctrlwinlag measured, takes this tick's input and so
// GD's button of the tick before. The custom level of --ctrlwintrig, its window raised at 13,404: a wave pressed
// on 13,404 does not climb on 13,405 in GD, where the model climbed with the recorded window passed as well.
inline bool g_ctrlWaveNow = true;
inline bool ctrlOffNowAt(long long t, bool secondHalf, double x) {
    if (secondHalf) {
        if (ctrlOff2At(t)) return true;
    } else if (!g_ctrlWin.empty() && ctrlOffAt(t)) {
        return true;
    }
    return !secondHalf && g_ctrlWinTrig && !g_ctrlTrigs.empty() && t > g_ctrlTrigFrom && g_rotTrig.empty()
           && ctrlTrigStateAt(x) == 1;
}
// Jump ticks that come from a window's re-push (= w1+1). GD's buffered jump has
// holding already at 0 when it fires, so **no hover starts** -- the model side,
// which mirrors it with a synthetic press, would create s.action=1, so the jump
// on exactly this tick does not accumulate rHover. Filled only when a replay is
// loaded (always empty during the search).
inline std::vector<long long> g_winRePushJump;
inline bool rePushNoHoverAt(long long t) {
    for (long long v : g_winRePushJump)
        if (t == v) return true;
    return false;
}

inline void toFrame(int f, double X, double Y, double& u, double& v) {
    switch (f & 3) {
        case 0:  u =  X; v =  Y; break;
        case 1:  u = -Y; v =  X; break;   // rot 90:  travel -Y
        case 2:  u = -X; v = -Y; break;   // rot 180: travel -X (the reverse runs)
        default: u =  Y; v = -X; break;   // rot 270 (= -90): travel +Y
    }
}
inline void fromFrame(int f, double u, double v, double& X, double& Y) {
    switch (f & 3) {
        case 0:  X =  u; Y =  v; break;
        case 1:  X =  v; Y = -u; break;
        case 2:  X = -u; Y = -v; break;
        default: X = -v; Y =  u; break;
    }
}
struct UnlinkTrig {
    double cx;   // world x
    int8_t v;    // 1 unlinks, -1 links again
};
inline std::vector<UnlinkTrig> g_unlinkTrigs;   // sorted by cx at load
// The level is in two-player mode (levelsettings `twoPlayer=1`, the mod writes it only then): the second body is a
// player of its own with a button of its own (a plan's input bit 1), and no gravity portal either one crosses flips
// the other -- the link a dual portal makes does not exist. 0 on every level without the key.
inline int g_twoPlayer = 0;
// --dualbornlinked (default on since 2026-10-23; --no-dualbornlinked is the off arm): a dual portal of a two-player level gives the new body the INVERSE gravity and the negated vy of the first, as the game does
// unless the layer's +0x860 is up (an Options trigger's Unlink Dual Gravity, g_unlinkTrigs). The model took every two-player level for unlinked at birth (the second body born with the
// first's own gravity and vy). Birth only: after it a two-player level stays unlinked (a gravity portal of one body flips the other body only on a level without twoPlayer=1; the game's
// own dump of a two-player level shows lone flips of the second body, t=150 and t=9,217 on the 59 KB lab level, with the pair in the same mode).
inline bool g_dualBornLinked = true;
// Solving ONE body of a two-player level against the other's plan. `--ghostlevels <file>` (lines `tick,level`) names the button the OTHER body
// (the second slot of the State) takes at each tick -- the effect level, as a `--replay` of its plan writes in the `act`/`act2` column -- and makes
// that body a ghost: it is stepped with its plan (its speed portals still count) but its death ends nothing. The search then branches the first
// slot's button only. `--swapbodies` puts the level's SECOND player into the first slot (and the first into the second) before the call begins, so
// that every rule of the search, which reads the first slot, serves the other player; `--bodybit N` writes the plan's levels as bit N-1 of the mask
// (`input=<press>,<level << (N-1)>`), 2 for the second player. All three are off (0 / empty / 1) and change nothing without a two-player level.
inline bool g_ghostOn = false;
inline long long g_ghostBase = 0;
inline std::vector<uint8_t> g_ghostLvl;
inline bool g_swapBodies = false;
inline int g_bodyBit = 1;
inline int g_startAct2 = -1;   // --startact2 <0|1>: the second player's button as the anchor's tick left it (-1 = not said)
// Set by a SEARCH call on a two-player level that has both bodies (cli.hpp, before the search): the call searched the first body against a ghost that holds
// nothing, and cliMain then searches the second against the first's plan (cliMain's second pass) and writes the two as one mask plan.
inline bool g_twoBodyCall = false;
// --pinrows <file> (lines `tick,x,y`): the ticks where GD holds PLAYER 1 on a spot the physics does not give it -- a Teleport trigger (3022) that a Spawn
// loop fires again every tick, onto a target a Move trigger is carrying, with the controls off: x stays where the teleport put it, y follows the target, and
// vy reads 0 (measured on a custom two-player level, t=3..168: x 644.5 throughout, y 2085 -> 2055, the button of either player doing nothing). The model
// fires a teleport trigger once, at the crossing, so from the next tick it lets the body fall and run on. With the rows the caller recorded (the pinned ticks
// of GD's own attempt: x did not move), the first player's body is put at the recorded (x, y) at the end of each such tick, vy 0, off the ground.
// Player 1 only: in a two-player level's swapped pass that is the second slot. Empty (every call that was not given the file) changes nothing.
struct PinRow {
    long long t;
    float x, y;
};
inline std::vector<PinRow> g_pinRows;   // sorted by t
inline const PinRow* pinRowAt(long long t) {
    if (g_pinRows.empty()) return nullptr;
    size_t lo = 0, hi = g_pinRows.size();
    while (lo < hi) {
        const size_t m = (lo + hi) / 2;
        if (g_pinRows[m].t < t) lo = m + 1;
        else hi = m;
    }
    return (lo < g_pinRows.size() && g_pinRows[lo].t == t) ? &g_pinRows[lo] : nullptr;
}
inline int ghostLevelAt(long long t) {
    const long long i = t - g_ghostBase;
    return (i >= 0 && i < (long long)g_ghostLvl.size()) ? (int)g_ghostLvl[(size_t)i] : 0;
}
// --p2taps (default OFF): in a two-player level a Tap trigger (1595) is fired by the SECOND player's press edge and armed by the first
// player's x (cli.hpp itemGates). The second player's button by tick for a walk of a plan (--replay: the plan's bit 1, set before the walk);
// the search's own passes read the ghost's levels (ghostLevelAt) instead.
inline bool g_p2Taps = false;
// --p2tappress (default OFF, two-player levels, the per-body wrapper): a Tap trigger (1595) of a two-player level that player 1's plan arms
// (its x passes the Tap's) gets a press of the SECOND player's a few ticks later, forced into the second body's pass -- the one thing a search that
// holds the other body on nothing cannot ask for. The model does not know what the Tap opens (its chain is not in the level data), so the press is
// made blind, once, and the game's own recording of that attempt (the moving geometry the loop keeps) shows the door open to every later call.
// `--mustpress t0:t1,...` (set by the wrapper for its second pass, or by hand): the searched body's button is 1 on ticks t0..t1 and 0 on t1+1,
// whatever the search would have chosen.
inline bool g_p2TapPress = false;
inline std::vector<std::pair<long long, int>> g_mustPress;   // (tick, level), sorted by tick
inline int mustPressAt(long long t) {
    if (g_mustPress.empty()) return -1;
    size_t lo = 0, hi = g_mustPress.size();
    while (lo < hi) {
        const size_t m = (lo + hi) / 2;
        if (g_mustPress[m].first < t) lo = m + 1;
        else hi = m;
    }
    return (lo < g_mustPress.size() && g_mustPress[lo].first == t) ? g_mustPress[lo].second : -1;
}
inline std::vector<uint8_t> g_p2ReplayLvl;
inline long long g_p2ReplayBase = 0;
inline int secondLevelAt(long long t) {
    if (g_ghostOn) return ghostLevelAt(t);
    const long long i = t - g_p2ReplayBase;
    return (i >= 0 && i < (long long)g_p2ReplayLvl.size()) ? (int)g_p2ReplayLvl[(size_t)i] : 0;
}
// Whether the pair is unlinked during a tick that starts at (u, v) in `frame`: a gravity portal of one body flips the other
// (step 5's partner call) only while it is not. A two-player level is unlinked throughout, with or without --dualbornlinked.
inline bool unlinkTrigAt(int frame, double u, double v) {
    if (g_unlinkTrigs.empty()) return false;
    double X = u, Y = v;
    if (frame & 3) fromFrame(frame, u, v, X, Y);
    int8_t st = -1;
    for (const UnlinkTrig& t : g_unlinkTrigs) {
        if (t.cx > X) break;
        st = t.v;
    }
    return st == 1;
}
inline bool dualUnlinkedAt(int frame, double u, double v) {
    if (g_twoPlayer) return true;
    return unlinkTrigAt(frame, u, v);
}
// Whether the body a dual portal gives birth to takes the first body's own gravity and vy (unlinked) rather than the inverse and the negation. --dualbornlinked reads it from the
// Options triggers alone, also on a two-player level; otherwise a two-player level is unlinked at birth as it always was.
inline bool dualBornUnlinkedAt(int frame, double u, double v) {
    if (g_twoPlayer && !g_dualBornLinked) return true;
    return unlinkTrigAt(frame, u, v);
}
// The travel coordinate of a world point in this frame (what `cx` becomes).
inline double frameU(int f, double X, double Y) {
    double u, v;
    toFrame(f, X, Y, u, v);
    return u;
}
// ...and the PERPENDICULAR one (what `cy` becomes). A 2900 fires only when the
// player's box overlaps it on this axis -- see the gate in applyRotation.
inline double frameV(int f, double X, double Y) {
    double u, v;
    toFrame(f, X, Y, u, v);
    return v;
}

// ---- turning one object into a frame's coordinates -------------------------
//
// What has to come along:
//   - the box: centre through toFrame, hw/hh swapped on the odd frames
//   - `rot`: the object's own turn is measured against the screen, so it moves
//     with the frame (used by the oriented rings and the dash angle)
//   - the slope line (sy0/sy1 are absolute heights at the box's left/right
//     edge): a turned slope is a different line, so the two endpoints are
//     carried as points and re-read
//   - tpY (an absolute target y for a teleport) becomes the target's v
inline void turnObj(Obj& o, int f) {
    if ((f & 3) == 0) return;
    const double cx = o.cx, cy = o.cy;
    double u, v;
    toFrame(f, cx, cy, u, v);
    // the slope's two surface points, in world, before the box moves
    const double sxL = cx - o.hw, sxR = cx + o.hw;
    const double syL = o.sy0, syR = o.sy1;
    o.cx = u; o.cy = v;
    if (f & 1) std::swap(o.hw, o.hh);
    o.rot += 90.0 * (f & 3);
    if (o.slope) {
        double uL, vL, uR, vR;
        toFrame(f, sxL, syL, uL, vL);
        toFrame(f, sxR, syR, uR, vR);
        if (uL <= uR) { o.sy0 = vL; o.sy1 = vR; }
        else          { o.sy0 = vR; o.sy1 = vL; }
    }
    if (o.tpY != 0.0) {
        double tu, tv;
        toFrame(f, cx, o.tpY, tu, tv);
        o.tpY = tv;
    }
    // the exit half is a POINT of its own (its x is not the portal's cx), so
    // it turns as one -- the teleport block reads its v as the target
    if (o.tpEx != 0.0 || o.tpEy != 0.0) {
        double eu, ev;
        toFrame(f, o.tpEx, o.tpEy, eu, ev);
        o.tpEx = eu; o.tpEy = ev;
    }
}

}  // namespace dp
