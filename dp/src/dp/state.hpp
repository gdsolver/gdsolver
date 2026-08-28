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
    float slopeM;
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
    // [2026-08-19 D9] Marks the tick on which, in a rotated frame, the ball left
    // its surface through a gravity flip. On the next tick GD alone writes
    // vy := -1.000 (measured where pBallOff is applied). Same 1-tick lifetime
    // and not-carried-by-the-anchor hole as pFlap.
    uint8_t pBallOff = 0;
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
    // Implemented for the SWING only, where it was measured; outside mode 7
    // stepOne drops it so a stale bit cannot cross a mode portal. PERSISTENT,
    // unlike the p* one-shots above, and carried by the anchor
    // (--start field 25, read straight off GD's own byte).
    uint8_t boost = 0;
    // [2026-08-21 r93] Marks the tick on which a warp interrupted a ride = THE
    // RAMP'S SLOPE-EXIT LAUNCH VALUE TO EMIT ON THE NEXT TICK (0 = none). Same
    // 1-tick lifetime as pFlap / pBallOff, not carried by the anchor. It holds
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
    uint8_t slopeT2 = 0;   // second body's ride counter (see slopeT)
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
    // Pinned against a solid's UNDERSIDE (see "cube/ceilstop"). Its own bit on
    // purpose: `grounded` is true for a player merely standing on the floor too,
    // and using that as the "still held" test let any block whose underside
    // passed within a pixel of the player's head grab it -- 13 of the 21 levels
    // got worse. Declared HERE, after the six members the aggregate
    // initialisers fill positionally (`State init{...}`), so nothing shifts.
    uint8_t ceilPin = 0;
    // Armed by an id-2866 box (see FlipHeadBox): the cube's HEAD hitting a solid
    // flips gravity instead of stopping. Sticky once set. Same placement rule as
    // ceilPin -- after the positionally-initialised members.
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
};

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
