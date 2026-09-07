#pragma once
#include "dp/level_loader.hpp"

namespace dp {

// objects overlapping an x window, via the sorted array
struct XSlice {
    const std::vector<Obj>& v;
    size_t lo = 0;
    double maxHw = 0.0;
    explicit XSlice(const std::vector<Obj>& objs) : v(objs) {
        for (const Obj& o : objs) maxHw = std::max(maxHw, o.hw);
    }
    // Rewind for a window that starts BEHIND the cursor. The old code only ever
    // advanced (callers were monotone in x); gameplay rotation broke that, and
    // the first fix -- rebuilding the slice -- put an O(N) scan on every group
    // of every tick (lv21's regression went 11.6 -> 29.5 min). The vector is
    // sorted by cx, so a binary search puts the cursor back in log N.
    void seekTo(double x0) {
        const double lim = x0 - maxHw - 40.0;
        size_t a = 0, b = v.size();
        while (a < b) {
            const size_t m = (a + b) / 2;
            if (v[m].cx < lim) a = m + 1; else b = m;
        }
        lo = a;
    }
    // callers advance monotonically in x -- EXCEPT under reverse (State::rev),
    // where the travel coordinate DECREASES every tick. [2026-08-16]
    // The cursor only ever moved forward, so everything behind it became
    // invisible for the rest of the section. Measured on lv22: after the
    // rotation at t=11,405 the player runs in reverse along a ceiling made of
    // 30 px blocks, and the model saw uid 6288/6289/6290 (cx 16,095/16,065/
    // 16,035) but NEVER 6291/6292 (16,005/15,975) -- it lost its support at
    // x=16,010 and fell off a surface GD keeps walking on.
    // `seekTo` is the binary search that already exists for the rotation
    // rebuild; firing it only when the window actually moved back keeps the
    // forward case at its old cost (rebuilding every tick cost lv21 11.6 ->
    // 29.5 min once, which is why this is a rewind and not a rebuild).
    template <class F>
    void forRange(double x0, double x1, F&& f) {
        if (lo > 0 && v[lo - 1].cx + v[lo - 1].hw + 40 >= x0) seekTo(x0);
        while (lo < v.size() && v[lo].cx + v[lo].hw + 40 < x0) ++lo;
        for (size_t i = lo; i < v.size(); ++i) {
            if (v[i].cx - v[i].hw > x1 + 40) break;
            f(v[i]);
        }
    }
};

struct State {
    float y, vy;      // vy is WORLD frame (dump convention): y += 0.225 * vy
    uint8_t mode;     // 0 cube, 1 ship
    uint8_t held;     // ship: input level; cube: unused (jump is an event)
    uint8_t grounded; // cube only
    uint8_t flip;     // gravity flipped (upsideDown): all vertical signs mirror
    float xAbs;       // absolute x, float-accumulated like GD (see advanceX)
    const Obj* snapObj;  // GD m_objectSnappedTo: last solid stood on (null = none)
    float snapDist;      // GD m_snapDistance: x - snapObj->cx at the last contact
    const Obj* usedOrb;  // GD m_touchedRings: orb already used in this contact
    // One ring per HOLD. GD will not activate a second ring until the button
    // has been released and pressed again, and lv14 x=16875 y=585 is where that
    // shows: a yellow ring (uid 4774) and a gravity ring (uid 4775) sit on the
    // same square, the plan holds through both, and GD fires only the yellow one
    // (t=13015, vy := 11.180, then plain gravity). The model fired the yellow
    // one and then the gravity one on the very next tick (4.472), flipped, and
    // went up where GD came down. Per-object memory alone cannot express this --
    // the second ring is a different object and was never used.
    uint8_t ringHold;
    // "This press has already been spent" -- GD's +0x986, mirrored into +0x98a once
    // per tick at 0x389f18 and tested by ringJump. `ringHold` is the same idea for
    // rings ALONE; 0x986 is the shared latch of every consumer, and brief-022 named
    // the gap ("nothing does for whatever consumed the press").
    // Set by a ring firing and by the grounded impulse branch (the cube's jump, the
    // ball's tap, the spider's flip -- GD's updateJump clears 0x986 at 0x38bbef when
    // it jumps). Cleared on release, like GD's releaseButton.
    // READ IT AS `s.pressSpent`, WRITE IT AS `c.pressSpent`, and do not "tidy" the
    // read to `c.`: GD's pushButton walks m_touchedRings FIRST and returns before
    // updateJump if a ring fires, so in GD a ring PREEMPTS the grounded jump on a
    // shared tick. The model evaluates them in the opposite order, and reading the
    // entering value is the only thing that reproduces GD's precedence. Measured on
    // lv14 t=1,859, where a press rises on a grounded cube and a yellow orb (uid
    // 541) is in reach on the same tick: GD fires the orb with b98a=1 and no jump
    // runs at all (`vy=0.2160 -> -11.1800`, the whole change through the ring path).
    // Reading `c.` here would delete that firing and lose GD's own y=225.049.
    uint8_t pressSpent;
    // Riding a slope last tick, and the gradient it was riding. Leaving the
    // top is what launches the player (see slopeExitVy), so the model has to
    // remember that it WAS on one.
    uint8_t onSlope;
    // Ticks since this ride FIRST touched a slope, saturated at 24. GD's exit
    // impulse ramps up over the first 0.1 s of the ride (see slopeExitVy /
    // slopeRampFactor): m_slopeStartTime is written on the air->slope
    // transition only (collidedWithSlopeInternal 0x390757: guarded on
    // !m_wasOnSlope), so a chain of contiguous ramps keeps ONE start time.
    // 24 = 0.1 s at 240 ticks/s, past which the factor is 1.0 and further
    // counting cannot change anything.
    uint8_t slopeT = 0;
    // Did THIS ride ever become a landing? GD's exit launch comes out of the
    // ride, and a contact that never landed has no ride to launch from --
    // measured at lv19 t=14,633, where the UFO meets the ramp at svy +6.323,
    // fails GD's |vy| <= 5.0 hitGround gate, and GD flies straight on while the
    // model launches on leaving at 14,643.
    //
    // NOT the same as `grounded`, and the difference is load-bearing: the seat
    // sets grounded only `if (rideLands && !(flipForRide && ridesTop))`, so a
    // FLIPPED rider on a floor ramp's top lands without ever setting it. lv16
    // t=8,875 is exactly that (enters at svy -0.051, lands, grounded stays 0)
    // and GD does launch it at 8,913 -- gating the launch on `grounded` would
    // have deleted a launch GD makes.
    //
    // It has to be remembered across ticks because the landing happens at the
    // ride's start and the launch reads it at the ride's end. Recomputing at the
    // exit tick from that tick's own velocity agrees on all 40 rides in the
    // corpus -- none of them crosses 5.0 mid-ride -- but the DP generates entry
    // velocities the corpus never contains, and there the proxy and the
    // mechanism part company where no replay instrument can see it.
    uint8_t rideLanded = 0;
    float slopeM;
    // Ticks since gravity last flipped, saturated at 24 -- the same 0.1 s at
    // 240 ticks/s as slopeT above, and read the same way: GD stamps the time in
    // flipGravity (player+0x800) and collidedWithObjectInternal (:1180-1226)
    // spares the side/crush kill while `now - that < 0.1`, snapping the player
    // to the face instead. Past 24 nothing can change, so it saturates.
    // Carried by the anchor (--start field 28); -1 there means "the caller did
    // not say", which is read as no grace. 0 is a real value (flipped on the
    // anchor tick itself), so it cannot serve as the sentinel.
    // 255 and not 24: this is the value every State built WITHOUT saying
    // anything gets, and the safe answer there is "no grace" (any value at or
    // above kFlipGraceTicks). 24 was inside the window, so a freshly
    // constructed state arrived already graced -- measured on lv22, where the
    // section at t=20,200 seated a cube that GD kills and lost 360 ticks of
    // tracking. The step's own increment clamps it back down to the cap.
    uint8_t flipT = 255;
    // Ticks since the player last touched an id-1859 ceiling arm, saturating.
    // GD writes 2 into its counter on the touch and decays it every tick, so
    // the arm holds for the touch tick and the one after (kArmTicks); while it
    // holds, a cube / robot / spider answers a ceiling with a bonk instead of
    // dying on it (modifiers.hpp, armBoxTouch, three measured points).
    // 255 for the same reason flipT uses it: a State built without saying
    // anything must come out UNARMED, and the step's increment clamps it back
    // to the cap. Carried by the anchor as --start field 29 (-1 = not said).
    uint8_t armT = 255;
    // ...and the same shape for the DART SLIDE arm (id 1755): while it holds, a
    // WAVE is pushed out of a solid's top instead of passing through it
    // (modifiers.hpp, slideBoxTouch). 255 = never armed, for the same reason
    // armT uses it. NOT carried by --start yet: an anchor taken inside the arm's
    // 2-tick window would need it, and nothing has measured one.
    uint8_t slideT = 255;
    // GD's flying band, carried PER STATE. It has to be: the band is written
    // when a mode portal actually fires, and firing needs the player's box to
    // touch the portal in y as well as x. lv1 offers two lanes into its last
    // ship section -- portals at (22935,239) and (24045,405) -- and a shared
    // per-layer band took the second one for every state, including the ones
    // flying the low lane at y=223. The model then pinned them to the high
    // band's floor (240 + 15) while GD flew on at 223.6, and lv1 went from
    // cleared to stuck at x=24,253.
    float bandFloor = 0.f;
    float bandCeil = 1e9f;
    // [2026-09-02] WHEN the band was last written, and which way it is
    // animating. GD's band height is not the portal's H on its own: the ground
    // sprites tween over 0.5 s into the band and 0.4 s back out of it, and
    // while a Static Camera has the band (branch A of getMin/MaxPortalY) the
    // height GD reports is
    //     322 - (322 - H) * p        322 = winSize.height + 2
    // with p that tween's progress. Measured against GD's own sprite column on
    // lv22's 21,140 ticks: 119 ticks in and 95 out, ease-in-out at rate 2 and
    // 1.5, no lag from the portal's tick -- every one of those ticks within
    // 0.02 of GD, worst 0.0086.
    // Per STATE and not per level, for the same reason the band itself is: the
    // portal that writes it is one this worldline had to overlap. lv22 has a
    // ship portal its own verified run passes the x of three times and only
    // fires once, at a different height each time.
    // Carried as ONE BYTE, the way ceilT and slopeT are, rather than as the
    // event's tick: an absolute tick needs four bytes and four of padding
    // beside them, and sizeof(State) went 248 -> 256 for it -- 3% on every
    // node of every level, including the levels this never touches.
    //   bit 7      the target: 1 = animating in, 0 = retracting
    //   bits 0-6   ticks elapsed since the event, saturating at 127 = settled
    // The one thing this cannot represent is a tween INTERRUPTED part way,
    // which GD starts from wherever p had got to; here it restarts from the
    // endpoint. lv22's run never does it (its two animate-ins both start from
    // a fully retracted band and its two retracts from a fully raised one), so
    // there is nothing measured to model -- but a level that flips the band
    // twice inside half a second would be off by however far the first tween
    // had come, and this is where to look.
    uint8_t bandAnim = 0;
    // ...and WHOSE band it is, which is what getMin/MaxPortalY branches on:
    // branch A (the ground sprites, and therefore the animation above) needs
    // `0x23c != 0 && 0x2a0 == 0` -- a static camera holding it AND the mode
    // not having claimed it back.
    //   bit 0   staticY   (0x23c: a Static Camera with a Y axis has it)
    //   bit 1   fromMode  (0x2a0: an animate-in has claimed it)
    // Both are carried per state rather than derived from x. The 1914 that
    // sets staticY does fire at a level-determined x, but the portal that
    // takes the band back is one this worldline had to overlap, and the two
    // interleave: an animate-in leaves staticY set and only a 1914 clears
    // fromMode again.
    // 0 = neither, which is what resetLevelVariables leaves and what every
    // level without a Static Camera keeps for its whole run -- so branch B,
    // bit for bit as before.
    uint8_t bandBranch = 0;
    // The y the band is derived FROM. Normally the cy of the portal that last
    // wrote the band, but inside a dual it is pinned to the DUAL portal's cy:
    // animateInDualGroundNew reads its y from the dual ground layer (this+0x408),
    // which is placed when the dual starts, not from the portal that fired.
    // Measured on lv16 (pmin/pmax columns): the two ship portals at x=12,825
    // sit at cy 545 and 445, and GD's floor there is 330 -- which comes from
    // neither of them but from the dual portal's cy=509 with H=300.
    float bandRefY = 0.f;
    // Per-state x speed. The layer's shared accumulator is still what the
    // near-object window and the goal test are built from, but the SWITCH has
    // to be decided on the state's own x: stair snaps put the player up to a
    // pixel ahead of the shared timeline, and on lv18 that is exactly enough to
    // move the portal at x=3,381 one tick late (GD switches at t=2574 with the
    // player at 3341.47, the shared accumulator was still at 3340.2). One tick
    // of 0.9 vs 1.1 is 0.316 px and it never closes again.
    float dx;
    // Which touch-trigger boxes this state has flown through, and the tick of
    // the last one (see TouchTrig). Carried per state because the whole point is
    // that two states at the same tick can disagree about whether a door is
    // open. It is NOT in keyOf -- the layer is partitioned by it instead, the
    // same way `dx` is, so states with different masks can never merge.
    uint32_t trig = 0;
    int32_t trigT = -1;
    // Which of the level's gravity portals this state has already SPENT. GD
    // latches one on first overlap, not on first firing, so a pass taken at
    // the polarity the portal would set spends it even though flipGravity does
    // nothing (measured, lv22 uid 13833: hasBeenActivated 0 -> 1 at t=6,300 on
    // exactly such a pass, still 1 at t=6,327 where the player comes back
    // flipped and GD refuses). The bit is Obj::gpBit.
    // Carried per state for the same reason as `trig`: two states at one tick
    // can disagree about which portals they have spent. Like `trig` it is NOT
    // in keyOf -- the layer is partitioned by it, so states with different
    // masks never merge.
    // AN ANCHOR DOES NOT SEED THIS YET, and seedcheck says so out loud: lv12
    // 4 ticks `whole=0x1/0x0 seeded=0x0/0x0`, lv22 5 ticks `whole=0x7/0x0`.
    // The --anchor-state key is here (`owns=portal`, keys `portal`/`portal2`,
    // uids) and nothing writes it: the payload producer is the mod's
    // anchorPayload, which is behind Config::touchPayload and off by default.
    // Why that is not the --rotqueue situation, where the same hole forced the
    // whole mechanism to be opt-in. There the unseeded value is a LIE that
    // invents work -- channel 0, nothing consumed, so the anchor re-fires every
    // rotation the run already passed, and quick_regress lost tracking in 12 of
    // lv22's sections. Here the unseeded value is 0 = "nothing spent", which is
    // exactly the behaviour of the build before this field existed: an anchored
    // section can only fail to INHERIT the refusal, never invent one. Measured,
    // not argued -- quick_regress PASS with no level worse, and the whole-run
    // arms move one level (lv22 6,375 -> 11,539 dead) with the other 21
    // bit-identical.
    // So the cost of the hole is that anchored instruments cannot SEE this
    // rule, which is why it was measured on whole runs.
    // PER HALF, like usedOrb/usedPad and unlike trig: the ccl probe printed
    // hasBeenActivated() and hasBeenActivatedByPlayer() going up together, so
    // lv22's single body cannot tell them apart -- but a shared flag would mean
    // the second half of a dual can never fire a gravity portal the first half
    // has crossed, and dual levels plainly do not behave that way. The
    // by-player flag is the one a per-player refusal reads.
    uint32_t portalLatch = 0;
    uint32_t portalLatch2 = 0;
    // ...and the tick each individual box was entered on. `trigT` is the LAST
    // box only, which is what markTouched's own note says is not enough: the
    // switch band's per-box delay needs each punch's own tick, and the
    // level-wide g_touchFireT beside it cannot serve a search where two states
    // punched the same box at different ticks.
    //
    // Only meaningful where the matching bit of `trig` is set -- 0 is "not
    // entered", and nothing reads it without checking the bit first. A tick
    // fits a uint16 with room (the longest level in the corpus ends near
    // 24,000).
    //
    // 32 x uint16 takes State from 248 to 312 bytes (+25.8%). The frontier is
    // capped by count, not by bytes, so this is memory rather than search
    // width -- but it is the largest single addition the struct has taken, and
    // the cost lands in the cold loop rather than in any replay harness.
    uint16_t fireB[32] = {};
    // How far this state has travelled since it punched the locked box
    // (g_lockBox), while that lock is open. The lock makes an object's x the
    // player's own, offset by wherever both were when it fired:
    //
    //     x(t) = base + (moves) + (playerX(min(t, t0+lockTicks)) - playerX(t0))
    //
    // and playerX(t) - playerX(t0) is just the advance summed over the ticks
    // between, so ACCUMULATING it costs one float and removes the need to
    // remember an x at all. Once the window closes the sum stops growing, which
    // is the `min` in the formula.
    //
    // Four platforms and three yellow pads on lv19 ride on this. Their x is
    // therefore a property of the state, and the recording of them is the
    // recorded run's player path -- right for the plan that made it and wrong
    // by the whole difference for any other.
    float lockOff = 0.f;
    // ---- the 2.2 trigger queue (frames.hpp) ----
    // Which queued triggers this state has consumed. One bit per entry of
    // g_rotQ, and the corpus's only level with any has 30 of them. Consumption
    // inside a channel is strictly front-to-back, so the cursor GD keeps per
    // channel (+0x348) does not need its own field: it is the popcount of this
    // mask restricted to that channel. GD never resets that cursor on a channel
    // switch -- `rotateGameplay` writes only the active channel and the reverse
    // dictionary, and `createCheckpoint` saves all three, which is what a
    // persistent value looks like -- so leaving and returning to a channel
    // resumes where it stopped, and the popcount stays honest.
    uint32_t rotSpent = 0;
    // The channel being walked. Not derivable: it is whatever the last 2900
    // with `swarm` set pointed at.
    uint8_t rotChan = 0;
    // Per-channel reverse, one bit per channel. Also not derivable -- lv22's
    // channel 1 is reached both from a 2900 with gnddir=2 (reverse) and from
    // five with gnddir=0 (not), so the value depends on which fired last.
    uint16_t rotRev = 0;
    // ---- dual (GameObjectType 23 splits, 24 merges) ----
    // Measured on lv16 x=10,551: the moment the portal fires, GD creates a
    // second player AT THE SAME POINT with the opposite gravity and the opposite
    // vy, and from then on the two are mirror images while both are airborne
    // (p1 486.401 / p2 524.017, p1 481.735 / p2 528.683 -- the sum is 1010.418
    // every tick, and p2's vy is exactly -p1's). The mirror is NOT permanent,
    // because the two halves collide with different geometry, so the second
    // player is carried as its own set of fields rather than derived.
    // A fresh edge on the same tick as a ship->UFO portal becomes a flap on the
    // NEXT tick (GD buffers it; lv19 t=13,892->13,893). A pending bit with a
    // 1-tick lifetime. The anchor (--start) does not carry it -- a documented
    // hole only when a section head lands on the 1 tick right after the portal.
    uint8_t pFlap = 0;
    // [2026-08-19 D9, REMOVED 2026-09-03] `pBallOff` used to mark the tick on
    // which a ball left its surface through a gravity flip in a rotated frame,
    // so that the next tick could write vy := -1.000. There is no such write in
    // the game: every writer of PlayerObject's m_yVelocity was enumerated (the
    // 42 setYVelocity xrefs included) and not one of them writes a fixed +-1.0.
    // Its positive witness (lv22 t=6,307) belonged to a worldline that has been
    // re-solved away, its negative witness (lv16 t=4,237) is Section A of the
    // ball/portal table, five in-situ arms at the identical configuration have
    // GD writing plain gravity instead, and the rule fired ZERO times across the
    // 22 verified solutions. Do not reintroduce it without a live measurement.
    // [2026-08-22 r102] Marks hitting a black orb (drop ring) WHILE RISING AT
    // vp >= 2.0. Skips the next tick's terminal clamp exactly once (the
    // calibration-rig dropair sweep is in kRingDrop's note). Same 1-tick
    // lifetime as pFlap, not carried by the anchor.
    uint8_t pNoTerm = 0;
    // [2026-08-25] GD's velocity-limit exemption, byte [player+0x952] in
    // 2.2081. Set when a velocity comes from OUTSIDE the mode's own physics:
    // the slope machinery (postCollision +0x18ca, written next to the launch
    // record m_slopeVelocity), and the RED ring/pad ONLY (bumpPlayer clears
    // it for every other pad/orb right after propellPlayer, then re-sets it
    // for type==34 at +0x191; ringJump sets it for type 35 at +0xc7a).
    // Cleared at the head of updateJump's flying branch the first tick the
    // incoming gravity-frame vy is back inside (-6.4, +8.0)/chi
    // (0x38c527..0x38c59e) -- NOTE the fall-side edge is 6.4, not the clamp's
    // 8, so a slope exit at 6.4..8 keeps the flag alive while the fall
    // accelerates past the terminal forever. While set, the terminal clamp is
    // skipped outright (0x38ca9f: cmp [rdi+0x952],0 / jne past the clamp).
    // Measured where it broke: lv22 x=5,391 -- a flipped swing leaves the
    // 1743 staircase at ~8 and GD keeps adding kSwingG per tick to 14.99
    // while the model sat clamped at 8.000 (fixup rows with edvy growing
    // +0.086/tick); injected vy=20 at t=3,830 passed through unclamped while
    // the same injection at t=4,000 clamped to 8.000 (the flag, not gy1, was
    // the difference).
    // [2026-09-06] Carried by SHIP, UFO and SWING -- the three modes whose
    // updateJump branch reads GD's byte (dp/slopes.hpp boostLatchMode has the
    // addresses and the reasoning; the wave is exempt from the clamp
    // unconditionally at 0x38caa8, so the byte cannot change anything there).
    // It was swing-only until then, which is why lv16's flipped ship pinned
    // 6.400 on the tick after a ramp launch where GD decays 6.906 -> 6.390
    // over six ticks first. Outside those three stepOne drops it so a stale
    // bit cannot cross a mode portal. PERSISTENT, unlike the p* one-shots
    // above, and carried by the anchor (--start field 25, read straight off
    // GD's own byte).
    uint8_t boost = 0;
    // [2026-08-21 r93] Marks the tick on which a warp interrupted a ride = THE
    // RAMP'S SLOPE-EXIT LAUNCH VALUE TO EMIT ON THE NEXT TICK (0 = none). Same
    // 1-tick lifetime as pFlap, not carried by the anchor. It holds
    // a value because by the next tick the ride's information (slopeT) is
    // already gone.
    float pExitVy = 0.f;
    // [2026-08-19 night 3] THE UID OF THE RAMP THAT STARTED THE CURRENT RIDE.
    // Not rewritten during the ride, so "is the ramp we are on the one we
    // started on, or one we moved onto across a seam" is a uid comparison. The
    // seam hand-over clamp (rect top) HAPPENS ONLY ON THE RAMP MOVED ONTO
    // (12/12 on the calibration rig slopeland4). -1 in the air. The anchor
    // (--start) does not carry it -- a documented hole only when a section head
    // lands mid-ride (same as pFlap).
    int32_t slopeUid0 = -1;
    // [2026-08-20] The uid of THE RAMP CURRENTLY RIDDEN (slopeUid0 is the ramp
    // the ride started on, so it cannot tell the 2nd and later pieces of a
    // chain apart). Used as the gate "while held up by a flat actual object,
    // DO NOT TRANSFER ONTO A NEW RAMP". -1 in the air. Not carried by the
    // anchor (same hole as slopeUid0).
    int32_t slopeUidNow = -1;
    // Both take the same input and either one dying ends the run.
    uint8_t dual;
    // the second body had nothing within reach this tick (see stepBoth)
    uint8_t freeHalf = 0;
    float y2, vy2, slopeM2, snapDist2;
    uint8_t grounded2, flip2, ringHold2, onSlope2;
    uint8_t pressSpent2 = 0;   // second body's pressSpent (see pressSpent)
    uint8_t slopeT2 = 0;   // second body's ride counter (see slopeT)
    uint8_t rideLanded2 = 0;  // second body's landing record (see rideLanded)
    // The second body's own velocity-limit exemption (see boost). It became a
    // per-half quantity on 2026-09-06, when the flag stopped being swing-only:
    // the corpus' one witness is a DUAL ship (lv16 t=8,913, where both halves
    // leave a mirrored ramp at 6.906 and both carry the latch for six ticks),
    // and a shared byte would have handed p1's answer to p2 and then thrown
    // p2's away in the merge -- the shape [[gd-per-half-state-field-has-three-
    // sites]] records. All three sites are wired: here, swapHalves, and the
    // merge list in fixup.hpp.
    // NOT carried by the anchor: --start has one boost field (25) and it seeds
    // p1's. Same documented hole as slopeUid02 / pressSpent2.
    uint8_t boost2 = 0;
    // The second body's own MODE and its own ceiling-ramp push-down counters.
    // `mode` and `ceilT`/`ceilM4` used to be shared outright ("shared fields
    // come from the first half", stepBoth), which is right only for as long as
    // the two bodies meet everything on the same tick -- and they do not. Each
    // is tested at its own y, so one can clear a portal's window, or be pressed
    // by a ramp, while the other is not: measured on lv20 t=17,110, p1 clears
    // the mode portal uid13881 (cy=239 hh=43) by 0.16 px while p2 is 2.2 px
    // outside, and p1 has been pressed down the ceiling chain for 14 ticks while
    // p2 has just arrived from below.
    // Everything inside stepOne reads these off the state it was handed, so
    // swapHalves carrying them is the whole of the mechanism -- and they cost
    // nothing: sizeof(State) is 232 either way, they land in existing padding.
    uint8_t mode2 = 0, ceilT2 = 0, ceilM42 = 0;
    // ...and its own SIZE, for exactly the same reason. `mini` stayed on the
    // shared list above long after `mode` left it, and that is wrong in both
    // directions: a size portal one half clears and the other misses resized
    // NEITHER body (the swapped step's result was dropped by stepBoth's merge)
    // when only the second took it, and BOTH when only the first did.
    // Measured on the calibration rig `dualmode` (py/mklevel.py; an input-free
    // dual ship separates by itself, so a portal at floor height is taken by one
    // half and never reached by the other): the halves differ in size for 4,676
    // of its 6,964 dual ticks, 67%. The official corpus never does -- lv16's
    // whole cold run is 207,761 dual ticks with p2vsize == vsize throughout --
    // so this costs it nothing and is not measurable on it either.
    uint8_t mini2 = 0;
    // The second body's own slopeUid0 / slopeUidNow (see those two). Same reason as mode2 and
    // mini2: each half rides its own ramp, and the pair's ramps are mirror images that never
    // share a uid.
    int32_t slopeUid02 = -1, slopeUidNow2 = -1;
    const Obj* snapObj2;
    const Obj* usedOrb2;
    const Obj* usedPad2[4];
    // Pads: one fire per contact, PER OBJECT -- a LIST, like GD's own touched
    // set, not a single slot. lv13 stacks three pads at x=1395 (y=92/150/152)
    // and a single slot let two of them re-fire each other alternately: pad A
    // fires and becomes the slot, pad B fires next and evicts A, A is then
    // "unused" again, and the cube ratchets ~30 px higher than GD every time it
    // crosses the stack. Four slots is more than any stack in the game needs;
    // entries are dropped as soon as the player leaves that pad's box.
    const Obj* usedPad[4];
    uint8_t mini;        // size portal 18 = mini (half 9), 17 = back to normal
    // ROBOT: hover ticks still available on this jump (see kRobotHoverTicks).
    // Set to the full budget when the jump fires, spent one per tick while the
    // button stays held, and dropped to 0 the moment it is released -- GD's
    // release flag is only cleared by the next jump, so the budget cannot be
    // picked back up in mid-air.
    uint8_t rHover = 0;
    // DASH ring (37) / gravity dash ring (38): while the button stays held the
    // player travels on a straight line at the ring's own angle and gravity is
    // switched off entirely. Measured on lv21's ring at (2445,229): the press
    // is at t=210, y freezes at 225.7924 with vy=0 from t=212, and it stays
    // frozen for 70 ticks until the release at t=280 -- gravity resumes on
    // t=282 with the usual -0.216 step. So the dash is not timed, it is held.
    uint8_t dashing = 0;
    float dashSlope = 0.f;   // dy per px of x (0 for a rot=0 ring)
    // GAMEPLAY ROTATION (id 2900): which frame this state is playing in.
    // `xAbs` and `y` are always the CURRENT frame's coordinates, so every rule
    // below stays written in "x is the clock, y is height" -- what changes is
    // the geometry handed to it (see turnObj / Level::turned). 0 everywhere in
    // lv1-21, which have no rotation objects at all.
    uint8_t frame = 0;
    // [2026-08-21 r52] DID THE ROTATION FRAME CHANGE ON THE PREVIOUS TICK?
    // Entering or leaving a frame changes GD's `upsideDown` (0->1 at lv22
    // t=6,322). That change is not a portal's doing, yet the model's
    // "no-change gate" only asks "is the current up different from the wanted
    // up", so a gravity portal the player is still sitting inside CAME BACK TO
    // "CORRECT" IT on the next tick and halved vy. The generalisation ("a
    // gravity portal fires only on the tick it is entered") is rejected:
    // lv5/lv14/lv18/lv20/lv21 regress (GD really does re-fire in some places).
    // One bit to close just this.
    uint8_t frameChg = 0;
    // [2026-08-21 r66] Consecutive ticks spent pushed by a ceiling ramp, and its
    // |m|x4. On the tick after release GD sets
    // vy = -slopeExitVy x slopeRampFactor(ceilT) (calibration rig ceilrel x 3
    // speeds; k = number of pushed ticks matches exactly in 13/14 units). It
    // changes the future release vy, so it goes into the dedupe key (keyOf).
    uint8_t ceilT = 0;
    uint8_t ceilM4 = 0;
    // The player's SPRITE rotation, degrees, cocos convention (clockwise +).
    // Carried because GD tests a turned hazard against the player's ORIENTED
    // box, and that box is the player's rect turned by exactly this
    // (measured: 6x6 mini / 10x10 full, centre on the player, cfg
    // hitboxtrace=1 `pobb` lines). Without it the model has to fall back on the
    // bound, which over-kills -- see the wave's hazard branch.
    // The WAVE and the CUBE maintain it; every other mode leaves it at 0 and
    // keeps the old bound-only test, which is the conservative direction.
    float rot = 0.f;
    // Sign of the cube's spin. GD keeps it in m_rotationSpeed and NEGATES IT ON
    // EVERY GRAVITY FLIP -- measured on lv22's seeded clear: the only two sign
    // changes in 4,000 ticks are at t=732 (flip 1->0) and t=1,308 (flip 0->1),
    // both mid-air, and no flip ever passes without one. A take-off always sets
    // it positive (rspd goes 0 -> +415.3846 at t=261/437/547/617).
    uint8_t rotNeg = 0;
    // NO `rotRate` FIELD YET, ON PURPOSE. GD holds the spin's magnitude in
    // m_rotationSpeed (+0x720) as a STAKE -- an event writes it and
    // updateRotation spends it every tick until another event overwrites it --
    // and the ball's air rate is written by only two callers, flipGravity and
    // ringJump. So a ball that leaves the ground any other way (off a step, off
    // a pad) keeps rolling at the GROUND rate in mid-air, which is 20% of the
    // corpus's airborne ball ticks (5,019 of 24,551 at a rate ratio of exactly
    // 1.000) and which no expression in `grounded` can reproduce.
    //
    // Carrying that needs a field here, and a field here needs an anchor seed
    // and a serial cold. The GROUNDED half needs neither: while the ball is on
    // the floor the rate is a pure function of size and speed, so it is
    // recomputed in place (see the ball branch in step.hpp). That is the half
    // that went in first -- one variable, measurable today, and no State growth
    // until the air half actually requires it.
    // Pinned against a solid's UNDERSIDE (see "cube/ceilstop"). Its own bit on
    // purpose: `grounded` is true for a player merely standing on the floor too,
    // and using that as the "still held" test let any block whose underside
    // passed within a pixel of the player's head grab it -- 13 of the 21 levels
    // got worse. Declared HERE, after the six members the aggregate
    // initialisers fill positionally (`State init{...}`), so nothing shifts.
    uint8_t ceilPin = 0;
    // Armed by an id-2866 box (see FlipHeadBox): the cube's HEAD hitting a solid
    // flips gravity instead of stopping. ~~Sticky once set.~~ Same placement rule
    // as ceilPin -- after the positionally-initialised members.
    //
    // [2026-09-07] **"STICKY ONCE SET" IS REFUTED BY GD.** Marked wrong here the
    // day it was refuted rather than the day a replacement is found, so nobody
    // reads the live citation at step.hpp's arming site under a dead claim.
    //
    // gdref is GD replaying this level's own verified plan, so it ARMS BY
    // CONSTRUCTION at t~2,707 and carries its own positive control:
    //   t= 2,749  up 0->1, vy 10.816 -> exactly 0, og 0->1, y 320.771->321.008
    //             -- GD flips.  (the arm is live)
    //   t=11,342  up stays 0, og stays 0, vy runs 8.296 -> 5.920 at exactly
    //             -0.216/tick -- GD SAILS STRAIGHT THROUGH.  (the arm is not)
    // So the arm does not survive to 11,342, and the model's acquireFlip fires
    // there where GD does nothing. That is lv22's whole-run killer.
    //
    // NOT one-shot either: scanning lv22 for t=2,749's own signature (|vy| large,
    // then vy exactly 0, og 0->1, up flips, y unchanged -- which no pad, orb or
    // portal produces) gives firings at t=2,749, t=2,890 and t=2,961 -- ALL
    // THREE mini cube (vsize 0.600), and none has any gravity portal / pad /
    // orb / rotated frame in contact. ~~So it decays rather than being consumed.~~
    //
    // ============================================================
    // [2026-09-07, MEASURED IN GD] IT DOES NOT DECAY EITHER, AND
    // THERE IS NO DECAY CONSTANT TO FIND. THE 2866 IS A MOVER AND
    // IT RIDES THE PLAYER. Everything from here to the end of the
    // bracket discussion below is superseded; it is kept because
    // the reasoning is instructive about HOW it went wrong.
    //
    // `hitboxtrace=1` on a gdref replay, reading +0xb80 after every
    // PlayerObject::update (the `mod:` lines, render_trace.hpp):
    //     t=2,706  flipGrav=-2706      (counting down, never set)
    //     t=2,707  flipGrav=1          <- arms
    //     t=2,749  flipGrav=1  FLIP
    //     t=2,890  flipGrav=1  FLIP
    //     t=2,961  flipGrav=1  FLIP
    //     t=3,388  flipGrav=1
    //     t=3,389  flipGrav=0          <- releases
    // PINNED AT 1 FOR 682 TICKS / 983 px, and this is the ONLY armed
    // interval in the whole level (26 edge lines, all levels of the
    // run). A counter reading 1 AFTER update was set to 2 during
    // that tick, so it is RE-SET EVERY TICK -- and the same lines
    // show `dart` and `force` running to -3,389 unbounded, so the
    // decrement is unconditional and has no floor. `hitHead` pins at
    // 1 from 2,432 then releases at 2,843 and falls away, which is
    // the internal control that a pinned counter does drop when its
    // source stops.
    //
    // WHY CONTACT NEVER ENDS: uid 2860 MOVES. groups.live.txt gives
    // it 649 distinct positions and it tracks the player at a
    // constant gap of -1.186 px in x, following in y as well
    // (cy 282 -> 326.8 -> 299.6 -> 327.0 -> 171.0 while the player
    // rises and falls). It starts moving at t=2,722, fifteen ticks
    // after first contact. The player never leaves it.
    //
    // So the binary was right all along -- set 2, decrement
    // unconditionally -- and BOTH readings were confirmed directly.
    // The missing piece was never in the decrement.
    //
    // AND THE ARITHMETIC THAT SAID OTHERWISE WAS RUN ON A FICTION.
    // objrects holds uid 2860 at its ENTRY position (3735, 255)
    // forever, so the AABB window "t=2,707..2,733" computed from
    // that table, and every distance derived from it, describes an
    // object that is not there ([[gd-locked-object-position-lies]]).
    // The static box is fiction from t=2,722 on.
    //
    // COROLLARY, and it explains a coincidence rather than leaving
    // it: m_stateNoAutoJump (+0xb74, id 1813) arms and releases on
    // EXACTLY the same ticks. uid 2861 is an id-1813 in the same
    // rigid group -- dx 0.0, dy 0.0 across all 649 shared ticks,
    // one offset value each. One moving pair arms both counters.
    // ============================================================
    //
    // [CORRECTED] That scan also returned t=4,592 and THAT ONE IS NOT A HEAD
    // BONK -- it is a SWING, and a swing flips gravity on a tap, which when it
    // lands produces the identical shape (model trace has act=1/held=1 at 4,591,
    // and the swing taps again at 4,589 flipping the other way). A signature
    // identifies a shape, not a code path. Dropping it moves the ACTIVE end of
    // the bracket from 1,885 ticks to 183.
    //
    // [SUPERSEDED -- there is no decay constant; see the measured block above.
    // The "bracket" below is the gap between flips inside ONE continuous
    // contact, which is why it grew every time another firing was found. It was
    // never a lifetime. Kept for the reasoning, not for the numbers.]
    // WHAT IS NOT KNOWN is the decay constant. Active for at least 254 ticks /
    // 451 px past the box (t=2,961, x=4,204.2) -- that end rests on observed
    // firings.
    //   [CORRECTED again] It said 183 (t=2,890) because the scan that found the
    //   firings required |vy| > 3.0 and t=2,961 enters at 2.936 -- an arbitrary
    //   threshold of mine cutting a real case by 0.064, on a scan whose output
    //   the bracket was then built on. t=2,961 has the full signature: up 0->1,
    //   vy 2.936 -> exactly 0, og 0->1, y +0.039. Found by the ceiling-contact
    //   census below, which was looking for something else.
    //   IT PASSES THE SAME TAP TEST that excluded 4,592, and that test is what
    //   licenses the number: 4,592 is a SWING at full size, where a tap
    //   reproduces the shape; t=2,961 is MINI CUBE, vsize 0.600, identical to
    //   both earlier survivors, and a mini cube has no tap-flip mechanic. The
    //   hardness is the MODE, not the signature -- so 2,961 inherits the
    //   protection rather than needing a new argument. (Checked only after the
    //   bracket was already committed, which is the wrong order: the exclusion
    //   rule has to run BEFORE the number moves, or it is being applied in one
    //   direction only.) Inactive by 8,635 ticks / 12,360 px (t=11,342) -- that end rests
    // on an ABSENCE, which is only evidence if a crossing of the right shape
    // occurred in between and GD declined it; an opportunity census over
    // 4,592..11,342 found three qualifying crossings, two of them TELEPORTS
    // (9,866 / 10,089, dy +-57.522) and so confounded, leaving 11,342 itself as
    // the only clean declined one. Do not write a constant from this bracket.
    //
    // THE BINARY SAYS THE COUNTER IS SET TO 2 AND DECREMENTED EVERY TICK:
    //   0x215ba1  mov dword ptr [r14 + 0xb80], 2   collisionCheckObjects, on
    //             `cmp ecx, 0xb32` (= 2866). The only setter in the whole .text.
    //   0x389f40  dec dword ptr [r15 + 0xb80]      PlayerObject::update. The only
    //             other writer. update's 915-instruction prefix has exactly ONE
    //             exit that skips it, `cmp [r15+0x9c0],0 / jne` = m_isDead, so it
    //             runs on every tick the player is alive. (First order only: a
    //             backward jump that then leads past the decrement would not have
    //             been caught.)
    // So 2 ticks -- and GD flips 42 ticks after arming. THE CONTRADICTION IS ON
    // THE SET SIDE and is unresolved. Both axes of the raw box say the overlap
    // ends at t=2,734: entry is exact in X (3717 - 9 = 3708 against GD's
    // 3,708.886) and the exit is bound by Y (the player rises out at 273 + 9).
    //
    // AND THE MATCH EXTENT IS NOW READ RATHER THAN INFERRED: there is no
    // inflation. collisionCheckObjects' per-object loop takes the PLAYER's plain
    // rect once (0x2149b8, vtable +0x490) into xmm7/8/9/10, takes each object's
    // plain rect (0x214ad0, the same +0x490 -- only GameObjectType 0x19, the
    // slope, goes to the two-argument +0x488 form), and skips the object on a
    // bare AABB reject: obj.minX > player.maxX, player.minX > obj.maxX,
    // obj.minY > player.maxY, player.minY > obj.maxY. No margin either side.
    // 2866 is GameObjectType 40, so it takes the plain box.
    //
    // So four links are read from the binary and verified -- match, set,
    // decrement, and two readers -- and it still does not add up.
    //
    // THE CONTRADICTION HAS TWO ESCAPES, NOT ONE, AND THEY ARE ALTERNATIVES:
    // closing either forces the other.
    //   (a) THE MULTI-HOP CONTROL FLOW. A backward jump in update's prefix that
    //       then leads past 0x389f40. The forward-jump-and-ret scan cannot see
    //       it; a real CFG would close it.
    //   (b) THE ATTRIBUTION. That the flip at t=2,749 is didHitHead's AT ALL is
    //       the FIFTH link and it is NOT read -- it is inferred from the
    //       signature, and a signature cannot identify a path here because the
    //       +-2 is consumed by the seat inside its own tick. 2,749 and 2,890
    //       survive on "a mini cube has no tap-flip mechanic", which is an
    //       argument about what ELSE could have done it, not evidence that this
    //       did. It is the same reasoning that correctly removed t=4,592.
    // A CLEAN CFG IS THEREFORE NOT A DEAD END: it would prove the attribution
    // wrong, and the arm expired on schedule exactly as the binary says.
    //
    // AND THERE IS A PROBE FOR (b) THAT DOES NOT NEED A FLIP. The second
    // consumer's boolean is read twice, and both reads gate COLLISION
    // RESOLUTION rather than the flip:
    //   0x392474  cmp [rsp+0x35], dil / je   -> else a path keyed on
    //             m_isUpsideDown (+0x9bf) choosing between xmm13 / xmm14
    //   0x3928ed  cmp [rsp+0x35], dil / jne  -> a different continuation
    // So a LIVE arm resolves a head contact differently from a DEAD one, and
    // that difference is in the POSITION stream -- it is not consumed inside the
    // tick the way the +-2 is. Any ordinary ceiling contact between 2,890 and
    // 11,342 could therefore date the arm's expiry without a flip ever
    // occurring. Nobody has looked for one.
    //
    // ...AND IT IS THE CEILING RESOLUTION-vs-KILL GATE, which makes the probe
    // far stronger than "a position difference". Reading both sides:
    //   armed   (fall-through 0x39247f) picks (xmm13,xmm14) or (xmm9,xmm10) on
    //           m_isUpsideDown, compares against xmm15, and resolves.
    //   unarmed (je 0x39289b)           takes the mode-flag path
    //                                   (0x9bf / 9b9 / 9ba / 9bc / 9c4).
    // That is the branch modifiers.hpp's CEILING ARM note already describes --
    // "puts the classic ground modes into the ceiling RESOLUTION arm instead of
    // the KILL" -- and its inputs are m_stateHitHead(0xb7c) > 0 OR platformer
    // OR m_stateFlipGravity(0xb80) > 0. So a live 2866 arm is the difference
    // between GD resolving a ceiling contact and KILLING THE PLAYER, which is
    // the most observable outcome there is.
    //
    // WHICH IS A SECOND, SEPARATE MODEL DEFECT: modifiers.hpp says the
    // discriminant for the resolution arm "is this object", meaning 1859 alone.
    // GD's gate is 1859 OR 2866 OR platformer. Between t=2,890 and t=11,342
    // lv22's only 1859s are at x ~ 11,341..11,458, so most of that window has
    // no 1859 at all -- which is exactly where the probe can run cleanly.
    //
    // TWO CAUTIONS FOR WHOEVER RUNS IT:
    //   * DO NOT REUSE THE OPPORTUNITY CENSUS'S POPULATION. That looked for
    //     crossings of the shape that would fire acquireFlip, a NARROWER
    //     predicate than "contacts where the armed boolean changes resolution".
    //     Its only survivor in this window was 11,342 itself, so reusing it
    //     would make the probe answer with the disputed tick. The population is
    //     the whole question; derive it from what the two paths above actually
    //     produce.
    //   * ORDER: read what the two paths produce, let that define the
    //     population, THEN census. Not the other way round.
    //
    // AND THE PROBE READS SURVIVALS, NOT DEATHS, WHICH MAKES IT SIMPLER.
    // gdref is a SOLUTION -- GD never dies in it -- so there is nothing to look
    // for on the kill side. That is the strength: at every qualifying ceiling
    // contact in the window GD SURVIVED, therefore the gate was satisfied,
    // therefore something armed it. platformer is out (classic level) and the
    // only 1859s lie at x ~ 11,341..11,458, so across most of 2,890..11,342
    // THE 2866 ARM IS THE SOLE POSSIBLE INPUT and a survival proves it was live
    // at that tick. The tick of the LATEST such contact is a lower bound on the
    // arm's life; any of them far past 2,736 refutes the binary's two ticks
    // through a consumer that never involves a flip.
    // That also removes the last dependence on the disputed attribution: the
    // 2,749 flip does not enter, and neither does didHitHead.
    //
    // [2026-09-07 RUN, AND IT DOES NOT ACHIEVE THAT] The census was run with the
    // population pre-registered before filtering (a positive = a tick in
    // (2890,11342), classic ground mode, player box overlapping a type-0 solid
    // on the binary's own bare AABB, head leading, head crossing the near face,
    // x outside the 1859 span 11,341..11,458). Canary passed: the detector finds
    // t=2,749 and t=2,890 when the window is widened to include them.
    // THREE CONTACTS, AND NONE IS FLIP-FREE:
    //   t= 2,961  uid 3101   IS A FLIP (up 0->1, vy 2.936 -> 0, og 0->1)
    //   t= 9,866  uid 6183   the spider-orb teleport pair -- confounded
    //   t=10,089  uid 6194   the same pair
    // So the window offers no un-confounded ceiling contact and the flip-free
    // probe cannot date the arm FROM THIS LEVEL. The design is sound and the
    // corpus does not supply it -- a statement about lv22, not about the arm.
    // The teleport pair confounds this the same way it confounded the
    // opportunity census: a teleport in the same tick means a survival cannot be
    // read as the gate being satisfied.
    //
    // AND THE ARM HAS A SECOND CONSUMER, which the model does not model: besides
    // didHitHead's flip, PlayerObject::collidedWithObjectInternal tests it at
    // +0x3db (0x391e4b, `cmp [r14+0xb80], r13d`, > 0) to compute a local boolean,
    // four instructions after m_stateHitHead and the platformer flag. GD uses the
    // arm as a COLLISION MODIFIER with two consumers; the model treats it as a
    // flip enable.
    //
    // HOW THE ORIGINAL CLAIM GOT IN: the measurement cited at the arming site
    // (step.hpp, lv22 t=2,707, pre-move x 3,708.886 against the box's left edge
    // 3,717) established WHICH TICK THE COUNTER GOES POSITIVE. It says nothing
    // about whether it returns to zero. An onset measurement was read as a
    // persistence measurement -- and the sibling arm four lines below it (id
    // 1859) had its decay measured explicitly ("set to 2 by the touch and
    // stepped down every tick"), so the assumption was never tested rather than
    // tested and confirmed.
    uint8_t fgArm = 0;
    uint32_t parent;  // node arena index
    uint8_t action;   // input level THIS tick (for plan reconstruction)
    // Reverse (a same-frame id 2900). A SEPARATE AXIS from frame: it reverses
    // only the direction of travel (gravity / up-down stay as they are). travel
    // is the product frame x rev. All 0 in lv1-21.
    // KEEP IT AT THE END: this struct is filled by positional initialisers in
    // places (the 3811 note above), and adding a member mid-way shifts every
    // later member by one. It was actually put right after frame once, and a 1
    // passed via --start came out as 0 when the replay started (start:
    // init.rev=1 / revdbg init.rev=0).
    uint8_t rev = 0;
    // ROUTE TIGHTNESS (dp/clearance.hpp).
    //
    // How many ticks this lineage has spent with less vertical room than the
    // model's own error. It is carried, not recomputed: a route property has no
    // meaning at a single state, and the thing it exists to rank -- the states
    // that reach goalX -- all sit in open sky past the last object, where their
    // own clearance is identical and says nothing (measured: spread 0.00).
    //
    // NOT in keyOf, deliberately. It is a property of how a state was REACHED,
    // not of the state, and two states that differ only in it answer every
    // future input identically. Keying on it would split every cell by history
    // and multiply the frontier for nothing.
    uint16_t tight = 0;
    // GD's m_jumpBuffered (PlayerObject+0x985), mirrored. Set by pushButton,
    // cleared by releaseButton, by the ball's tap and by a ring that consumes the
    // press -- and NOT set at all while controls are off, because pushButton
    // returns immediately there. `action` cannot stand in for it: the caller
    // writes `action` from the RAW plan value, deliberately (a hand held across a
    // no-control window continues from the tick the window lifts), so a rule that
    // reads `action` fires inside the window where GD has no press at all -- which
    // is what made the first cube re-jump rule look refuted on lv22 t=20,236.
    // **At the END of the struct on purpose**: cli.hpp:528 builds an anchor with a
    // POSITIONAL `State{...}`, so a field inserted anywhere earlier shifts every
    // member after it and every anchored replay dies on its first tick (measured,
    // 2026-09-03: all 22 levels went "400 -> 1 ticks" until this moved down here).
    uint8_t jumpBuf = 0;
    // THE SPIN'S SIZE, in DEGREES PER TICK -- not a rate. GD holds the rate in
    // m_rotationSpeed (+0x720) as degrees per second and the tail multiplies by
    // (dt/60) = 1/240; this holds the value AFTER that division, because that is
    // what gets added to `rot`. Seeding it is therefore `rot[t] - rot[t-1]`
    // straight from the reference, with no factor of 240. Naming it `rotRate`
    // was rejected for exactly that reason: a reader who assumes deg/sec puts
    // the 240 into both the seed and the application, where the two errors
    // cancel and nothing measurable ever disagrees.
    //
    // A STAKE, which is why it has to be carried. An event writes it and every
    // later tick spends it. The ball's AIR value is written by only two callers
    // in GD, flipGravity and ringJump, so a ball that leaves the ground any
    // other way -- off a step, off a pad -- keeps turning at the GROUND rate in
    // mid-air. That is 20% of the corpus's airborne ball ticks (5,019 of
    // 24,551, measured at a rate ratio of exactly 1.000) and no expression in
    // `grounded` can produce it. The grounded value alone needs no field, which
    // is why a0c4c8f shipped without one.
    //
    // `rotLaw` and the staked `rev`/`rotgp` were designed alongside this and
    // dropped: the law is already baked into the size, and the sign those two
    // inputs decide is baked into `rotNeg` at stake time. Neither had a reader.
    //
    // ONE READER: the ball branch's own application. The contact tests that
    // advanced the player one step (pRotE / pRotHere / pRotPad / pRotSp) still
    // hard-code `mini ? 2.25 : 1.7307692` and MUST keep doing so -- those run in
    // CUBE mode, and every writer of this field is on a ball path, so pointing
    // them here would hand the cube a ball's step or a stale zero. They can be
    // folded in on the day the cube gets a stake of its own; that is a change to
    // the cube's behaviour and belongs in its own landing, not this one.
    // [2026-09-06] Those four now advance only under `--no-satrotraw`: the
    // advance was measured to be a sign inversion against the cube's own
    // rotation law and is deleted by default (constants.hpp, g_noSatRotRaw).
    // Nothing above changes -- the arm that keeps the expression keeps the
    // hard-coded step, for the same reason.
    //
    // **At the END of the struct on purpose** -- same reason as jumpBuf above.
    float rotStep = 0.f;
};

// THIS ASSERT IS A QUESTION, NOT A BUDGET. If you added a field and the build
// stopped here, ask whether it ACCUMULATES -- whether its value at tick t
// depends on the ticks before t rather than only on this one.
//
// If it does, three things have to happen before the number below is updated,
// because three separate defects on 2026-09-04 were each exactly this and each
// was found only after it had changed an answer:
//
//   1. SEED IT in the --start anchor scan (cli.hpp). A state handed to --start
//      mid-level starts at the field's default, which for fireB read as "fired
//      at tick 0", for lockOff as "never rode anything", and for the queue's
//      three as "channel 0, nothing consumed" -- and that last one re-fired
//      every rotation the run had already passed.
//   2. PRINT IT in --seeddump (cli.hpp). A field missing from that line is a
//      field the check below cannot see.
//   3. RUN oneoff/py/seedcheck.py, which compares "seeded at t0" against "run
//      from t=0" field by field and must report zero unexpected differences.
//      Run it DENSE: 11 anchors found 5 of lv19's 47.
//
// If it does not accumulate -- a value recomputed every tick from this tick's
// inputs -- none of that applies and the number is all that changes.
// WHAT THIS DOES NOT ENFORCE, so the next reader does not mistake its reach:
// it watches the SIZE OF State and nothing else. Adding a namespace-scope
// GLOBAL passes straight through it, and a global has the same discipline for
// a different reason -- it must be cleared in reset.hpp or it survives into
// the next in-process call, which in a one-session cold run is the next LEVEL.
// That happened the same day this assert was written (8f1ae6b added three
// payload globals and reset none of them), so the two rules are siblings and
// neither mechanism covers the other.
// AND IT IS SILENT ON THE EASIEST ADDITION OF ALL: a one-byte field that lands
// in existing PADDING leaves sizeof unchanged, so this assert never fires.
// `boost2` (e6324c4) is exactly that -- a uint8_t added beside the other
// per-half bytes, size still 344, build green, no question asked. The seeding
// hole it carries (--start has one boost field and it seeds p1's) was caught
// because its author wrote it down, which is discipline and not this
// mechanism. So when the new field is a uint8_t, check by hand what this
// cannot: the three per-half sites if it has a second body (declaration,
// swapHalves, the merge list in fixup.hpp) and which --start field, if any,
// seeds it.
static_assert(sizeof(State) == 344,
              "State changed size. If the new field ACCUMULATES over ticks, "
              "seed it in the --start anchor scan, print it in --seeddump, and "
              "run oneoff/py/seedcheck.py to zero before updating this. (This "
              "says nothing about globals -- those belong in reset.hpp.)");

// arena entry for witness reconstruction, packed: bit31 = action, rest parent
struct Node {
    uint32_t packed;
    uint32_t parent() const { return packed & 0x7fffffffu; }
    uint8_t action() const { return (uint8_t)(packed >> 31); }
};
// "no value" sentinel for the parallel dedupe's u32 slots (file scope: the
// dedupe's structs are local classes inside main, which MSVC will not let
// reference a function-scope enumerator in a member initializer)
enum : uint32_t { kNone = 0xffffffffu };
// --threads N: worker threads for the per-layer expansion (0/1 = serial).
// The DP is the whole runtime of a solve (measured on lv16: 1,147 s of DP
// against 49 s of GD replay over 13 iterations), and inside a layer every state
// is independent -- the only shared thing is the dedupe map, and that stays on
// the main thread (see the two phases in the search loop). Default is
// deliberately small: the regression runs six levels at once, and six solves
// each grabbing every core is slower than six solves taking a quarter of one.
inline int g_threads = 4;

}  // namespace dp
