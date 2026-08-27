#pragma once
#include "dp/frames.hpp"

namespace dp {

// `rot` is GD's own getRotation() for that tick (grouptrace's 8th column, added
// 2026-08-13). hw/hh are HALF the AXIS-ALIGNED BOUND, so for a turned object
// they are not the shape -- see Obj::oriented and the note on the portal test.
// Recordings made before the column existed load with rot = 0, i.e. exactly the
// old behaviour.
struct DynSample { int t; float cx, cy, hw, hh; uint8_t on; float rot = 0.f; };
// [2026-08-22 r104] Interpolate linearly between the recording's rows
// (--no-dyninterp turns it off). Default ON: restores smooth motion to moving
// geometry that grouptrace's 0.05px threshold turned into a staircase.
inline bool g_dynInterp = true;
// (defined up here, before Dynamics, because applyTriggers reads g_autoTrig;
// the touch-trigger story lives with TouchTrig below)
struct TrigCtl {          // one object a trigger ends up moving
    int uid;
    float dx, dy;         // total offset once the move has finished
    // How long the move takes, from the firing tick. NOT rounded: GD's duration
    // is in seconds and the physics step is 1/240 s, so the tick count is
    // dur*240 with its fraction. Rounding it cost 4.5 px on lv20's 240 px doors
    // (uid 17399, dur 0.435002 -> 104.4 ticks, rounded to 104).
    double durTicks;
    int ease;             // GD's EasingType, raw (see gdEase)
    double erate;         // its rate (period, for the elastic family)
    // lockToPlayerX anywhere in the chain, in TICKS (0 = not locked). While it
    // lasts the object's x is the player's x, offset by wherever both were when
    // it fired -- so it has a closed form too, just one that needs the SIM's own
    // player x rather than only the level data:
    //
    //     x(t) = base + (moves) + (playerX(min(t, t0+lockTicks)) - playerX(t0))
    //
    // Measured on lv20 (uid 13276 and its group, 2,400 samples): residual
    // 0.002 px, with t0 = the crossing tick itself. No +1 here -- a lock is a
    // per-tick copy of a position, not an eased move, so it has neither the
    // easing's slow start nor the recorder's threshold lag.
    //
    // This is the one class where a RECORDING is provably wrong for anyone
    // else: it followed the recorded run's player, so replaying it against a
    // plan that reaches the trigger at a different x is off by the whole
    // difference. lv20's structure at x=24,900 is exactly this.
    double lockTicks;
    // lockToPlayerY: counted and reported, never used. No level in the suite
    // has one, and guessing at a y lock would be the same mistake made twice.
    double lockYTicks;
};
// Touch boxes a re-anchor caught with their move ALREADY RUNNING (see the
// `init.trig` block at the --start handling). The "has it moved past half its
// offset" test there can only answer "open" or "shut"; an anchor dropped in the
// middle of the slide is neither, and answering "shut" puts the door back where
// it started for the whole tail.
//
// Measured on lv22 (2026-08-13): the wall that forms the FLOOR of the first
// rotated section (uids 1212/1213/1214/1221, one column of id 1 blocks at
// cx=2115) slides cy 345 -> 225 between t=1,697 and t=1,814. Every anchor the
// driver takes in that window -- t=1,730 is the one the loop settles on -- has
// it 20.4 px into its 120, i.e. 17%, so the bit stayed clear, the tail ran
// against the wall at REST, and the spider's tap in the rotated frame found no
// surface for 700 px and teleported to world x=1,459.5 where GD lands at
// 2,143.5. From the head the same plan is exact, which is what says the
// geometry was never the problem.
//
// For these boxes the recording IS the timeline (the live overlay comes from
// this plan's own replays), so the object is re-dated to its own first-motion
// row instead of to the state's trigT -- shift 0, phase preserved.
inline uint32_t g_recPhase = 0;
// GD's easing, which is cocos2d-x's CCEase* family reached through
// EffectGameObject::m_easingType / m_easingRate. In = t^rate and Out = t^(1/rate)
// are cocos's own convention, not a guess.
//
// Measured against grouptrace on the lv19 and lv20 bootstrap runs (655 objects
// over four families -- None, EaseInOut, ElasticOut, BounceOut): with this
// curve, the exact duration and the fire tick below, the closed form
//     pos(t) = base + offset * gdEase(type, rate, (t - fireT) / durTicks)
// reproduces every recorded sample to within **0.004 px**. See
// py/trigger_curve_fit.py, which is the harness that says so.
//
// The families with no such measurement yet (Exp / Sine / Back, and the In/Out
// halves of Elastic and Bounce) are cocos's formulas placed here unverified;
// they are still far closer than the 0.5-0.5cos(pi*u) stand-in they replace.
inline double gdEase(int kind, double rate, double u) {
    u = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
    const double p = (rate > 0.0) ? rate : 2.0;
    const double kPi = 3.14159265358979;
    auto bounceOut = [](double x) {
        if (x < 1.0 / 2.75) return 7.5625 * x * x;
        if (x < 2.0 / 2.75) { x -= 1.5 / 2.75;   return 7.5625 * x * x + 0.75; }
        if (x < 2.5 / 2.75) { x -= 2.25 / 2.75;  return 7.5625 * x * x + 0.9375; }
        x -= 2.625 / 2.75;
        return 7.5625 * x * x + 0.984375;
    };
    auto elasticOut = [&](double x, double period) {
        if (x <= 0.0 || x >= 1.0) return x;
        return std::pow(2.0, -10.0 * x)
             * std::sin((x - period / 4.0) * kPi * 2.0 / period) + 1.0;
    };
    auto elasticIn = [&](double x, double period) {
        if (x <= 0.0 || x >= 1.0) return x;
        const double y = x - 1.0;
        return -std::pow(2.0, 10.0 * y)
             * std::sin((y - period / 4.0) * kPi * 2.0 / period);
    };
    switch (kind) {
        case 0: return u;                                     // None (linear)
        case 1: {                                             // EaseInOut
            const double t = 2.0 * u;
            return t < 1.0 ? 0.5 * std::pow(t, p)
                           : 1.0 - 0.5 * std::pow(2.0 - t, p);
        }
        case 2: return std::pow(u, p);                        // EaseIn
        case 3: return std::pow(u, 1.0 / p);                  // EaseOut
        case 4: return u < 0.5 ? elasticIn(2.0 * u, p) * 0.5
                               : elasticOut(2.0 * u - 1.0, p) * 0.5 + 0.5;
        case 5: return elasticIn(u, p);
        case 6: return elasticOut(u, p);
        case 7: return u < 0.5 ? (1.0 - bounceOut(1.0 - 2.0 * u)) * 0.5
                               : bounceOut(2.0 * u - 1.0) * 0.5 + 0.5;
        case 8: return 1.0 - bounceOut(1.0 - u);
        case 9: return bounceOut(u);
        case 10: return u < 0.5
                     ? 0.5 * std::pow(2.0, 10.0 * (2.0 * u - 1.0))
                     : 0.5 * (2.0 - std::pow(2.0, -10.0 * (2.0 * u - 1.0)));
        case 11: return u == 0.0 ? 0.0 : std::pow(2.0, 10.0 * (u - 1.0));
        case 12: return 1.0 - std::pow(2.0, -10.0 * u);
        case 13: return -0.5 * (std::cos(kPi * u) - 1.0);
        case 14: return 1.0 - std::cos(u * kPi * 0.5);
        case 15: return std::sin(u * kPi * 0.5);
        case 16: {
            const double o = 1.70158 * 1.525;
            double x = 2.0 * u;
            if (x < 1.0) return 0.5 * (x * x * ((o + 1.0) * x - o));
            x -= 2.0;
            return 0.5 * (x * x * ((o + 1.0) * x + o) + 2.0);
        }
        case 17: { const double o = 1.70158; return u * u * ((o + 1.0) * u - o); }
        case 18: {
            const double o = 1.70158, x = u - 1.0;
            return x * x * ((o + 1.0) * x + o) + 1.0;
        }
        default: return u;
    }
}
// How many ticks after the move really starts does grouptrace FIRST write a row
// for it? The recorder drops changes under 0.05 px (grouptrace.hpp kEps), and an
// eased move spends its first ticks in the thousandths, so the recording's
// apparent first motion is 1-2 ticks late -- 1 for a linear move, 2 for
// EaseInOut. That lag is not an unknown: with the offset, the duration and the
// curve in hand it is computable, which is what this does. Everything
// downstream then works in TRUE first-motion ticks and the artefact cancels.
// See the ZoomTrig block for the measurements. Progress is mapped through x
// because the state cannot carry the crossing tick.
inline double camScaleAt(double x, double dx) {
    double cur = 1.0;
    if (dx <= 0.0) dx = 1.0;
    for (const auto& z : g_zoomTrigs) {
        if (z.cx > x) break;
        if (z.durTicks <= 0.0) { cur = z.target; continue; }
        const double u = (x - z.cx) / (dx * z.durTicks);
        cur = (u >= 1.0) ? z.target
                         : cur + (z.target - cur) * gdEase(z.ease, z.rate, u);
    }
    return cur;
}

inline int recordLag(double dx, double dy, double durTicks, int ease,
                     double erate) {
    // Nothing moves -- a toggle-only group. A boolean flip has no threshold to
    // climb, so its first row IS its first tick and there is no lag to remove.
    if (durTicks <= 0.0 || (std::fabs(dx) < 0.5 && std::fabs(dy) < 0.5))
        return 0;
    for (int n = 1; n <= 240; ++n) {
        const double e = gdEase(ease, erate, (double)n / durTicks);
        if (std::fabs(dx * e) >= 0.05 || std::fabs(dy * e) >= 0.05) return n;
    }
    return 1;
}
// Autonomous triggers: touch=0 / spawn=0 move rows. GD activates one when the
// PLAYER's x crosses the trigger's own x (EffectGameObject::spawnXPosition()
// returns getPosition().x for exactly this class), and the controlled group's
// first motion lands 2 ticks after the crossing -- measured live on lv19
// uid 15242 (crossing x=30,375 -> first motion t = crossing + 2; reaching it
// on SCREEN does not fire it).
//
// Unlike a touch trigger this is not a per-state fact: every worldline of a
// solve crosses a given x at (nearly) the same tick, so the firing tick is a
// LEVEL property. It is resolved once -- from the frontier's leading x during
// the search, from the single trajectory in --replay, or from the recording
// itself for triggers behind a re-anchor -- and stored in fireT. -1 = not
// crossed yet; controlled objects sit at their FIRST sample until then.
//
// Why this exists when grouptrace already records the motion: the recording
// plays on the RECORDED run's timeline. A bootstrap corpse crossed x=30,375
// at t=23,397 where the real run crosses at t=21,690 -- 1,707 ticks of wall
// phase error, and the DP planned around a wall that was not there (a whole
// session went to diagnosing it). Deriving the fire tick from the solve's own
// x removes the dependence on what the recording happened to do; the recording
// only contributes the SHAPE of the motion, re-timed to this run's crossing.
struct AutoTrig {
    int uid = 0;              // reporting only
    double cx = 0;            // player x that fires it
    std::vector<TrigCtl> ctl;
    // Resolved TRUE effect tick: the last tick the object is still at rest is
    // fireT-1, and gdEase((t-fireT)/dur) is its position from fireT on.
    int fireT = -1;
    // The player's x when it fired, for the lockToPlayer term (see TrigCtl).
    // Resolved with fireT; behind a re-anchor only `cx` itself is known, which
    // is the crossing x to within one tick of travel.
    double fireX = 0;
    // Where the player was on the last tick this trigger's lock was still open.
    // Trails px while the window runs so the object can hold that x afterwards.
    double lockLastX = 0;
    // Ticks between the crossing and the move actually starting.
    // MOVE (901) = 1. Measured 2026-08-09 over the lv19 and lv20 bootstrap runs
    // (nodeath=1 grouptrace=1, so the dump and the recording share one
    // timeline): fitting the closed form to all 655 single-owner objects puts
    // the origin at crossing+1 for EVERY one of them, min = med = max, with a
    // worst-case residual of 0.004 px. py/trigger_curve_fit.py reproduces it.
    //
    // It used to say 2, from lv19 uid 15242 (crossing t=21,688 against first
    // RECORDED motion t=21,690). That measurement was right and its reading was
    // wrong: grouptrace does not write sub-0.05 px changes, and an EaseInOut
    // move is under that for its first two ticks (see recordLag). The extra
    // tick was the recorder's threshold, not GD's.
    // TOGGLE (1049) = 0: a boolean has no such threshold, so its measurement
    // (lv20, the three groups whose toggle is the first thing to touch them --
    // uid 4528 x=6,465, uid 10316 x=17,715, the trio at x=22,455 -- all flip on
    // the very tick the player crosses) stands as read. The later toggles in
    // that level show a large negative offset only because their group had
    // already been flipped by an earlier trigger; they are not independent
    // measurements and are not evidence of anything.
    int delay = 1;
};
inline std::vector<AutoTrig> g_autoTrig;
// Travel coordinate of each touch box, by its bit in State::trig (filled by
// loadTouchTriggers). ownTouch's proximity gate reads it: the one measured
// punch pair (box ~3,255..3,300 lifting uid18119 at 3,486) is 230px apart,
// while the pre-band grazes that phantomed the whole switch band sat 500+px
// from the ceiling they were credited with delaying.
inline float g_touchBoxU[32] = {};
// The tick box b was FIRST entered by any state of THIS solve (-1 = not yet).
// A level property like AutoTrig::fireT, and the same approximation ("the
// first group to reach it wins"). Deliberately not carried by the state: the
// state's trigT only remembers its LAST box, and the per-box delay of the
// switch band needs every punch's own tick. Bits the ANCHOR arrived with stay
// -1 -- their effect is already inside the live recording (it replayed this
// very plan's prefix), so crediting them again would double-count the delay.
inline int g_touchFireT[32] = {-1, -1, -1, -1, -1, -1, -1, -1,
                               -1, -1, -1, -1, -1, -1, -1, -1,
                               -1, -1, -1, -1, -1, -1, -1, -1,
                               -1, -1, -1, -1, -1, -1, -1, -1};
// uids some Rotate (1346) trigger turns. A rotation carries its group around a
// centre object, so no offset describes it and the closed form must not claim
// it -- lv21 has 200 objects that look like plain moves until you notice a
// rotate on the same group (their recorded path missed the move-only formula by
// 120 px). Filled by loadAutoTriggers, which walks the same chains.
inline std::unordered_set<int> g_rotated;
// ON by default; --no-trigclosed restores the recording for A/B. For an
// autonomously moved object the FORMULA places it, not the recording -- but
// only where the object passes autoClosed (a per-object check at load).
//
// Why a formula beats a measurement here: a recording ends where its attempt
// died, it is one run's timeline, and it only exists for objects some plan
// actually reached. The formula has none of those limits and, where both exist,
// they agree to 0.002 px. For a lockToPlayer object the recording is not merely
// limited but WRONG for anyone else -- it followed the recorded run's player.
// Objects the formula cannot describe (move-to-target with no offset, anything
// a rotation carries, two locks, a y lock) fail autoClosed and keep theirs.
inline bool g_trigClosed = true;
// --trigraw: autonomous triggers behind the anchor (before it) use **the
// recording's tick as is** when a recording exists (no switching over to the x
// estimate through the 60 tick gate). est's "how many ticks ago, by
// distance/speed" assumes x advanced monotonically, which breaks behind the
// rotation maze: at lv22's t=14,250 anchor, uid10628 (x=15,075, real firing
// t=10,520) was re-dated to est=13,845, group 331's whole timeline slid by
// +3,325 ticks, the ON return of "off@10,520 -> on@14,198" flew into the future
// at t~17.5k, and the floor GD was riding was missing in the model alone. The
// driver takes the anchor from the same dump as the snap (the recording of this
// attempt's replay), so for that pair the recording's time axis = this run's
// time axis and a shift of 0 is correct. Without the flag the behaviour is
// exactly as before.
inline bool g_trigRaw = false;
// --dyndbg <uid>: dump one moving object's placement inputs at load (see the
// print near the fireT resolution).
inline int g_dynDbg = -1;
struct Dynamics {
    // Stable storage: `snapObj` / `usedOrb` / `usedPad` hold raw Obj* across
    // ticks, so these must never reallocate after load. The rects are MUTATED
    // in place once per tick by seek().
    std::vector<Obj> objs;
    std::vector<std::vector<DynSample>> samples;  // parallel to objs
    // [2026-08-25] Does a trigger ROTATE this object anywhere in the recording?
    // GD's collision test branches on the object's own "is turned" flag, and a
    // rotate-target carries it for the whole run -- not only once the angle has
    // grown away from zero. See the note at turnedBox's axis-aligned branch for
    // the measurement (lv22's spider portal uid 1156). Parallel to objs; empty
    // in an old Dynamics, which reads as "never", i.e. the previous behaviour.
    std::vector<uint8_t> everRot;
    std::vector<size_t> cur;                      // per-object cursor
    // which window each one belongs in, mirroring the static split
    enum Bucket : uint8_t { NEAR, PORT, PAD, ORB, SPEED, SLOPE };
    std::vector<uint8_t> bucket;
    // Last tick's FINAL cy (after seek AND applyTriggers). dcy for a
    // trigger-CONTROLLED object must be the delta of its analytic position,
    // not of the raw samples: the recorder SKIPS rows when the move since the
    // last row is under its epsilon, so an eased tail records every 2nd-3rd
    // tick and the raw-sample dcy reads 0 on the skipped ticks -- while the
    // analytic position keeps moving. The ride gates on dcy != 0, so the
    // rider dropped grounded for exactly those ticks (lv22 t=849/851, the
    // hanging spider on the bobbing intro blocks: one +0.129 gravity blip per
    // skipped row).
    std::vector<float> prevCy;
    bool lastStep1 = false;
    // A moving SLOPE carries its surface line with it. sy0/sy1 are absolute
    // heights, so they are re-derived from the load-time values plus however
    // far the object has travelled in y. (Pure translation only -- a ROTATING
    // slope would need its line recomputed, and nothing rides one yet.)
    std::vector<float> baseCy, baseSy0, baseSy1;
    std::vector<uint8_t> on;   // toggled in? (0 = the object is not there)
    // ---- touch-trigger control (see TouchTrig, below) ----
    // Which touch triggers move this object and by how much once they have.
    // Zero for every object in lv1-18, so all of this is dead weight there.
    std::vector<uint32_t> trigMask;
    // The recording's motion is NOT this object's touch chain (wrong direction
    // or far beyond its offset), so it is an autonomous fact of the recorded
    // worldline and plays for every state -- the switch band's ceiling: chain
    // says nudge +10, the recording descends -100. A door whose recording IS
    // its chain (lv19) stays touch-gated. Filled by the loader.
    std::vector<uint8_t> recAuto;
    std::vector<float> trigDx, trigDy;
    std::vector<double> trigDur;
    std::vector<int> trigEase;
    std::vector<double> trigErate;
    // The tick this object FIRST moved in the recording (-1 = it never did).
    // That is the anchor the recorded trajectory is re-timed against, so a
    // frontier group that fired the trigger at a different tick than the run
    // that recorded it still sees the right shape at the right offset.
    // Shared by the touch and the autonomous machinery.
    std::vector<int> trigRecFire;
    // ...and how many ticks late that FIRST ROW is (see recordLag). The true
    // start is trigRecFire - recLag, which is the tick the analytic curve and
    // the re-timing shift are both written against.
    std::vector<int> recLag;
    // Does the closed form reproduce this object's recording? Decided per
    // object at load by replaying the formula against every recorded sample
    // (see the check in loadLevel). Only consulted under --trigclosed.
    std::vector<uint8_t> autoClosed;
    // ---- autonomous-trigger control (see AutoTrig, above) ----
    // Which g_autoTrig entry anchors this object's re-timing (-1 = none).
    // An index, not a mask: the fire tick is a level property so no state
    // carries it, and lv21 has 174 autonomous moves (a 32-bit mask cannot).
    // An object moved by several autonomous triggers is anchored on the
    // EARLIEST-crossed one -- its recFire is that controller's first effect,
    // and the later controllers ride the same shift (their relative timing is
    // embedded in the recording).
    std::vector<int> autoAnchor;
    std::vector<float> autoDx, autoDy;   // summed final offset (ease fallback)
    std::vector<double> autoDur;
    std::vector<int> autoEase;
    std::vector<double> autoErate;
    // Every controller's move kept SEPARATELY, because they superpose: each one
    // starts at its own crossing and runs its own curve, and the object is the
    // sum. Adding the offsets up under one anchor is only right when there is
    // one controller -- lv20 has objects under five, and the sum-under-one-
    // anchor form was off by hundreds of px on those. Measured on the lv20
    // bootstrap: the superposition matches all 384 multi-controller objects to
    // 0.002 px (py/trigger_curve_fit.py's sibling check).
    struct AutoPart { int trig; float dx, dy; double dur; int ease; double erate; };
    std::vector<std::vector<AutoPart>> autoParts;
    // The TOUCH chains kept per box (trig = the box's State::trig bit), where
    // trigDx/trigDy above are their sum. The sum is enough for a door -- one
    // box, one move -- and undecidable for the switch band, where YELLOW boxes
    // (+30 up) and RED boxes (-840 down) drive the same spikes: every per-box
    // question (which way does THIS punch push? is the recorded descent any
    // single box's chain?) has no answer in the summed form. Filled only for
    // touch-controlled objects; empty everywhere lv1-18.
    std::vector<std::vector<AutoPart>> touchParts;
    // lockToPlayerX window in ticks (0 = none) and the g_autoTrig entry that
    // opened it -- NOT necessarily the object's anchor. See TrigCtl::lockTicks.
    std::vector<double> autoLock;
    std::vector<int> autoLockTrig;
    bool anyTrig = false;
    bool anyAuto = false;
    int lastT = -1;

    size_t size() const { return objs.size(); }

    // Place the trigger-controlled objects for a frontier group whose states
    // have touched the triggers in `mask`, `fireHi` being the latest tick any of
    // them fired (-1 = none).
    //
    // Called right after seek(), and it has to be IDEMPOTENT: seek() early-
    // returns when the tick has not changed, so the second group of the same
    // layer would otherwise stack its offset on top of the first group's. It
    // rebuilds each rect from the object's OWN first sample instead of from
    // whatever is currently in objs[i].
    //
    // The first sample and not the current one, deliberately. Once the driver
    // replays a plan that DOES touch the trigger, grouptrace records the door
    // sliding open -- and that recording would then be applied to every state,
    // including the ones that never touched anything. The whole point is that
    // the two worlds differ, so for these objects the recording is ignored and
    // the mask is the only thing that moves them.
    //
    // When the bit IS set the recording is replayed, re-timed so that its first
    // motion lands on this group's firing tick. That is the whole point of not
    // implementing trigger semantics: lv19's third touch trigger is a "move to
    // target" lift, not a simple offset, and m_moveOffset for it is (0,0) --
    // GD's own trace has the ball riding it grounded from x=28,783 to x=29,001
    // across what is otherwise a spike floor. No closed form was going to
    // produce that; the replay already contains it.
    //
    // Only when there is no recording yet (the first iteration, before any plan
    // has touched the box) does it fall back to the static offset, eased. That
    // fallback is no longer an approximation: the curve is GD's own easing by
    // type and rate (gdEase), the duration is the dump's dur*240 unrounded, and
    // the origin is the true first-motion tick -- 0.004 px against 655 recorded
    // objects. It used to be 0.5-0.5*cos(pi*u) over a rounded duration, which
    // was within a few percent of EaseInOut and up to 4.5 px off on lv20's
    // 240 px doors. A union of start and end rect was tried before that and is
    // wrong in BOTH directions -- conservative for a door that opens a gap,
    // optimistic for a platform that rises under your feet.
    // `px` is where the player is THIS tick (the group's leading x during the
    // search, the single trajectory's x in a replay). Only the lockToPlayer
    // term reads it.
    void applyTriggers(uint32_t mask, int fireHi, int t, double px = 0.0) {
        if (!anyTrig && !anyAuto) return;
        for (size_t i = 0; i < objs.size(); ++i) {
            const uint32_t m = trigMask[i];
            const int aA = m ? -1 : autoAnchor[i];   // touch control wins
            if ((!m && aA < 0) || samples[i].empty()) continue;
            const auto& sm = samples[i];
            const DynSample& s0 = sm[0];
            double cx = s0.cx, cy = s0.cy, hw = s0.hw, hh = s0.hh;
            uint8_t onv = s0.on;
            float rotv = s0.rot;   // GD's own getRotation (see turnedBox)
            // Who controls this object this tick, where its first motion is
            // anchored, and the analytic fallback's offset/duration/curve.
            // Touch: fired = this GROUP's mask bit, anchor = the group's entry
            // tick (fireHi), 5 ticks of box->motion latency (measured, lv19
            // door), and the recording is re-timed against the entry tick as it
            // always was. Auto: fired = the level-wide fireT is resolved AND
            // already reached, anchor = fireT, which IS the true first-motion
            // tick -- so the recording is re-timed against its OWN true start,
            // recFire minus the recorder's threshold lag.
            bool fired; int anchor, recAnchor; float fdx, fdy;
            double fdur; int lat, fease; double ferate;
            if (m) {
                fired = (mask & m) != 0;
                anchor = fireHi; recAnchor = trigRecFire[i];
                fdx = trigDx[i]; fdy = trigDy[i];
                fdur = trigDur[i]; lat = 5;
                fease = trigEase[i]; ferate = trigErate[i];
                // ...but `fireHi` is ONE scalar for the whole state (trigT is
                // the tick of the LAST box that state entered), while this
                // object may belong to a different door entirely. Every extra
                // box a branch touches re-dated every door it had already
                // opened. When this object HAS a recording, its own first-motion
                // row is the tick it opens -- the live overlay is this plan's
                // own GD replay -- so anchor on that and let shift be 0.
                //
                // Measured on lv22 (2026-08-13): with the touch boxes turned
                // into the rotated frame (see touchFor) the player finally
                // enters box 30 mid-section, and with the old shared anchor
                // that re-dated the WALL the section stands on -- the replay's
                // exit moved from world x=2,143.5 (GD's value) to 1,425.4.
                //
                // g_recPhase additionally says "the anchor was taken while this
                // one was still sliding", which the state's mask cannot express.
                if (trigRecFire[i] >= 0) {
                    if (g_recPhase & m) fired = true;
                    // A DUAL-controlled object (autonomous descent + touch
                    // nudge, the switch band's ceiling): the recording's
                    // motion is the AUTONOMOUS part and plays for EVERY
                    // state. Gated on the touch mask alone, an untouched
                    // state saw the ceiling at its entry position forever --
                    // dynpos printed two beliefs per tick, 367.75 (rest) and
                    // 296.8 (the recorded descent), and the t=2,164-anchored
                    // solve (before the descent's first motion, so no
                    // recPhase either) crossed the sealed band with zero
                    // punches and died in GD at the same t=2,764 as every
                    // other unpunched worldline. (autoAnchor alone missed it:
                    // the ceiling is classified touch-only; recAuto is the
                    // discriminator that caught it.)
                    if (autoAnchor[i] >= 0 || (i < recAuto.size() && recAuto[i]))
                        fired = true;
                    anchor = recAnchor; lat = 0;
                }
            } else {
                const int f = g_autoTrig[(size_t)aA].fireT;
                fired = (f >= 0 && f <= t);
                anchor = f; recAnchor = trigRecFire[i] - recLag[i];
                fdx = autoDx[i]; fdy = autoDy[i];
                fdur = autoDur[i]; lat = 0;
                fease = autoEase[i]; ferate = autoErate[i];
            }
            if (fired) {
                // [2026-08-26] A touch the STATE plans that the RECORDING never
                // made. With a recording present the branch below replays it
                // wholesale, so a planned punch changed NOTHING in the model's
                // world -- the switch band's ceiling (lv22 x=3,255..3,675,
                // hanging blocks) kept descending on the recorded (unpunched)
                // curve and the frontier died under it, which is the measured
                // "frontier dies four ticks after touching" of the 32-cap note.
                // GD's real response to the punch, measured on uid18119 with a
                // jump added at t=2,560: the ceiling RE-BASES at its position
                // of the touch tick, nudges up ~10px, and the descent easing
                // restarts from rest (335 at the punch, 338 at t=2,582 where
                // the unpunched curve reads 328). So when the state's own fire
                // tick is far from the recording's motion anchor -- this touch
                // is NOT what the recording shows -- FREEZE the recording at
                // the touch tick and apply the box's analytic chain offset
                // from there. The re-descent stays unmodeled (frozen): the
                // tail is replayed in GD either way, and that replay's own
                // recording carries the true post-punch world into the next
                // iteration. Recordings that ARE the state's touch (lv19/20
                // doors: recAnchor within a beat of the fire) are untouched.
                // Gated on the object ALSO having an auto controller: the
                // switch-band ceiling is auto-descent + touch-nudge, so its
                // recorded motion is the descent and never this punch; a
                // touch-only door's recording IS its own touch, and freezing
                // it at fireHi (the state's LAST box, one scalar for all
                // doors) re-dated lv19's earlier doors off a later box
                // (quick_regress lv19 t=19,800: 400 -> 3 before this gate).
                bool nearBox = false;
                if (m && (mask & m)) {
                    const uint32_t touched = mask & m;
                    for (int b = 0; b < 32; ++b)
                        if (((touched >> b) & 1u)
                            && std::fabs((double)g_touchBoxU[b]
                                         - (double)objs[i].cx) < 400.0) {
                            nearBox = true;
                            break;
                        }
                }
                // An object whose recording is a WORLDLINE fact rather than the
                // state's own touch (recAuto, decided at load from the per-box
                // chains) takes the per-box path below; the single-scalar hold
                // here keeps serving the dual-controlled (autoAnchor) class it
                // was measured on.
                const bool recAutoObj = m && i < recAuto.size()
                                        && recAuto[i] != 0;
                const bool ownTouch = m && (mask & m) && nearBox
                    && trigRecFire[i] >= 0
                    && !recAutoObj && autoAnchor[i] >= 0
                    && fireHi > recAnchor + 8;
                if (trigRecFire[i] >= 0 && !(g_trigClosed && autoClosed[i])) {
                    const int shift = (anchor >= 0) ? (anchor - recAnchor) : 0;
                    int tt = t - shift;
                    // NOT frozen forever: an eternal freeze let ANY grazed box
                    // hold the whole descent open and the anchored solve
                    // crossed the sealed band with zero punches (a phantom
                    // SOLVED at x=24,145 while GD dies at 2,763). The measured
                    // response is "the recording, DELAYED": hold ~60 ticks at
                    // the punch position, then the descent resumes from rest
                    // -- recorded(t-60) reads 285 at t=2,764 where the punched
                    // GD run measures 285.7. Consecutive punches slide the
                    // window (fireHi is the LAST box), which undercounts the
                    // cumulative delay of a punch chain -- the safe side: the
                    // model plans extra punches, and extra yellow hits are
                    // exactly what the mechanic accepts.
                    if (ownTouch && tt > fireHi) {
                        const int hold = 60;
                        tt = (t - shift > fireHi + hold) ? (tt - hold) : fireHi;
                    } else if (recAutoObj && (mask & m)) {
                        // Cumulative per-box delay. GD's measured response to a
                        // punch is "the recording, held ~60 ticks at the punch
                        // position" (rec(t-60) reads 285 at t=2,764 where the
                        // punched run measures 285.7) -- and the +30 the chain
                        // promises does NOT persist (285.7 measured, 315 if it
                        // did), so the punch's whole effect is the delay. Each
                        // touched box that pushes AWAY (dy > 0, the yellow
                        // punches) contributes its own min(60, t - F) window
                        // from its own fire tick -- for a single box this is
                        // the exact hold above. Boxes the anchor arrived with
                        // (F = -1) are already inside the live recording and
                        // contribute nothing.
                        int delay = 0;
                        for (const AutoPart& p : touchParts[i]) {
                            if (p.dy <= 0.f) continue;
                            if (!((mask >> p.trig) & 1u)) continue;
                            const int F = g_touchFireT[p.trig & 31];
                            if (F < 0 || t <= F) continue;
                            if (std::fabs((double)g_touchBoxU[p.trig & 31]
                                          - (double)objs[i].cx) >= 400.0)
                                continue;
                            delay += std::min(60, t - F);
                        }
                        tt -= delay;
                    }
                    // --shiftdbg <uid>: print this uid's re-timing once. An
                    // instrument for the suspicion that the descent phase of
                    // the t=2776 spike (uid18118) is off from live by tens of
                    // ticks (docs/HANDOFF.md, last section)
                    if (g_shiftDbgUid >= 0 && objs[i].uid == g_shiftDbgUid
                        && !g_shiftDbgDone) {
                        g_shiftDbgDone = true;
                        std::printf("shiftdbg: uid=%d anchor=%d recAnchor=%d "
                                    "shift=%d t=%d tt=%d touch=%d\n",
                                    objs[i].uid, anchor, recAnchor, shift, t,
                                    tt, m ? 1 : 0);
                    }
                    // binary search: these lists are ~100 samples and this runs
                    // once per object per group per tick
                    size_t lo = 0, hi = sm.size();
                    while (lo + 1 < hi) {
                        const size_t mid = (lo + hi) / 2;
                        if (sm[mid].t <= tt) lo = mid; else hi = mid;
                    }
                    const DynSample& s = sm[lo];
                    cx = s.cx; cy = s.cy; hw = s.hw; hh = s.hh; onv = s.on;
                    rotv = s.rot;
                    // [2026-08-22 r104] **Interpolate linearly between rows.**
                    // grouptrace does not write changes under 0.05px, so an
                    // object moving at 0.045px/tick is recorded as "jumps
                    // 0.09px once every 2 ticks". GD moves smoothly, so holding
                    // (the staircase) is a systematic error of up to 0.09px and
                    // **cannot reproduce the tick at which contact breaks, not
                    // even to within 1 tick** (the lv22@6,129 family).
                    // Position and rot only. on (a boolean) and hw/hh stay
                    // stepped.
                    if (g_dynInterp && lo + 1 < sm.size()) {
                        const DynSample& n = sm[lo + 1];
                        const int span = n.t - s.t;
                        if (span > 0 && tt > s.t) {
                            const double u = (double)(tt - s.t) / (double)span;
                            cx = (float)((double)s.cx
                                         + ((double)n.cx - (double)s.cx) * u);
                            cy = (float)((double)s.cy
                                         + ((double)n.cy - (double)s.cy) * u);
                            rotv = (float)((double)s.rot
                                           + ((double)n.rot - (double)s.rot) * u);
                        }
                    }
                    // A recording ends where its attempt died, and holding the
                    // last sample freezes a door mid-slide. Measured on lv19
                    // (2026-08-05): the corridor recording stops at t=20,012
                    // with the slide 32 px into its 45, the model rides the
                    // 67%-open slat 12.94 px above where GD rides the OPEN
                    // door, the 1.1x speed portal at (28,163,303) fires 13
                    // ticks apart, and the whole endgame runs 6.8 px of x
                    // adrift (the 27,686 / 29,024 pincer of the 60-iteration
                    // run). When the map knows the full offset, finish the
                    // slide: add the analytic eased INCREMENT from where the
                    // recording ends -- continuous at the seam, and the small
                    // map-vs-real mismatch (the ease fit is within a few
                    // percent) stays exactly as large as it already was.
                    // Move-to-target pieces (offset 0,0 -- the lift) cannot be
                    // completed analytically; those need the driver's nodeath
                    // re-record instead.
                    // For a recAuto object the summed fdx/fdy is opposing
                    // chains added together, but the slide still has to be
                    // finished: the band's recording is a DEAD attempt's
                    // (grouptrace stops ~25 ticks after the seal kills the
                    // recorder at t=2,764), and holding its last row left the
                    // ceiling at 260.59 forever -- a 13 px slit over the lane
                    // that GD closes, which the anchored solve threaded for
                    // yet another no-punch phantom. The autonomous descent's
                    // own chain is on file per box (the red -840/1005.6t tail
                    // is the same movers), so complete with the largest part
                    // whose dy agrees with the recorded motion's direction.
                    double bdx = fdx, bdy = fdy, bdur = fdur;
                    int bease = fease; double berate = ferate;
                    if (recAutoObj) {
                        bdur = 0.0;
                        const double dirY = (double)s.cy - (double)s0.cy;
                        float best = 0.f;
                        for (const AutoPart& p : touchParts[i])
                            if ((double)p.dy * dirY > 0.0
                                && std::fabs(p.dy) > best) {
                                best = std::fabs(p.dy);
                                bdx = p.dx; bdy = p.dy; bdur = p.dur;
                                bease = p.ease; berate = p.erate;
                            }
                    }
                    if (lo + 1 == sm.size() && tt > s.t && bdur > 0) {
                        const double full = std::hypot((double)bdx, (double)bdy);
                        const double done = std::hypot((double)s.cx - (double)s0.cx,
                                                       (double)s.cy - (double)s0.cy);
                        if (full > 0.5 && done < full - 0.5) {
                            auto ease = [&](double u) {
                                return gdEase(bease, berate, u);
                            };
                            const double uEnd =
                                (double)(s.t - recAnchor) / bdur;
                            const double uNow =
                                (double)(tt - recAnchor) / bdur;
                            // normalized so the terminal position is EXACTLY
                            // rest + offset (the measured open position),
                            // whatever small phase error the recording ended
                            // with; the increment form overshot by that error
                            const double eEnd = ease(uEnd);
                            if (eEnd < 0.999) {
                                const double de =
                                    (ease(uNow) - eEnd) / (1.0 - eEnd);
                                cx += ((double)s0.cx + (double)bdx - (double)s.cx) * de;
                                cy += ((double)s0.cy + (double)bdy - (double)s.cy) * de;
                            }
                        }
                    }
                    // ...and the planned touch's own chain, applied FROM the
                    // frozen base (the measured re-base + nudge; the note at
                    // ownTouch above). 5 = the measured box->motion latency.
                    if (ownTouch) {
                        double u = 1.0;
                        if (fdur > 0) u = (double)(t - fireHi - 5) / fdur;
                        if (u > 0.0) {
                            const double e = gdEase(fease, ferate, u);
                            cx += fdx * e;
                            cy += fdy * e;
                        }
                    } else if (recAutoObj && (mask & m)) {
                        // A touched box that pushes TOWARD the player (dy < 0,
                        // the red skulls) is applied as a step: the user-stated
                        // and measured mechanic is an immediate slam, and the
                        // chain's 1005.6-tick easing belongs to the autonomous
                        // descent, not to this. A state that punches red sees
                        // the seal at once and dies in the model exactly where
                        // GD kills it.
                        for (const AutoPart& p : touchParts[i]) {
                            if (p.dy >= 0.f) continue;
                            if (!((mask >> p.trig) & 1u)) continue;
                            const int F = g_touchFireT[p.trig & 31];
                            if (F < 0 || t < F + 5) continue;
                            if (std::fabs((double)g_touchBoxU[p.trig & 31]
                                          - (double)objs[i].cx) >= 400.0)
                                continue;
                            cx += p.dx;
                            cy += p.dy;
                        }
                    }
                } else {
                    // The closed form. Either there is no recording at all (the
                    // door nobody has opened yet) or --trigclosed is on and this
                    // object's recording AGREES with the formula, in which case
                    // the formula is the better source: it does not end where
                    // some attempt died and it does not carry another run's
                    // phase. `on` and the size still come from the recording --
                    // a toggle is not a move and has no closed form here.
                    // ...and if the object is locked to the player, its x is
                    // THIS run's player x, which is the whole reason a recording
                    // cannot serve it (see TrigCtl::lockTicks).
                    if (aA >= 0 && autoLockTrig[i] >= 0
                        && g_autoTrig[(size_t)autoLockTrig[i]].fireT >= 0) {
                        AutoTrig& A = g_autoTrig[(size_t)autoLockTrig[i]];
                        // The lock starts on the CROSSING, one tick before an
                        // eased move would (measured, 0.002 px over 2,400
                        // samples), and runs for its own duration. `lockLastX`
                        // trails the player while the window is open so that
                        // once it closes the object holds where the player left
                        // it -- the same running-assignment trick fireT uses,
                        // and like fireT it is level-wide: the last group of a
                        // tick wins. States within a tick differ in x by a
                        // speed group at most, which is the same approximation
                        // gFire already makes for doors.
                        const int lockT0 = A.fireT - A.delay;
                        const int lockEnd = lockT0 + (int)autoLock[i];
                        if (t >= lockT0 && t <= lockEnd) {
                            A.lockLastX = px;
                            cx += px - A.fireX;
                        } else if (t > lockEnd) {
                            cx += A.lockLastX - A.fireX;
                        }
                    }
                    if (trigRecFire[i] >= 0 && sm.size() > 1) {
                        const int shift = (anchor >= 0) ? (anchor - recAnchor) : 0;
                        const int tt = t - shift;
                        size_t lo = 0, hi = sm.size();
                        while (lo + 1 < hi) {
                            const size_t mid = (lo + hi) / 2;
                            if (sm[mid].t <= tt) lo = mid; else hi = mid;
                        }
                        hw = sm[lo].hw; hh = sm[lo].hh; onv = sm[lo].on;
                        rotv = sm[lo].rot;
                    }
                    if (recAutoObj) {
                        // No recording yet (first iteration). The summed
                        // offset is opposing chains added together, so play
                        // each TOUCHED box's own part instead. The autonomous
                        // descent is unknowable here (its starter is a
                        // collision trigger the dump cannot time); the next
                        // iteration's recording carries it.
                        for (const AutoPart& p : touchParts[i]) {
                            if (!((mask >> p.trig) & 1u)) continue;
                            const int F = g_touchFireT[p.trig & 31];
                            if (F < 0) continue;
                            double u = 1.0;
                            if (p.dur > 0) u = (double)(t - F - 5) / p.dur;
                            if (u <= 0.0) continue;
                            const double e = gdEase(p.ease, p.erate, u);
                            cx += p.dx * e;
                            cy += p.dy * e;
                        }
                    } else if (m || autoParts[i].empty()) {
                        // touch trigger (one box, one summed offset), or an
                        // autonomous object with no move at all (toggle only)
                        double u = 1.0;
                        if (anchor >= 0 && fdur > 0)
                            u = (double)(t - anchor - lat) / fdur;
                        const double e = gdEase(fease, ferate, u);
                        cx += fdx * e;
                        cy += fdy * e;
                    } else {
                        // Superposition: every controller on its own clock.
                        for (const AutoPart& p : autoParts[i]) {
                            const int f = g_autoTrig[(size_t)p.trig].fireT;
                            if (f < 0 || t < f) continue;
                            const double e = (p.dur > 0.0)
                                ? gdEase(p.ease, p.erate, (double)(t - f) / p.dur)
                                : 1.0;
                            cx += p.dx * e;
                            cy += p.dy * e;
                        }
                    }
                }
            }
            // --shiftdbg <uid> also samples the object's EVALUATED position
            // every 20 ticks -- the belief the kill tests actually see, which
            // no amount of reasoning about the composition rules substitutes
            // for (the switch-band phantom survived three composition fixes
            // that were all aimed at the wrong mechanism).
            if (g_shiftDbgUid >= 0 && objs[i].uid == g_shiftDbgUid
                && t % 20 == 0)
                std::printf("dynpos: uid=%d t=%d cx=%.2f cy=%.2f on=%d\n",
                            objs[i].uid, t, cx, cy, (int)onv);
            objs[i].cx = cx; objs[i].cy = cy; objs[i].hw = hw; objs[i].hh = hh;
            // dcy from the FINAL analytic position, not the raw samples (see
            // prevCy's declaration: the recorder skips sub-epsilon rows and
            // the raw dcy reads 0 on those ticks while the face still moves).
            // Only on a 1-tick advance -- an anchor jump keeps seek's
            // row-based value, same reasoning as seek's step1 gate.
            if (lastStep1 && i < prevCy.size())
                objs[i].dcy = (double)cy - (double)prevCy[i];
            // If the recording says "gone by this tick", the analytic side must
            // not bring it back. The live recording is this very run's own GD
            // replay, so `on` is the truth there. Returning sm[0] as it was here
            // left the block GD switched off at t=10,512 (lv22 uid11470) standing
            // in the model, and the player sliding down the corridor died of a
            // "side collision" at t=11,359.
            if (cur[i] < sm.size() && sm[cur[i]].t <= t && sm[cur[i]].on == 0)
                onv = 0;
            // ...and the mirror for the ON direction, for objects nothing
            // MOVES (a pure toggle). The shifted search above re-times the
            // recording against the controller's predicted firing tick -- the
            // right thing for a door's motion, but an autonomous TOGGLE has no
            // re-timing basis: its "anchor" is the FIRST x-crossing while the
            // crossing that actually fires is the same unsolved gate 2899 has
            // (lv22's uid 6442 fires on its THIRD crossing of x=15,765,
            // together with the 2899 pair). The live recording is this plan's
            // own GD replay, so for a no-move object its unshifted clock is
            // the only truth. Without this, lv22's ship portal uid 6440
            // (group 309, toggled OFF at t=10,116 / ON at 14,225) read the
            // pre-toggle sample through a +1,2xx shift and NEVER fired -- the
            // model kept the cube falling past x=15,785 where GD flies a ship.
            {
                bool noMove = fdx == 0.f && fdy == 0.f;
                for (const AutoPart& p : autoParts[i])
                    if (p.dx != 0.f || p.dy != 0.f) { noMove = false; break; }
                // PORT/SPEED only. The first cut applied this to every no-move
                // object and lv22's frame-3 corridor (t=11,000 segment, the
                // uid 11470 neighbourhood) broke: the regress follow shrank
                // 400 -> 351 and two solid-support families appeared. The
                // solids' interplay with the shifted read is validated the
                // other way (the standing force-off above), so only the
                // interactive classes with a measured case move to the
                // unshifted clock.
                if (!m && noMove
                    && (bucket[i] == PORT || bucket[i] == SPEED)
                    && cur[i] < sm.size() && sm[cur[i]].t <= t) {
                    uint8_t v = sm[cur[i]].on;
                    // Re-ENABLING takes effect the tick AFTER the recorded row
                    // (measured: uid 6440's ON row is t=14,225 -- the toggle
                    // itself ran during 14,224 -- and GD's portal fires during
                    // 14,226, one tick later; the model firing at 14,225 came
                    // out -1.404 where GD has -1.512). The OFF direction stays
                    // on the row's own tick, same as the standing rule above
                    // (validated on uid 11470).
                    if (v && sm[cur[i]].t == t && cur[i] > 0
                        && !sm[cur[i] - 1].on)
                        v = 0;
                    onv = v;
                }
            }
            on[i] = onv;
            if (g_dynDbg == objs[i].uid)
                std::printf("dyntrig t=%d uid=%d fired=%d m=%08x cx=%.3f "
                            "cy=%.3f hw=%.1f hh=%.1f on=%d\n",
                            t, objs[i].uid, (int)fired, (unsigned)m, cx, cy,
                            hw, hh, (int)onv);
            turnedBox(objs[i], bucket[i], (double)rotv,
                      i < everRot.size() && everRot[i] != 0);
            // a moving SLOPE carries its surface line (same rule as seek())
            if (objs[i].slope) {
                const double dy = cy - (double)baseCy[i];
                objs[i].sy0 = (double)baseSy0[i] + dy;
                objs[i].sy1 = (double)baseSy1[i] + dy;
            }
        }
    }

    // Put every object where it was at tick t. Cursors move forward with the
    // layer loop, so this is O(1) amortised; a backwards jump (--start, or the
    // witness resim starting over) rewinds and re-scans, which happens once.
    void seek(int t) {
        if (objs.empty() || t == lastT) return;
        const bool back = (t < lastT);
        // dcy is PER-TICK surface motion, so the position-difference form is
        // only meaningful when this seek advanced exactly one tick. The FIRST
        // seek of an anchored replay jumps from the load position (lastT=-1)
        // straight to t0, and the difference was the platform's WHOLE journey:
        // lv19 t=21,800 anchored with dcy=136, which put every mover inside
        // the `0.6 + |dcy|` support tolerance and seated the ship on a lift
        // 91 px overhead. A jump (either direction) falls back to the
        // recording's own consecutive rows, same as the backward branch.
        const bool step1 = !back && (t == lastT + 1);
        lastStep1 = step1;
        lastT = t;
        for (size_t i = 0; i < objs.size(); ++i) {
            const auto& sm = samples[i];
            if (sm.empty()) continue;
            size_t c = back ? 0 : cur[i];
            while (c + 1 < sm.size() && sm[c + 1].t <= t) ++c;
            cur[i] = c;
            const DynSample& s = sm[c];
            // per-tick y motion of the SURFACE itself (see Obj::dcy).
            // On a BACKWARDS seek (a --start anchor, or the witness resim
            // starting over) there is no previous position to subtract, and
            // zeroing it costs the rider its first step: the ride only fires
            // when `dcy != 0`, so a player anchored mid-ride skips one surface
            // step and stays that much behind for the whole ride. Measured on
            // lv22's ball corridor (2026-08-14): the anchored resim followed
            // 166 ticks and then reported dy=-1.53 growing 0.17/tick with
            // dvy=+0.0000 -- one missed step of a floor that sinks at exactly
            // that rate. The recording knows the answer: when the sample landed
            // on THIS tick and the one before it is the tick before, their
            // difference IS the per-tick motion.
            // last FINAL position before this tick's placement overwrites it
            // (applyTriggers reads it to rebuild dcy for controlled objects)
            if (i < prevCy.size())
                prevCy[i] = step1 ? objs[i].cy : (float)s.cy;
            objs[i].dcy =
                step1 ? ((double)s.cy - (double)objs[i].cy)
                      : (c > 0 && sm[c].t == t && sm[c - 1].t == t - 1)
                            ? ((double)sm[c].cy - (double)sm[c - 1].cy)
                            : 0.0;
            objs[i].cx = s.cx; objs[i].cy = s.cy;
            objs[i].hw = s.hw; objs[i].hh = s.hh;
            on[i] = s.on;
            turnedBox(objs[i], bucket[i], (double)s.rot,
                      i < everRot.size() && everRot[i] != 0);
            if (objs[i].slope) {
                const double dy = (double)s.cy - (double)baseCy[i];
                objs[i].sy0 = (double)baseSy0[i] + dy;
                objs[i].sy1 = (double)baseSy1[i] + dy;
            }
            if (g_dynDbg == objs[i].uid)
                std::printf("dynseek t=%d uid=%d cur=%zu/%zu cy=%.3f "
                            "(sample t=%d)\n",
                            t, objs[i].uid, c, sm.size(), (double)objs[i].cy,
                            s.t);
        }
    }
    // A MOVING object that is also being TURNED. The recording's hw/hh are half
    // the axis-aligned BOUND, which for a turned box is not the shape at all --
    // and the model was using it as the hitbox.
    //
    // Measured on lv22 (2026-08-13): the spider portal uid 434 (really 34x86) is
    // turned by a trigger from -2.45 deg at t=768 to -39.2 deg at t=810, so its
    // recorded bound grows 37.6x87.4 -> 80.7x88.1. The model fired it THREE
    // TICKS early, which put the whole rest of the run 66 px off in y and was the
    // first divergence on both the seeded clear and every runR3/runR4 plan.
    // A 2-D injection sweep (py/portal_sweep2.py) pinned the real region: at
    // -26 deg only 1 of 168 grid points inside the bound actually fires.
    //
    // The nominal size comes back out of the bound and the angle (the same
    // inversion Obj::oriented already documents for static rotations):
    //   a = W|c| + H|s| , b = W|s| + H|c|  ->  W = (a c - b s)/cos2t , etc.
    // Only PORTALS get this: `oriented` is wired to the portal test and tuned
    // there, and a 90-degree multiple leaves the bound correct anyway.
    static void turnedBox(Obj& o, uint8_t bucketOf, double rotDeg,
                          bool everRot = false) {
        if (bucketOf != PORT) return;
        o.rot = rotDeg;
        // [2026-08-18] **Do not throw away the real box built from w0/h0 at
        // load.** This used to drop oriented to 0 on every seek and rebuild it
        // by inverting the bounding box, and since the inversion does not work
        // near 45 degrees (|cos2θ| < 0.1), grouped portals alone were **firing
        // on the bounding box**. Measured on lv21 uid24194 (id111 UFO portal,
        // rot=43, w0/h0 = 34x86, bound 167x172): GD switches at t=18,087
        // (x=23,929), but the model did so at the bound's left edge at t=18,075
        // (x=23,910) -- 12 ticks / 19px early. Turning **does not change the
        // box's size**, so only the angle needs to be redrawn.
        if (o.oriented) {
            const double th2 = rotDeg * 3.14159265358979 / 180.0;
            o.rc = std::cos(-th2); o.rs = std::sin(-th2);
            return;
        }
        const double om = std::fabs(std::fmod(rotDeg, 90.0));
        // [2026-08-25] A portal a trigger ROTATES is "turned" in GD's sense for
        // the whole run, even while its angle still reads 0 -- and GD's turned
        // branch tests the PLAYER's oriented box too, which an axis-aligned
        // AABB test throws away. That is a real difference: a cube is always
        // spinning, so the window it presents to the portal is not a square.
        //
        // Measured on lv22's spider portal uid 1156 (2085,255, 34x86, static
        // rot 0, rotated by its group from t=1,612). The model fired it at
        // t=1,582 and GD at t=1,583, leaving a CONSTANT 0.087/tick vy error
        // (0.216 cube gravity vs 0.129 spider) that never washes out -- lv22
        // cold has been stuck behind it. Injection sweep at t=1,582, x fixed at
        // 2,053.519, varying only y (GD fires / does not):
        //     y=213.874 FIRES    y=209.000 FIRES    y=208.000 FIRES
        //     y=207.500 no       y=201.138 no       y=199.472 no
        // A plain AABB cannot produce that boundary at ANY half-width (the same
        // y=201.138 fires one tick later at x=2,054.817), but the two-box SAT
        // with the player's box turned by its rotation TWO TICKS BACK -- which
        // is exactly the `pRotHere` the portal loop already computes -- puts the
        // edge at y=208.00 against an observed bracket of (207.500, 208.000].
        // Four more points (including the t=1,583 fire at margin 0.075) fit the
        // same line.
        //
        // Only the near-0 case is taken: at ~90 degrees the bound's halves are
        // the box's SWAPPED, and no measurement covers that.
        // [2026-08-25] OFF BY DEFAULT (--rotport). The measurement above stands,
        // but switching it on cost lv22's cold run 41% -> 15% (x=9,827 ->
        // x=3,689, and the DP calls went 8s -> 94s): with 133 of lv22's moving
        // objects rotate-targeted, every one of them starts testing against the
        // player's turned box, and the route landscape that comes out is worse
        // even though each individual verdict is closer to GD. The measured
        // boundary is also contaminated by something this note cannot yet
        // separate -- GD fires this portal at t=1,583 on one attempt and
        // t=1,582 on another with the SAME plan, so the object's phase carries
        // across attempts. Re-open it with a per-object gate, not a global one.
        if (g_rotPort && om <= 0.5 && everRot && o.hw > 0.5 && o.hh > 0.5) {
            o.oriented = 1;
            o.ohw = o.hw; o.ohh = o.hh;   // axis aligned: the bound IS the box
            const double th0 = rotDeg * 3.14159265358979 / 180.0;
            o.rc = std::cos(-th0); o.rs = std::sin(-th0);
            return;
        }
        if (om <= 0.5 || om >= 89.5) return;   // axis aligned: bound == shape
        const double th = rotDeg * 3.14159265358979 / 180.0;
        const double c = std::fabs(std::cos(th)), s = std::fabs(std::sin(th));
        const double det = c * c - s * s;      // cos 2t
        if (std::fabs(det) < 0.1) return;      // near 45 deg: not invertible
        const double W = ((double)o.hw * c - (double)o.hh * s) / det;
        const double H = ((double)o.hh * c - (double)o.hw * s) / det;
        if (W <= 0.5 || H <= 0.5) return;
        o.oriented = 1;
        o.ohw = W; o.ohh = H;
        o.rc = std::cos(-th); o.rs = std::sin(-th);
    }
    // Dynamic objects cannot live in the sorted-by-cx index, so their window is
    // a linear scan. It is bounded by the number of moving objects in the level
    // (1,731 at worst) and runs once per layer, not once per state.
    // Turn every moving object into a gameplay frame (see turnObj / frameLevel).
    // `collect` is a linear scan and the parallel arrays are indexed by position,
    // so nothing has to be re-sorted -- only the geometry and the OFFSETS turn.
    // Offsets are vectors, so they take the same linear map without the
    // translation part, which toFrame already is (a pure rotation about 0).
    // Not handled: lockToPlayerX (autoLock). A lock copies the player's WORLD x
    // onto the object, which in a turned frame is a v, not a u -- lv22's two
    // locks are both in the un-rotated switch band, so this stays a documented
    // hole rather than a guess.
    void turn(int f) {
        if ((f & 3) == 0) return;
        auto rotVec = [&](float& dx, float& dy) {
            double u, v;
            toFrame(f, (double)dx, (double)dy, u, v);
            dx = (float)u; dy = (float)v;
        };
        for (size_t i = 0; i < objs.size(); ++i) {
            for (DynSample& s : samples[i]) {
                double u, v;
                toFrame(f, (double)s.cx, (double)s.cy, u, v);
                s.cx = (float)u; s.cy = (float)v;
                if (f & 1) std::swap(s.hw, s.hh);
            }
            turnObj(objs[i], f);
            baseCy[i] = objs[i].cy;
            baseSy0[i] = (float)objs[i].sy0;
            baseSy1[i] = (float)objs[i].sy1;
            rotVec(trigDx[i], trigDy[i]);
            rotVec(autoDx[i], autoDy[i]);
            for (AutoPart& p : autoParts[i]) rotVec(p.dx, p.dy);
            for (AutoPart& p : touchParts[i]) rotVec(p.dx, p.dy);
        }
        std::fill(cur.begin(), cur.end(), (size_t)0);
        lastT = -1;
    }

    void collect(uint8_t b, double x0, double x1,
                 std::vector<const Obj*>& out) const {
        for (size_t i = 0; i < objs.size(); ++i) {
            const Obj& o = objs[i];
            if (g_dynDbg >= 0 && o.uid == g_dynDbg)
                std::printf("dyncollect uid=%d b=%d/%d on=%d cx=%.3f hw=%.3f "
                            "win=[%.1f,%.1f] -> %s\n",
                            o.uid, (int)bucket[i], (int)b, (int)on[i], o.cx,
                            o.hw, x0, x1,
                            (bucket[i] != b || !on[i])          ? "SKIP(bucket/on)"
                            : (o.cx + o.hw < x0 || o.cx - o.hw > x1)
                                ? "SKIP(window)" : "in");
            if (bucket[i] != b || !on[i]) continue;
            if (o.cx + o.hw < x0 || o.cx - o.hw > x1) continue;
            out.push_back(&o);
        }
    }
};

}  // namespace dp
