#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <unordered_map>
#include "dp/frames.hpp"
#include "dp/game_event.hpp"
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <type_traits>

namespace dp {


// `rot` is GD's own getRotation() for that tick (grouptrace's 8th column, added
// 2026-08-13). hw/hh are HALF the AXIS-ALIGNED BOUND, so for a turned object
// they are not the shape -- see Obj::oriented and the note on the portal test.
// Recordings made before the column existed load with rot = 0, i.e. exactly the
// old behaviour.
// `env`: the row is not the object's rect but the box GD's own random numbers can put it in
// (the MOD's areaenv.hpp, --groups' ninth column). The level loader turns such rows into a
// hazard twin of the object (see envTwinUid).
struct DynSample { int t; float cx, cy, hw, hh; uint8_t on; float rot = 0.f; uint8_t env = 0; };
// One moving object's rows (Dynamics::samples): the recording's own vector in the call's group timeline (groupLayersFor's
// merged / --bootauto parse, unchanged for the length of a call), read where it is, or rows of the object's own once something
// changes them (mut). The loader copied every timed object's rows out of the timeline: 113M rows of a custom level's recording
// were 3.6 GB written again at every load, and a cap ladder loads the level for every attempt (e10f287e). Reads as a const vector.
struct DynRows {
    using value_type = DynSample;
    using const_iterator = std::vector<DynSample>::const_iterator;
    using iterator = const_iterator;
    DynRows() = default;
    DynRows(std::vector<DynSample> v) : own(std::move(v)) {}
    static DynRows ref(const std::vector<DynSample>& v) { DynRows r; r.shared = &v; return r; }
    DynRows& operator=(std::vector<DynSample> v) { own = std::move(v); shared = nullptr; return *this; }
    const std::vector<DynSample>& rows() const { return shared ? *shared : own; }
    operator const std::vector<DynSample>&() const { return rows(); }
    std::vector<DynSample>& mut() {   // the object's own rows from here on (a copy of the timeline's the first time)
        if (shared) { own = *shared; shared = nullptr; }
        return own;
    }
    size_t size() const { return rows().size(); }
    bool empty() const { return rows().empty(); }
    const DynSample& operator[](size_t k) const { return rows()[k]; }
    const DynSample& front() const { return rows().front(); }
    const DynSample& back() const { return rows().back(); }
    const_iterator begin() const { return rows().begin(); }
    const_iterator end() const { return rows().end(); }
    const DynSample* data() const { return rows().data(); }

private:
    const std::vector<DynSample>* shared = nullptr;
    std::vector<DynSample> own;
};
// [2026-08-22 r104] Interpolate linearly between the recording's rows (always;
// --no-dyninterp is gone since the flag clean-up): restores smooth motion to
// moving geometry that grouptrace's 0.05px threshold turned into a staircase.
// (defined up here, before Dynamics, because applyTriggers reads g_autoTrig;
// the touch-trigger story lives with TouchTrig below)
// --movetarget: a Move in target mode and a Stop (1616) act on the
// touch chains' closed form. Without it both are inert there: a target-mode Move
// carries ox=oy=0, and a Stop's target group holds only triggers the walk skips.
// Measured on lv22 (2026-09-21, attempt 30 of a flags-on cold): touching a red
// block spawns group 540 = Stop uid18096 (halts group 543, the two Moves that
// lower the ceiling group 265 over 4.19 s) + Move uid18093 (target mode, y only,
// 0.5 s, ease 9) that takes the ceiling to a total of -122 px. GD's rect of the
// killing spike uid18105 at t=2,599 is 96 px down; the model had it 33 px down
// and never killed, so the loop fenced the spot off with deadbands.
// ON since 2026-09-21, its own unit; always on since the
// flag clean-up.
// Per touch box (the State::trig bit): the Move triggers a Stop in its chain
// halts. Filled with the boxes (buildTouchMoveTicks); read by applyTriggers.
inline std::vector<std::vector<int>> g_touchStops;

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
    // --movetarget (note at the top of this file). `mover` is the Move trigger whose
    // hop set this entry's motion, so a Stop naming that Move can freeze it.
    // `tmode` marks a target-mode Move (0 = not one; 1 both axes, 2 x only,
    // 3 y only): (tdx, tdy) is where the group ends up relative to its
    // placement, and the offset it adds is decided when it fires (destination
    // minus where the group is by then).
    int mover = 0;
    uint8_t tmode = 0;
    float tdx = 0.f, tdy = 0.f;
    // --spawnlag: how many ticks later than an immediate chain this entry's Move
    // fires, because a Spawn on the way queued it (its delay, or its place in an ordered spawn).
    // 0 without the flag.
    int spawnLag = 0;
    // --anchormove (removed): a Rotate entry with a centre group (its position changes at the crossing, also when
    // the centre is the target group itself); a Rotate without one turns it about its own position.
    uint8_t orbit = 0;
};
// uid -> x of every object the dump marks spawn-triggered (objrects' `spawn` column), triggers
// with no row in triggers.txt included -- lv22's Pickup uid18173 is the first of its ordered
// group and has none. Filled by the CLI under --spawnlag.
inline std::unordered_map<int, double> g_spawnableX;
inline int spawnLagTicks(double seconds) {
    if (seconds <= 0.0) return 0;
    return std::max(0, (int)std::ceil(seconds * 240.0 - 1e-9) - 1);
}
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
inline TouchMask g_recPhase{};
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
        // [2026-09-01] The two Exponential ENDPOINTS, read out of the binary
        // (getEasedValue, the 0.001f at rodata 3A83126F is real). Both were
        // wrong, in opposite directions, and both are permanent: an object
        // whose move ends on one of these curves stops in the wrong place and
        // stays there, so the hitbox is off for the rest of the level.
        //
        // ExpIn: GD subtracts 0.001 and therefore stops at 0.999 -- one tenth
        // of a percent SHORT of the offset it was given, forever. The model
        // reached 1.0, i.e. it overshot. cocos has no u==0 guard either (it
        // returns -0.0000234 there), so the guard goes too: match the binary,
        // not the tidier curve. (Wrong for 2.2081, see below.)
        // --expinzero (removed): ...but the binary has the
        // guard. PR #2 read 2.2081's GameToolbox::getEasedValue (0x68b70) as
        // returning 0 at u == 0 before the exponential, and so does the decompile
        // (case 0xb: 0.0 unless u != 0.0, then powf(2, (u - 1) * 10) - 0.001). The
        // difference is 0.0000234 of the move's offset, on the move's first tick only.
        // (always on: the switch --expinzero is removed)
        // --expinzero (removed): ExpIn easing returns 0 at u == 0 (dynamics.hpp gdEase). From PR #2. On by default since
        // 2026-10.
        case 11: return u == 0.0 ? 0.0 : std::pow(2.0, 10.0 * (u - 1.0)) - 0.001;
        // ExpOut: GD special-cases t==1 to land exactly. Without it the curve
        // stops at 1 - 2^-10 = 0.9990234, 0.0977% short. Six moves in lv22 use
        // this, the largest 51 px, so the model was parking them ~0.05 px low.
        case 12: return u >= 1.0 ? 1.0 : 1.0 - std::pow(2.0, -10.0 * u);
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
inline double instantE(int te, int f) { return (te <= f) ? 0.0 : 1.0; }
// How many ticks after the move really starts does grouptrace FIRST write a row
// for it? The recorder drops changes under 0.05 px (grouptrace.hpp kEps), and an
// eased move spends its first ticks in the thousandths, so the recording's
// apparent first motion is 1-2 ticks late -- 1 for a linear move, 2 for
// EaseInOut. That lag is not an unknown: with the offset, the duration and the
// curve in hand it is computable, which is what this does. Everything
// downstream then works in TRUE first-motion ticks and the artefact cancels.
// See the ZoomTrig block for the measurements. Progress is mapped through x
// because the state cannot carry the crossing tick.
// --zoomchain: rows since zoom k's crossing row, for the row at (x, t). The first state to see
// the zoom crossed fixes the crossing for the invocation (modifiers.hpp).
inline long long zoomRowsSince(size_t k, double x, double dx, long long t) {
    const long long est = (long long)std::floor((x - g_zoomTrigs[k].cx) / dx + 1e-6);
    if (t < 0 || k >= g_zoomCrossN) return est;
    long long tc = g_zoomCrossT[k].load(std::memory_order_relaxed);
    if (tc == kZoomCrossUnknown) {
        long long want = t - est;
        // the earliest wins if two threads see it on different layers
        while (!g_zoomCrossT[k].compare_exchange_weak(tc, want, std::memory_order_relaxed)) {
            if (tc != kZoomCrossUnknown && tc <= want) break;
        }
        tc = g_zoomCrossT[k].load(std::memory_order_relaxed);
    }
    return t - tc;
}
// --zoomchain: zoom z's value d rows after its crossing row, starting from `from`. A tween starts
// on the row before (d + 1 ticks in); an instant zoom lands on the row itself.
inline double zoomValueAfter(const ZoomTrig& z, double from, long long d) {
    if (z.durTicks <= 0.0) return d >= 0 ? z.target : from;
    const double e = (double)(d + 1);
    if (e <= 0.0) return from;
    if (z.frac) {   // the camera's float tween (buildZoomFrac): the value it has after e steps, and the one it rests on after the last
        const std::vector<float>& f = *z.frac;
        const float fr = (size_t)e < f.size() ? f[(size_t)e] : f.back();
        const float a = (float)from;
        return (double)(((float)z.target - a) * fr + a);   // GJValueTween::step: (to - from) * eased + from, all float
    }
    if (e >= z.durTicks) return z.target;
    return from + (z.target - from) * gdEase(z.ease, z.rate, e / z.durTicks);
}
inline double camScaleChain(double x, double dx, long long t) {
    size_t m = 0;   // the zooms crossed by x
    while (m < g_zoomTrigs.size() && g_zoomTrigs[m].cx <= x) ++m;
    if (m == 0) return 1.0;
    // rows since each crossing, newest first, back to the first zoom that had settled before the
    // next one started (everything earlier is then irrelevant)
    thread_local std::vector<long long> n;
    n.clear();
    size_t k = m;
    while (k > 0) {
        --k;
        n.push_back(zoomRowsSince(k, x, dx, t));
        if (n.size() >= 2) {
            const ZoomTrig& z = g_zoomTrigs[k];
            const long long dStart = n[n.size() - 1] - n[n.size() - 2] - 1;   // rows into z when the next started
            if (z.durTicks <= 0.0 ? dStart >= 0 : (double)(dStart + 1) >= z.durTicks) break;
        }
    }
    // forward from zoom k: its start value is irrelevant only if it settled, so a chain that ran
    // back to the first zoom starts from 1
    double from = 1.0;
    for (size_t j = 0; j < n.size(); ++j) {
        const size_t idx = k + j;
        const long long nj = n[n.size() - 1 - j];
        if (j + 1 < n.size()) {
            const long long nNext = n[n.size() - 2 - j];
            from = zoomValueAfter(g_zoomTrigs[idx], from, nj - nNext - 1);
        } else {
            from = zoomValueAfter(g_zoomTrigs[idx], from, nj);
        }
    }
    return from;
}
inline double camScaleAt(double x, double dx, long long t) {
    double cur = 1.0;
    if (dx <= 0.0) dx = 1.0;
    return camScaleChain(x, dx, t);
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
    // (always on: the switch --reclag0 is removed)
    // --reclag0 (removed) (ON since 2026-10-24): a recorded object's first row can be the move's own first tick. The count in recordLag starts at n = 1
    // and so cannot return 0 for an ease whose value at u = 0 already clears the recorder's 0.05 px: ExponentialInOut (10) is
    // 0.5 * 2^-10 there, 0.732 px of a 1,500 px move. A custom level, a plate group moved by a Move of -1,500 over 480 ticks
    // with that ease: the first row, t=17,564, is 0.732 px from the base, and the rows after it (0.799 at 17,567, 0.871 at
    // 17,570) are the curve at k = 3, 6 from that tick -- so the move began on that row and the lag is 0. The count said 1, the
    // object was re-timed one tick back (shift 1) and was read where it had been a tick before: 4.5 px behind the game at
    // t=17,750, where the mini ship the model flew over its edge died on a block that was no longer there.
    for (int n = 0; n <= 240; ++n) {
        const double e = gdEase(ease, erate, (double)n / durTicks);
        if (std::fabs(dx * e) >= 0.05 || std::fabs(dy * e) >= 0.05) return n;
    }
    return 1;
}
inline double recLagCurve(double full, double e) { return std::fabs(full * e); }
// --offafter (default on since 2026-10): an object whose recorded row turns it OFF on tick t is still there for tick t's collision
// pass. The recorder writes a row after the tick's trigger pass, where an x-crossing Toggle switches its group, and that
// pass comes after the collisions -- the same reason a recorded ON lands the tick after its row (the PORT / SPEED rule
// below). A custom level t=15,447: blocks 12500 / 12501 (group 217, carried along with the player) read off on row 15,447;
// GD's cube stands on 12501's top at that row's position (736.074 = 706.074 + 30) with vy 0 and falls from 15,448, where
// the model let it fall a tick early. The rows' positions are the tick's own (the moves come before the collisions).
inline bool g_offAfter = true;
// --trigsupply-all (gd-04, T29): the shadow's on/off of an object for the tick being placed (trigshadow_live.hpp supplyOnHook). Null without the supply. Read where the per-tick `on` is made, for an object the
// recording knows nothing about (one row: its state at load): its collision-time state is the shadow's disable counter before this tick's step (a Toggle that fires in step c acts on the collisions of step c + 1).
inline bool (*g_supplyOnHook)(int uid, int& on) = nullptr;
// K1 (Dynamics::sqHead): the answers g_supplyOnHook would give for the movers `uids` (0 off, 1 on, 2 no answer) are kept in `v`, one per uid, from call to call; the hook below brings `v` to the group
// being placed and lists (in `changed`, as places in `uids`) the movers whose answer is not the one `v` held. `st` is the reader's own state (search_dp.hpp supplyOnSync); a copy of a mirror starts clean.
struct SupOnMirror {
    std::vector<uint8_t> v;
    std::shared_ptr<void> st;
    SupOnMirror() = default;
    SupOnMirror(const SupOnMirror&) {}
    SupOnMirror& operator=(const SupOnMirror&) { v.clear(); st.reset(); return *this; }
};
inline void (*g_supplyOnSyncHook)(SupOnMirror& m, const std::vector<int>& uids, std::vector<uint32_t>& changed) = nullptr;
// --supobs (default off, observation only; search_dp.hpp supObsGroup): of the movers of a frame-0 group's lists that the supply placed, what would differ if the model's formula did not run
// for them. Two counts: the half size (hw / hh, which the supply does not write) against the object's first row (its size at load) and against that size times the shadow's own scale;
// and the movers whose supplied x lies outside their list's window (they are in the list only by the place the model's formula gave them).
inline bool g_supObs = false;
struct SupObsStat {
    long long placed = 0, noRow = 0, sizeLoad = 0, sizeDiff = 0, sizeDiffScaled = 0, sizeScaledMatch = 0, sizeDiffUnscaled = 0, scaledButLoad = 0, outWin = 0, outWinBy[5] = {};
    double maxDiff = 0.0; int maxDiffUid = -1;
};
inline SupObsStat g_supObsStat;
// The on/off to use at tick `tt` for row `k` of a recording (the row in force at tt).
inline uint8_t recOnAt(const std::vector<DynSample>& sm, size_t k, int tt) {
    if (g_offAfter && sm[k].on == 0 && sm[k].t == tt && k > 0 && sm[k - 1].on) return 1;
    return sm[k].on;
}
inline int recLagUpN(int bestN, int lag, double full, double moved, double dur, int ease,
                     double erate) {
    // (always on: the switch --reclagup is removed)
    // --reclagup (removed) (default off): the level loader inverts the first row's displacement for the closest
    // n <= lag, on the ground that a row cannot come later than the count above. It can, by one tick:
    // the recorder compares FLOAT positions (grouptrace kEps = 0.05f), and where dy*e(n) sits on 0.05
    // itself the double test above counts it while the float difference falls short. Custom level, Move
    // uid 9344 (EaseInOut rate 2, 1,680 ticks, -90 px) on uid 4498 at y=705: 90*e(28) is
    // 0.050000000000000003 in double but |float(705 - 90 e) - 705| = 0.04998779, so the first row is
    // k=29 (0.054 recorded) where the count says 28, and the closed form ran one tick behind GD's rows
    // (the start fitted to all of them is 14,944, worst 0.0007 px; 14,945 is out by 0.079). The flipped
    // mini cube landing under that block sat 0.065 px high, inside the ramp beside it, took the ramp
    // and launched off the block's end where GD fell. With the flag the inversion also takes lag+1,
    // only when it is clearly the closer one: its error under half of lag's, and lag's over the rows'
    // own 3-decimal rounding.
    if (bestN != lag || dur <= 0.0) return bestN;
    const double eL = std::fabs(recLagCurve(full, gdEase(ease, erate, (double)lag / dur)) - moved);
    const double eU = std::fabs(recLagCurve(full, gdEase(ease, erate, (double)(lag + 1) / dur)) - moved);
    return (eL > 0.001 && eU < 0.5 * eL) ? lag + 1 : bestN;
}
inline int g_rotLagN = 0;   // objects the flag gave a lag (the CLI prints it)
inline int rotRecordLag(double totalDeg, double durTicks, int ease, double erate) {
    if (durTicks <= 0.0 || std::fabs(totalDeg) < 0.05) return 0;
    for (int n = 1; n <= 240; ++n)
        if (std::fabs(totalDeg * gdEase(ease, erate, (double)n / durTicks)) >= 0.05) return n;
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
// Always on (was --lockmod): a Move locked to the player's x moves its group by the player's step TIMES the Move's m_moveModX (prepareMoveActions,
// modifiers.hpp's --locknodx note: 'the player's delta times the move's mod'). The model took the multiplier as 1. A custom level's thorns ride a duration-15 s lock with a mod of
// -0.07 (a parallax): GD's recorded x runs -7.0% of the player's step (ratio -0.0699 / -0.0701 over two 200-tick windows, 1.3 and 1.6 px a tick), the model's +100% put them 16 px
// further from the recording every ten ticks. Dumps without the movemodx column read 1.0, so they are untouched.
// Always on (was --multilock): an object that several x-crossing Moves lock to the player's x takes ALL of them, each from its own crossing for its own window
// (the loader kept the first and counted the rest as 'two locks on one object: unmodelled'). GD adds the player's step once per open lock, so while two windows overlap the
// object runs at twice the player's speed (a custom level's platform: 2.466 a tick for 23 ticks) and the second lock goes on following after the first one closed.
template <class A> inline double lockMod(const A& a) { return a.lockModX; }
struct AutoTrig {
    int uid = 0;              // reporting only
    double cx = 0;            // player x that fires it
    double cy = 0;            // --autotrigframe: its y, for the travel coordinate of a turned channel (g_autoTrigFrame)
    bool chanStatic = false;   // --chanmove (removed): a Toggle --chantog would have dropped (used when the rotation queue is refused)
    int chan = 0;             // --chanmove (removed): the trigger channel a Move sits on; it fires only while State::rotChan is that channel (0 = the crossing alone)
    std::vector<TrigCtl> ctl;
    // Resolved TRUE effect tick: the last tick the object is still at rest is
    // fireT-1, and gdEase((t-fireT)/dur) is its position from fireT on.
    int fireT = -1;
    // The player's x when it fired, for the lockToPlayer term (see TrigCtl).
    // Resolved with fireT; behind a re-anchor only `cx` itself is known, which
    // is the crossing x to within one tick of travel.
    double fireX = 0;
    // --autoown: the most ticks any state of the frontier that fired it could cross it after the leader, each at
    // its own speed then (model.hpp fireAutoTriggers). 0 for a trigger fired before the search or by a single
    // state: every group of the call shares that crossing.
    int fireSpreadT = 0;
    // Where the player was on the last tick this trigger's lock was still open.
    // Trails px while the window runs so the object can hold that x afterwards.
    double lockLastX = 0;
    // --lockmod (triggers.hpp): the lock's Move m_moveModX, the multiplier on the player's step (1 unless the level sets one)
    double lockModX = 1.0;
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
    // --anchormove (removed): the root is a Toggle, whose on/off the recording reads at the crossing.
    uint8_t toggleRoot = 0;
    // The objects a Toggle in this chain switches, which way, and how many ticks after fireT
    // (the chain's spawn delays) -- the root's own switch when the root is a Toggle, and every
    // spawned one below it. Read by the on/off events (autoOnEvents); nothing else.
    struct Tog {
        int uid;
        uint8_t on;
        double delay;
        int grp = 0;   // the group the Toggle switches (read by --tapdisarm, Dynamics::disarmEv)
    };
    std::vector<Tog> tog;
    // --autostop: this entry is an x-crossing STOP (id 1616), and these are the Moves in its
    // target group that it halts (empty for every other entry). Its delay is 0: fireT is the
    // crossing tick itself, the last tick a halted Move still advances.
    std::vector<int> stopMoves;
    // A ROTATE root (id 1346): its whole angle (times360 * 360 + degrees), its duration in ticks,
    // its easing and whether the objects it turns keep their facing. rotAngleOk is false when the
    // dump predates the times360 column (the angle is then not known). Read by --rotlag.
    double rotTotal = 0.0, rotDurT = 0.0, rotErate = 2.0;
    int rotEase = 0;
    uint8_t rotLock = 0, rotAngleOk = 0;
    // --boxarm: a Move that is switched off at its crossing unless a box has switched its group back on first
    // (triggers.hpp loadAutoTriggers). Its fire tick is read through autoFireT. Only ever set under the flag.
    uint8_t offInit = 0;
    // --rotqtoggle: an x-crossing Toggle that switches a rotation-queue entry (a 2899 / 2900), kept even when it moves nothing else (its fireT is what the queue's skip test reads, frames.hpp
    // g_rotQAutoTog). Behind a turned anchor it is dated by the world x (cli.hpp, before the anchor loop), not by the turned frame's travel coordinate.
    uint8_t rotQOnly = 0;
};
inline std::vector<AutoTrig> g_autoTrig;
// --autotrigframe (ON since 2026-10-24; --no-autotrigframe is the off arm; works through --chanmove (removed)'s channelled Moves, so it does nothing without it): a channelled trigger is crossed along
// the TRAVEL AXIS OF ITS CHANNEL, not along world x. GD keeps the position-triggered objects of a gameplay-rotation channel in a list of their
// own, sorted along that channel's direction (LevelTools::sortChannelOrderObjects, 0x32d6b0: by x for direction 0, by y for 1, by -y for 2, by -x
// for 3, the comparators at 0x324c80 / 0x324dc0 / 0x324e60 / 0x324d20), and GJBaseGameLayer::checkSpawnObjects (0x21a8f0) walks the list of the
// ACTIVE channel with a cursor: the head fires when the player's x has reached its x -- or, when the player is sideways (the flag at +0x9c3),
// its y has reached the object's y -- and the walk stops at the first one that has not been reached. Nothing else is asked: no distance across
// the lane. --chanmove (removed) already restricted a channelled Move to the states that have its channel up, but compared the state's travel
// coordinate (State::xAbs, which in a turned frame is world y or -x) with the trigger's WORLD x. Measured on a custom level (the cold's
// iteration 5, channel 1 = gameplay frame 3 from t=13,399): the Move uid15790 at (21345, 4815) moves group 277 by +120 px in x over 840
// ticks. The ship crosses y=4815 at t=15,341 and the group's first recorded motion is t=15,343; the model took the trigger for one still ahead
// (the anchor's 5,214 against 21,345), left the block at its base 21,765 and clamped the ship on it at t=15,726, where GD flew on (the block 55
// px further on) and died at t=15,946. With the flag the trigger is read at (u, v) = toFrame(frame, cx, cy) in the frame of the state that has the
// channel up, in the search, in the replay, and behind the anchor (the anchor's channel is --start's rotChan; the x track's rows of that frame
// are read from the dump's world x, y and gframe, g_xTrackW). Frame 0 is the old comparison. Not converted: the head-of-list blocking (a
// trigger that is not reached holds back the later ones of its channel -- the model's scan only skips), --witnessauto's rollout, boxarm's offInit crossing,
// and a trigger of channel 0 (GD walks only the active channel's list, so while a turned channel is up the channel-0 list is not read at all).
inline bool g_autoTrigFrame = true;
// --xtrack's other columns, parallel to g_xTrack: the dump's world x, world y and gframe at each row (empty when the file has no y or gframe column).
inline std::vector<std::array<double, 3>> g_xTrackW;
// --xtrackchan (default off): --xtrack's `rotch` column, GD's active rotation-queue channel on each row (the dump's rotch), parallel to g_xTrack
// (empty when the file has no such column). A channelled auto trigger (AutoTrig::chan != 0) fires only while the player is in its channel, so the
// anchor dates one from the first row IN THAT CHANNEL with x >= cx, as the live walk does (cli.hpp, model.hpp), and not from the first row past cx.
// lv22 14338: a Move on channel 10 at x=15,945 is crossed in channels 0 / 5 / 7 / 9 at ticks 11,162 / 11,727 / 12,754 / 12,949 and fires only at 14,338
// (the block it moves, uid 11018, starts at 14,340 in GD); the first-row read dated it 11,163, the move was 1,677 ticks done at an anchor of 14,338,
// and the block stood 207 px too high -- a false death at t=14,636.
inline std::vector<int> g_xTrackCh;
inline bool g_xTrackChan = false;
// --xtrackworld (default off): read --xtrack's `y` and `gframe` columns (g_xTrackW). The mod writes them since p9xtrackcols, and --autotrigframe would start dating a turned-frame anchor's
// triggers from them: a default run's loop changed because the file grew columns. Without this flag (or --trigshadowworld, which reads them for the shadow) the columns are dropped after the parse, as if the file had the old four.
inline bool g_xTrackWorld = false;
// --autostop (on by default since 2026-10-06; --no-autostop is the off arm): an x-crossing Stop
// halts the Moves in its target group where the player crosses it. The model read a Stop only
// inside a touch chain (g_touchStops), so an autonomous one reached nothing and its Moves ran to
// the end. Custom level, x=17,655 (Stop
// uid 24193, target group 88): group 88 holds four Moves, one of them the lockToPlayerX Move
// uid 23832 (x=17,415, 1 s); GD's recording of the locked block uid 23983 follows the player
// up to t=10,014 -- the tick the player crosses x=17,655 -- and holds from 10,015, while the
// model kept it under the player for the lock's whole 240 ticks.
inline bool g_autoStop = true;
// --autospread (print only, model.hpp fireAutoTriggers): the triggers fired by the frontier's leader
// while states were still behind them, open until none is.
inline bool g_autoSpread = false;
struct AutoSpreadOpen { size_t i; long long leadT; size_t behind, of; };
inline std::vector<AutoSpreadOpen> g_autoSpreadOpen;
// Per g_autoTrig entry: how many ticks after its fire its movers can still be moving.
inline std::vector<double> g_autoRun;
struct AutoOwnGuard {
    std::vector<std::pair<size_t, int>> saved;
    AutoOwnGuard() = default;
    AutoOwnGuard(const AutoOwnGuard&) = delete;
    AutoOwnGuard& operator=(const AutoOwnGuard&) = delete;
    ~AutoOwnGuard() {
        for (const auto& p : saved) g_autoTrig[p.first].fireT = p.second;
    }
};
inline long long g_autoOwnT0 = -1;   // the search's start, set by search_dp.hpp
inline void autoOwnApply(AutoOwnGuard& G, double gx, double gdx, long long t) {
    if (!(gdx > 0.0)) return;
    if (g_autoRun.size() != g_autoTrig.size()) {
        g_autoRun.assign(g_autoTrig.size(), 0.0);
        for (size_t i = 0; i < g_autoTrig.size(); ++i)
            for (const TrigCtl& c : g_autoTrig[i].ctl)
                g_autoRun[i] = std::max(g_autoRun[i],
                                        c.durTicks + (double)c.spawnLag + c.lockTicks);
    }
    for (size_t i = 0; i < g_autoTrig.size(); ++i) {
        AutoTrig& A = g_autoTrig[i];
        if (A.fireT < 0) continue;
        // (always on: the switch --autoownpre is removed)
        // The own crossing, worked back at the group's present speed, is capped at the trigger's fireSpreadT past the
        // leader's: no state of the frontier that fired it was further behind than that. The present speed is the speed
        // since the crossing only if it has not changed: a custom level's group sped up from 1.05 to 1.30 px a tick after
        // its crossings of triggers at x 1,065..2,355 (fired before the search, a spread of 0), the uncapped reading put
        // them up to 305 ticks late, its movers sat where they had been, and that half of the frontier died where the
        // leader's ticks kept it alive.
        // --autoownpre (removed): ...but a trigger the anchor already had fired (fireT at or before the search's own start, `t0`)
        // and that lies AHEAD of the group's x keeps that tick. Only a level that turns the player round puts a fired
        // trigger ahead of the player -- the anchor dated it from the recording because the run crossed it on an earlier
        // pass (cli.hpp, sharedPast) -- and "unfired while it has not reached it" then put its movers back to where they
        // were before that pass. SubZero 4002 coin, anchor t=24,705 at x 29,514 running right: Toggle uid 17712
        // (x 30,975, fired at 21,908 on the way out) anchors slabs uid 16501/16509 that a Move dropped 600 px at 22,619;
        // the search kept them up (fired=0) and its plans fell through where every replay of them stood, so the loop
        // flew the same dying plan five times.
        if (g_autoOwnT0 >= 0 && gx < A.cx && (long long)A.fireT <= g_autoOwnT0)
            continue;   // --autoownpre (removed) (above)
        int own = -1;
        if (gx >= A.cx) {
            const long long k = (long long)std::floor((gx - A.cx) / gdx);
            own = std::min((int)(t - 1 - k) + A.delay, A.fireT + A.fireSpreadT);
            if (own <= A.fireT) continue;                          // not behind the leader
            if ((double)(t - own) > g_autoRun[i] + 2.0) continue;  // its movers are done for it too
        }
        if (g_trigDbgT >= 0 && t == g_trigDbgT)   // --trigdbg <tick>: what this group's placement reads
            std::printf("autoown t=%lld gx=%.1f gdx=%.3f trig=%zu cx=%.1f fireT %d -> %d spread=%d run=%.0f\n",
                        t, gx, gdx, i, A.cx, A.fireT, own, A.fireSpreadT, g_autoRun[i]);
        G.saved.push_back({i, A.fireT});
        A.fireT = own;
    }
}
// ...and, per g_autoTrig entry, the Stop entries that name its root (built with g_autoTrig,
// cli.hpp). Empty under --no-autostop.
inline std::vector<std::vector<int>> g_autoStopBy;
// --dynobbrot (on by default since 2026-10-06; --no-dynobbrot is the off arm): a moving hazard that
// a trigger also turns takes its turned box (the --obb corners, bhw/bhh along bc/bs) at the
// recording's angle for the tick, not at the angle it was exported with. It reads the recording's
// angle, so a closed-form hazard with no recorded row is not covered. The box was only ever loaded once, so a hazard whose rotation changed kept
// testing the player against its placed orientation. Custom level, spike uid 23082 (id 8, scaled
// to a 24 x 311 box, placed at rot 69, turned to 82.4 by t=16,121 while its group carries it):
// GD kills the wave at t=16,121, 4.84 px past the long face at the recorded angle (the wave's
// half 5 turned by 33.8 deg reaches 7.0); at the export angle the same face is 27.69 px away,
// and the model died 8 ticks later, on the static box.
inline bool g_dynObbRot = true;
// The tick a halted Move last advances (the earliest resolved Stop naming entry `k`), or
// 1 << 30 when nothing stops it.
inline int autoStopT(int k) {
    int st = 1 << 30;
    if (k < 0 || (size_t)k >= g_autoStopBy.size()) return st;
    for (const int s : g_autoStopBy[(size_t)k]) {
        const int f = g_autoTrig[(size_t)s].fireT;
        if (f >= 0) st = std::min(st, f);
    }
    return st;
}
// --tapdisarm (g_tapDisarm): per g_autoTrig entry, the boxes whose chains switch its trigger (TouchTrig::trigTog,
// tied by the CLI): the box's bit, which way, and how many ticks after the box fires. Empty without the flag.
struct AutoDisarm {
    int box;
    uint8_t on;
    double delay;
};
inline std::vector<std::vector<AutoDisarm>> g_autoDisarm;
// dupbatch (search_dp.hpp): which of a group's touch fire ticks a placement READ. The group's ticks are the one input of its tick that the batching key does not hold, so two groups with one key
// build one tick if they agree on every tick the build read: the build is a function of its inputs, and the first read that returns another value is where two runs of it can part. The search
// hands a set to the frame-0 Dynamics for a leader's prepare (Dynamics::fireSink); the per-box array and the scalar (the latest tick of any box) are read through the two wrappers below, which note
// the read when the object being placed is one the set is kept for (Dynamics::tickTrack). Without a set -- every caller but that one -- a read is the plain load it was.
struct FireReadSet {
    std::atomic<uint64_t> box[(kTouchBits + 63) / 64];   // (objects are placed across the pool)
    std::atomic<bool> scalar{false};
    std::atomic<bool> all{false};                        // the build took something this set cannot follow (the previous group's place carried over): every tick counts as read
    bool everyObject = false;                            // set by the caller before the build: note every object's reads, whatever Dynamics::tickTrack says (a turned frame: nothing is supplied there)
    FireReadSet() { clear(); }
    void clear() { for (auto& w : box) w.store(0, std::memory_order_relaxed); scalar.store(false, std::memory_order_relaxed); all.store(false, std::memory_order_relaxed); everyObject = false; }
    void mark(size_t b) {
        if (b >= (size_t)kTouchBits) return;
        std::atomic<uint64_t>& w = box[b >> 6];
        const uint64_t bit = 1ull << (b & 63);
        if (!(w.load(std::memory_order_relaxed) & bit)) w.fetch_or(bit, std::memory_order_relaxed);
    }
    void markScalar() { if (!scalar.load(std::memory_order_relaxed)) scalar.store(true, std::memory_order_relaxed); }
    bool test(size_t b) const { return (box[b >> 6].load(std::memory_order_relaxed) >> (b & 63)) & 1; }
    int count() const { int n = 0; for (int b = 0; b < kTouchBits; ++b) n += test((size_t)b) ? 1 : 0; return n; }
};
struct FireTicks {
    const uint16_t* p = nullptr;
    FireReadSet* sink = nullptr;
    FireTicks() = default;
    FireTicks(const uint16_t* q, FireReadSet* s = nullptr) : p(q), sink(s) {}   // (implicit: every caller with the plain array)
    explicit operator bool() const { return p != nullptr; }
    uint16_t operator[](size_t b) const { if (sink) sink->mark(b); return p[b]; }
};
struct FireHi {
    int v = -1;
    FireReadSet* sink = nullptr;
    operator int() const { if (sink) sink->markScalar(); return v; }
};
// Is entry k's trigger switched off where the player crosses it (fireT less its delay), in a state with this
// mask and these fire ticks? The last switch to land by the crossing decides; none = on, as placed. A switch
// landing on the crossing's own tick counts (not measured). GD counts the switches (toggleGroup's +0x4c0, off
// while negative), so two offs need two ons; the last one is enough for the chains seen so far.
// --boxarm: an entry marked offInit starts OFF instead, so with no box tied to it, or in a state that has entered
// none (fireB null), it is switched off -- a box outside the touch window (the CLI ties only the first kTouchBits)
// must not let it fire.
inline bool autoDisarmed(int k, TouchMask mask, FireTicks fireB) {
    const bool offInit = k >= 0 && (size_t)k < g_autoTrig.size() && g_autoTrig[(size_t)k].offInit;
    if (!fireB || k < 0 || (size_t)k >= g_autoDisarm.size() || g_autoDisarm[(size_t)k].empty()) return offInit;
    const AutoTrig& A = g_autoTrig[(size_t)k];
    if (A.fireT < 0) return false;
    const double cross = (double)(A.fireT - A.delay);
    double best = -1e300;
    uint8_t on = offInit ? 0 : 1;
    for (const AutoDisarm& d : g_autoDisarm[(size_t)k]) {
        if (d.box < 0 || d.box >= kTouchBits || !(mask & touchBit(d.box))) continue;
        const double te = (double)fireB[d.box] + d.delay;
        if (te > cross || te < best) continue;
        best = te;
        on = d.on;
    }
    return on == 0;
}
// --boxarm: entry k's fire tick as THIS state sees it -- its fireT, or -1 for an offInit entry no box of the state
// has switched back on by its crossing. Every placement that clocks a Move off fireT reads it through here; an
// entry that is not offInit (every entry without the flag) reads fireT itself.
inline int autoFireT(int k, TouchMask mask, FireTicks fireB) {
    const AutoTrig& A = g_autoTrig[(size_t)k];
    if (A.fireT < 0 || !A.offInit) return A.fireT;
    return autoDisarmed(k, mask, fireB) ? -1 : A.fireT;
}

// ---- an object's on/off as a sequence of switches ----------
// One switch of an object's on-state: a Toggle (or an activate) that some trigger's chain
// reaches, taking effect `delay` ticks after that trigger fires. A box (`box` >= 0, the
// State::trig bit) fires at the state's own fireB[box], and only in a state that entered it. An
// x-crossing trigger (`autoIdx` >= 0, its g_autoTrig index) fires at that trigger's fireT, a tick
// the whole level shares and that the search resolves as it goes -- so it is read when the event
// is resolved, not copied in. Otherwise `at` is the fire tick (-1 = never).
//
// A touch mask alone cannot say this. It is monotone -- a bit that is set stays set -- so it has
// no way to express a switch-off later in the same chain, two chains fighting over one object,
// or a delay before either takes effect. Nothing reads these yet.
struct OnEvent {
    int box = -1;
    int at = -1;
    uint8_t on = 1;       // 1 = the object comes on, 0 = it goes off
    double delay = 0.0;   // ticks after the source fires (the chain's spawn delays)
    int autoIdx = -1;
    int grp = -1;         // the group the switch acts on (--tapdisarm's resolveOnGroups; -1 = not known)
};

// --offafter: an x-crossing Toggle's OFF switches the object out of the tick AFTER its trigger pass, as its recorded row
// does (recOnAt)
inline double offAfterLag(const OnEvent& e) {
    return (g_offAfter && e.autoIdx >= 0 && e.on == 0) ? 1.0 : 0.0;
}
// The object's on-state at tick t: `init` (its on/off right after the game's reset) until
// something switches it, then the event with the LATEST effective tick (fire + delay) among those
// that have taken effect by t. On a tie the one later in the list wins, which is chain order --
// NOT known to be the game's order, so a tie between two sources is a place to
// measure, not to trust.
inline bool resolveOn(bool init, const OnEvent* ev, size_t n, TouchMask mask,
                      FireTicks fireB, int t) {
    bool on = init;
    double best = -1e300;
    for (size_t k = 0; k < n; ++k) {
        const OnEvent& e = ev[k];
        double f;
        if (e.box >= 0) {
            if (e.box >= kTouchBits || !fireB || !(mask & touchBit(e.box))) continue;
            f = fireB[e.box];
        } else if (e.autoIdx >= 0) {
            if ((size_t)e.autoIdx >= g_autoTrig.size()) continue;
            const int ft = g_autoTrig[(size_t)e.autoIdx].fireT;
            if (ft < 0) continue;
            if (autoDisarmed(e.autoIdx, mask, fireB)) continue;   // --tapdisarm
            f = ft;
        } else {
            if (e.at < 0) continue;
            f = e.at;
        }
        const double te = f + e.delay + offAfterLag(e);   // --offafter
        if (te > t || te < best) continue;
        best = te;
        on = e.on != 0;
    }
    return on;
}
// --recendtail: the same, from `init` as it stood at tick `after` -- only the switches that land after it.
inline bool resolveOnAfter(bool init, int after, const OnEvent* ev, size_t n, TouchMask mask,
                           FireTicks fireB, int t) {
    bool on = init;
    double best = -1e300;
    for (size_t k = 0; k < n; ++k) {
        const OnEvent& e = ev[k];
        double f;
        if (e.box >= 0) {
            if (e.box >= kTouchBits || !fireB || !(mask & touchBit(e.box))) continue;
            f = fireB[e.box];
        } else if (e.autoIdx >= 0) {
            if ((size_t)e.autoIdx >= g_autoTrig.size()) continue;
            const int ft = g_autoTrig[(size_t)e.autoIdx].fireT;
            if (ft < 0) continue;
            if (autoDisarmed(e.autoIdx, mask, fireB)) continue;   // --tapdisarm
            f = ft;
        } else {
            if (e.at < 0) continue;
            f = e.at;
        }
        const double te = f + e.delay + offAfterLag(e);   // --offafter
        if (te <= (double)after || te > t || te < best) continue;
        best = te;
        on = e.on != 0;
    }
    return on;
}

// --tapdisarm: the object's on/off the way GD keeps it, by GROUP. toggleGroup (0x223bc0) acts only when the
// group's bit (the effect manager's +0x4f8) differs from the request, and then moves a count on every object in
// the group (+0x4c0: +1 on, -1 off), the object being off while the count is negative. So the object is off
// while any group it sits in is off, and a group is as its last switch left it (on until one lands). An event
// with no group (-1) stands for a group of its own. resolveOn's one-list form cannot say this: a custom level's
// spike sits in two switched groups, and switching one back on left it out in GD.
inline bool resolveOnGroups(const OnEvent* ev, size_t n, TouchMask mask, FireTicks fireB, int t) {
    struct G { int grp; double best; uint8_t on; };
    G gs[16];
    size_t ng = 0;
    for (size_t k = 0; k < n; ++k) {
        const OnEvent& e = ev[k];
        double f;
        if (e.box >= 0) {
            if (e.box >= kTouchBits || !fireB || !(mask & touchBit(e.box))) continue;
            f = fireB[e.box];
        } else if (e.autoIdx >= 0) {
            if ((size_t)e.autoIdx >= g_autoTrig.size()) continue;
            const int ft = g_autoTrig[(size_t)e.autoIdx].fireT;
            if (ft < 0) continue;
            if (autoDisarmed(e.autoIdx, mask, fireB)) continue;
            f = ft;
        } else {
            if (e.at < 0) continue;
            f = e.at;
        }
        const double te = f + e.delay + offAfterLag(e);   // --offafter
        if (te > t) continue;
        const int key = e.grp >= 0 ? e.grp : -2 - (int)k;
        size_t j = 0;
        while (j < ng && gs[j].grp != key) ++j;
        if (j == ng) {
            if (ng == 16) continue;   // more groups than any object seen; the rest are left out
            gs[ng++] = {key, -1e300, 1};
        }
        if (te < gs[j].best) continue;   // a tie goes to the later in the list, as in resolveOn
        gs[j].best = te;
        gs[j].on = e.on;
    }
    for (size_t j = 0; j < ng; ++j)
        if (!gs[j].on) return false;
    return true;
}
// The switches the x-crossing triggers give each object (AutoTrig::tog), by object uid, each
// tied to its trigger's index in `autos` -- which has to be the final g_autoTrig, sorted, since
// the index is how the event finds its fire tick.
inline std::unordered_map<int, std::vector<OnEvent>> autoOnEvents(
        const std::vector<AutoTrig>& autos) {
    std::unordered_map<int, std::vector<OnEvent>> out;
    for (size_t a = 0; a < autos.size(); ++a)
        for (const AutoTrig::Tog& g : autos[a].tog) {
            OnEvent ev;
            ev.autoIdx = (int)a;
            ev.on = g.on;
            ev.delay = g.delay;
            ev.grp = g.grp;
            out[g.uid].push_back(ev);
        }
    return out;
}
// Travel coordinate of each touch box, by its bit in State::trig (filled by
// loadTouchTriggers). ownTouch's proximity gate reads it: the one measured
// punch pair (box ~3,255..3,300 lifting uid18119 at 3,486) is 230px apart,
// while the pre-band grazes that phantomed the whole switch band sat 500+px
// from the ceiling they were credited with delaying.
inline float g_touchBoxU[kTouchBits] = {};
// --keycensus: how many times each box divided the dedupe key (search_key.hpp).
// The window's whole cost lands there -- a box in the window splits the key for
// as long as it is moving -- so this says WHICH boxes are worth their bit.
// Relaxed atomics: the layer expansion is parallel and nobody reads the tally
// until the run ends, so the interleaving does not matter, only the per-box
// order of magnitude. Off by default; the counting is one increment.
inline bool g_keyCensus = false;
inline std::array<std::atomic<long long>, kTouchBits> g_keyCount{};
// The tick box b was FIRST entered by any state of THIS solve (-1 = not yet).
// A level property like AutoTrig::fireT, and the same approximation ("the
// first group to reach it wins"). Deliberately not carried by the state: the
// state's trigT only remembers its LAST box, and the per-box delay of the
// switch band needs every punch's own tick. Bits the ANCHOR arrived with stay
// -1 -- their effect is already inside the live recording (it replayed this
// very plan's prefix), so crediting them again would double-count the delay.
// FILLED RATHER THAN LISTED. This used to be 32 literal -1s, which is a trap
// the moment kTouchBits moves: a shorter list leaves the tail at 0, and 0 here
// means "entered at tick 0" rather than "not yet". The fill cannot go out of
// step with the width.
inline std::array<int, kTouchBits> g_touchFireT = [] {
    std::array<int, kTouchBits> a{};
    a.fill(-1);
    return a;
}();
// ...and whether that first entry was made by the body the pass had at the START of the collision pass only (markTouched's preY met the box, the body the pass left did not): GD dates such a
// touch two ticks before the row its motion starts on, which is the tick the recording's own touch record (--rectouch) carries -- slamBoxRecorded's `+ 2`. [M] two measured cases: a custom level
// custom level A (box uids 64925 / 64931, the cube lands on their plate at 15,337, GD's group rows start 15,339) and custom level B (box uid 608, lands 1,298, rows 1,300).
inline std::array<char, kTouchBits> g_touchFireEntryOnly{};
// ...and only when the recording IS that attempt's (--recprov rec,anchor,reach from the mod): the
// recording behind the last --groups and the anchor's rows must be one attempt, and the recording is read
// only up to the tick it reached. The mod keeps the deepest recording, so the two can be different
// attempts, and another attempt's recording holds that attempt's switches at the same ticks -- the same
// box entered on the same tick by a different run says nothing about this run's world. Without --recprov
// (an older mod, or a recording of unknown attempt) the recording is not read.
inline bool g_recProvGiven = false;
inline long long g_recProvRec = -1, g_recProvAnchor = -1, g_recProvReach = -1;
inline bool formulaRecProvOk(long long t) {
    return g_recProvGiven && g_recProvRec >= 0 && g_recProvRec == g_recProvAnchor && t <= g_recProvReach;
}
inline std::array<int, kTouchBits> g_touchRecEntry = [] {
    std::array<int, kTouchBits> a{};
    a.fill(-1);
    return a;
}();
// --rectouchentered uid:tick,... (from the mod, kept with the recording when it adopts one): the boxes the
// RECORDING's own attempt entered, over all its rows, by bit -- -1 = not entered while it was recorded.
// g_touchRecEntry above is the anchor attempt's, which is a different attempt whenever the mod kept a deeper
// recording than the anchor's (--recprov). Given only by a mod that knows; without it nothing reads it.
inline bool g_recTouchEnteredGiven = false;
inline std::unordered_map<int, int> g_recTouchEnteredT;   // as given: uid -> tick
inline std::array<int, kTouchBits> g_recOwnEntry = [] {
    std::array<int, kTouchBits> a{};
    a.fill(-1);
    return a;
}();
// Always on (was --touchonrecentry): ...and the state's side of that comparison is the tick the anchor was told it entered the box (--touchentered, g_touchRecEntry),
// where the box was opened at the anchor. fireB of such a box is the first motion its chain showed in the recording (cli.hpp, the anchor's touch scan keeps it so as not to date the motion earlier),
// which is a different quantity from an entry: a box both attempts entered on tick 1 read as entered on tick 36 against 1, the split fell on tick 1, and an object the recording holds on from tick 1369
// (a slab an unfollowed Spawn chain switches back on) was resolved from the one switch the state knew, off. A custom level, a mini cube falling through slab uid 577 at t=2,316 that GD lands on.
// The entry counts whether or not the anchor opened the box (its mask bit): a toggle ring both attempts pressed at t=12,403 has no bit at the anchor, read as "not entered" against the recording's
// entry, and the split resolved a hazard the recording has off since 12,448 as on (a UFO/cube dying on it at 12,511 where GD lives to 12,824). A box entered after the anchor has no entry
// here and compares by its own fireB, as before.
// How many formula-driven placements --formularec served from the recording, and how many it left
// on the formula (a box the state and the attempt disagree on). Tallied on the replay (single
// thread) for the print; relaxed atomics so a threaded search may count too.
inline std::atomic<long long> g_formulaRecHit{0}, g_formulaRecMiss{0};
// uids some Rotate (1346) trigger turns. A rotation carries its group around a
// centre object, so no offset describes it and the closed form must not claim
// it -- lv21 has 200 objects that look like plain moves until you notice a
// rotate on the same group (their recorded path missed the move-only formula by
// 120 px). Filled by loadAutoTriggers, which walks the same chains.
inline std::unordered_set<int> g_rotated;
// What a Rotate does to one object, from the dump alone -- the input side of
// the computed timeline that will replace the recording for these objects.
//
// MEASURED 2026-09-01 against grouptrace, lv21 uid15367 (in the turned group,
// orbiting uid15390, both carried down 150 by the same move) over the full 960
// ticks of a -720 degree turn:
//
//   the model today, best single shift of the recording   1.65-2.18 px
//   closed form, absolute angle                           0.0142 px
//   per-tick recurrence in double                         0.0142 px
//   per-tick recurrence, position stored through float    0.0060 px
//
// The first two of those agree EXACTLY, which is what says the composition is
// not where the last hundredth of a pixel lives: it is rounding. After a full
// -720 the object should return to its entry point and GD leaves it at
// (-0.005, +0.013) -- 960 ticks of a float store (the cvtpd2ps in GD's move
// path). Hence the recurrence below, and hence `float` and not `double` for
// the carried position.
//
// lv22 uid254 (7 degrees, BounceOut, over 120 ticks) reaches 0.0940 px by the
// same route, and there the float store makes no difference at all -- 120
// ticks of 7 degrees has nothing to accumulate. Its residual is the shape of
// gdEase case 9 at rate 2, not the composition: the endpoint is exact
// (R(-7 deg) on (0,162) predicts dx=+19.75/dy=-1.21, GD gives +19.743/-1.208)
// and the object's move alone reproduces its cy to 1.208 px, which IS the
// rotation's y-component. That easing is being re-read from the binary; until
// it is, 0.0940 px is the expected value for a Bounce-eased rotate and not a
// defect to chase.
struct RotSpec {
    int trigUid = 0;       // the Rotate, for reporting and for its fire tick
    int centreUid = 0;     // the single object of the centre group
    double total = 0.0;    // t360*360 + deg -- the WHOLE angle
    double durT = 0.0;     // ticks
    int ease = 0;
    double erate = 2.0;
    int lockrot = 0;       // does the object's own facing follow the orbit?
    // Resolved at load, once the objects are placed:
    int anchor = -1;       // g_autoTrig index of the Rotate (-1 = not autonomous)
    int centreIdx = -1;    // index into dyn of the centre object
    float relX = 0.f, relY = 0.f;   // entry position MINUS the centre's entry
};
inline std::unordered_map<int, RotSpec> g_rotSpec;   // uid -> how it turns
struct RotTailPart {
    int trigUid = 0;      // the autonomous Rotate (its g_autoTrig entry gives the fire tick)
    int centreUid = 0;    // the single object of its centre group; 0 = each object turns about itself
    double total = 0.0, durT = 0.0, erate = 2.0;
    int ease = 0;
    uint8_t lock = 0;     // lock rotation: the object orbits but keeps its facing
    double trigCx = 0.0;  // the Rotate's x, for the crossing order
};
inline std::unordered_map<int, std::vector<RotTailPart>> g_rotTailParts;   // turned uid -> its Rotates
inline int g_rotTailN = 0;   // objects the loader armed (the CLI prints it)
inline std::vector<std::pair<int, double>> g_xTrack;
// --xtrack's `btn` / `mode` columns (the mod's, repair.hpp xtrackArg; one per row, -1 = the column is not in the file): the button the game held on the row (the press latch [player+0x985], the plan's input as the game took it) and the row's mode
// index. The anchor's Tap history (cli.hpp, "the Taps' presses before the anchor") is read from them.
inline std::vector<int8_t> g_xTrackBtn, g_xTrackMode;
// --xtrackwalk <trace.csv> (cli.hpp): the rows of a model walk's own trace (tick, x, y, frame) that carry --xtrack on from its last row to the anchor, for a call anchored past the end of its track.
inline std::string g_xTrackWalkPath;
// The camera track (--camtrack <file>, or camL / camW columns on --xtrack): rows "tick,x,camL,camW" of ONE attempt -- the player's x, the left edge of the view in object
// space and the width it shows (window width / zoom). An Area action centred on the screen (special -3 .. -11) reads it (areaCentreX). The screen's offset from the player
// is looked up by the player's X, not by the tick: camera triggers and zoom fire on x, and a plan that reaches an x at another tick sees the same camera there.
// Empty when neither is given. g_camRows: the player's x (running maximum), camL - x, camW.
inline std::vector<std::array<double, 4>> g_camRows;
inline bool g_camFromFile = false;   // --camtrack was given: the xtrack's columns do not replace it
inline std::vector<std::array<double, 4>> g_camXtrackRaw;   // the rows as read: x, camL, camW (tick order)
// ...made into g_camRows: x as a running maximum (so a lookup by x is a binary search) and the offsets from the player.
inline void buildCamRows() {
    g_camRows.clear();
    double xm = -1e18;
    for (const auto& r : g_camXtrackRaw) {
        xm = std::max(xm, r[0]);
        g_camRows.push_back({xm, r[1] - r[0], r[2]});
    }
}
inline constexpr double kAreaArmTol = 1.0;
struct AreaAct {
    int uid = -1, id = 0, target = 0, special = 0, dirType = 0, eInType = 0, eInBuf = -3;
    double cx = 0, len = 0, off = 0, modFront = 1, modBack = 1, deadzone = 0, moveX = 0, moveY = 0, eInRate = 2;
    bool xy = false, inbound = false;
    // a move that is not xy (its distance along ccpForAngle(angle - 90)), an Area Scale's (3008) factors and an Area
    // Rotate's (3007) angle
    double moveDist = 0, angle = 0, scaleX = 1, scaleY = 1, rot = 0;
    // --areascreen: a centre that is a point of the screen (special -3 .. -11) rather than a player; fx is the point's share of the visible width (0, .5, 1)
    bool screen = false;
    double fx = 0.5;
};
// Always on (was --areascreen): an Area action whose centre is a point of the SCREEN (processAreaEffects 0x2283e0, special -3 .. -11: the
// camera position + the window size / zoom times a fraction; -3 the middle) is placed from a camera track (--camtrack, or the xtrack's camL / camW columns): the screen's
// offset from the player where the track first reached the player's x.
// Measured on a custom level (screen-centred Area Moves, special -3, len 276, inbound), the centre taken as the camera's left edge in object space plus half the visible
// width: 837 of 1132 recorded objects reproduce their rows to 1 px (603 with the centre taken as the player), the three solid blocks a spider lands on to 0.25 px (the
// model had them at their last row, 210 px down, and the spider fell through). A camera that is held where a track ends is NOT the camera ahead -- with the track
// stopping at the anchor, the same level's plans died in their own re-simulation ("crush") and three custom levels stopped clearing -- so the mod writes the whole of the
// deepest attempt.
// Always on (was --areastart): the recording's first row (t=1) is the position before any action ran, whatever x the action sits at (a trigger at x <= 0 is crossed at the
// start, and xRefAt(0) < cx then read the base row as already acted on and left the object unarmed).
// --areaadd (default off): an Area Move acts on an object a closed form also moves. The arming test above asks the area formula alone to reproduce
// the recording, which an object with autonomous Moves never passes, so the area's displacement was left out and the model's hazard stood where
// the Moves alone put it. Measured on a custom level (hazard uid 86481, Area Move uid 86187: group 2780, y -30, ease SineInOut, len 300, off 33,
// two-sided): GD's hazard sat -4.5 / -14.9 / -24.3 / -29.9 px from the model's at t = 8,400 / 8,440 / 8,480 / 8,540, which the formula given the
// recorded player's x reproduces to 0.01 px. With the flag a closed-form object (no touch control, no lock) with Area Move actions only is placed
// at its closed-form position plus the actions' displacement for the group's x, provided the rows up to the anchor are no worse for it (the
// loader checks them against the recorded run's own x). A shape's angle / distance variation does not count against an action that moves by
// x / y (xy), nor its x / y variation against one that moves by angle and distance: GD reads only the pair the mode names.
inline bool g_areaAdd = true;
// The centre an action reads when the player is at px: the player's x, or for a screen centre that x plus the camera's offset from the player where the track first
// reached px (held at its last row past it).
inline double areaCentreX(const AreaAct& a, double px, int /*tick*/) {
    if (!a.screen || g_camRows.empty()) return px;
    size_t k = (size_t)(std::lower_bound(g_camRows.begin(), g_camRows.end(), px,
                                         [](const std::array<double, 4>& r, double b) { return r[0] < b; }) - g_camRows.begin());
    if (k >= g_camRows.size()) k = g_camRows.size() - 1;
    return px + g_camRows[k][1] + a.fx * g_camRows[k][2];
}
inline std::vector<AreaAct> g_areas;
// getEasedAreaValue: -1 the value itself, -2 the curve, a buffer index the 101-point table of the curve (linear
// between; unknown (-3) is taken as a table for a curve, the value for none). GameToolbox::getEasedValue's curves are
// the Move trigger's (gdEase).
inline double areaEase(const AreaAct& a, double u) {
    if (a.eInBuf == -1 || (a.eInBuf == -3 && a.eInType == 0)) return u;
    if (a.eInBuf == -2) return gdEase(a.eInType, a.eInRate, u);
    const float uf = (float)u;
    const int k = (int)(uf * 100.0f);
    if (k >= 100) return gdEase(a.eInType, a.eInRate, 1.0);
    const double lo = gdEase(a.eInType, a.eInRate, k / 100.0), hi = gdEase(a.eInType, a.eInRate, (k + 1) / 100.0);
    return (double)(uf - (float)k * 0.01f) * (hi - lo) * 100.0 + lo;
}
// The displacement of one action on an object whose x without area offset is `bx`, the centre P1 at `px`;
// false when it makes no call (u >= 1).
// getAreaObjectValue (direction type 1) for an object at x `bx`, the centre P1 at `px`.
inline float areaValue(const AreaAct& a, double bx, double px) {
    const float s0 = (float)(bx - (px + a.off));
    const float s = s0 * (0.f <= s0 ? (float)a.modBack : (float)a.modFront);
    const float x = s / (float)(int)a.len;
    const float dz = (float)a.deadzone;
    float r = dz == 0.f ? x : (x - dz) / (1.f - dz);
    r = r > 1.f ? 1.f : (r < 0.f ? 0.f : r);
    return a.inbound ? 1.f - r : r;
}
inline bool areaDisp(const AreaAct& a, double bx, double px, double& dx, double& dy) {
    if (a.id != 3006) return false;
    const float u = areaValue(a, bx, px);
    if (!(u < 1.f)) return false;
    const double k = 1.0 - areaEase(a, (double)u);
    if (a.xy) {
        dx = k * a.moveX;
        dy = k * a.moveY;
    } else {   // processAreaMoveGroupAction: kk * m along ccpForAngle((angle - 90) deg)
        const float ang = ((float)a.angle - 90.f) * 0.017453292f;
        const float t = (float)k * (float)a.moveDist;
        dx = (double)(t * std::cos(ang));
        dy = (double)(t * std::sin(ang));
    }
    return true;
}
// processAreaTransformGroupAction (3008): f = (S - 1)(1 - e) + 1, multiplied into the object's scale; an Area
// Rotate (3007): (1 - e) * R added to its angle. False when the action makes no call.
inline bool areaScaleRot(const AreaAct& a, double bx, double px, double& fx, double& fy, double& dr) {
    if (a.id != 3008 && a.id != 3007) return false;
    const float u = areaValue(a, bx, px);
    if (!(u < 1.f)) return false;
    const double k = 1.0 - areaEase(a, (double)u);
    if (a.id == 3008) {
        fx = (a.scaleX - 1.0) * k + 1.0;
        fy = (a.scaleY - 1.0) * k + 1.0;
    } else {
        dr = k * a.rot;
    }
    return true;
}
// Where an object with base (bx, by), unturned half sizes (ha, hb) at angle r0 stands under `acts` (g_areas
// indices) for P1 at `px`: its centre, and -- when a scale or rotate acts on it (`sized`) -- the half sizes of its
// turned, scaled box's bounding rect (getObjectRect) and its angle.
struct AreaPose {
    double cx = 0, cy = 0, hw = 0, hh = 0, rot = 0;
    bool sized = false;
};
inline AreaPose areaPoseOf(const std::vector<int>& acts, double bx, double by, double ha, double hb, double r0,
                           double px, int tick) {
    AreaPose p;
    p.cx = bx;
    p.cy = by;
    double sx = 1.0, sy = 1.0, dr = 0.0;
    for (const int k : acts) {
        const AreaAct& a = g_areas[(size_t)k];
        if (a.id == 3007 || a.id == 3008) p.sized = true;
        if (px < a.cx) continue;   // not crossed yet
        const double cpx = areaCentreX(a, px, tick);   // --areascreen
        double dx = 0.0, dy = 0.0, fx = 1.0, fy = 1.0, r = 0.0;
        if (areaDisp(a, bx, cpx, dx, dy)) { p.cx += dx; p.cy += dy; }
        if (areaScaleRot(a, bx, cpx, fx, fy, r)) { sx *= fx; sy *= fy; dr += r; }
    }
    p.rot = r0 + dr;
    if (p.sized) {
        const double th = p.rot * (3.14159265358979 / 180.0);
        const double c = std::fabs(std::cos(th)), s = std::fabs(std::sin(th));
        const double w = ha * sx, h = hb * sy;
        p.hw = w * c + h * s;
        p.hh = w * s + h * c;
    }
    return p;
}
inline int g_recEndTailN = 0, g_recEndTailTried = 0, g_recEndTailOnN = 0;   // printed only
inline constexpr int kRecEndTailTicks = 4000;     // simulated past an object's last row; held after
inline constexpr int kRecEndTailCheck = 120;      // the rows before the last one that must agree
inline constexpr double kRecEndTailTol = 0.5, kRecEndTailRotTol = 0.2, kRecEndTailOff = 4.0;
// OFF, and the reason is not the physics -- it is that ADDING a rotation in the
// placement path double-applies it. Every collidable turned object in the
// corpus HAS a recording, and a recording is GD's own answer with the orbit
// already in it; turning it again on top is simply wrong. deathref caught it
// in one run: lv21_t13304 PASS -> FAIL, the model killed at t=12,980 against
// GD's 13,416, a rotated hazard swung twice as far as it should.
//
// The population makes this decisive rather than a tuning matter. Of lv21's
// 872 computable orbits, 806 are type 7 -- DECORATION, which the loader never
// admits and which therefore costs nothing standing still; the collidable ones
// are 62 hazards + 4 (type 47), all recorded. lv22 is 264 decoration and 9
// solids, all recorded. So there is no population of "turned objects the model
// holds still": the whole set is 75 objects that already have GD's answer.
//
// What 008 is actually worth is therefore RE-TIMING, not motion: a recording
// is one run's timeline, and one shift cannot carry a rotate and a move that
// cross at different x (1.65-2.18 px). Capturing that means REPLACING those
// objects' samples with rows computed from the definitions, each trigger on
// its own fire tick -- not adding a rotation to the placement path. That is a
// different change, and it is the one to make.
inline bool g_rotCompute = false;
// Load-time self-check: replay the computed timeline against every recorded
// rotated object and report the residual. The corpus-wide version of the
// harness that measured the two objects above.
inline bool g_rotCheck = false;
// Load-time counters, so "the orbit did not arm" can be told apart from "the
// object never reached the router".
inline size_t g_rotSeen = 0, g_rotRouted = 0;
// --rotextend (default off): an autonomously turned object's samples[] go on past the last row its
// recordings hold, computed from its Rotate, to the end of the turn (level_loader.hpp, stage 1'').
// A recording ends where its attempt died, and past its last row an object holds that row, so a
// blade still sweeping stands still in the model. Measured on lv21 (coins off, the loop's recorder
// replay of iteration 2, integ f04b889): the bar of group 98 -- eight id 1582 blades turning -720
// degrees about uid 14886 over 960 ticks -- killed every attempt at t=11,271 and the recording
// stopped there; the bootstrap that would have carried it on is on another clock and left out. The
// model killed on the held row a tick late (11,273 against GD's dead row 11,272), and with the turn
// carried on it kills on 11,272, on the same blade (uid 15024).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --slideparts (default off): past the last row of its recording, an object with two or more
// autonomous moves goes on by each move's own eased increment from that move's crossing, instead
// of the recording arm's completion, which reads the moves as one (their summed offset over one
// duration, from the recording's first motion) and so finishes a sequence as a single slide.
// Measured on lv21 (coins off, rot-extend d8d3c83 with --rotextend, the recorder's replay of
// iteration 5): uid 16678 (id 1583, groups 215/216: Move +90 over 144 ticks from x=15,960, then
// Move -210 over 168 from x=16,138) is recorded to t=12,454 at y=241.614, 21 ticks into the
// second move; the completion took the -120 as 95% done and put it at 216.406 on the next tick
// and at 35.550 eight ticks later. GD killed on it at 12,454 (dead row 12,455) and the model flew
// on to 13,935.
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)

// GD's rotation, one tick of it. `sign` is the mapping from the dump's angle to
// a y-up rotation matrix, measured as -1 on both subjects (GD's positive turn
// is clockwise on screen).
//
// The position is carried as float on purpose -- see the note above; it is the
// difference between 0.0142 px and 0.0060 px on a 960-tick turn.
inline void rotStep(float& x, float& y, double cxPrev, double cyPrev,
                    double cxNow, double cyNow, double dTheta) {
    const double c = std::cos(dTheta), s = std::sin(dTheta);
    const double rx = (double)x - cxPrev, ry = (double)y - cyPrev;
    x = (float)(cxPrev + c * rx - s * ry + (cxNow - cxPrev));
    y = (float)(cyPrev + s * rx + c * ry + (cyNow - cyPrev));
}
// Always on (--no-trigclosed is gone since the flag clean-up). For an
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
// --anchornoop (on by default since 2026-10): an autonomous object whose ANCHOR is a Toggle that
// moves it nowhere, and whose recording already shows that Toggle's state on its first row, is read
// on the recording's own clock (shift 0) and casts no vote for the anchor's fire tick. The shift
// pairs the anchor's fire tick with the object's first recorded change, which is only the anchor's
// effect when the anchor's effect is IN the recording. A custom level's 17 pads (group 171) are
// anchored on a Toggle-OFF at x=-51.5 whose effect precedes the first row (`init,...,0`); the
// first change on file is the ring at x=5,914 switching them ON at t=4,771, so the pads were read
// 4,771 ticks ahead and stood at their post-x=12,800 position (+750/-75) from the anchor on.
// Dynamics::anchorUnrec.
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// --xtrack <file> (default none): GD's own x on every tick of the attempt the anchor was taken
// from, `tick,x` rows (a header naming `tick` and `x`, as gdref's csv has, is honoured). An
// autonomous trigger behind the anchor is then dated by the first tick at which GD's x reached
// it -- the definition the x estimate approximates with one dx. That estimate divides the whole
// distance by the speed the static portals give at x0, which a level whose portals sit in groups
// (routed to dyn, never in L.speeds) or off the path the player took gets wrong by hundreds of
// ticks, and no list of portals says which ones GD actually went through.
// (g_xTrack and g_camRows are declared before AreaAct, above.)
// GD's x at tick t on the track's attempt (--recendtail): the track's own row, linear between rows, and past
// either end straight on by the end's last step. 0 without a track.
inline double xRefAt(int t) {
    const auto& v = g_xTrack;
    if (v.empty()) return 0.0;
    const size_t n = v.size();
    if (t <= v.front().first) {
        if (n < 2 || v[1].first <= v[0].first) return v[0].second;
        return v[0].second - (v[1].second - v[0].second) / (v[1].first - v[0].first) * (v[0].first - t);
    }
    if (t >= v.back().first) {
        if (n < 2 || v[n - 1].first <= v[n - 2].first) return v[n - 1].second;
        return v[n - 1].second
               + (v[n - 1].second - v[n - 2].second) / (v[n - 1].first - v[n - 2].first) * (t - v[n - 1].first);
    }
    const auto it = std::lower_bound(v.begin(), v.end(), t,
                                     [](const std::pair<int, double>& a, int b) { return a.first < b; });
    if (it->first == t) return it->second;
    const auto& a = *(it - 1);
    return a.second + (it->second - a.second) * (double)(t - a.first) / (double)(it->first - a.first);
}
// ...and the first tick its x reaches `cx` (-1 = never, the track standing still at its end).
inline int xRefCross(double cx) {
    const auto& v = g_xTrack;
    if (v.empty()) return -1;
    for (size_t k = 0; k < v.size(); ++k)
        if (v[k].second >= cx) {
            if (k == 0) return v[0].first;
            // between two rows: the first whole tick at or past cx
            const auto& a = v[k - 1];
            for (int t = a.first + 1; t <= v[k].first; ++t)
                if (xRefAt(t) >= cx) return t;
            return v[k].first;
        }
    const size_t n = v.size();
    if (n < 2) return -1;
    const double step = (v[n - 1].second - v[n - 2].second) / (double)(v[n - 1].first - v[n - 2].first);
    if (!(step > 0.0)) return -1;
    return v[n - 1].first + (int)std::ceil((cx - v[n - 1].second) / step - 1e-9);
}
inline int g_recLeadsN = 0;   // objects it re-routed in the last load (printed only)
inline int g_touchRecLeadsN = 0;   // objects it re-routed in the last load (printed only)
inline int g_recAnchorXN = 0;   // objects it re-paired in the last load (printed only)
// Autonomous lag: an object moved by a touch box AND an autonomous
// trigger (Level::formula) dates that trigger from its recording with the lag of
// the AUTONOMOUS move (Dynamics::autoLag), not the touch move's (recLag). The
// recorder writes nothing until 0.05 px, and a slow ease-in takes a long time to
// get there: lv22's descending ceiling (group 265, uid18098: -480 px over 1,005.6
// ticks, ease-in rate 2.5) first shows at t=2,256 at 0.052 px, while the curve
// through that row and the rows at 2,298 (0.571) and 2,411 (6.597) starts at
// 2,230 to 0.01 px. With recLag (1, the touch move's) the trigger fired at 2,255
// and the ceiling ran 2 px high at t=2,411 and 13.6 px high at t=2,842 -- enough
// for the model to fly past two of GD's deaths.
// ON by default since 2026-09-22, as ONE unit with the mod's anchor opening from
// the recorded positions (cfg dptouchentered, --touchentered below): the old
// opening put that ceiling at its final height, far from the player, which is
// what let the kill records at GD's deaths through the nearDynObjectPtr gate; the
// recorded opening puts it where GD has it, and only with this lag is "where GD
// has it" right. The pair: cold 22/22, per-level iterations and [fp] identical
// on 21 levels, lv22 the same 26 iterations and deaths by another route.
// Always on since the 0.2.0 flag clean-up (it was --automixlag).
// --lagfit (print only): for each object the lag above re-dates, the residual of
// its first recorded rows against the autonomous and the touch curve (level_loader).
inline bool g_lagFitDbg = false;
// --lockanchor: an anchor behind an auto lockToPlayerX trigger restores the lock's
// displacement from the recording (cli.hpp, the behind-the-anchor trigger loop).
// On since 2026-09-21; always on since the flag clean-up.
// --touchentered <uid,...|->: the touch boxes (by trigger uid) the attempt's
// recorded positions overlapped by the anchor. Given, the anchor scan opens
// only these from the recording (cli.hpp, `mayOpen`). "-" = given, none.
inline bool g_touchEnteredGiven = false;
inline std::unordered_set<int> g_touchEntered;
// ...and, when the caller writes `uid:tick`, the first tick the recorded positions
// overlapped that box. The anchor scan then takes the box's fire tick from it
// instead of from the recording's first motion, which for an object that an
// autonomous Move also drives is that Move's, not the box's: lv22's trap 18092
// (entered at 2,614 in the recorded rows) was dated 2,255 -- the start of the
// ceiling's slow descent -- so its target-mode slam had long finished at a t0=2,651
// anchor where GD's ceiling is still coming down 3 px a tick.
inline std::unordered_map<int, int> g_touchEnteredT;
// --rectouch uid:tick,... (read under --slambox, constants.hpp): the touch triggers GD itself fired in the
// attempt the call's recording (--groups) comes from -- the mod's touchfire map, written by
// playerTouchedTrigger and banked with that attempt -- each dated by the attempt's whole recorded rows
// with markTouched's own test, the date --touchentered gives up to t0 (-1 = the rows never enter it), so
// that the date and the state's entry tick come from one test. Filled per call by bit into g_recTouchBit.
inline bool g_recTouchGiven = false;
inline std::atomic<long long> g_statHoldRecBoxHits{0};   // --holdrecbox: (box, tick) pairs the punch hold left out (printed only; reset per invocation)
inline std::unordered_map<int, int> g_recTouch;
inline std::array<int, kTouchBits> g_recTouchBit = [] {
    std::array<int, kTouchBits> a{};
    a.fill(-1);
    return a;
}();
// --slambox: is the step box b would add already in the recording? Only when the recorded attempt fired
// that same box and the state entered it on the recorded entry's own tick -- the same activation, delta
// 0. Another box, or the same box entered on another tick, keeps its step. The old
// rule (--slamdedup) had only a tick to go on, so it took the state's box for the recording's one.
inline bool slamBoxRecorded(const std::array<int, kTouchBits>& rec, int b, int F) {
    return b >= 0 && b < kTouchBits && F >= 0 && rec[(size_t)b] >= 0 && (rec[(size_t)b] == F || (g_touchFireEntryOnly[(size_t)b] && rec[(size_t)b] == F + 2));
}
// --dyndbg <uid>: dump one moving object's placement inputs at load (see the
// print near the fireT resolution).
inline int g_dynDbg = -1;
// --geodump <file> <t0> <t1> <stride>: every moving object's position as the model computes it (t,uid,cx,cy,hw,hh,on), for the ticks t0..t1 every
// `stride`: print only, to set the model's geometry against a recording (measurement). Off unless asked.
inline std::FILE* g_geoDumpF = nullptr;
inline long long g_geoT0 = 0, g_geoT1 = -1, g_geoStride = 1;
// The search's thread pool, lent to the per-object loops of Dynamics::seek and
// Dynamics::applyTriggers: runs task(k) for every k in [0,n) and returns when
// all have run. Set by the search for exactly as long as its pool exists, on
// the thread that owns the pool, and only called there, between its parallel
// phases (seek and applyTriggers are serial-caller-only already). Empty
// everywhere else, which is the plain loop.
inline thread_local std::function<void(size_t, const std::function<void(size_t)>&)>
    g_dynParTasks;
// How many objects the loader put on the formula path, and how many of those
// never saw either controller fire in this call. The second is the guard's
// readout: an object that is formula-driven but whose triggers never fire sits
// at its base position, which is right, but if the count is the whole set the
// classification found objects the plan cannot reach and the formula is
// carrying nothing.
inline int g_formulaDriven = 0;
// (--noformula, which kept every object on the recording path, is gone since the flag clean-up.)
// --touchretime: re-time a touch-controlled object's recording against THIS
// state's entry into the box that reaches it, instead of playing it on the
// recording run's clock (the touch branch below sets anchor = recAnchor, so
// shift is 0 by construction). Measured on lv22's block row, group 508 under
// box uid17771 (worker 98, three arms of the same plan): the row's first moving
// row is two ticks after the ball's rect first overlaps the box -- t=6534 after
// an overlap from 6532, t=6508 after an injected overlap from 6506 without a
// landing, and no motion at all through t=6546 when the ball is held clear.
// Only boxes entered at or after the solve's own start are re-timed
// (g_touchRetimeFrom): a box the anchor arrived with has its fireB seeded from
// the recording's first motion, not from an entry, and its recording is this
// run's own. Also changes how recAuto is classified (level_loader.hpp). On
// since v0.1.4, always on since the flag clean-up; lv19's doors measured a
// 5-tick latency with a spawn delay in the chain, which this does not model.
inline long long g_touchRetimeFrom = 0;
// --touchretimelag: a touch-only object carries the recorder's threshold lag
// (level_loader.hpp) and the re-timed anchor adds it, so the box entry +
// latency (the motion's start) lines up with the recording's first ROW rather
// than being taken for it. Measured against the recording on lv22 uid1214 it
// moves the playback from two ticks ahead of the recorded row to one, and
// against the reference dumps lv22 tracks 19,100 -> 19,343 ticks with the
// standing "tracking shrank at t=2,600" gone. On since 2026-09-20, always on
// since the flag clean-up. It was held off for a while because a
// cold run then routed lv22 into a hole and did not clear inside the budget --
// that was a property of the base it was measured on (c935132 plus all five
// bundle units), and it does not reproduce once the other units are in: lv22
// clears in the same 32 iterations as without it, and the other 21 levels are
// fingerprint-identical.
// --recinterp: a recording's position between two close rows is interpolated,
// not held (Dynamics::seek).
// ON BY DEFAULT since 2026-09-20, PAIRED WITH
// --stickrelease. On its own it moves 9 of the corpus's 1,116 sections, all in
// lv22, and changes no verdict; its value is that it supplies the `dcy` the
// stick re-land needs on a tick the recorder skipped. Always on since the flag
// clean-up, like --stickrelease.
inline constexpr int kRecInterpGap = 4;
// The recorder's per-channel write threshold (grouptrace.hpp kEps): a row is
// written once any of x, y, w, h, rot has moved kRecEps or more from the last
// row written.
inline constexpr double kRecEps = 0.05;
// --recinterpsize: ...and so is its size. The recorder's threshold covers w and h as it covers x and y, so a
// slowly scaling object is a staircase too, and both placements (seek and the trigger path) held hw / hh at
// the older row. A custom level's block uid 2173 grows from 27.5 to 30 px about a fixed centre under an
// elastic ease: rows at 3,572 / 3,575 / 3,580 / 3,586 (29.849 / 29.899 / 29.959 / 30.000), and a cube
// standing on it rises with its top every tick in GD (3,224.9636 at 3,573, 3,225.0093 at 3,580) where the
// model held 3,224.9546 until the next row and climbed in steps of 0.025-0.03 px. A size step is interpolated
// when it is one a slow scaler makes (under kRecEps on the half size: the recorder's threshold is on the full
// width) across a gap that is short (kRecInterpGap) or not much longer than its neighbours -- under twice
// the shorter of the gaps before and after it, which an ease's slowing rows (3, 3, 5, 6, 10 there) meet and
// a long rest between two motions does not. Positions keep --recinterp's rule.
// On by default; --no-recinterpsize is the off arm.
inline bool g_recInterpSize = true;
// Whether the gap from sm[c] to sm[c+1] is not much longer than its neighbours: under twice the shorter of
// the gaps before and after it (both must exist).
inline bool recGapLikeNeighbours(const std::vector<DynSample>& sm, size_t c) {
    if (c == 0 || c + 2 >= sm.size()) return false;
    return sm[c + 1].t - sm[c].t < 2 * std::min(sm[c].t - sm[c - 1].t, sm[c + 2].t - sm[c + 1].t);
}
// Whether the size step from sm[c] to sm[c+1] is interpolated (see g_recInterpSize).
inline bool recSizeGapOk(const std::vector<DynSample>& sm, size_t c) {
    if (!g_recInterpSize || c + 1 >= sm.size()) return false;
    return sm[c + 1].t - sm[c].t <= kRecInterpGap || recGapLikeNeighbours(sm, c);
}
// --recinterpslow: ...and seek's position past kRecInterpGap by the same test. A slow drifter's rows are
// spaced far apart all along: a custom level's ceiling bars uid 1802 / 1807 (120 x 3.9 px, drifting a few
// hundredths of a px a tick) have rows 22-31 ticks apart, and an upside-down cube standing under 1802 rises
// with it in GD (3,614.962 at t=11,407) where seek held the row of 19 ticks before (3,614.934) and the model
// landed on the neighbouring bar. A channel is interpolated only when its step is under 2 * kRecEps (a
// larger one is a rest and then a jump); a gap with no row on one side keeps the hold.
// On by default; --no-recinterpslow is the off arm.
inline bool g_recInterpSlow = true;
inline float recSizeLerp(float a, float b, double u) {
    const double d = (double)b - (double)a;
    return std::fabs(d) < kRecEps ? (float)((double)a + d * u) : a;
}
// Whether the gap between sm[c] and sm[c+1] is such a stop.
inline bool recStallGap(const std::vector<DynSample>& sm, size_t c) {
    if (c == 0 || c + 2 >= sm.size()) return false;
    const DynSample &p = sm[c - 1], &s = sm[c], &n = sm[c + 1], &q = sm[c + 2];
    const int g = n.t - s.t;
    // (always on: the switch --recstallwide is removed)
    // --recstallwide (removed): ...and the row next to the gap may be two ticks from its neighbour. An eased
    // Move's last step into its stop (and its first step out) is under kRecEps on one tick, so the row that closes
    // it comes a tick later and spans two; the rates are then per tick. A custom level's slab uid 4862 (an eased
    // back-and-forth Move): rows at 6,422 (+0.059) and 6,424 (+0.000), none until 6,462 (+0.059, then +0.073 a
    // tick). The interpolation slid it 0.0015 px a tick across the stop, 0.017 px at 6,435, where GD's robot lands
    // on its edge with 0.008 px of overlap; the model missed the landing by one tick.
    const int sp = s.t - p.t, sq = q.t - n.t, lim = 2;
    if (g < 2 || sp < 1 || sq < 1 || sp > lim || sq > lim) return false;
    const double ch[3][4] = {{p.cx, s.cx, n.cx, q.cx}, {p.cy, s.cy, n.cy, q.cy},
                             {p.rot, s.rot, n.rot, q.rot}};
    for (const auto& v : ch) {
        const double before = std::fabs(v[1] - v[0]), after = std::fabs(v[3] - v[2]);
        if (before < kRecEps || after < kRecEps) continue;
        // (always on: the switch --recstallmin is removed)
        // --recstallmin (removed): a stall needs the mover fast on BOTH sides of the gap. recStallGap compares the gap's
        // step with the MEAN of the two neighbouring rates, so a drifter that sets off into a fast eased Move reads as a
        // stall: rows 2 ticks apart at ~0.035 px a tick, then 0.574 a tick; the gap's own 0.070 px is under the mean's 0.457
        // and the interpolation is refused, holding the older row. A custom level's platform uid 32307, rows 15,014 / 15,016 /
        // 15,018 / 15,020 (377.593 .. 377.378) and 15,021 (376.804): GD's robot lands on 15,019 and stays on the platform,
        // which has moved 0.036 that tick; the model landed on the held 377.448, saw the face drop 0.07 px a tick later
        // and let go. With the minimum, the gap's rate is compared with the slower side and the same gap interpolates.
        // Used by every reader of recStallGap.
        return std::fabs(v[2] - v[1]) < (g - 0.5) * 0.5 * (std::min(before / sp, after / sq) * 2.0);
    }
    return false;
}
inline constexpr int kTouchRetimeLat = 2;
// --touchretimebox: re-time a touch box's objects against the BOX's entry, not each
// object's own first motion. Without it every object is taken to start moving
// kTouchRetimeLat (+ its lag) after the entry, which holds for an object the box moves
// directly and not for one a spawn chain reaches later. lv22's spike row under box uid 1417
// (id 392, uid 17571 and 18201..18206): the recording has them first moving at 1,755 /
// 1,766 / 1,774 -- they drop one after another -- and the re-timing pinned all of them to
// the entry + 3, so five of the seven sank 10-18 ticks early. GD kills the spider on uid
// 17571 at t=1,813 in its unmoved place; the model had already lowered it and walked on.
// With this, the recording's entry into a box is estimated ONCE per box, as the earliest
// first motion (less latency and lag) among the objects only that box moves, and each
// object keeps its own delay behind it.
// ON BY DEFAULT since 2026-09-24; --no-touchretimebox restores the per-object
// latency. kDefTouchRetimeBox is what reset.hpp writes back.
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// The one touch box whose chain locks something to the player's x, and how long
// the lock lasts in ticks. A per-BOX quantity rather than a per-object one
// because what a state has to remember is "how far have I travelled since I
// punched THAT box", and the corpus needs exactly one of them: the census over
// all 22 levels found locks down a touch chain on lv19 only, through a single
// box (26 objects, 7 of them collidable). A second box would need a second
// accumulator, so it is reported rather than silently averaged -- the same
// treatment `alockN` already gives two locks on one object.
inline int g_lockBox = -1;
inline double g_lockTicks = 0.0;
inline int touchStopTick(const TouchMask& mask, FireTicks fireB, int moverUid) {
    int st = 1 << 30;
    if (moverUid == 0 || !fireB) return st;
    const int nb = std::min((int)g_touchStops.size(), kTouchBits);
    for (int b = 0; b < nb; ++b) {
        if (!(mask & touchBit(b))) continue;
        for (const int s : g_touchStops[(size_t)b])
            if (s == moverUid) st = std::min(st, (int)fireB[b] + (0));
    }
    return st;
}
// --shiftstat: one `shiftstat:` line per moving object, the first time it is
// placed, saying which recorded row the model ends up reading for it.
//
// The re-timing (`tt = t - shift`) is PER OBJECT, so "the model is N rows behind
// GD" has no single N -- which is exactly what stopped a blanket phase fix on
// 2026-09-03 (uid 434 came out at shift=1, but shift=0 objects are a row nearer).
// This counts the distribution over the objects a collision actually consumes,
// so the size of the re-anchoring family is a number rather than an impression.
// Diagnostic only: prints and changes nothing.
inline bool g_shiftStat = false;
// --incplace (on; --no-incplace is the off arm): seek() and applyTriggers() place only the objects whose place can
// have changed since the tick before. The rest of a level's moving objects sit still for long stretches (on
// the largest levels 99.99% of a tick's pure objects end it exactly as they ended the one before), and
// placing every one of them on every tick was ~45% of a heavy search. Nothing is approximated: an object is left
// alone only where placing it again would write the bytes it already holds (see Dynamics::incBuild).
inline bool g_incPlace = true;
// --incanchor (on since 2026-10-21; --no-incanchor is the off arm): the objects an autonomous trigger moves by a closed form (the bulk of a heavy level's
// "placed on every tick" set) are placed only on the ticks something about them can change: their own rows,
// the rows re-timed to the trigger's fire tick, the window of each of their Moves, and the ticks after a date
// they read (a trigger's or a Stop's fireT) changed. Between those a placement writes the bytes it already holds.
// --incplacecheck compares the result with the plain placement tick by tick.
inline bool g_incAnchor = true;
// --incplacecheck (off): every incremental placement is also made the plain way from the same starting
// state, and the two are compared byte for byte (objects, prevCy, on). The plain result is the one kept.
inline bool g_incCheck = false;
// Bumped by the CLI after the per-call fills of the purity-relevant vectors (areaArmed, armDep), so an index
// built for a cached Level under the last call's fills is rebuilt.
inline uint64_t g_dynPlaceGen = 1;
struct IncStats {
    long long seekFull = 0, seekInc = 0, procInc = 0, objsInc = 0;
    long long atLists = 0, atFull = 0, atListObjs = 0;
    long long checked = 0, mismatch = 0;
    long long gpStep = 0;   // --incplacecheck: objects whose step alone differed where groupPrevApply writes it next (incDiff)
    long long pTicks = 0, pPlaced = 0, pWakes = 0, pWokeObjs = 0, pSlots = 0;   // --incanchor
    long long wListed = 0;   // objects a list took because the supply had written them (incSupplyWrote)
    long long pSwSlots = 0, pSwListed = 0;   // slots with switches past the recording's end (the last index built); slots a list took for a switch's tick alone
    long long builds = 0, buildFail = 0;
    double buildMs = 0;
};
inline IncStats g_incStats;
// the quiet objects (Dynamics::sqHead): eligible objects of the Dynamics built last, calls, object-visits, visits passed by, visits checked under SQCHECK (a report)
struct SqStats { std::atomic<long long> objects{0}, heads{0}, visits{0}, passed{0}, checked{0}, anchored{0}, syncs{0}, flips{0}, gated{0}, todoA{0}, listChecked{0}, touchInert{0}, touchRec{0}, trGated{0}, selMade{0}, selReused{0}, selChecked{0}, colIx{0}, colScan{0}, colLooked{0}, colChecked{0}, nsHead{0}, nsGate{0}, nsSync{0}, nsAfter{0}, nsProc{0}, nsCollect{0}, nsHits{0}; };
struct SqTimer { std::atomic<long long>& a; std::chrono::steady_clock::time_point t0; bool on; SqTimer(std::atomic<long long>& x, bool o) : a(x), on(o) { if (on) t0 = std::chrono::steady_clock::now(); } ~SqTimer() { if (on) a.fetch_add((long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(), std::memory_order_relaxed); } };
inline SqStats g_sqStats;
inline std::atomic<bool> g_sqForeign{false};   // the supply wrote an object no open Dynamics owns: nothing is passed by from then on (sqTouched)
struct Dynamics;
inline thread_local Dynamics* t_sqDyn = nullptr;   // the Dynamics whose prepareGroup ran last on this thread (the owner of the lists the supply then places)
// DYNSEEK_CHECK=1 (Dynamics::seekOne): every searched backward seek is walked too; a difference takes the walk's row and is counted
inline const bool g_dynSeekCheck = std::getenv("DYNSEEK_CHECK") != nullptr;
inline std::atomic<long long> g_dynSeekChecks{0}, g_dynSeekDiff{0};
// --phaseprof: the census of the objects placed on every tick (Dynamics::censusAlways)
struct CenStats {
    long long ticks = 0, n = 0;
    long long zone[3][2] = {};            // [near, ahead, behind][unchanged, moved]
    long long never[2] = {}, behindCatch[2] = {};
    long long idleFired = 0, idleUnfired = 0;
};
inline CenStats g_cen;
// What Dynamics keeps for the incremental placement. A copy of a Dynamics (the gameplay frames) starts clean.
struct IncState {
    uint64_t gen = ~0ull;      // g_dynPlaceGen the index was built under
    uint32_t sig = 0;          // the flags it was built under
    bool valid = false;        // the index below is good for gen / sig
    bool settled = false;      // objs / prevCy / on hold the fixed point for the next one-tick advance
    bool lastFull = false;     // the seek before was a full pass
    int evT0 = 0, evT1 = -1;   // the ticks evStart covers
    std::vector<uint32_t> always;    // placed by every incremental seek
    std::vector<uint32_t> atList;    // what applyTriggers' first placement of a tick walks (not the no-ops)
    std::vector<uint32_t> nonPure;   // what a same-tick applyTriggers walks
    std::vector<uint32_t> evStart;   // CSR over ticks evT0..evT1: the objects with a row (or a change) there
    std::vector<uint32_t> evObj;
    std::vector<uint32_t> proc;      // always + the events of tick procT (what the incremental seek of that tick placed)
    int procT = INT_MIN;             // the tick proc is for; INT_MIN after a full pass
    int runLen = 0;                  // consecutive one-tick advances since the generation began (the index is built late)
    bool failed = false;             // the build was refused for this generation
    int atT = INT_MIN;               // the tick of applyTriggers' last first placement
    // The objects whose place is the closed form at their first row (like the uncontrolled ones) but whose on / off
    // is also switched past the recording's end by x-crossing Toggles: it changes where such a Toggle takes effect,
    // which is a tick known only once the search has resolved the trigger's fireT. They are placed again when one of
    // those fireT changes and on the tick a switch takes effect (dNext), besides their rows' ticks.
    std::vector<uint32_t> dList;
    std::vector<int> dTrigs;                         // the g_autoTrig entries their switches wait on
    std::vector<std::pair<int, double>> dEv;         // (entry, delay + lag) of those switches
    std::vector<int> dFire;                          // fireT of dTrigs at the last wake
    int dNext = INT_MAX;                             // the next tick (after the last wake) a dEv switch takes effect
    bool dKnown = false;                             // dFire / dNext describe a wake that has happened
    std::vector<uint32_t> procD;                     // scratch: proc + dList
    std::vector<uint32_t> procE;                     // scratch: a list + the anchored objects K1 has to do (Dynamics::sqGateAnchored)
    // The quiet objects left out of proc (Dynamics::sqShortOk): proc was made without the stable ones of `always`; procTodo = the ones of `always` that were to do then (they are in it),
    // procCut = how many were left out, firstCut = the same for the list incAtFirstList gave last (0 = none left out), procF = scratch for that list.
    bool procShort = false;
    std::vector<uint32_t> procTodo, procF;
    size_t procCut = 0, firstCut = 0;
    // --incanchor: the closed-form anchored objects ("P" slots), in ascending object order. Per slot: the object, its
    // trigger, the ticks of its own rows (early edge, the span of its changes), the span of its changes on the recording's
    // clock, the Moves it reads (CSR) and the interval `d` the current dates put its changes in.
    struct PSlot {
        uint32_t obj = 0;
        int anchor = -1;               // g_autoTrig index of the object's controller
        bool rows = false;             // the rows are re-timed to the controller's fire tick (trigRecFire >= 0 and > 1 row)
        int rowT1 = 0, rowTL = 0;      // first change / last row on the recording's clock
        int e0 = 0, e1 = -1;           // the first row's ticks (seek and the cursor read them)
        int u0 = 0, u1 = -1;           // the span of the object's own rows, on the clock of the tick
        int d0 = 0, d1 = -1;           // the span the dates put its changes in (recomputed when a date changes)
        int force = INT_MIN;           // placed through this tick whatever the spans say
        uint32_t p0 = 0, p1 = 0;       // its Moves: pPartTrig / pPartDur [p0, p1)
        uint32_t w0 = 0, w1 = 0;       // its switches past the recording's end: pSwTrig / pSwAt [w0, w1)
    };
    // A slot's on/off switches past the recording's end (Dynamics::tailOnEv), one entry a switch and one for the recording's end itself: the g_autoTrig entry whose fire tick dates it and
    // what is added to that tick (the switch's delay and lag), or -1 and the tick itself.
    std::vector<int> pSwTrig;
    std::vector<double> pSwAt;
    // The objects the supply's placement wrote (Dynamics::incSupplyWrote): the last tick whose lists take each (INT_MIN = none), and the ones that have one, ascending once incWrote has run.
    std::vector<int> wUntil;
    std::vector<uint32_t> wList, wBuf, wBufF;
    bool wSorted = true;
    std::vector<PSlot> pSlot;
    std::vector<int32_t> pSlotOf;                    // per object: its place in pSlot, -1 = none (K1 reads it)
    std::vector<int> pPartTrig;
    std::vector<int> pPartDur;                       // ceil(dur) + 3, capped
    std::vector<std::vector<uint32_t>> pDep;         // per g_autoTrig entry: the slots reading its date (controller, Moves, Stops)
    std::vector<int> pFire;                          // each entry's fireT as the objects were last placed under it
    bool pKnown = false;                             // pFire / d describe a placement of every slot
    std::vector<uint32_t> pWoke;                     // scratch
    IncState() = default;
    IncState(const IncState&) {}
    IncState& operator=(const IncState&) { IncState fresh; *this = std::move(fresh); return *this; }
    IncState(IncState&&) = default;
    IncState& operator=(IncState&&) = default;
};
// --trigsupply-all / --trigsupply-group: the supplied movers whose place in the shadow's timeline of the group being built lies inside the group's window, (uid, x). A mover the shadow supplies is
// placed by it (search_dp.hpp supplyPlaceMovers) only if it is already in the group's lists, and collect() cuts those by the model's own place -- a mover with no recording of its own stays at its
// static place until the shadow takes it over, so one that flies a long way (lv21 uid 18528, 290 px) was never collected where it is. The hook fills the hits for [x0, x1]; null when no supply is on.
using SupHits = std::vector<std::pair<int, double>>;
inline void (*g_supplyHitsHook)(double x0, double x1, SupHits& out) = nullptr;
inline std::atomic<long long> g_supHitAdded{0};   // movers collect() added at the shadow's place (printed with the trigsupply summary)
struct Dynamics {
    // Stable storage: `snapObj` / `usedOrb` / `usedPad` hold raw Obj* across
    // ticks, so these must never reallocate after load. The rects are MUTATED
    // in place once per tick by seek().
    std::vector<Obj> objs;
    std::vector<DynRows> samples;  // parallel to objs
    // [2026-08-25] Does a trigger ROTATE this object anywhere in the recording?
    // GD's collision test branches on the object's own "is turned" flag, and a
    // rotate-target carries it for the whole run -- not only once the angle has
    // grown away from zero. See the note at turnedBox's axis-aligned branch for
    // the measurement (lv22's spider portal uid 1156). Parallel to objs; empty
    // in an old Dynamics, which reads as "never", i.e. the previous behaviour.
    std::vector<uint8_t> everRot;
    // --dynobbrot: each object's turned-box axis as loaded (bc/bs at its placed rot), so seek can
    // turn it by the recording's angle minus that. Filled on the first seek with the flag; empty
    // otherwise.
    std::vector<double> obbBc0, obbBs0;
    // --rotobblatch: per object, -1 when the latch does not apply (an exported box, or not a --rotobb object), else
    // the first recorded tick its angle differs from the placed one (INT_MAX when no recorded row turns it). Its
    // obbOk is on from that tick, or while a placement turns it (obbLatch). Filled by the level loader.
    std::vector<int> obbLatchT;
    void obbLatch(size_t i, int t, double rotDeg) {
        if (i >= obbLatchT.size() || obbLatchT[i] < 0) return;
        double d = std::fmod(std::fabs(rotDeg - objs[i].rot), 360.0);
        if (d > 180.0) d = 360.0 - d;
        objs[i].obbOk = (t >= obbLatchT[i] || d > 0.001) ? 1 : 0;
    }
    // --shiftstat only: whether this object has already printed its line.
    // Sized lazily so an old Dynamics (or a run without the flag) carries
    // nothing.
    std::vector<uint8_t> shiftSaid;
    std::vector<size_t> cur;                      // per-object cursor
    // which window each one belongs in, mirroring the static split
    // COIN is last so the existing numbering is untouched. A coin only ever
    // reaches the dynamic set when a trigger controls it (--coins).
    enum Bucket : uint8_t { NEAR, PORT, PAD, ORB, SPEED, SLOPE, COIN };
    std::vector<uint8_t> bucket;
    // FORMULA-DRIVEN: this object's position is computed from its triggers
    // rather than replayed from the recording. Set by the loader for objects a
    // touch controller AND an autonomous one both reach, where BOTH actually
    // move something. GD applies both; a recording only ever holds one
    // attempt's answer to "was this touched" -- lv19's doors are recorded open,
    // lv22's switch band recorded untouched -- so for these objects neither
    // recording is the state's world, and everything the re-timing machinery
    // does below is an approximation of a world it cannot see. The two eased
    // moves ARE that world, to 0.0007 px against all 286 recorded rows of each
    // of lv19's seven (oneoff/py/fit96.py), so an object on this path takes
    // none of it: not the dual special case, not the ownTouch hold, not the
    // autonomous re-timing.
    std::vector<uint8_t> formula;
    // --formulanolock (speed.hpp): a formula-driven object whose autonomous chain locks it to the player
    std::vector<uint8_t> formulaLock;
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
    // --obbscale: the loaded bound halves (hw, hh) and box halves (ohw, ohh) of an object, taken before its first placement overwrites them (-1 = not yet).
    std::vector<float> baseHw, baseHh, baseOhw, baseOhh;
    // --tpmove (removed) (speed.hpp): a moving teleport portal's (id 747) target y, as loaded
    std::vector<double> baseTpY;
    // ...the target y follows the portal's own move: tpY is the portal's real y plus its offset
    void carryTpY(size_t i, double dy) {
        // (always on: the switch --tpmove is removed)
        // --tpmove (removed): a teleport portal (id 747) that a group moves takes its target with it. The
        // dump's tpY is the portal's real y plus its offset at load (Obj::tpY), and the placement moved cy alone,
        // so the player was put where the target was at load. SubZero 4003 t=12,468: portal uid 4687 (group 3,
        // loaded at y 975, target 2,169) has come down 600 px when the reversed cube on a ramp walks into it;
        // GD lands it at 1,569, the model at 2,169.
        if (objs[i].type == 28 && i < baseTpY.size() && baseTpY[i] != 0.0)
            objs[i].tpY = baseTpY[i] + dy;
    }
    std::vector<uint8_t> on;   // toggled in? (0 = the object is not there)
    // --activators: for an object an activator switches (a pickup or a toggle block), its
    // on/off is not the recording's -- a recording holds one run's presses -- but its t=0 value
    // (onInit, the recording's reset snapshot) with every switch that reaches it applied in time
    // order for THIS group (resolveOn: the boxes by the group's own fire ticks, the x-crossing
    // Toggles by their fireT). Empty for every other object, for one whose recording has no
    // snapshot, and for all of them without the flag. Filled by the level loader.
    std::vector<std::vector<OnEvent>> onEv;
    std::vector<uint8_t> onInit;
    // --tapdisarm: the same for an object an x-crossing trigger switches when a box can switch that trigger
    // off (g_autoDisarm) -- read only in a state where it has (the object's on/off is its recording's
    // otherwise). Filled by the level loader; empty for every other object and without the flag.
    std::vector<std::vector<OnEvent>> disarmEv;
    std::vector<uint8_t> disarmInit;
    // ...and per object, whether anything that places it asks autoDisarmed for a trigger a box can switch off: its
    // disarmEv, or a --recendtail switch (tailOnEv) from such a trigger. Such an object depends on the group's
    // boxes, so it is not one of the pure objects placed once per tick (isPure). Filled by the level loader.
    std::vector<uint8_t> disarmDep;
    // --areamove: per object the g_areas actions that move it (empty for every other object and without
    // the flag), its position without area offset (its first recorded row, before any action runs), and whether its
    // recorded rows passed the formula (only then is it placed by it). areaPxPrev is the group's x the tick before,
    // set by prepareGroup just before applyTriggers: the area pass runs before the player's step.
    std::vector<std::vector<int>> areaOf;
    std::vector<double> areaBaseX, areaBaseY;
    // ...and its unturned half sizes and angle at the base (for an Area Scale / Rotate)
    std::vector<double> areaBaseHa, areaBaseHb, areaBaseRot;
    std::vector<uint8_t> areaArmed;
    // --areaadd: objects whose closed form takes the Area Move actions of areaOf on top
    std::vector<uint8_t> areaAdd;
    // The displacement of areaOf[i]'s actions for the group's x on an object standing at x `bx` without area offset.
    bool areaAddDisp(size_t i, double bx, double& dx, double& dy) const {
        dx = dy = 0.0;
        if (i >= areaAdd.size() || !areaAdd[i]) return false;
        for (const int k : areaOf[i]) {
            const AreaAct& a = g_areas[(size_t)k];
            if (areaPxPrev < a.cx) continue;   // not crossed yet
            double ax = 0.0, ay = 0.0;
            if (areaDisp(a, bx, areaCentreX(a, areaPxPrev, areaTickPrev), ax, ay)) { dx += ax; dy += ay; }
        }
        return true;
    }
    double areaPxPrev = 0.0;
    int areaTickPrev = 0;   // ...and its tick (the camera of a screen-centred action is read there)
    // An armed object's place for this group: its base plus every crossed action's displacement, and with a scale or
    // rotate its box's half sizes and angle (written only then).
    template <class T, class H, class R>
    bool areaPlace(size_t i, T& cx, T& cy, H* hw = nullptr, H* hh = nullptr, R* rotv = nullptr) const {
        if (i >= areaArmed.size() || !areaArmed[i]) return false;
        const AreaPose p = areaPoseOf(areaOf[i], areaBaseX[i], areaBaseY[i], areaBaseHa[i], areaBaseHb[i],
                                      areaBaseRot[i], areaPxPrev, areaTickPrev);
        cx = (T)p.cx;
        cy = (T)p.cy;
        if (p.sized) {
            if (hw) *hw = (H)p.hw;
            if (hh) *hh = (H)p.hh;
            if (rotv) *rotv = (R)p.rot;
        }
        if (g_dynDbg == objs[i].uid)
            std::printf("areamove: uid=%d px(prev)=%.3f -> (%.3f,%.3f) half (%.3f,%.3f) rot %.2f\n", objs[i].uid,
                        areaPxPrev, p.cx, p.cy, p.hw, p.hh, p.rot);
        return true;
    }
    // --boxoff (removed): the boxes (State::trig bits) whose firing takes this object out. The object keeps
    // its recording otherwise: an off box says nothing about what else switches it on or off, and
    // moving its objects onto onEv's reset snapshot dropped whatever the recording held -- kitref
    // on the custom survey turned 11 replays that agreed with the game into divergences, on
    // levels where the box was never entered.
    std::vector<std::vector<uint8_t>> offBox;
    // ---- touch-trigger control (see TouchTrig, below) ----
    // Which touch triggers move this object and by how much once they have.
    // Zero for every object in lv1-18, so all of this is dead weight there.
    std::vector<TouchMask> trigMask;
    // The recording's motion is NOT this object's touch chain (wrong direction
    // or far beyond its offset), so it is an autonomous fact of the recorded
    // worldline and plays for every state -- the switch band's ceiling: chain
    // says nudge +10, the recording descends -100. A door whose recording IS
    // its chain (lv19) stays touch-gated. Filled by the loader.
    std::vector<uint8_t> recAuto;
    // --moverextend / --moverextendvel (split). A mover whose last three rows are consecutive ticks of one
    // constant velocity AND end at the recording's own last tick (the attempt's death) is a candidate.
    //  * --moverextend (Move data): it goes on only if one of the object's own Move triggers (partsAny, linear
    //    ease 0) has exactly that per-tick step (dx/dur, dy/dur within 0.02), and only for the Move's REMAINING
    //    duration: dur ticks minus the length of the constant-velocity run that ends at the last row (the run
    //    began where the Move began). Past that it holds, as before.
    //  * --moverextendvel (fallback hypothesis, NOT backed by trigger data): terminal velocity without a cap.
    // extVx/extVy are per tick, extCap the number of ticks past the last row it may go on (0 = none).
    // Built once (prepareExtend).
    std::vector<uint8_t> extOk;
    std::vector<float> extVx, extVy;
    std::vector<int> extCap;
    void prepareExtend() {
        extOk.assign(objs.size(), 0);
        extVx.assign(objs.size(), 0.f);
        extVy.assign(objs.size(), 0.f);
        extCap.assign(objs.size(), 0);
        int maxT = -1;
        for (const auto& sm : samples)
            if (!sm.empty()) maxT = std::max(maxT, sm.back().t);
        // (always on: the switch --recendquiet is removed)
        // --recendquiet (removed): the "recording's own last tick" those two tails go on from is the recorder's trailer (`end,<tick>`,
        // the tick its run reached) when it is later than every row: the recorder writes a row whenever an object has moved
        // 0.05 px from its last one, so a level that went quiet before the run ended has its last rows where things stopped,
        // not at the death. SubZero 4002: a pad's eased Move (+150 over 192 ticks) ends on its row t=19,699 at 1,532.000,
        // the file runs to 19,759, and the model went on by the Move's last step (+0.391 x 2): the pad 0.782 px high met a
        // falling cube at 19,735 and launched it (vy +9.6) where GD's falls past it onto a spike at 19,753. On by default.
        maxT = std::max(maxT, groupsEnd);
        for (size_t i = 0; i < samples.size(); ++i) {
            const auto& sm = samples[i];
            if (sm.size() < 3 || sm.back().t != maxT) continue;
            // An object with its own autonomous Move parts has the completion arm of applyTriggers (and the
            // per-part increments past its last row); it tracked the truth to 0.1 px at +1 tick where this tail ran
            // 2-7 px fast (a custom level's uid 28010, measured). Only objects without autoParts are extended.
            if (i < autoParts.size() && !autoParts[i].empty()) continue;
            const DynSample& a = sm[sm.size() - 3];
            const DynSample& b = sm[sm.size() - 2];
            const DynSample& c = sm.back();
            if (b.t != c.t - 1 || a.t != b.t - 1) continue;
            const float v1x = c.cx - b.cx, v1y = c.cy - b.cy, v2x = b.cx - a.cx, v2y = b.cy - a.cy;
            if (std::fabs(v1x - v2x) > 0.02f || std::fabs(v1y - v2y) > 0.02f) continue;
            if (std::fabs(v1x) < 0.05f && std::fabs(v1y) < 0.05f) continue;
            // the run of consecutive ticks at this velocity that ends at the last row
            int run = 2;
            bool reachedFirstRow = true;   // the run goes back to the series' first row
            for (size_t k = sm.size() - 3; k > 0; --k) {
                if (sm[k].t != sm[k - 1].t + 1
                    || std::fabs((sm[k].cx - sm[k - 1].cx) - v1x) > 0.02f
                    || std::fabs((sm[k].cy - sm[k - 1].cy) - v1y) > 0.02f) { reachedFirstRow = false; break; }
                ++run;
            }
            // A run that goes back to THIS object's own first row has nothing before it that shows where the Move began
            // (a gap -- the object standing still -- or a different velocity would): the rows do not date the Move, so the
            // data form is not taken. Judged on the object's own first row, not the recording's first tick: an
            // object that first appears mid-Move at t=100 is just as unknown as one present from t=0.
            const bool startUnknown = reachedFirstRow;
            int cap = -1;   // -1 = no Move found
            if (i < partsAny.size()) {
                for (const AutoPart& p : partsAny[i]) {
                    if (p.ease != 0 || p.dur <= 0.0) continue;
                    if (std::fabs(p.dx / p.dur - v1x) > 0.02 || std::fabs(p.dy / p.dur - v1y) > 0.02) continue;
                    cap = std::max(cap, std::max(0, (int)std::lround(p.dur) - run));
                }
            }
            if (startUnknown && cap >= 0) cap = -1;   // the data form needs the Move's start
            if (cap < 0 && g_moverExtendVel) cap = 1 << 28;
            if (cap <= 0) continue;
            extOk[i] = 1; extVx[i] = v1x; extVy[i] = v1y; extCap[i] = cap;
        }
    }
    // --recendextend: the last step of a mover whose recording ends at the recording's own last tick, when its last
    // three rows are consecutive ticks of one steady step in position (0.02 px) and angle (0.01 deg). recV* are per
    // tick; hw/hh go on by their last step too (a turning box's rect changes with its angle). Built once.
    std::vector<uint8_t> recOk;
    std::vector<float> recVx, recVy, recVr, recVhw, recVhh;
    void prepareRecEnd() {
        const size_t n = objs.size();
        recOk.assign(n, 0);
        recVx.assign(n, 0.f); recVy.assign(n, 0.f); recVr.assign(n, 0.f);
        recVhw.assign(n, 0.f); recVhh.assign(n, 0.f);
        int maxT = -1;
        for (const auto& sm : samples)
            if (!sm.empty()) maxT = std::max(maxT, sm.back().t);
        maxT = std::max(maxT, groupsEnd);
        for (size_t i = 0; i < samples.size() && i < n; ++i) {
            const auto& sm = samples[i];
            if (sm.size() < 3 || sm.back().t != maxT) continue;
            const DynSample& a = sm[sm.size() - 3];
            const DynSample& b = sm[sm.size() - 2];
            const DynSample& c = sm.back();
            if (b.t != c.t - 1 || a.t != b.t - 1) continue;
            const float vx = c.cx - b.cx, vy = c.cy - b.cy, vr = c.rot - b.rot;
            if (std::fabs(vx - (b.cx - a.cx)) > 0.02f || std::fabs(vy - (b.cy - a.cy)) > 0.02f
                || std::fabs(vr - (b.rot - a.rot)) > 0.01f)
                continue;
            if (std::fabs(vx) < 0.05f && std::fabs(vy) < 0.05f && std::fabs(vr) < 0.005f) continue;
            recOk[i] = 1; recVx[i] = vx; recVy[i] = vy; recVr[i] = vr;
            recVhw[i] = c.hw - b.hw; recVhh[i] = c.hh - b.hh;
        }
    }
    // The ticks (1..kRecEndExtendTicks) object i goes on past its last row at recording tick tr, 0 when it holds.
    int recEndTicks(size_t i, int tr) const {
        if (i >= recOk.size() || !recOk[i]) return 0;
        const auto& sm = samples[i];
        if (sm.empty() || tr <= sm.back().t) return 0;
        return std::min(tr - sm.back().t, kRecEndExtendTicks);
    }
    // --rottail: per object, its Rotates with the g_autoTrig entry and the centre's
    // position resolved, the object's own box (w, h at angle 0) for its bounding box at a new angle (0 when the
    // rows cannot give it), and the recording's last tick. Filled by the level loader.
    struct RotTail { int a; double cx, cy, total, durT, erate; int ease; uint8_t lock, own; };   // own: no centre
    std::vector<std::vector<RotTail>> rotTail;
    std::vector<uint8_t> rotTailMove;   // --rottailmove: a Move carries it too; the tail turns the angle alone
    std::vector<float> rotBoxW, rotBoxH;
    int rotTailMaxT = -1;
    // --recendtail: an armed object's simulated placement from its last row on, the offset to the
    // last row already in it (x/y/r[k] is the recording's tick t0 + k), and the ticks its lock is open on the
    // track's clock. Empty x = not armed. Filled by the level loader.
    struct RigTail { int t0 = -1; int lockA = 1 << 30, lockB = -1; std::vector<float> x, y, r; };
    std::vector<RigTail> rigTail;
    // ...and each object's switches past its last row (OnEvent, x-crossing and touch alike), and whether any is a
    // touch one (the placement then depends on the group: not a pure object).
    std::vector<std::vector<OnEvent>> tailOnEv;
    std::vector<uint8_t> tailOnBox;
    int tailRecEnd = -1;   // the recording's last tick over every object (the switches start after it)
    int groupsEnd = -1;    // the recorder's trailer, the tick its run reached (-1 unknown; --recendquiet (removed))
    // Turn object i's placement (cx, cy, rot) on from the last row (model tick tl) to model tick t; false when no
    // Rotate of its turned between the two.
    bool rotTailFrom(size_t i, int tl, int t, double& cx, double& cy, float& rot) const {
        bool moved = false;
        for (const RotTail& r : rotTail[i]) {
            const int F = g_autoTrig[(size_t)r.a].fireT;
            if (F < 0) continue;
            auto theta = [&](int tau) -> double {
                if (tau <= F) return 0.0;
                const double u = r.durT > 0.0 ? std::min(1.0, (double)(tau - F) / r.durT) : 1.0;
                return r.total * gdEase(r.ease, r.erate, u);
            };
            const double d = theta(t) - theta(tl);
            if (d == 0.0) continue;
            // GD's angle runs clockwise; in the y-up frame that is a turn by -d.
            if (!r.own) {
                const double a = -d * (3.14159265358979 / 180.0);
                const double c = std::cos(a), s = std::sin(a);
                const double rx = cx - r.cx, ry = cy - r.cy;
                cx = r.cx + c * rx - s * ry;
                cy = r.cy + s * rx + c * ry;
            }
            if (!r.lock) rot += (float)d;
            moved = true;
        }
        return moved;
    }
    // The turned-box axis at angle rotDeg (the --dynobbrot rule, for a placement that moved the angle).
    void recEndAxis(size_t i, double rotDeg) {
        // --rotobblatch: a placement that turns it past the recording latches it (and the latch stays)
        if (i < obbLatchT.size() && obbLatchT[i] >= 0 && !objs[i].obbOk) {
            double d = std::fmod(std::fabs(rotDeg - objs[i].rot), 360.0);
            if (d > 180.0) d = 360.0 - d;
            if (d > 0.001) objs[i].obbOk = 1;
        }
        if (!g_dynObbRot || !objs[i].obbOk || i >= everRot.size() || !everRot[i] || i >= obbBc0.size()) return;
        const double a = -(rotDeg - objs[i].rot) * (3.14159265358979 / 180.0);
        const double ca = std::cos(a), sa = std::sin(a);
        objs[i].bc = obbBc0[i] * ca - obbBs0[i] * sa;
        objs[i].bs = obbBc0[i] * sa + obbBs0[i] * ca;
    }    std::vector<float> trigDx, trigDy;
    std::vector<double> trigDur;
    std::vector<int> trigEase;
    std::vector<double> trigErate;
    // The tick this object FIRST moved in the recording (-1 = it never did).
    // That is the anchor the recorded trajectory is re-timed against, so a
    // frontier group that fired the trigger at a different tick than the run
    // that recorded it still sees the right shape at the right offset.
    // Shared by the touch and the autonomous machinery.
    std::vector<int> trigRecFire;
    // --touchretimebox: per touch bit, the tick the RECORDING entered that box, estimated from
    // the earliest first motion among the objects only it moves (-1 = none; empty = flag off).
    // Filled by the loader.
    std::vector<int> boxRecEntry;
    // ...and how many ticks late that FIRST ROW is (see recordLag). The true
    // start is trigRecFire - recLag, which is the tick the analytic curve and
    // the re-timing shift are both written against.
    std::vector<int> recLag;
    // ...and the lag of the AUTONOMOUS move alone, for an object that also has a
    // touch move (formula-driven): its recLag is the touch move's, which is the
    // wrong curve for dating the autonomous trigger. Equal to recLag for every
    // other object. Read unless --no-automixlag.
    std::vector<int> autoLag;
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
    // --anchornoop: 1 = autoAnchor's Toggle switches this object and moves it nowhere, and the
    // recording's first row already holds the state it switches to, so no recorded change is the
    // anchor's (see g_anchorNoop). Filled by the loader for every object; read only under the flag.
    std::vector<uint8_t> anchorUnrec;
    // --recanchorx: the recording run's own fire tick of autoAnchor (its crossing on --xtrack plus the
    // delay) where the recording's first change came BEFORE that crossing, else -1. Filled by the loader.
    std::vector<int> recAnchorX;
    // "This object's touch entry moves it nowhere (dx=dy=0) but it ALSO has a
    // real autonomous mover." Deliberately a SEPARATE flag rather than widening
    // autoAnchor: autoAnchor is read by :632, :710, :853 and cli.hpp:1652/:1981,
    // and widening it would drag the ownTouch punch-freeze (:853) and the
    // anchor-seeding rf scan (cli.hpp:1652, both the ahead branch at :1670 and
    // the behind branch at :1718) along with it -- both were measured as
    // unintended effects of the wider forms. Read at the fire gate below and
    // NOWHERE else.
    std::vector<uint8_t> recSelfFire;
    // --touchrecleads (removed): 1 = touch-controlled, its box chains move it nowhere, no autonomous controller reaches
    // it, and the recording moves it before the recorded attempt entered any of its boxes. Fired from the
    // recording's first change on its own clock, and kept out of --touchretime and its box-entry estimate.
    std::vector<uint8_t> touchLeads;
    // Also reached by an autonomous controller (level_loader.hpp). Read only by
    // --touchretime, which leaves such objects on the recording's clock.
    std::vector<uint8_t> autoReach;
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
    struct AutoPart {
        int trig; float dx, dy; double dur; int ease; double erate;
        // --movetarget only: see TrigCtl's fields of the same names.
        int mover = 0;
        uint8_t tmode = 0;
        float tdx = 0.f, tdy = 0.f;
        // --spawnlag: TrigCtl::spawnLag, the smallest over the paths merged into this part.
        int lag = 0;
    };
    std::vector<std::vector<AutoPart>> autoParts;
    std::vector<std::vector<AutoPart>> partsAny;   // --moverextend: every Move the object's groups listen to (loader)
    // Was this object's recording REPLACED by the computed orbit (the rotation split)?
    // The split stores `entry_C + R(theta)*rel` and drops `C(t) - entry_C`
    // -- level_loader's own words, "recorded = orbit + the centre's own
    // translation", with the translation subtracted out at the fit check. The
    // only path that ever added it back sits behind g_rotCompute, which is
    // false and which no flag can set, so for these objects the translation
    // was silently lost. They share their move controllers with their centre
    // (the split refuses them otherwise), so their own autoParts IS that
    // translation; this marks them so the recording arm can add it.
    std::vector<uint8_t> rotSplit;
    // --bootauto: 1 for an object whose rows came from the left-out recording (on that recording's clock, not this run's); it travels with the level, so a cached level keeps it
    std::vector<uint8_t> bootRows;
    bool fromBootRows(size_t i) const { return i < bootRows.size() && bootRows[i]; }
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
    // --multilock: the object's other locks (the --multilock note above): {autonomous trigger index, window in ticks, mover uid,
    // start x -- the anchor's x for a lock the anchor is past (the recorded displacement sits in the first lock), NaN: the trigger's own fireX}
    std::vector<std::vector<std::array<double, 4>>> autoLockExtra;
    void addExtraLocks(size_t i, int t, double px, TouchMask mask, FireTicks fireB, double& cx) const {
        if (i >= autoLockExtra.size()) return;
        for (const std::array<double, 4>& L : autoLockExtra[i]) {
            const int k = (int)L[0];
            if (k < 0 || (size_t)k >= g_autoTrig.size() || autoFireT(k, mask, fireB) < 0) continue;
            AutoTrig& A = g_autoTrig[(size_t)k];
            const int lockT0 = A.fireT - A.delay;
            int lockEnd = lockT0 + (int)L[1];
            if (g_autoStop) lockEnd = std::min(lockEnd, autoStopT(k));
            lockEnd = std::min(lockEnd, touchStopTick(mask, fireB, (int)L[2] != 0 ? (int)L[2] : A.uid));
            const double from = std::isnan(L[3]) ? A.fireX : L[3];
            if (t >= lockT0 && t <= lockEnd) {
                A.lockLastX = px;
                cx += (px - from) * lockMod(A);
            } else if (t > lockEnd) {
                cx += (A.lockLastX - from) * lockMod(A);
            }
        }
    }
    // --lockstop: the Move that locks the object (its ctl entry's mover, else the entry's own
    // uid), the one a touch chain's Stop has to name. Filled at the first placement under the flag.
    std::vector<int> lockMover;
    void prepareLockMover() {
        lockMover.assign(objs.size(), 0);
        for (size_t i = 0; i < objs.size() && i < autoLockTrig.size(); ++i) {
            const int k = autoLockTrig[i];
            if (k < 0 || (size_t)k >= g_autoTrig.size()) continue;
            const AutoTrig& A = g_autoTrig[(size_t)k];
            int mv = A.uid;
            for (const TrigCtl& c : A.ctl)
                if (c.uid == objs[i].uid && c.lockTicks > 0.0 && c.mover != 0) { mv = c.mover; break; }
            lockMover[i] = mv;
        }
    }
    // ...and the touch side. Non-zero means "while this object's box has been
    // punched and the window is open, this object's x IS the player's x,
    // offset by wherever both were when it fired" -- so its x is a property of
    // the STATE, not of the level, and a recording of it is one attempt's
    // player path. dynamics' own note on TrigCtl::lockTicks calls this "the one
    // class where a RECORDING is provably wrong for anyone else".
    std::vector<double> touchLock;
    // --lockx (removed): no autonomous Move moves this object in x but the lock, so its x
    // is the lock's alone and the recording's x can be replaced, not corrected.
    bool autoLockXOnly(size_t i) const {
        if (i >= autoParts.size()) return true;
        for (const AutoPart& p : autoParts[i])
            if (std::fabs(p.dx) > 0.001f) return false;
        return true;
    }
    bool anyTrig = false;
    bool anyAuto = false;
    int lastT = -1;
    // ---- the group's own previous place -------------------------------------------------------------------------------------------------------------------------
    // seek() carries `prevCy` and `objs.cx / cy` from the last placement THIS Dynamics made, whichever group that was. A mover a touch box moves is where the group's boxes put it, so the previous
    // place of one group's tick t is another group's t - 1 whenever a thread places groups of different boxes one after the other: dcx / dcy (the surface's step the player rides) and Obj::pcy came
    // out of a different world (lv22 uid 5332 at t=5652: prevCy 943.2327 with cy 943.6221; the ball GD's floor carries landed on 8 of 24 passes of one body, and the plan the search made from those
    // lineages died at 6520 in a replay of its own inputs). groupPrevPass places the group's t - 1 over the objects that depend on the group (the non-pure ones) and keeps that place;
    // groupPrevApply writes dcx / dcy / pcy from it after the group's own placement of t.
    uint64_t lastGroupKey = 0;
    bool lastGroupKeyValid = false;
    std::vector<double> gpCx, gpCy;
    template <class F> void forEachNonPure(F&& f) {
        const bool q = sq.active;   // (quiet objects are left out of the whole group's placement: see sqHead)
        if (inc.valid && inc.gen == g_dynPlaceGen && byBucketN == objs.size()) { for (uint32_t i : ((q && sq.walkOn) ? sq.walk : inc.nonPure)) { if (q && sq.skip[i]) continue; f((size_t)i); } }
        else { for (size_t i = 0; i < objs.size(); ++i) if (!(q && sq.skip[i]) && !isPure(i)) f(i); }
    }
    // ---- quiet objects: a group's whole placement left out for the ones it cannot change --------------------------------------------------------------------------
    // With --trigsupply-all an object with no recording (at most one row) is non-pure (isPure's last clause), so every group of every tick walks it four times: the group's previous place
    // (groupPrevPass), seek, applyTriggers and groupPrevApply. On a custom level with thousands of them (a custom level: 12,225 of 25,771 moving objects in its first search, 21,031 in its anchor
    // search) that was the seek and applyTriggers buckets of prepareGroup, half the layers' time of the anchor search -- and 99.96% of those object-visits wrote the bytes the object held.
    //
    // WHICH objects (sqEligible): non-pure for that clause alone, at most one row, no autonomous controller (no anchor, no parts) and nothing seekOne reads the tick for. What the four walks
    // do to such an object, read off the code:
    //   * applyTriggersImpl (also groupPrevPass's call of it): no touch mask and no anchor, so its body leaves at "(!m && aA < 0) -> continue" after areaPlace, which answers false for an
    //     object that is not area-armed (an armed one is non-pure for another clause). Nothing is written, the supply's on/off hook is not reached.
    //   * seekOne with one row: the cursor stays 0, and every comparison of the tick with a row's needs a second row (the interpolations, recOnAt's and --onlag's "the row before", the
    //     extensions' velocity, which prepareExtend / prepareRecEnd take from three rows) -- except obbLatch (the tick against obbLatchT), and those objects are left out. What it writes is
    //     then the row's values and the step from the bytes the object holds (prevCy, pcy, dcx, dcy, dhh).
    //   * groupPrevPass / groupPrevApply: the step from the place the object held before the group's walks.
    // So the four walks are a function of the bytes the object holds (its Obj, on[i], prevCy[i]) and of whether the seek was a one-tick advance (step1; a jump writes pcy = NaN and the
    // rows' own step) -- not of the tick, not of the group. That is the whole argument: if the walks of the last call, a one-tick advance, left the object's bytes as they were, and nobody
    // has written the object since, the walks of this call (the same tick or the next) leave them again.
    // The list of exclusions is incAnchorClass's (obbLatchT, rotTail, offBox, tailOnEv -- every one here, where the slots take the ones no box reaches --, rotSplit, rigTail, extOk, recOk, pendBox) with the opposite controller test: that class is the pure
    // objects WITH an anchor, this one the supply's non-pure objects WITHOUT one. No date of a trigger and no padding of ticks is read: an object is left out of a call only on what the
    // call before it was seen to do.
    //
    // WHEN (sqHead, at the head of prepareGroup): the last call's walks left it unchanged (`stable`, compared byte for byte after groupPrevApply: sqAfterA), that call's seek was a one-tick
    // advance, this call is at its tick or the next, no seek and no applyTriggers ran in between (seekEpoch, applyImplCalls), and nothing wrote it in between -- the one writer outside the
    // walks is the supply's placement (search_dp.hpp supplyPlaceMovers), which says so (sqTouch). The classification, the index lists and isPure are not changed: the four loops pass the
    // object by (sq.skip), nothing else.
    // K1CHECK=1 (env, read once): nothing is passed by; every object the rule would pass by is walked and its bytes held against the ones it had, and a difference stops the process.
    // Cost: a call looks only at the objects that are not stable (`todo`: a snapshot at the head, a comparison after the walks), and the walks over the non-pure objects (the group's
    // previous place, a same-tick applyTriggers, groupPrevApply) take `walk` -- the non-pure objects that are not eligible plus the ones to do -- instead of every non-pure object. The
    // seek and the first applyTriggers of a tick keep their own lists and pass the stable ones by on the byte.
    struct Quiet {
        Quiet() = default;
        Quiet(const Quiet&) {}   // (a copy of a Dynamics -- the gameplay frames' levels -- starts clean: what was seen of this one's objects says nothing about the copy's)
        Quiet& operator=(const Quiet&) { Quiet fresh; *this = std::move(fresh); return *this; }
        Quiet(Quiet&&) = default;
        Quiet& operator=(Quiet&&) = default;
        bool built = false, on = false, check = false, cmp = true, active = false, walkOn = false;
        size_t nObjs = 0; uint64_t gen = 0;
        std::vector<uint32_t> s;                  // the eligible objects, ascending
        std::vector<int32_t> slot;                // per object: its place in `s`, -1 = not eligible
        std::vector<uint8_t> skip;                // per object: stable, so passed by (read by the four loops while `active`; never set under K1CHECK)
        std::vector<uint8_t> stable;              // per eligible object: walks with a one-tick seek left it, and nobody wrote it since
        std::vector<uint32_t> todo;               // the eligible objects (places in `s`) that are not stable: the ones a call walks (not under K1CHECK, which walks all)
        std::vector<uint32_t> hard, walk;         // the non-pure objects that are not eligible (ascending) / those plus the ones to do, for the open call
        uint64_t hardFor = ~0ull;                 // the index build `hard` was made from (incBuilds)
        std::vector<Obj> snap; std::vector<uint8_t> snapOn; std::vector<float> snapPcy;   // an object's bytes at the head, for the ones walked
        int lastT = INT_MIN; bool lastStep1 = false; uint64_t epoch = 0; long long applies = 0;   // the state of the Dynamics when the last call's walks ended
        uint64_t epochHead = 0;   // seekEpoch at the head of the open call (did its walks seek?)
        // K1, the eligible objects an autonomous trigger moves (k1SlotClass): placed by the anchored slots' rule (a slot's span, a date that changed) and, besides that, by the supply's on/off.
        std::vector<uint8_t> anch;                // per eligible object: it is one of those
        std::vector<uint8_t> res;                 // per eligible object: stable, with a step still held (dcx / dcy / pcy a groupPrevApply would write as 0 / 0 / cy: float residue of the closed form)
        std::vector<uint32_t> resList;            // the places of those (checked against `stable` and `res` when used)
        // ...and the other way round: per eligible object, stable with a step a call that does not run groupPrevApply would write otherwise (seekOne's dcx and applyTriggers' dcy: the closed
        // form's double against its own float); gpOpen: the open call runs groupPrevApply (sqHead's gpWill)
        std::vector<uint8_t> gres; std::vector<uint32_t> gresList; bool gpOpen = false;
        std::vector<uint32_t> aK;                 // their places in `s`, ascending
        std::vector<uint32_t> tiK;                // the eligible objects a touch box reaches and moves by nothing (sqTouchInert): their places in `s`, ascending
        int tiRowT = INT_MIN;                     // the latest tick of those objects' one row (their placement compares the tick with it: sqGateAnchored)
        std::vector<std::pair<uint32_t, uint32_t>> tiSw;   // the ones of them with a --recendtail switch: (place in `s`, place in mK), passed by only while the supply answers their on/off
        // the eligible objects a touch box reaches and moves by nothing whose rows hold its on/off alone (sqTouchRec), by their box: places in `s`; the earliest second row among them (r1), the
        // latest first row (t0), the least of (second row - the tick the recording's clock is shifted from) (q), and the boxes their switches read (sw)
        struct TrBox { int box = -1; std::vector<uint32_t> k; int r1 = INT_MAX, t0 = INT_MIN; long long q = LLONG_MAX; std::vector<int> sw; };
        std::vector<TrBox> trBox;
        std::vector<uint32_t> mK;                 // the eligible objects whose placement asks the supply's on/off (the anchored ones and the touch-inert ones): places in `s`, ascending
        std::vector<int> aUid;                    // the uids of mK, in the same order
        std::vector<uint32_t> todoA;              // the ones of them to do in the open call (objects, ascending): the seek and the first applyTriggers walk them as well
        std::vector<uint8_t> woke;                // scratch: per slot, the dates changed under it
        SupOnMirror mir;                          // the supply's on/off of aUid as the walks last left it
        std::vector<uint32_t> flipJ;              // scratch: places in aUid whose supplied on/off changed
        std::vector<uint32_t> sel;                // the slots' objects this tick reaches (sqGateAnchored), valid for tick selT while selOk
        bool selOk = false; int selT = INT_MIN;
        // ...and how long it stays that list (sqGateAnchored): made at tick selLo with no date changed under a slot, it is the list of every tick through selHi -- the tick before the first edge of
        // any slot's spans after selLo -- while no slot is written (pSlotMut) and the index is the same one (incBuilds)
        uint64_t selMut = ~0ull, selBuilds = ~0ull; int selLo = INT_MIN; long long selHi = LLONG_MIN;
        long long syncs = 0, flips = 0, gated = 0;
        // the placed-every-tick set (inc.always) without the eligible objects, and per eligible object whether it is of that set; made for index build alwFor (sqShortOk)
        std::vector<uint32_t> alwHard; std::vector<uint8_t> inAlw; uint64_t alwFor = ~0ull;
        std::vector<uint32_t> tNow;               // scratch (incAtFirstList)
        // collect's index (sqIx*): the stable objects by the cell of their cx, per bucket; the ones with no cell (a wide or not finite box) in `wide`; the bucket's objects that are not eligible in `hard`
        struct Ix {
            bool live = false;                                   // the open run of calls has it whole: set where a call's walks end (sqAfterA), dropped at the next head
            size_t hardN = (size_t)-1;                           // objs.size() `hard` was made for
            std::vector<std::vector<uint32_t>> hard;             // per bucket: its objects that are not eligible, ascending
            std::vector<std::vector<std::vector<uint32_t>>> cell; // per bucket, per cell: stable objects
            std::vector<std::vector<uint32_t>> wide;             // per bucket: stable objects with no cell
            std::vector<int32_t> at, pos;                        // per eligible object: its cell (-1 wide, -2 not in the index) and its place in that list
        } ix;
    } sq;
    // ---- collect over the stable quiet objects by x ---------------------------------------------------------------------------------------------------------------
    // collect() looked at every object of the bucket for every group: 19,781 of the NEAR bucket to keep 86 on a level with 25,921 moving objects, 90 microseconds a call, and 97% of them were on,
    // so the window alone left them out (measured over the 12,055 groups of one search and its walk). A stable quiet object holds the same bytes from call to call -- its box and its on/off with
    // them; that is what `stable` says, and every place that takes the mark away is known (the head's reset, the gate, the residue list, the supply's sqTouch). So the stable ones are kept by
    // the cell of their cx (kIxW wide; a box wider than kIxHw or not finite goes to a list of its own that every call reads), put in where the mark is given and taken out where it is taken.
    // A call then looks at the cells its window can reach, that list, the eligible objects that are not stable (`todo`) and the bucket's objects that are not eligible, makes the same tests,
    // and puts what it keeps in ascending order: the scan's list.
    // Used only while nothing can have written a stable object since the marks were given: between the end of a call's walks and the next seek / applyTriggers of this Dynamics (the epochs
    // sqHead's chain reads), not under K1CHECK / --incplacecheck (nothing is stable there), not after a foreign write (g_sqForeign).
    // COLLECTCHECK=1 (env, read once): every call that used the index is held against the scan, and a difference stops the process.
    static constexpr double kIxW = 128.0, kIxHw = 256.0;
    static constexpr int kIxCells = 4096;
    static int sqIxCellOf(double x) { const double c = std::floor(x / kIxW); return c < 0.0 ? 0 : c > (double)(kIxCells - 1) ? kIxCells - 1 : (int)c; }   // (monotone; what lies outside is kept in the end cells)
    void sqIxClear() {
        for (auto& b : sq.ix.cell) for (auto& c : b) c.clear();
        for (auto& w : sq.ix.wide) w.clear();
        std::fill(sq.ix.at.begin(), sq.ix.at.end(), -2);
    }
    void sqIxAdd(size_t k) {
        Quiet::Ix& X = sq.ix;
        if (X.at.size() != sq.s.size()) { X.at.assign(sq.s.size(), -2); X.pos.assign(sq.s.size(), 0); }
        if (X.at[k] != -2) return;
        const uint32_t i = sq.s[k];
        const size_t b = bucket[i];
        if (X.cell.size() <= b) { X.cell.resize(b + 1); X.wide.resize(b + 1); }
        const Obj& o = objs[i];
        const bool celled = std::isfinite(o.cx) && o.hw >= 0 && o.hw <= kIxHw;   // (a box that is not finite passes the window's test whatever the window: every call must see it)
        if (celled && X.cell[b].empty()) X.cell[b].resize((size_t)kIxCells);
        std::vector<uint32_t>& l = celled ? X.cell[b][(size_t)sqIxCellOf(o.cx)] : X.wide[b];
        X.at[k] = celled ? sqIxCellOf(o.cx) : -1; X.pos[k] = (int32_t)l.size();
        l.push_back(i);
    }
    void sqIxDel(size_t k) {
        Quiet::Ix& X = sq.ix;
        if (k >= X.at.size() || X.at[k] == -2) return;
        const size_t b = bucket[sq.s[k]];
        std::vector<uint32_t>& l = X.at[k] >= 0 ? X.cell[b][(size_t)X.at[k]] : X.wide[b];
        const size_t p = (size_t)X.pos[k];
        if (p + 1 != l.size()) { l[p] = l.back(); X.pos[(size_t)sq.slot[l[p]]] = (int32_t)p; }
        l.pop_back();
        X.at[k] = -2;
    }
    bool sqIxUsable(uint8_t b) const {
        return sq.built && sq.on && !sq.check && sq.ix.live && sq.ix.hardN == objs.size() && b < sq.ix.hard.size() && seekEpoch == sq.epoch && applyImplCalls == sq.applies && lastT == sq.lastT
            && !g_sqForeign.load(std::memory_order_relaxed);
    }
    long long applyImplCalls = 0;
    uint64_t pSlotMut = 0;    // counts the writes of the anchored slots' spans and forces (incPSyncAll, incPWake); the index build that makes the slots is counted by incBuilds
    uint64_t incBuilds = 0;   // index builds of this Dynamics (incBuild)
    // An object a touch box reaches whose touch moves it by nothing (a touch Toggle's target, mostly): one row, no recording of a motion, a zero offset, no part, no controller of another kind.
    // applyTriggersImpl takes it through
    // the touch arm (m != 0), and what that arm does to it, read off the code:
    //   * `fired` (the group's mask against its boxes), `anchor` (fireHi) and the fire ticks enter the closed form only: cx += trigDx * e, cy += trigDy * e, with both offsets 0. The arms that read
    //     them otherwise need a recording (trigRecFire >= 0: the re-timing, the hold, ownTouch), a part (touchParts, autoParts: recAuto's steps, the parts tail, the superposition), an anchor or a
    //     lock (aA >= 0, which m != 0 rules out), a formula -- none of which it has.
    //   * the tails (rotTail, rigTail, the steady-step extension), the area pass and the switches offBox, onEv, disarmEv, pendBox are each on a table it has no entry in: the list below.
    //   * its --recendtail switches (tailOnEv, tailOnBox: the touch Toggle itself) write the on/off alone, from the group's mask and fire ticks, and the hook at the arm's end then writes the
    //     supply's over it -- when the supply answers. So an object with such switches is passed by only while the supply answers for it (sqGateAnchored reads the mirror: sq.tiSw).
    //   * the tick is compared with its one row's tick (the recording's "gone by this tick": sm[cur].t <= t), so nothing is passed by until the tick is past the latest such row (sq.tiRowT).
    //   * its on/off is the supply's for the group being placed (the hook at the arm's end, one row): watched by the mirror, as the anchored ones' is (sqGateAnchored).
    // What is left is the row's values and the step from the bytes the object holds, as for the other quiet objects. K1CHECK holds every walk the rule would leave out against the bytes.
    bool sqTouchInert(size_t i) const {
        if (!g_supplyOnSyncHook || !trigMask[i]) return false;
        if ((i < onEv.size() && !onEv[i].empty()) || (i < formula.size() && formula[i]) || (i < autoLockTrig.size() && autoLockTrig[i] >= 0) || (i < rigTail.size() && rigTail[i].lockB >= 0)
            || (i < areaArmed.size() && areaArmed[i]) || (i < areaAdd.size() && areaAdd[i]) || (i < disarmDep.size() && disarmDep[i])
            || (i < armDep.size() && armDep[i]))
            return false;   // (isPureButSupply's other clauses, but tailOnBox)
        if (i >= samples.size() || samples[i].size() != 1 || objs[i].uid < 0) return false;
        if (i >= trigRecFire.size() || trigRecFire[i] >= 0) return false;
        if (i >= trigDx.size() || i >= trigDy.size() || trigDx[i] != 0.f || trigDy[i] != 0.f) return false;
        if (i >= touchParts.size() || !touchParts[i].empty() || i >= autoParts.size() || !autoParts[i].empty()) return false;
        if (i >= autoAnchor.size() || autoAnchor[i] >= 0) return false;
        if (i < recAuto.size() && recAuto[i]) return false;
        if (i < touchLock.size() && touchLock[i] > 0.0) return false;
        if (i < formulaLock.size() && formulaLock[i]) return false;
        if (i < disarmEv.size() && !disarmEv[i].empty()) return false;
        if (i < obbLatchT.size() && obbLatchT[i] >= 0) return false;
        if (i < rotTail.size() && !rotTail[i].empty()) return false;
        if (i < offBox.size() && !offBox[i].empty()) return false;
        if (i < rotSplit.size() && rotSplit[i]) return false;
        if (i < rigTail.size() && !rigTail[i].x.empty()) return false;
        if (i < extOk.size() && extOk[i]) return false;
        if (i < recOk.size() && recOk[i]) return false;
        if (objs[i].pendBox >= 0) return false;
        return true;
    }
    // The same object once the flights' recordings gave it rows: every row has the place, the size and the angle of the first, and only the on/off differs (a touch Toggle's target as GD's flights
    // saw it switch). Its placement is the touch arm's recording branch, where the group's fire tick of its box shifts the recording's clock (--touchretime: tt = t - shift) and a row is looked up
    // at tt; seekOne looks one up at t; the switches (tailOnEv, --touchonrec (removed)) take over past the recording's end or from the first tick the state's entry of a switch box differs from the
    // recorded attempt's. While t and tt are both inside the first row's stretch, away from its ends, and no switch box differs, all of that reads the first row: the bytes are the same whatever
    // the group and the tick. sqGateAnchored holds exactly those conditions for each box (sq.trBox) and takes the mark off the box's objects where one fails; from then on they are walked as before.
    // The supply's on/off is not asked for an object with more than one row. One box only (the shift is that box's fire tick), and the same tables must be empty as for sqTouchInert.
    bool sqTouchRec(size_t i) const {
        if (!trigMask[i]) return false;
        int nb = 0; for (int b = 0; b < kTouchBits; ++b) if (trigMask[i].test(b)) ++nb;
        if (nb != 1) return false;
        if ((i < onEv.size() && !onEv[i].empty()) || (i < formula.size() && formula[i]) || (i < autoLockTrig.size() && autoLockTrig[i] >= 0) || (i < rigTail.size() && rigTail[i].lockB >= 0)
            || (i < areaArmed.size() && areaArmed[i]) || (i < areaAdd.size() && areaAdd[i]) || (i < disarmDep.size() && disarmDep[i]) || (i < armDep.size() && armDep[i]))
            return false;   // (isPureButSupply's other clauses, but tailOnBox)
        if (i >= samples.size() || samples[i].size() < 2) return false;
        const auto& sm = samples[i];
        for (const DynSample& r : sm) if (r.cx != sm[0].cx || r.cy != sm[0].cy || r.hw != sm[0].hw || r.hh != sm[0].hh || !(r.rot == sm[0].rot)) return false;
        if (i >= trigRecFire.size() || trigRecFire[i] < 0) return false;
        if (i >= trigDx.size() || i >= trigDy.size() || trigDx[i] != 0.f || trigDy[i] != 0.f) return false;
        if (i >= touchParts.size() || !touchParts[i].empty() || i >= autoParts.size() || !autoParts[i].empty()) return false;
        if (i < partsAny.size() && !partsAny[i].empty()) return false;   // (--ctlautotail's tail)
        if (i >= autoAnchor.size() || autoAnchor[i] >= 0) return false;
        if (i < recAuto.size() && recAuto[i]) return false;
        if (i < touchLock.size() && touchLock[i] > 0.0) return false;
        if (i < formulaLock.size() && formulaLock[i]) return false;
        if (i < disarmEv.size() && !disarmEv[i].empty()) return false;
        if (i < obbLatchT.size() && obbLatchT[i] >= 0) return false;
        if (i < rotTail.size() && !rotTail[i].empty()) return false;
        if (i < offBox.size() && !offBox[i].empty()) return false;
        if (i < rotSplit.size() && rotSplit[i]) return false;
        if (i < rigTail.size() && !rigTail[i].x.empty()) return false;
        if (i < extOk.size() && extOk[i]) return false;
        if (i < recOk.size() && recOk[i]) return false;
        if (objs[i].pendBox >= 0) return false;
        if (i < tailOnEv.size()) for (const OnEvent& e : tailOnEv[i]) if (e.box >= kTouchBits) return false;
        return true;
    }
    bool sqEligible(size_t i) const {
        if (isPure(i)) return false;
        if (!isPureButSupply(i)) return sqTouchInert(i) || sqTouchRec(i);   // non-pure for the supply's clause alone, or for a touch that moves nothing
        if (i >= samples.size() || samples[i].size() > 1) return false;
        if ((i < autoAnchor.size() && autoAnchor[i] >= 0) || (i < autoParts.size() && !autoParts[i].empty())) return k1SlotClass(i);   // (K1: the anchored ones have their own rule, sqHead; k1SlotClass has the same exclusions)
        if (i < obbLatchT.size() && obbLatchT[i] >= 0) return false;
        if (i < rotTail.size() && !rotTail[i].empty()) return false;
        if (i < offBox.size() && !offBox[i].empty()) return false;
        if (i < tailOnEv.size() && !tailOnEv[i].empty()) return false;
        if (i < rotSplit.size() && rotSplit[i]) return false;
        if (i < rigTail.size() && !rigTail[i].x.empty()) return false;
        if (i < extOk.size() && extOk[i]) return false;
        if (i < recOk.size() && recOk[i]) return false;
        if (objs[i].pendBox >= 0) return false;
        return true;
    }
    bool sqSame(size_t k, size_t i) const {
        const float p = i < prevCy.size() ? prevCy[i] : 0.f;
        return std::memcmp(&sq.snap[k], &objs[i], sizeof(Obj)) == 0 && sq.snapOn[k] == on[i] && std::memcmp(&sq.snapPcy[k], &p, sizeof(float)) == 0;
    }
    // K1: what the anchored eligible objects (sq.aK) need of this call beyond the stable rule. An object of them is not stable when (1) the index of the anchored slots is not good, (2) its slot is
    // active at this tick (incPActive: its rows' ticks, the span its dates put its changes in, a force) or a date it reads changed under it (what incPWake will take at the seek; read here without taking
    // it), or (3) the supply's on/off for it is not what the walks last left (the mirror: g_supplyOnSyncHook). The seek and the first applyTriggers walk the slots that are active, so the ones this
    // makes not stable are added to their list (sq.todoA, incProcList / incAtFirstList); the later walks of the group take them through `todo`.
    void sqGateAnchored(int t, TouchMask mask = TouchMask{}, const uint16_t* fireBRaw = nullptr) {
        SqTimer tmG(g_sqStats.nsGate, g_phaseProf);
        sq.todoA.clear();
        sq.selOk = false;
        // the touch-recorded ones (sqTouchRec), a box at a time: passed by only while the tick, and the tick on the recording's shifted clock, are inside the first row's stretch (the previous-place
        // pass places at t - 1, the rows' edges read a tick either side: 3 ticks clear of each end), and the state's entry of every box their switches read is the recorded attempt's
        // (--touchonrec (removed)'s split). The fire ticks are read through the dupbatch's set where one is in (a follower of this build must have the same ticks).
        if (!sq.trBox.empty()) {
            const FireTicks fireB(fireBRaw, fireSink);
            for (const Quiet::TrBox& B : sq.trBox) {
                bool ok = (long long)t >= (long long)B.t0 + 4 && (long long)t + 3 < (long long)B.r1;
                if (ok && mask.test(B.box)) ok = fireB && (long long)t - (long long)fireB[(size_t)B.box] + 3 < B.q;
                for (size_t q = 0; ok && q < B.sw.size(); ++q) {
                    const int b = B.sw[q];
                    int st = (mask.test(b) && fireB) ? (int)fireB[(size_t)b] : -1;
                    if (g_touchRecEntry[(size_t)b] > 0) st = g_touchRecEntry[(size_t)b];
                    if (st != g_recOwnEntry[(size_t)b]) ok = false;
                }
                if (!ok) for (const uint32_t k : B.k) if (sq.stable[k]) { g_sqStats.trGated.fetch_add(1, std::memory_order_relaxed); sq.stable[k] = 0; if (!sq.check) { sq.skip[sq.s[k]] = 0; sq.todo.push_back(k); sqIxDel(k); } }
            }
        }
        const size_t na = sq.aK.size();
        if (sq.mK.empty()) return;
        const bool incOk = inc.valid && inc.gen == g_dynPlaceGen && byBucketN == objs.size() && !inc.pSlot.empty();
        bool allWoke = !incOk;
        bool anyWoke = false;   // a slot reads a date that is not the one its object was last placed under
        if (incOk) {
            if (!inc.pKnown || inc.pFire.size() != g_autoTrig.size() || inc.pDep.size() != g_autoTrig.size()) allWoke = true;
            else {
                for (size_t b = 0; b < g_autoTrig.size() && !anyWoke; ++b) if (g_autoTrig[b].fireT != inc.pFire[b] && !inc.pDep[b].empty()) anyWoke = true;
                if (anyWoke) {
                    sq.woke.assign(inc.pSlot.size(), 0);
                    for (size_t b = 0; b < g_autoTrig.size(); ++b)
                        if (g_autoTrig[b].fireT != inc.pFire[b]) for (const uint32_t q : inc.pDep[b]) sq.woke[q] = 1;
                }
            }
        }
        auto unst = [&](size_t k) {
            if (!sq.stable[k]) return;
            ++sq.gated; g_sqStats.gated.fetch_add(1, std::memory_order_relaxed);
            sq.stable[k] = 0;
            if (!sq.check) { sq.skip[sq.s[k]] = 0; sq.todo.push_back((uint32_t)k); sqIxDel(k); }
        };
        // the slots this tick reaches, in one pass over them (incProcList takes the same list, sq.sel, instead of making it again)
        // The pass was every slot for every call (8,491 slots on a search of a level with 25,921 moving objects: 9 of the call's 333 s). What it reads is the tick, each slot's spans and force (incPActive), and the dates
        // against the ones the objects were last placed under (woke) -- nothing of the group: a group's latch, its touch ticks and its events reach a slot only as a date (g_autoTrig[].fireT).
        // So with every date a slot reads as it was placed under, and no slot written since the list was made, a slot is in the list of a later tick exactly when it was in the made one, up to the
        // first edge of any slot's spans (a span's first tick, the tick after its last, the tick after a force): the list is kept through the tick before that edge. A changed date makes the
        // list again, and the seek that follows writes the slots (incPWake), which ends what was kept. Under K1CHECK the seek makes its own list and holds this one against it (incProcList0).
        if (incOk) {
            const bool kept = !allWoke && !anyWoke && sq.selMut == pSlotMut && sq.selBuilds == incBuilds && t >= sq.selLo && (long long)t <= sq.selHi;
            if (kept) {
                for (const uint32_t o : sq.sel) { const int32_t k = sq.slot[o]; if (k >= 0 && sq.anch[(size_t)k]) unst((size_t)k); }
                g_sqStats.selReused.fetch_add(1, std::memory_order_relaxed);
            } else {
                sq.sel.clear();
                long long edge = LLONG_MAX;   // the first tick after t at which some slot's answer can change
                auto span = [&](int a, int b) { if (a > b) return; if (t < a) edge = std::min(edge, (long long)a); else if (t <= b) edge = std::min(edge, (long long)b + 1); };
                for (size_t q = 0; q < inc.pSlot.size(); ++q) {
                    const IncState::PSlot& s = inc.pSlot[q];
                    if (allWoke || (anyWoke && sq.woke[q]) || incPActive(s, t)) {
                        sq.sel.push_back(s.obj);
                        if (s.w1 > s.w0 && g_phaseProf && incPSwActive(s, t)) ++g_incStats.pSwListed;
                        const int32_t k = sq.slot[s.obj];
                        if (k >= 0 && sq.anch[(size_t)k]) unst((size_t)k);
                    }
                    if (s.force >= t) edge = std::min(edge, (long long)s.force + 1);
                    span(s.e0, s.e1); span(s.u0, s.u1); span(s.d0, s.d1);
                    for (uint32_t w = s.w0; w < s.w1; ++w) { const int c = incPSwTick(w); if (c != INT_MIN && c > INT_MIN + 8 && c < INT_MAX - 8) span(c - kPSwLo, c + kPSwHi); }
                }
                if (!allWoke && !anyWoke) { sq.selMut = pSlotMut; sq.selBuilds = incBuilds; sq.selLo = t; sq.selHi = edge - 1; }
                else sq.selMut = ~0ull;   // (made with a slot woken by a date: not the list of the spans alone)
                g_sqStats.selMade.fetch_add(1, std::memory_order_relaxed);
            }
            sq.selOk = true; sq.selT = t;
        } else {
            for (const uint32_t k : sq.aK) unst(k);   // (the index of the slots is not good: nothing is known of when these change)
        }
        // the touch-inert ones while the tick is not past their rows (their placement compares the two: sqTouchInert)
        if (!sq.tiK.empty() && (long long)t <= (long long)sq.tiRowT + 2) for (const uint32_t k : sq.tiK) unst(k);
        // the supply's on/off
        sq.flipJ.clear();
        { SqTimer tmS(g_sqStats.nsSync, g_phaseProf); g_supplyOnSyncHook(sq.mir, sq.aUid, sq.flipJ); }
        ++sq.syncs; sq.flips += (long long)sq.flipJ.size();
        g_sqStats.syncs.fetch_add(1, std::memory_order_relaxed); g_sqStats.flips.fetch_add((long long)sq.flipJ.size(), std::memory_order_relaxed);
        for (const uint32_t j : sq.flipJ) unst(sq.mK[j]);
        // a touch-inert object with a switch of its own, where the supply does not answer: its on/off is the switch's, from the group's mask and fire ticks
        for (const auto& e : sq.tiSw) if (e.second >= sq.mir.v.size() || sq.mir.v[e.second] > 1) unst(e.first);
        // the anchored objects that are not stable: the ones to do
        if (sq.check) { for (const uint32_t k : sq.aK) if (!sq.stable[k]) sq.todoA.push_back(sq.s[k]); }
        else {
            for (const uint32_t k : sq.todo) if (sq.anch[k]) sq.todoA.push_back(sq.s[k]);
            std::sort(sq.todoA.begin(), sq.todoA.end());
        }
        g_sqStats.todoA.fetch_add((long long)sq.todoA.size(), std::memory_order_relaxed);
        g_sqStats.anchored.store((long long)na, std::memory_order_relaxed);
    }    void sqHead(int t, bool gpWill = false, TouchMask mask = TouchMask{}, const uint16_t* fireB = nullptr) {
        SqTimer tmH(g_sqStats.nsHead, g_phaseProf);
        static const bool chk = std::getenv("K1CHECK") != nullptr;
        if (extOk.size() != objs.size()) prepareExtend();   // (sqEligible reads them; seek prepares them the same way)
        if (recOk.size() != objs.size()) prepareRecEnd();
        if (!sq.built || sq.nObjs != objs.size() || sq.gen != g_dynPlaceGen) {
            sq = Quiet();
            sq.built = true; sq.check = chk || g_incCheck; sq.cmp = !g_incCheck; sq.nObjs = objs.size(); sq.gen = g_dynPlaceGen;   // (--incplacecheck: nothing is passed by and the placement is compared by its own check, which keeps the plain arm's result)
            sq.slot.assign(objs.size(), -1); sq.skip.assign(objs.size(), 0);
            if (g_supplyOnHook)
                for (size_t i = 0; i < objs.size(); ++i)
                    if (sqEligible(i)) { sq.slot[i] = (int32_t)sq.s.size(); sq.s.push_back((uint32_t)i); }
            const size_t m = sq.s.size();
            sq.anch.assign(m, 0); sq.res.assign(m, 0); sq.gres.assign(m, 0);
            for (size_t k = 0; k < m; ++k) {
                const size_t i = sq.s[k];
                if (k1SlotClass(i)) { sq.anch[k] = 1; sq.aK.push_back((uint32_t)k); sq.mK.push_back((uint32_t)k); sq.aUid.push_back(objs[i].uid); }
                else if (trigMask[i] && samples[i].size() > 1) {   // (sqTouchRec: sqEligible let it in)
                    int b = 0; while (b < kTouchBits && !trigMask[i].test(b)) ++b;
                    Quiet::TrBox* B = nullptr; for (Quiet::TrBox& x : sq.trBox) if (x.box == b) { B = &x; break; }
                    if (!B) { sq.trBox.emplace_back(); B = &sq.trBox.back(); B->box = b; }
                    B->k.push_back((uint32_t)k);
                    const int r1 = samples[i][1].t;
                    // the tick the recording's clock is shifted from (applyTriggersImpl's --touchretime: anchor - recAnchor = the box's fire tick - this)
                    const long long c = ((size_t)b < boxRecEntry.size() && boxRecEntry[(size_t)b] >= 0) ? (long long)boxRecEntry[(size_t)b]
                                        : (long long)trigRecFire[i] - kTouchRetimeLat - ((i < recLag.size()) ? std::max(0, recLag[i] - 1) : 0);
                    B->r1 = std::min(B->r1, r1); B->t0 = std::max(B->t0, samples[i][0].t); B->q = std::min(B->q, (long long)r1 - c);
                    if (i < tailOnEv.size()) for (const OnEvent& e : tailOnEv[i]) if (e.box >= 0 && std::find(B->sw.begin(), B->sw.end(), e.box) == B->sw.end()) B->sw.push_back(e.box);
                }
                else if (trigMask[i]) { sq.tiK.push_back((uint32_t)k); sq.mK.push_back((uint32_t)k); sq.aUid.push_back(objs[i].uid); sq.tiRowT = std::max(sq.tiRowT, samples[i][0].t);
                                        if ((i < tailOnEv.size() && !tailOnEv[i].empty()) || (i < tailOnBox.size() && tailOnBox[i])) sq.tiSw.push_back({(uint32_t)k, (uint32_t)(sq.mK.size() - 1)}); }   // (sqTouchInert: sqEligible let it in)
                else { sq.mK.push_back((uint32_t)k); sq.aUid.push_back(objs[i].uid); }   // a supply-only mover (no recording rows, no touch box, not anchored): a flip of the shadow's on / off for it (an Event trigger's chain, a custom level pad uid 402 at t=5398) must un-stable it; it was in no class, so the mirror never named it and the rule passed it by with the on it had at its first walk
            }
            g_sqStats.touchInert.store((long long)sq.tiK.size(), std::memory_order_relaxed);
            { long long n = 0; for (const Quiet::TrBox& x : sq.trBox) n += (long long)x.k.size(); g_sqStats.touchRec.store(n, std::memory_order_relaxed); }
            sq.stable.assign(m, 0); sq.snap.resize(m); sq.snapOn.assign(m, 0); sq.snapPcy.assign(m, 0.f);
            sq.todo.resize(m); for (size_t k = 0; k < m; ++k) sq.todo[k] = (uint32_t)k;
            sq.on = m > 0;
            g_sqStats.objects.store((long long)m, std::memory_order_relaxed);
        }
        sq.active = false;
        sq.ix.live = false;   // (collect's index: whole again where this call's walks end)
        const bool quiet = g_dynDbg == -1 && g_shiftDbgUid < 0 && !g_shiftStat && !(g_rotCompute && !g_rotSpec.empty()) && !g_geoDumpF;   // (--geodump writes a line for every object the touch arm places)
        if (!sq.on || !quiet || g_sqForeign.load(std::memory_order_relaxed)) { sq.lastT = INT_MIN; sq.mir.v.clear(); sq.todoA.clear(); return; }   // (a call that does not look: what the supply's on/off was is not known at the next)
        const size_t m = sq.s.size();
        const bool chain = sq.lastT != INT_MIN && (t == sq.lastT || t == sq.lastT + 1) && lastT == sq.lastT && sq.lastStep1 && seekEpoch == sq.epoch && applyImplCalls == sq.applies;
        if (!chain && (sq.check || sq.todo.size() != m)) {   // what was seen holds along an unbroken run of calls only: every object is to do again
            std::fill(sq.stable.begin(), sq.stable.end(), (uint8_t)0);
            for (size_t k = 0; k < m; ++k) sq.skip[sq.s[k]] = 0;
            sq.todo.resize(m); for (size_t k = 0; k < m; ++k) sq.todo[k] = (uint32_t)k;
            sqIxClear();
        }
        // a call that runs groupPrevApply writes the held step of every non-pure object as the difference of two places of the same tick: 0 for one at rest. An object that stands with a residue (the closed
        // form's double against the float it is stored in: dcx 0.000859 for uid 60501 of a custom level from t=18541 on) is therefore not left out of that call.
        if (gpWill && !sq.resList.empty()) {
            for (const uint32_t k : sq.resList) if (sq.stable[k] && sq.res[k]) { sq.stable[k] = 0; if (!sq.check) { sq.skip[sq.s[k]] = 0; sq.todo.push_back(k); sqIxDel(k); } }
            sq.resList.clear();
        }
        // ...and a call that does not run it leaves the step to seekOne and applyTriggers. For an anchored object at rest those write the closed form's double against a float of it: applyTriggers'
        // dcy = cy - prevCy (-0.000015 for uid 14685 of a custom level at t=5549, where the call before, which ran groupPrevApply, had left 0), and where the object is sought, seekOne's dcx, whose
        // old place is a float (-0.000064 for uid 41645 at t=22595, the second tick of a search that starts over: every object is sought there). So a stable anchored object whose held step is
        // not the one such a call writes is not left out of it (sqAfterA: gres). Taking the mark off is always safe for these: an object to do is sought and placed, as it was on every
        // tick before the rule.
        if (!gpWill && !sq.gresList.empty()) {
            for (const uint32_t k : sq.gresList) {   // (the flag comes off with the list: the object is in it again where it is marked again)
                if (sq.stable[k] && sq.gres[k]) { sq.stable[k] = 0; if (!sq.check) { sq.skip[sq.s[k]] = 0; sq.todo.push_back(k); sqIxDel(k); } }
                sq.gres[k] = 0;
            }
            sq.gresList.clear();
        }
        sq.gpOpen = gpWill;
        sqGateAnchored(t, mask, fireB);
        sq.epochHead = seekEpoch;
        auto snapOne = [&](size_t k) { const size_t i = sq.s[k]; std::memcpy(&sq.snap[k], &objs[i], sizeof(Obj)); sq.snapOn[k] = on[i]; sq.snapPcy[k] = i < prevCy.size() ? prevCy[i] : 0.f; };
        if (sq.check) { for (size_t k = 0; k < m; ++k) snapOne(k); }   // (every object is walked and compared; `stable` is the rule's word, `skip` stays 0)
        else for (const uint32_t k : sq.todo) snapOne(k);
        // the non-pure walks' list for this call
        sq.walkOn = false;
        if (!sq.check && sq.todo.size() != m && inc.valid && inc.gen == g_dynPlaceGen && byBucketN == objs.size()) {
            if (sq.hardFor != incBuilds) {
                sq.hard.clear();
                for (const uint32_t i : inc.nonPure) if (sq.slot[i] < 0) sq.hard.push_back(i);
                sq.hardFor = incBuilds;
            }
            std::sort(sq.todo.begin(), sq.todo.end());   // (places in `s` ascend with the objects)
            sq.walk.clear();
            size_t a = 0;
            for (const uint32_t k : sq.todo) { const uint32_t i = sq.s[k]; while (a < sq.hard.size() && sq.hard[a] < i) sq.walk.push_back(sq.hard[a++]); sq.walk.push_back(i); }
            while (a < sq.hard.size()) sq.walk.push_back(sq.hard[a++]);
            sq.walkOn = true;
        }
        g_sqStats.heads.fetch_add(1, std::memory_order_relaxed);
        g_sqStats.visits.fetch_add((long long)m, std::memory_order_relaxed);
        if (!sq.check) g_sqStats.passed.fetch_add((long long)(m - sq.todo.size()), std::memory_order_relaxed);
        sq.active = true;
        t_sqDyn = this;
    }
    void sqAfterA() {
        if (!sq.active) return;
        SqTimer tmA(g_sqStats.nsAfter, g_phaseProf);
        sq.active = false;
        const size_t m = sq.s.size();
        const bool sought = seekEpoch != sq.epochHead;
        long long checked = 0;
        size_t keep = 0;
        const size_t nDo = sq.check ? m : sq.todo.size();
        for (size_t q = 0; q < nDo; ++q) {
            const size_t k = sq.check ? q : (size_t)sq.todo[q];
            const size_t i = sq.s[k];
            const bool same = sqSame(k, i);
            if (sq.check && sq.cmp && sq.stable[k]) {
                ++checked;
                if (!same) {
                    const unsigned char* pa = (const unsigned char*)&sq.snap[k]; const unsigned char* pb = (const unsigned char*)&objs[i];
                    size_t off = 0; while (off < sizeof(Obj) && pa[off] == pb[off]) ++off;
                    std::printf("sqcheck: MISMATCH t=%d uid=%d: the rule would pass the object by and the walks changed it (first differing byte %zu of %zu; cx %.4f->%.4f cy %.4f->%.4f "
                                "dcx %.6f->%.6f dcy %.6f->%.6f pcy %.4f->%.4f on %d->%d)\n", lastT, objs[i].uid, off, sizeof(Obj), sq.snap[k].cx, objs[i].cx, sq.snap[k].cy, objs[i].cy,
                                sq.snap[k].dcx, objs[i].dcx, sq.snap[k].dcy, objs[i].dcy, sq.snap[k].pcy, objs[i].pcy, (int)sq.snapOn[k], (int)on[i]);
                    {   // (diagnosis) what kind of object, and what the rule knew of it
                        const int32_t q = i < inc.pSlotOf.size() ? inc.pSlotOf[i] : -1;
                        std::printf("sqcheck: object %zu anchored=%d slot=%d slotActive=%d sel=%d woke=%d sought=%d lastStep1=%d lastT=%d t0=%d rows=%zu autoAnchor=%d fireT=%d prevCy=%.6f\n", i, (int)sq.anch[k], (int)q,
                                    q >= 0 ? (int)incPActive(inc.pSlot[(size_t)q], lastT) : -1, (int)std::binary_search(sq.sel.begin(), sq.sel.end(), (uint32_t)i), q >= 0 && (size_t)q < sq.woke.size() ? (int)sq.woke[(size_t)q] : -1,
                                    (int)(seekEpoch != sq.epochHead), (int)lastStep1, lastT, samples[i].empty() ? -1 : samples[i][0].t, samples[i].size(), i < autoAnchor.size() ? autoAnchor[i] : -2,
                                    (i < autoAnchor.size() && autoAnchor[i] >= 0 && (size_t)autoAnchor[i] < g_autoTrig.size()) ? g_autoTrig[(size_t)autoAnchor[i]].fireT : -2, i < prevCy.size() ? (double)prevCy[i] : 0.0);
                    }
                    std::fflush(stdout);
                    std::abort();
                }
            }
            // Only walks WITH a one-tick seek show that the walks leave it: a later group of the tick does not seek (its walks are the step from the held place alone), so it can take the mark away, not give it.
            const bool st = sought ? (lastStep1 && same) : (sq.stable[k] && same);
            sq.stable[k] = st ? 1 : 0;
            // (an anchored one also where the previous place seekOne keeps is not this place's float: a call that seeks it writes that, and prevCy is held with the bytes)
            if (st) { const bool rs = objs[i].dcx != 0.0 || objs[i].dcy != 0.0 || !(objs[i].pcy == (float)objs[i].cy) || (sq.anch[k] && i < prevCy.size() && !(prevCy[i] == (float)objs[i].cy));
                      if (rs && !sq.res[k]) sq.resList.push_back((uint32_t)k); sq.res[k] = rs ? 1 : 0; } else sq.res[k] = 0;
            // what a call without groupPrevApply writes for an anchored object at rest that it seeks and places: prevCy = pcy = (float)cy, dcx = cx - (float)cx (seekOne's old place is a float; the
            // row's own x cancels), dcy = cy - prevCy. One that only places it writes dcy alone, which is the same value once prevCy is that float. Held otherwise: not left out of such a call.
            if (st && sq.anch[k]) {
                const float fy = (float)objs[i].cy;
                const bool nfix = i < prevCy.size() && prevCy[i] == fy && objs[i].pcy == fy && objs[i].dcx == objs[i].cx - (double)(float)objs[i].cx && objs[i].dcy == objs[i].cy - (double)fy;
                if (!nfix && !sq.gres[k]) sq.gresList.push_back((uint32_t)k);
                sq.gres[k] = nfix ? 0 : 1;
            } else sq.gres[k] = 0;
            if (!sq.check) { sq.skip[i] = st ? 1 : 0; if (!st) sq.todo[keep++] = (uint32_t)k; else sqIxAdd(k); }
        }
        if (!sq.check) sq.todo.resize(keep);
        if (!sq.check && byBucketN == objs.size()) {   // collect's index is whole: the marks are the ones just given
            if (sq.ix.hardN != objs.size()) {
                sq.ix.hard.assign(byBucket.size(), {});
                for (size_t b = 0; b < byBucket.size(); ++b) for (const uint32_t i : byBucket[b]) if (sq.slot[i] < 0) sq.ix.hard[b].push_back(i);
                sq.ix.hardN = objs.size();
            }
            sq.ix.live = true;
        }
        if (checked) g_sqStats.checked.fetch_add(checked, std::memory_order_relaxed);
        sq.lastT = lastT; sq.lastStep1 = lastStep1; sq.epoch = seekEpoch; sq.applies = applyImplCalls;
    }
    // the supply's placement wrote this object (after the walks of the call that is open or just closed): its next walks put the model's own place back, so it is not stable
    bool sqTouch(const Obj* o) {
        if (objs.empty() || o < objs.data() || o >= objs.data() + objs.size()) return false;
        const size_t i = (size_t)(o - objs.data());
        incSupplyWrote(i);
        if (sq.built && i < sq.slot.size() && sq.slot[i] >= 0) {
            const size_t k = (size_t)sq.slot[i];
            if (sq.stable[k]) { sq.stable[k] = 0; if (!sq.check) { sq.skip[i] = 0; sq.todo.push_back((uint32_t)k); sqIxDel(k); } }
        }
        return true;
    }
    // [R] The supply's placement (search_dp.hpp supplyPlaceMovers) runs after a group's placement of tick T and writes the object's place, its step and its previous place (cx, cy, dcx, dcy, pcy,
    // the scale) from the trigger shadow's timeline. The plain pass puts the model's own bytes back: every object is sought and placed at T + 1 (cy the model's, dcy = cy - prevCy with prevCy the
    // place it held, which is the supply's: seekOne), and again at T + 2, the first tick whose previous place is the model's too; a later group of tick T whose dates differ places every pure
    // object once more (applyTriggersImpl's samePure). The index lists leave an object out where a placement writes the bytes it holds, and after such a write it does not hold them: an anchored
    // slot outside its span (uid 6137 of a custom level from t=3430: the shadow's 155.964294 where its closed form ended on 155.964005 at t=3405), an object with no controller between its
    // switches. So the lists of T (the calls after the write), T + 1 and T + 2 take the object, whatever class it is in; a write at T + 1 moves the end on. --incplacecheck holds it.
    void incSupplyWrote(size_t i) {
        if (!g_incPlace || !inc.valid) return;
        if (inc.wUntil.size() != objs.size()) { inc.wUntil.assign(objs.size(), INT_MIN); inc.wList.clear(); }
        if (inc.wUntil[i] == INT_MIN) { inc.wList.push_back((uint32_t)i); inc.wSorted = false; }
        inc.wUntil[i] = lastT + kWroteTicks;
    }
    static constexpr int kWroteTicks = 2;   // T + 1 and T + 2 (above)
    // the written objects the lists of tick t take, ascending. One whose end is past t + kWroteTicks was written at a later tick than this one: the seek went back, which places every object.
    const std::vector<uint32_t>& incWrote(int t) {
        std::vector<uint32_t>& w = inc.wList;
        size_t keep = 0;
        for (size_t k = 0; k < w.size(); ++k) {
            const int u = inc.wUntil[w[k]];
            if (u < t || (long long)u > (long long)t + kWroteTicks) { inc.wUntil[w[k]] = INT_MIN; continue; }
            w[keep++] = w[k];
        }
        w.resize(keep);
        if (!inc.wSorted) { std::sort(w.begin(), w.end()); inc.wSorted = true; }
        return w;
    }
    const std::vector<uint32_t>* incWithWrote(const std::vector<uint32_t>* l, int t, std::vector<uint32_t>& buf) {
        if (inc.wList.empty()) return l;
        const std::vector<uint32_t>& w = incWrote(t);
        if (w.empty()) return l;
        buf.clear();
        std::set_union(l->begin(), l->end(), w.begin(), w.end(), std::back_inserter(buf));
        g_incStats.wListed += (long long)(buf.size() - l->size());
        return &buf;
    }
    // The lists of the seek and of a tick's first applyTriggers without the stable quiet objects. Those loops pass a stable object by on its byte (sq.skip), but the object was still in the
    // list: where a level's objects with no recording are most of the placed-every-tick set (the supply's on/off makes them non-pure: isPure), a tick walked the whole set to place a fifth
    // of it (a custom level: 14,403 in the set, 12,230 of them quiet, 11 of those to do on a tick of a walk). With the open call passing objects by and few of them to do, the list is made
    // from the set without the eligible objects (sq.alwHard) plus the eligible ones of the set that are to do (sqTodoAlways): an object is left out only where the loop would have passed
    // it by, so the same objects are placed, in the same order. The byte tests in the loops stay. QLISTCHECK=1 (env, read once): each such list is held against the full one.
    bool sqShortOk() {
        if (!sq.active || sq.check || sq.todo.size() * 8 > sq.s.size() || !inc.valid) return false;
        if (sq.alwFor != incBuilds) {
            sq.inAlw.assign(sq.s.size(), 0); sq.alwHard.clear();
            for (const uint32_t i : inc.always) { const int32_t k = sq.slot[i]; if (k >= 0) sq.inAlw[(size_t)k] = 1; else sq.alwHard.push_back(i); }
            sq.alwFor = incBuilds;
        }
        return true;
    }
    void sqTodoAlways(std::vector<uint32_t>& out) const {
        out.clear();
        for (const uint32_t k : sq.todo) if (sq.inAlw[k]) out.push_back(sq.s[k]);
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
    }
    // QLISTCHECK: the objects of `l` the loop will place (not passed by) are the ones of `l` + the whole placed-every-tick set, which is the list as it was made before
    void sqListCheck(const std::vector<uint32_t>& l, const char* what, int t) {
        std::vector<uint32_t> full;
        std::set_union(l.begin(), l.end(), inc.always.begin(), inc.always.end(), std::back_inserter(full));
        size_t a = 0; bool same = true;
        for (const uint32_t i : full) {
            if (sq.active && sq.skip[i]) continue;
            while (a < l.size() && (sq.active && sq.skip[l[a]])) ++a;
            if (a >= l.size() || l[a] != i) { same = false; break; }
            ++a;
        }
        while (same && a < l.size()) { if (!(sq.active && sq.skip[l[a]])) same = false; ++a; }
        for (size_t q = 1; same && q < l.size(); ++q) same = l[q - 1] < l[q];
        g_sqStats.listChecked.fetch_add(1, std::memory_order_relaxed);
        if (!same) {
            std::printf("qlistcheck: MISMATCH %s t=%d: the short list (%zu) does not place the objects the full one (%zu) does\n", what, t, l.size(), full.size());
            std::fflush(stdout);
            std::abort();
        }
    }
    bool groupPrevPass(TouchMask mask, int fireHi, const uint16_t* fireB, float lockPrev, int t1, double pxPrev) {
        const bool quiet = g_dynDbg == -1 && g_shiftDbgUid < 0 && !g_shiftStat && !(g_rotCompute && !g_rotSpec.empty());
        if (objs.empty() || (!anyTrig && !anyAuto) || !quiet) return false;
        if (lockMover.size() != objs.size()) prepareLockMover();
        const uint64_t pe = pureEpoch; const int pt = pureT; const std::vector<int> pf = pureFire;
        pureEpoch = seekEpoch; pureT = t1; pureFire.assign(g_autoTrig.size(), 0);
        for (size_t k = 0; k < g_autoTrig.size(); ++k) pureFire[k] = g_autoTrig[k].fireT;
        applyTriggersImpl(mask, fireHi, fireB, lockPrev, t1, pxPrev, true);
        gpPending = true;
        gpCx.resize(objs.size()); gpCy.resize(objs.size());
        forEachNonPure([&](size_t i) { gpCx[i] = (double)objs[i].cx; gpCy[i] = (double)objs[i].cy; });
        pureEpoch = pe; pureT = pt; pureFire = pf;
        return true;
    }
    bool gpPending = false;   // the previous-place pass ran for the open call and groupPrevApply has not yet (--incplacecheck reads it: incDiff)
    void groupPrevApply() {
        gpPending = false;
        if (!lastStep1) return;
        forEachNonPure([&](size_t i) {
            if (i >= gpCx.size()) return;
            objs[i].dcx = (double)objs[i].cx - gpCx[i];
            objs[i].dcy = (double)objs[i].cy - gpCy[i];
            objs[i].pcy = (float)gpCy[i];
        });
    }

    size_t size() const { return objs.size(); }

    // ---- caches that change nothing but the time taken ----------------------
    // applyTriggers runs once per frontier GROUP, and a tick can have many
    // groups. For most moving objects the placement it computes does not depend
    // on the group at all: an object no touch box reaches, that is not
    // formula-driven and not locked to the player, is placed from seek()'s state
    // for this tick, its own static data and the level-wide autonomous clocks
    // (AutoTrig::fireT) -- nothing the group carries. Those "pure" objects are
    // placed once and left alone until one of the three changes. Profiled on a
    // level with 868 moving objects: applyTriggers was 9.5% of the main thread,
    // which is the critical path (the workers are idle ~70% of the time).
    uint64_t seekEpoch = 0;              // bumped by every seek() that rewrites
    uint64_t pureEpoch = ~0ull;          // what the pure objects were placed for:
    int pureT = -1;                      //   seek epoch, tick,
    std::vector<int> pureFire;           //   and every AutoTrig::fireT
    bool isPure(size_t i) const {
        // --trigsupply-all (T29): the on/off of an object with no recording is the shadow's for the group being placed (g_supplyOnHook), so it is not the same for two groups of one tick
        return isPureButSupply(i) && !(g_supplyOnHook && i < samples.size() && samples[i].size() <= 1);
    }
    bool isPureButSupply(size_t i) const {
        return !trigMask[i]
            && !(i < onEv.size() && !onEv[i].empty())   // --activators: depends on the group
            && !(i < formula.size() && formula[i])
            && !(i < autoLockTrig.size() && autoLockTrig[i] >= 0)
            // --recendtail: the lock reads the group's x, a touch switch its boxes
            && !(i < rigTail.size() && rigTail[i].lockB >= 0)
            && !(i < tailOnBox.size() && tailOnBox[i])
            // --areamove: placed from the group's x
            && !(i < areaArmed.size() && areaArmed[i])
            && !(i < areaAdd.size() && areaAdd[i])   // --areaadd: the same, on top of a closed form
            // --tapdisarm: the on/off reads autoDisarmed, which asks the group's boxes
            && !(i < disarmDep.size() && disarmDep[i])
            // --boxarm: a Move of it fires only where the group's boxes switched it back on
            && !(i < armDep.size() && armDep[i]);
    }
    // --boxarm: a part, anchor or lock of the object is an offInit entry (AutoTrig::offInit), so where it is
    // placed depends on the state's boxes. Filled by the CLI once the level is loaded, under the flag only.
    std::vector<uint8_t> armDep;
    // collect() walks one bucket's objects instead of all of them. Built in
    // seek(), which runs on the main thread between the parallel phases:
    // collect() is also called from step() on the worker threads, so it must
    // never build this itself. Until it exists collect() takes the full walk.
    std::vector<std::vector<uint32_t>> byBucket;
    size_t byBucketN = (size_t)-1;
    bool anyUidM1 = false;   // an object --dyndbg's default of -1 would name
    size_t nNonPure = 0;     // objects a same-tick applyTriggers still places
    std::unordered_map<int, uint32_t> uidIdx;   // uid -> index in objs (collect()'s supplied-mover hits); built with the buckets
    void rebuildBuckets() {
        uidIdx.clear();
        for (size_t i = 0; i < objs.size(); ++i) if (objs[i].uid >= 0) uidIdx[objs[i].uid] = (uint32_t)i;
        byBucket.assign(8, {});
        for (size_t i = 0; i < objs.size() && i < bucket.size(); ++i)
            if (bucket[i] < byBucket.size())
                byBucket[bucket[i]].push_back((uint32_t)i);
        anyUidM1 = false;
        for (const Obj& o : objs) anyUidM1 |= (o.uid == -1);
        nNonPure = 0;
        for (size_t i = 0; i < objs.size(); ++i) nNonPure += !isPure(i);
        byBucketN = objs.size();
    }
    // The per-object loops of seek() and applyTriggers() split across the
    // search's pool (g_dynParTasks) in runs of kParRun objects. body(b, e)
    // handles objects [b, e). Only for a loop whose body writes nothing but its
    // own object's slots -- then every object is placed by the same arithmetic
    // whichever thread runs it, and the result is the serial loop's. `work` is
    // how many objects the call will actually place -- a second group of the
    // same tick places only the non-pure ones, usually a handful, and the pool
    // is not woken for a handful. Below kParMin the loop stays on this thread.
    // 512 against 2,048: no slower on levels with 868 and 1,063 moving objects,
    // and faster on one with 4,350 (77.5 s against 80.7).
    static constexpr size_t kParMin = 512, kParRun = 64;
    // ...over positions [0, n) of whatever list the caller walks (all the objects, or an index list of them)
    template <class F> void forRunsN(bool par, size_t work, size_t n, F&& body) {
        if (!par || !g_dynParTasks || work < kParMin) { body((size_t)0, n); return; }
        g_dynParTasks((n + kParRun - 1) / kParRun, [&](size_t k) {
            body(k * kParRun, std::min(n, (k + 1) * kParRun));
        });
    }
    template <class F> void forRuns(bool par, size_t work, F&& body) {
        forRunsN(par, work, objs.size(), std::forward<F>(body));
    }
    IncState inc;   // see incBuild

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
    // `fireB` is the group's per-box firing ticks (State::fireB, aggregated the
    // same conservative way `fireHi` is: the LATEST tick in the group). Only
    // the formula path reads it; it may be null, and then no object is
    // formula-driven for this call.
    // `lockOff` is how far the state has travelled since it punched the locked
    // box (State::lockOff). Applied only on the formula path: a recording of a
    // locked object already contains the recorded run's own lock, so adding
    // this to it would count the ride twice.
    // --advfollow: per object, the x of the Advanced Follow trigger that carries it (-1e30: none). Filled by the level loader.
    std::vector<double> advFollowX;
    bool anyAdvFollow = false;
    // After the tick's placement: an object an Advanced Follow carries stands at the player's x of the tick before once that x
    // has crossed the trigger (areaPxPrev, set by prepareGroup just before applyTriggers; the same crossing test as an Area action).
    void placeAdvFollow() {
        if (!anyAdvFollow) return;
        for (size_t i = 0; i < objs.size() && i < advFollowX.size(); ++i)
            if (advFollowX[i] > -1e29 && areaPxPrev >= advFollowX[i]) objs[i].cx = (float)areaPxPrev;
    }
    void applyTriggers(TouchMask mask, int fireHi, const uint16_t* fireB,
                       float lockOff, int t, double px = 0.0) {
        applyTriggersBase(mask, fireHi, fireB, lockOff, t, px);
        placeAdvFollow();
    }
    // --incplacecheck: the plain arm of a comparison is running. [R] A plain placement of every object notes the index's dates as placed under and takes every slot's `force` off
    // (incNotedPlaced, incPSyncAll below): right for a run whose placement it is, but the comparison's plain arm follows the incremental arm of the same call, whose notes are the ones the
    // run without the check carries on. A slot woken by a date at tick T (incPWake: placed at T and T + 1, the tick its step settles) lost its T + 1 to the plain arm, and the check then
    // reported the object at T + 1 ([M] a custom level, anchor 3012: uid 154735 at t=29833, dcy -5.997 against 0, its slot's spans ended at 29742 and force unset). The plain arm leaves them.
    bool chkPlainArm = false;
    void applyTriggersBase(TouchMask mask, int fireHi, const uint16_t* fireB,
                           float lockOff, int t, double px = 0.0) {
        if (!g_incCheck || !g_incPlace || !inc.valid || inc.gen != g_dynPlaceGen || byBucketN != objs.size()) {
            applyTriggersImpl(mask, fireHi, fireB, lockOff, t, px, true);
            return;
        }
        // --incplacecheck: the same placement walked over the index lists and over every object, from the same state
        if (chkT == t) {
            // the first call of an incrementally sought tick: the seek and this call, against the full seek and the plain call
            chkT = INT_MIN;
            IncSnap a, b;
            applyTriggersImpl(mask, fireHi, fireB, lockOff, t, px, true);
            incTake(a);
            incPut(chkPre);
            seekFull(t, false, true, g_dynDbg == -1 && !anyUidM1);
            chkPlainArm = true;
            applyTriggersImpl(mask, fireHi, fireB, lockOff, t, px, false);
            chkPlainArm = false;
            incTake(b);
            ++g_incStats.checked;
            g_incStats.mismatch += (long long)incDiff(a, b, "seek+applyTriggers", t);
            return;
        }
        IncSnap pre, a, b;
        incTake(pre);
        applyTriggersImpl(mask, fireHi, fireB, lockOff, t, px, true);
        incTake(a);
        incPut(pre);
        chkPlainArm = true;
        applyTriggersImpl(mask, fireHi, fireB, lockOff, t, px, false);
        chkPlainArm = false;
        incTake(b);
        ++g_incStats.checked;
        g_incStats.mismatch += (long long)incDiff(a, b, "applyTriggers", t);
    }
    // `lists`: walk the index lists of the incremental placement (incBuild) where they are good, instead of
    // every object.
    // dupbatch: the set a leader's build notes its tick reads in (FireReadSet above; null but for that build), and the objects it is kept for: 1 = the object stays on this placement (the supply
    // has no slot for it), 0 = the supply places it. An object beyond the table is noted (an empty table notes every object). setTickTrack also takes in every object of the lock pass when one
    // noted object is in it: those are placed in a chain through AutoTrig::lockLastX, the one thing a placement reads that another object's wrote.
    FireReadSet* fireSink = nullptr;
    std::vector<uint8_t> tickTrack;
    long long tickTrackGen = -1;
    template <class F> void setTickTrack(F&& stays, long long gen) {
        tickTrack.assign(objs.size(), 0);
        bool lockIn = false;
        for (size_t i = 0; i < objs.size(); ++i)
            if (stays(objs[i].uid)) { tickTrack[i] = 1; if (i < autoLockTrig.size() && autoLockTrig[i] >= 0) lockIn = true; }
        if (lockIn) for (size_t i = 0; i < objs.size() && i < autoLockTrig.size(); ++i) if (autoLockTrig[i] >= 0) tickTrack[i] = 1;
        tickTrackGen = gen;
    }
    void applyTriggersImpl(TouchMask mask, int fireHiRaw, const uint16_t* fireBRaw,
                           float lockOff, int t, double px, bool lists) {
        ++applyImplCalls;   // (sqHead: a call outside prepareGroup's walks ends the quiet objects' run)
        if (!anyTrig && !anyAuto) return;
        // (serial caller: the per-object loops below are the parallel part)
        if (lockMover.size() != objs.size()) prepareLockMover();
        // Skip the pure objects when they are already placed for exactly this
        // (seek epoch, tick, autonomous clocks). Never with a print flag on: those
        // print per call, and skipping would change what they say.
        const bool quiet = g_dynDbg == -1 && g_shiftDbgUid < 0 && !g_shiftStat
                           && !(g_rotCompute && !g_rotSpec.empty());
        bool samePure = quiet && pureEpoch == seekEpoch && pureT == t
                        && pureFire.size() == g_autoTrig.size();
        for (size_t k = 0; samePure && k < g_autoTrig.size(); ++k)
            samePure = (pureFire[k] == g_autoTrig[k].fireT);
        if (!samePure) {
            pureEpoch = seekEpoch;
            pureT = t;
            pureFire.resize(g_autoTrig.size());
            for (size_t k = 0; k < g_autoTrig.size(); ++k)
                pureFire[k] = g_autoTrig[k].fireT;
        }
        // With no print flag on, the only thing an object's placement writes
        // outside its own slots is AutoTrig::lockLastX (the lockToPlayerX
        // branch), and nothing but that branch reads it. So the objects that
        // can take it (autoLockTrig >= 0) are placed afterwards, in index order
        // on this thread, and every other object is placed in runs across the
        // pool (forRuns). The rotation branch, which reads another object, runs
        // only under g_rotCompute, and `quiet` excludes that too.
        const bool par = quiet && !anyUidM1 && byBucketN == objs.size();
        // The objects this call can write at all, in index order (incBuild): a same-tick call skips the pure
        // ones, and the first call of a tick has nothing to do for a pure object no autonomous trigger
        // anchors. Where the lists are not good the loops below walk every object, as they always did.
        const std::vector<uint32_t>* idxs = nullptr;
        if (lists && quiet && g_incPlace && inc.valid && inc.gen == g_dynPlaceGen && byBucketN == objs.size())
            idxs = samePure ? ((sq.active && sq.walkOn) ? &sq.walk : &inc.nonPure) : incAtFirstList(t);   // (sq.walk: the non-pure objects but the quiet ones, sqHead)
        else if (!samePure && inc.valid && inc.gen == g_dynPlaceGen && !chkPlainArm) {
            incNotedPlaced(t);   // every object is walked: the d-class ones are placed too
            incPSyncAll();       // ...and so are the anchored ones
        }
        if (idxs) { ++g_incStats.atLists; g_incStats.atListObjs += (long long)idxs->size(); }
        else ++g_incStats.atFull;
        const bool sqPass = sq.active;
        auto place = [&](size_t b, size_t e, bool lockPass) {
        for (size_t kk = b; kk < e; ++kk) {
            const size_t i = idxs ? (size_t)(*idxs)[kk] : kk;
            if (sqPass && sq.skip[i]) continue;   // a quiet object (sqHead)
            if (samePure && isPure(i)) continue;
            if (par && lockPass != (i < autoLockTrig.size() && autoLockTrig[i] >= 0))
                continue;
            // the group's fire ticks as this object's placement reads them (dupbatch: noted for the objects of tickTrack while a leader's set is in)
            FireReadSet* const rdSink = (fireSink && (fireSink->everyObject || i >= tickTrack.size() || tickTrack[i])) ? fireSink : nullptr;
            const FireTicks fireB(fireBRaw, rdSink);
            const FireHi fireHi{fireHiRaw, rdSink};
            const TouchMask m = trigMask[i];
            // FORMULA-DRIVEN OBJECTS LEAVE HERE, before anything reads a
            // sample. `fireHi` cannot serve them: it is the latest box THE
            // STATE entered, which need not be a box that reaches THIS object,
            // so two doors opened by two different boxes would be handed the
            // same tick. The fit wants 20,504 for one and 20,591 for the
            // other. What this object needs is the latest tick among the boxes
            // that both reach it and the state has entered -- which is exactly
            // what the per-box array gives, and is why this path could not be
            // written before State carried one.
            // Deliberately NOT conditioned on `fireB` being non-null: which
            // path an object takes is a property of the object, and making it
            // depend on a pointer would let a caller that forgot the array
            // silently put a formula-driven object back on the recording path
            // -- with the autonomous fields the loader filled in for the
            // formula still set, which is a combination that exists nowhere
            // else. A null array simply means no box has been entered.
            // --boxparts (formula 2) only while the recording holds no motion of this object: where it
            // does, the recording path's re-timing is the measured one (lv22 uid2748, +9 px over 48 ticks
            // from t=2,745, is k^2/128 there and 16% short in the part formula).
            if (i < formula.size() && formula[i]
                && !(formula[i] == 2 && trigRecFire[i] >= 0)
                && !samples[i].empty()) {
                // --formularec: every box that moves it as the attempt left it -> seek's position
                // (the recording at t, unshifted) stands. Delta 0 only: a box entered a tick apart
                // takes the formula.
                // --formulanolock: the formula has no lock term, so such an object reads the recording
                // whichever attempt left it (as far as it reaches) -- the same read as an own attempt's
                const long long lockReach = (g_recProvGiven && g_recProvRec >= 0) ? (long long)g_recProvReach
                                                                                  : (long long)samples[i].back().t;
                const bool lockObj = i < formulaLock.size() && formulaLock[i];
                const bool lockRead = lockObj && t <= lockReach;
                if (formulaRecProvOk(t) || lockRead) {
                    bool agree = true;
                    for (int b = 0; b < kTouchBits && agree; ++b) {
                        if (!(m & touchBit(b))) continue;
                        const int rec = g_touchRecEntry[(size_t)b];
                        const bool st = fireB && (mask & touchBit(b));
                        if (st) agree = rec >= 0 && (int)fireB[b] == rec;
                        // --formularectouch: an entry dated t shows in the state's mask from t + 1 (the
                        // placement for t runs before t's collision pass), so the attempt's counts from
                        // then too. Without the flag the test is the old one.
                        else agree = !(rec >= 0 && ((rec < t)));
                    }
                    if (agree) {
                        // Written here, not left to seek: another group placed this object on
                        // the same tick before (pureEpoch), and its formula value would stand.
                        // seek's read: the last row at or before tt, linear across a recorder gap
                        // of at most kRecInterpGap (--recinterp).
                        const auto& sm = samples[i];
                        auto recAt = [&](int tt, double& rx, double& ry) {
                            size_t lo = 0, hi = sm.size();
                            while (lo + 1 < hi) {
                                const size_t md = (lo + hi) / 2;
                                if (sm[md].t <= tt) lo = md; else hi = md;
                            }
                            const DynSample& s = sm[lo];
                            rx = s.cx; ry = s.cy;
                            if (s.t < tt && lo + 1 < sm.size() && sm[lo + 1].t - s.t <= kRecInterpGap
                                && !recStallGap(sm, lo)) {
                                const DynSample& n = sm[lo + 1];
                                const double u = (double)(tt - s.t) / (double)(n.t - s.t);
                                rx = (double)s.cx + u * ((double)n.cx - (double)s.cx);
                                ry = (double)s.cy + u * ((double)n.cy - (double)s.cy);
                            }
                        };
                        double rx = 0.0, ry = 0.0, px = 0.0, py = 0.0;
                        // ...a locked one a row behind: its lock moves it after the tick's collision pass, so
                        // the pass meets it where the row before left it (a mini swing riding the block row of
                        // --formulanolock's note sits 1 row of the block's descent above the unshifted read)
                        const int lag = lockObj ? 1 : 0;
                        recAt(t - lag, rx, ry);
                        recAt(t - 1 - lag, px, py);
                        objs[i].cx = (float)rx;
                        objs[i].cy = (float)ry;
                        objs[i].dcy = ry - py;
                        if (g_dynDbg == objs[i].uid || g_dynDbg == -3)
                            std::printf("frec: uid=%d t=%d cx=%.3f cy=%.4f dcy=%+.4f\n",
                                        objs[i].uid, t, rx, ry, ry - py);
                        g_formulaRecHit.fetch_add(1, std::memory_order_relaxed);
                        continue;
                    }
                    g_formulaRecMiss.fetch_add(1, std::memory_order_relaxed);
                }
                const DynSample& s0 = samples[i][0];
                int fb = -1;
                const TouchMask mm = m & mask;   // the boxes that reach this object and the state has entered (made once: the loops below asked `(m & mask) & touchBit(b)` bit by bit, 8% of lv22's prepareGroup)
                if (fireB && mm.any())
                    for (int b = 0; b < kTouchBits; ++b)
                        if (mm.test(b))
                            fb = std::max(fb, (int)fireB[b]);
                const int aa0 = autoAnchor[i];
                const int ft0 = (aa0 >= 0 && (size_t)aa0 < g_autoTrig.size())
                                ? autoFireT(aa0, mask, fireB) : -1;   // --boxarm
                // The offset at an arbitrary tick. Being a closed form is the
                // point: it answers for t-1 as readily as for t, which is what
                // makes dcy exact here even across an anchor jump, where the
                // recording-driven branch has to fall back to two consecutive
                // recorded rows and reads 0 when the recorder skipped one.
                // --movetarget: the touch side PER BOX, each part at its own
                // box's firing tick (the sum above answers one tick for all of
                // them, which is undecidable once a Stop or a target-mode Move
                // is in play); a Stop freezes the Moves it names at the tick
                // its box fires; a target-mode part's distance is decided on
                // the tick it starts. The group's position is read off this
                // object's own offset, which is right when the object and the
                // target's reference point share their controllers (lv22's
                // ceiling group 265 does: uid18094 is one of its members).
                auto stopTick = [&](int mover) -> int {
                    int st = 1 << 30;
                    if (mover == 0 || !fireB) return st;
                    const int nb = std::min((int)g_touchStops.size(), kTouchBits);
                    for (int b = 0; b < nb; ++b) {
                        if (!mask.test(b)) continue;
                        // --spawnlag: the Move's last step is the fire tick's own (a touch
                        // Stop fires in checkCollisions, after that tick's move step), so it
                        // freezes at f, not f + 1. lv22 it8: box uid18091 (entered at 2730)
                        // stops the spike row's -480 descent, and one step too many put the row
                        // 0.4 px below the game until the replay died at 2775.
                        for (const int s : g_touchStops[(size_t)b])
                            if (s == mover)
                                st = std::min(st, (int)fireB[b] + (0));
                    }
                    return st;
                };
                auto plainAt = [&](int tt, double& ox, double& oy) {
                    ox = 0.0; oy = 0.0;
                    // A MOVE OF DURATION 0 IS DONE ON ITS OWN TICK: its eased
                    // fraction is 1, and the `/dur` guard that kept it out of
                    // the sum dropped the move altogether. SubZero 4002's group
                    // 87 falls 600 px that way (trigger uid18255 at x=31,899,
                    // dur 0), so every formula-driven member of it stayed where
                    // it started: the saw uid16730 was tested at cy=2,303 while
                    // the recording -- and GD -- have it at 1,703, and the model
                    // walked through the kill GD makes at t=22,893 (ledger
                    // 2026-09-23 06:5x). The recording-driven members were right
                    // all along, which is why this showed up as one object
                    // disagreeing with its own group.
                    if (fireB)
                        for (const AutoPart& p : touchParts[i]) {
                            if (p.tmode) continue;
                            if (!mm.test(p.trig)) continue;
                            // --spawnlag: a queued Spawn on the way starts it later
                            const int f = (int)fireB[p.trig] + (p.lag);
                            const int te = std::min(tt, stopTick(p.mover));
                            if (te <= f) continue;
                            const double e = p.dur > 0.0
                                ? gdEase(p.ease, p.erate,
                                         (double)(te - (f + 1)) / p.dur)
                                : 1.0;
                            ox += p.dx * e; oy += p.dy * e;
                        }
                    if (ft0 >= 0) {
                        int te = std::min(tt, stopTick(g_autoTrig[(size_t)aa0].uid));
                        if (g_autoStop) te = std::min(te, autoStopT(aa0));   // --autostop
                        if (te >= ft0) {
                            const double e = autoDur[i] > 0.0
                                ? gdEase(autoEase[i], autoErate[i],
                                         (double)(te - ft0) / autoDur[i])
                                : instantE(te, ft0);   // --instantlag
                            ox += autoDx[i] * e; oy += autoDy[i] * e;
                        }
                    }
                };
                auto targetAt = [&](int tt, double& ox, double& oy) {
                    plainAt(tt, ox, oy);
                    if (!fireB) return;
                    struct TP { int f; const AutoPart* p; double ddx, ddy; };
                    TP tp[8];
                    int n = 0;
                    for (const AutoPart& p : touchParts[i])
                        if (p.tmode && mm.test(p.trig) && n < 8)
                            tp[n++] = {(int)fireB[p.trig] + (p.lag), &p,
                                       0.0, 0.0};
                    std::sort(tp, tp + n, [](const TP& a, const TP& b) { return a.f < b.f; });
                    auto contrib = [&](const TP& q, int at, double& cx, double& cy) {
                        cx = 0.0; cy = 0.0;
                        if (at <= q.f || q.p->dur <= 0.0) return;
                        const double e = gdEase(q.p->ease, q.p->erate,
                                                (double)(at - (q.f + 1)) / q.p->dur);
                        cx = q.ddx * e; cy = q.ddy * e;
                    };
                    for (int k = 0; k < n; ++k) {
                        // The tick whose position it reads. Without --spawnlag the tick it
                        // starts (f + 1). With it, the fire tick f itself: GD samples the group
                        // when the Move-To fires -- after that tick's move step for a touch
                        // (checkCollisions), before the next one for a queued spawn
                        // (updateSpawnTriggers) -- which is the position at the END of tick f.
                        // lv22 it8 (box uid18091 entered at 2730, the spike row still rising
                        // under a yellow box from 2686): read at f + 1 the row lands 0.399 px
                        // below the game for the rest of the move; the old reading matched
                        // only because the unlagged yellow part ran one tick early.
                        const int s = tp[k].f + (0);
                        double bx = 0.0, by = 0.0;
                        plainAt(s, bx, by);
                        for (int j = 0; j < k; ++j) {
                            double cx, cy;
                            contrib(tp[j], s, cx, cy);
                            bx += cx; by += cy;
                        }
                        const int md = tp[k].p->tmode;   // 1 both, 2 x only, 3 y only
                        tp[k].ddx = (md == 3) ? 0.0 : (double)tp[k].p->tdx - bx;
                        tp[k].ddy = (md == 2) ? 0.0 : (double)tp[k].p->tdy - by;
                    }
                    for (int k = 0; k < n; ++k) {
                        double cx, cy;
                        contrib(tp[k], tt, cx, cy);
                        ox += cx; oy += cy;
                    }
                };
                const bool perBox = !touchParts[i].empty();
                auto offsetAt = [&](int tt, double& ox, double& oy) {
                    if (perBox) { targetAt(tt, ox, oy); return; }
                    ox = 0.0; oy = 0.0;
                    // dur 0 = done on its own tick (the superposition arm below
                    // already reads it that way); see the note in plainAt
                    if (fb >= 0 && tt > fb) {
                        const double e = trigDur[i] > 0.0
                            ? gdEase(trigEase[i], trigErate[i],
                                     (double)(tt - (fb + 1)) / trigDur[i])
                            : 1.0;
                        ox += trigDx[i] * e; oy += trigDy[i] * e;
                    }
                    // NO +1 on this side. The two ticks mean different things:
                    // `fireB` is the tick the box was ENTERED and the move
                    // starts the tick after, while `AutoTrig::fireT` is already
                    // the EFFECT tick (`fireT = crossing + delay`, cli.hpp), so
                    // the move starts on it. The recording-driven branch says
                    // the same by setting `lat = 0` for the autonomous case and
                    // 5 for the touch case. With a spurious +1 lv19's door sits
                    // 0.0615 px above the recording at t=20,601.
                    if (ft0 >= 0 && tt >= ft0) {
                        const int te = g_autoStop ? std::min(tt, autoStopT(aa0)) : tt;
                        const double e = autoDur[i] > 0.0
                            ? gdEase(autoEase[i], autoErate[i],
                                     (double)(te - ft0) / autoDur[i])
                            : instantE(te, ft0);   // --instantlag
                        if (te >= ft0) { ox += autoDx[i] * e; oy += autoDy[i] * e; }
                    }
                };
                double fx = 0.0, fy = 0.0, px1 = 0.0, py1 = 0.0;
                offsetAt(t, fx, fy);
                offsetAt(t - 1, px1, py1);
                if (g_dynDbg == objs[i].uid)
                    std::printf("formdbg t=%d uid=%d perBox=%d fb=%d ft0=%d "
                                "autoDur=%.2f autoDy=%.1f trigDur=%.2f fy=%.3f "
                                "base=%.3f -> %.3f\n",
                                t, objs[i].uid, perBox ? 1 : 0, fb, ft0,
                                autoDur[i], autoDy[i], trigDur[i], fy,
                                (double)s0.cy, (double)s0.cy + fy);
                const double ncy = (double)s0.cy + fy;
                // ...and the face's PER-TICK motion, from the same closed form.
                // Without it the object teleports as far as anything standing
                // on it is concerned: the support tolerance is
                // `kSupportTol + |dcy|`, so a rider on a descending face keeps
                // its footing only while dcy says the face moved. This IS lv19
                // t=20,601 -- a ball riding uid14011 down at 0.062 px/tick --
                // and with dcy at zero the model drops the support on the first
                // tick, falls, and dies 45 ticks later on a hazard the ride
                // would have carried it over.
                //
                // NOT gated on `lastStep1`, unlike the branch below. That gate
                // exists because a position difference across an anchor jump is
                // the platform's whole journey rather than one tick; a formula
                // evaluated at t-1 is one tick by construction, so the case the
                // gate protects against cannot arise -- and the first tick
                // after an anchor, which is exactly where this object is
                // ridden, is not a 1-tick advance.
                objs[i].dcy = fy - py1;
                // x: the moves, plus the lock. The 158 px that `s0.cx + fx`
                // could not account for (uid14011 is recorded at cx=28,789 at
                // t=20,601 against a base of 28,631, with dx=0 and adx=0 on both
                // controllers) is a lockToPlayerX -- the recording shows its x
                // advancing by the player's own dx, from the tick the box fires.
                // With the lock the closed form covers x as well, so this writes
                // it; without one it stays on the recording, which is where the
                // objects the formula cannot fully describe belong.
                const bool locked = (g_lockBox >= 0 && i < touchLock.size()
                                     && touchLock[i] > 0.0
                                     && mask.test(g_lockBox));
                if (locked) objs[i].cx = (double)s0.cx + fx + (double)lockOff;
                // --boxparts (formula 2): the parts ARE this object's whole motion, so x as well.
                // In a turned frame (lv22's rotated section) a world-y dip is a frame-x move, and
                // writing y alone left the copy in frames 1 and 3 where it started.
                else if (formula[i] == 2) objs[i].cx = (double)s0.cx + fx;
                objs[i].cy = ncy;
                carryTpY(i, ncy - (double)baseCy[i]);   // --tpmove (removed)
                // ...and a ramp's ends with it, as the recording path does below: the surface is
                // sy0/sy1, and moving cy alone left lv22's slope uid 6949 (group 510) where it started,
                // 3.3 px above GD's at t=6,652, so the ball landed a tick early (quick lv22 t=6,600).
                if (formula[i] == 2 && objs[i].slope) {
                    const double dy = ncy - (double)baseCy[i];
                    objs[i].sy0 = (double)baseSy0[i] + dy;
                    objs[i].sy1 = (double)baseSy1[i] + dy;
                }
                // --dyndbg <uid> follows one object; --dyndbg -3 prints EVERY
                // formula-driven object, which is what you want when the set
                // shares a group and a position (lv19's seven doors all sit at
                // the same cy, so following one says nothing about which of
                // them the player is actually standing on).
                if (objs[i].uid == g_dynDbg || g_dynDbg == -3)
                    std::printf("fdrv: uid=%d t=%d box=%d autoFireT=%d "
                                "base=%.3f lock=%d off=%.3f cx=%.3f "
                                "fy=%+.4f dcy=%+.4f -> cy=%.4f\n",
                                objs[i].uid, t, fb, ft0, (double)s0.cy,
                                locked ? 1 : 0, (double)lockOff, objs[i].cx,
                                fy, fy - py1, ncy);
                continue;
            }
            const int aA = m ? -1 : autoAnchor[i];   // touch control wins
            // --areamove: an armed object no trigger controls keeps what seek() read (size, angle, on) and is moved
            // by the area pass from this group's x.
            if (!m && aA < 0) {
                double ahw = objs[i].hw, ahh = objs[i].hh, arot = std::nan("");
                if (areaPlace(i, objs[i].cx, objs[i].cy, &ahw, &ahh, &arot)) {
                    if (!std::isnan(arot)) {   // a scale or rotate acts on it
                        objs[i].hw = ahw;
                        objs[i].hh = ahh;
                        recEndAxis(i, arot);
                    }
                    continue;
                }
            }
            if (!m && aA < 0 && g_supplyOnHook && !gdev::g_supplyEventUids.empty() && objs[i].uid >= 0 && gdev::g_supplyEventUids.count(objs[i].uid) && i < samples.size() && samples[i].size() == 1) {
                // an object only an Event trigger's chain (a game event spawns a group whose Toggle switches it) acts on: no touch box and no crossing trigger of its own, so it never came this far and stood on
                // for the model whatever the shadow's timeline held. The shadow's on / off for the group being placed, as for an object an autonomous trigger controls (below, T29)
                int so = 0;
                if (g_supplyOnHook(objs[i].uid, so)) {
                    on[i] = (uint8_t)(so ? 1 : 0);
                    objs[i].recOn = (uint8_t)(so ? 1 : 0);
                    objs[i].onNew = 0; objs[i].onPrev = 1;
                    objs[i].pendOff = 0;
                }
                continue;
            }
            if ((!m && aA < 0) || samples[i].empty()) continue;
            const auto& sm = samples[i];
            const DynSample& s0 = sm[0];
            double cx = s0.cx, cy = s0.cy, hw = s0.hw, hh = s0.hh;
            uint8_t onv = s0.on;
            uint8_t onPrevV = 1;   // --onlag
            uint8_t onNewV = 0;
            float rotv = s0.rot;   // GD's own getRotation (see turnedBox)
            // --recendresid: the placement the parts tail left from the last row (tailCx, tailCy), and what its
            // parts took of the recording's own last step (tailLastDx, tailLastDy)
            bool partsTail = false;
            double tailCx = 0.0, tailCy = 0.0, tailLastDx = 0.0, tailLastDy = 0.0;
            bool rigTailed = false;   // --recendtail placed it past the last row (the tails below stand down)
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
            // The re-timing this object ends up with, for --shiftstat. 0 means
            // the re-timing branch was not taken at all, which is the same thing
            // as "reads the recording on its own clock".
            int shiftUsed = 0;
            if (m) {
                fired = (mask & m).any();
                anchor = fireHi; recAnchor = trigRecFire[i];
                fdx = trigDx[i]; fdy = trigDy[i];
                // (always on: the switch --touchfirstlat is removed)
                // --touchfirstlat (on by default since 2026-10-14; --no-touchfirstlat is the off arm): a touch-moved object the recording has not seen move yet (the analytic fallback, lat in
                // Dynamics::place) starts on the tick after the entry, as the formula path's per-box parts do, instead of 5 ticks
                // later. lv20 coin run: the ceiling pad uid5308 (group 1, Move -60 over 27.8 ticks, ease 3) is entered at 5,788;
                // GD's recording has it at 436.627 on 5,790 and the model on 5,794, and the ship dies under it at 5,812.
                fdur = trigDur[i]; lat = 1;
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
                    // The recSelfFire disjunct is the middle one. The first is
                    // dead here (no object reaching this line has autoAnchor >= 0:
                    // autoCtl implies m == 0, and formulaDriven leaves at :708);
                    // it served --noformula, which is gone since the flag
                    // clean-up, and removing it is left to a change of its own.
                    // The recAuto disjunct is untouched.
                    //
                    // Gated on the RECORDING's own first motion: without the tick
                    // test this forces `fired` from t=1 and the fired branch then
                    // interpolates across the leading gap (:931-942) -- measured
                    // on lv22 uid254, where sample[0] (1, 57.000) and sample[1]
                    // (315, 65.940) gave cy=62.552 at t=196.
                    // `trigRecFire[i] >= 0` is guaranteed by the enclosing :754.
                    //
                    // MEASURED AGAINST GD, and that measurement is the scope of
                    // the claim (2026-09-13). The class is nine objects, all in
                    // lv22 (2,094 triggered objects corpus-wide -> 157 controlled
                    // -> 15 whose touch moves nothing -> 9 that also move by
                    // themselves). A fresh recording of the same plan was taken
                    // from the game and held against both arms at the 20-tick
                    // print points around each object's first use: 33 comparison
                    // points, of which this branch puts 33 within 0.02 px of the
                    // game and the branchless form 27. The two differ on 4 of the
                    // 9 objects and 6 of the 33 points, by up to ~148 px, in the
                    // window between the recording's first motion and the late
                    // fire the touch mask alone produces.
                    // WHAT IT DOES NOT SHOW: the three objects the player
                    // actually rides in this corpus (uid 195, 254, 320) come out
                    // the same either way, so the corpus trajectory does not move
                    // -- the 22 replays are byte-identical with and without this.
                    const bool lead = i < touchLeads.size() && touchLeads[i];   // --touchrecleads (removed)
                    if (autoAnchor[i] >= 0
                        || (((i < recSelfFire.size() && recSelfFire[i]) || lead)
                            && t >= trigRecFire[i])
                        || (i < recAuto.size() && recAuto[i]))
                        fired = true;
                    anchor = recAnchor; lat = 0;
                    // --touchretime (see the flag): this state's own entry.
                    if (fireB && autoAnchor[i] < 0
                        && !(i < recAuto.size() && recAuto[i])
                        && !(i < autoReach.size() && autoReach[i])
                        && !lead
                        && !(g_recPhase & m)) {
                        int fb = -1, fbBox = -1;
                        const TouchMask mm = m & mask;
                        if (mm.any()) for (int b = 0; b < kTouchBits; ++b)
                            if (mm.test(b) && (int)fireB[b] > fb) {
                                fb = (int)fireB[b];
                                fbBox = b;
                            }
                        // --touchretimebox: the object's own delay behind the box's
                        // recorded entry, carried over to this state's entry.
                        const int boxEntry =
                            (fbBox >= 0 && (size_t)fbBox < boxRecEntry.size())
                                ? boxRecEntry[(size_t)fbBox] : -1;
                        // The `- 1` is not a fudge, and the whole lag is worse.
                        // TRIED AND REVERTED (2026-09-20): adding the whole
                        // recLag here, on the argument that the autonomous
                        // branch below subtracts the whole lag from recAnchor
                        // and that lv22 uid1214 still reads the recording one
                        // tick ahead with the flag on. It does land uid1214 on
                        // the row of the same tick (9 of its 12 clamp rows
                        // match at shift 0, against 8 at +1 with the -1), and
                        // lv22's tracking against the reference then FALLS from
                        // 19,343 to 18,596 ticks with 6 -> 10 diverged sections
                        // and the t=2,600 section back to 168 of 400. So the
                        // one tick that is left is the trace's own convention
                        // -- the recorder samples after the whole tick, and the
                        // model's clamp for tick t wants the row labelled t+1
                        // -- not a residual to be removed.
                        if (fb >= 0 && (long long)fb >= g_touchRetimeFrom)
                            anchor = (boxEntry >= 0)
                                ? fb + (trigRecFire[i] - boxEntry)
                                : fb + kTouchRetimeLat
                                      + ((i < recLag.size())
                                             ? std::max(0, recLag[i] - 1) : 0);
                    }
                }
            } else {
                const int f = autoFireT(aA, mask, fireB);   // --boxarm
                fired = (f >= 0 && f <= t);
                anchor = f;
                recAnchor = trigRecFire[i]
                    - (i < autoLag.size() ? autoLag[i] : recLag[i]);
                // --anchornoop: no recorded change is the anchor's, so nothing to pair its
                // fire tick with -- the recording on its own clock (shift 0).
                if (f >= 0 && i < anchorUnrec.size() && anchorUnrec[i])
                    recAnchor = f;
                // --recanchorx: the recording's first change was not the anchor's.
                else if (i < recAnchorX.size() && recAnchorX[i] >= 0)
                    recAnchor = recAnchorX[i];
                fdx = autoDx[i]; fdy = autoDy[i];
                fdur = autoDur[i]; lat = 0;
                fease = autoEase[i]; ferate = autoErate[i];
            }
            // --shiftstat, third line: the object's re-timing INPUTS, once, at
            // the tick its controller first counts as fired. This is where the
            // shift is actually decided, and it is the only place that can say
            // WHY -- the touch branch above sets anchor = recAnchor outright, so
            // a touch-controlled object with a recording is 0 by construction and
            // every non-zero shift comes from the autonomous branch, where the
            // model's own computed fireT is held against the recording's first
            // motion (trigRecFire - recLag).
            if (g_shiftStat && fired) {
                if (shiftSaid.size() != objs.size())
                    shiftSaid.assign(objs.size(), 0);
                if (!(shiftSaid[i] & 4)) {
                    shiftSaid[i] |= 4;
                    std::printf("shiftres: uid=%d bucket=%d branch=%s anchor=%d "
                                "recFire=%d recLag=%d shift=%d ease=%d "
                                "rate=%.3f dur=%.3f dx=%.3f dy=%.3f t=%d\n",
                                objs[i].uid, (int)bucket[i],
                                m ? "touch" : "auto", anchor, trigRecFire[i],
                                (i < recLag.size() ? recLag[i] : -1),
                                (anchor >= 0 ? anchor - recAnchor : 0),
                                fease, ferate, fdur, (double)fdx, (double)fdy,
                                t);
                }
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
                    // TouchMask, not uint32_t: both operands are TouchMask and
                    // the loop below runs to kTouchBits, so a narrow local
                    // truncated the high boxes away and then shifted past its
                    // own width for b >= 32. Boxes 32.. always read as "not
                    // touched" -- and at a width of 64 those are exactly the
                    // boxes the wider mask exists to carry.
                    const TouchMask touched = mask & m;
                    for (int b = 0; b < kTouchBits; ++b)
                        if (touched.test(b)
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
                if (trigRecFire[i] >= 0 && !autoClosed[i]) {
                    const int shift = (anchor >= 0) ? (anchor - recAnchor) : 0;
                    shiftUsed = shift;
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
                            if (!mask.test(p.trig)) continue;
                            const int F = g_touchFireT[p.trig & (kTouchBits - 1)];
                            if (F < 0 || t <= F) continue;
                            if (std::fabs((double)g_touchBoxU[p.trig & (kTouchBits - 1)]
                                          - (double)objs[i].cx) >= 400.0)
                                continue;
                            if (g_dynDbg == objs[i].uid)
                                std::printf("holddbg t=%d uid=%d box=%d F=%d recAnchor=%d recFire=%d dy=%.3f add=%d\n",
                                            t, objs[i].uid, p.trig, F, recAnchor, trigRecFire[i], (double)p.dy,
                                            std::min(60, t - F));
                            // --holddedup: a touch within 16 ticks of the recording's own motion anchor is a
                            // touch the recording holds (a custom level, t=5133: the plates' recording starts at 5121 from
                            // the box before, and box 23 fires 11 ticks later in the same pass of the player); the
                            // measured punch hold is for a touch far from it.
                            if (trigRecFire[i] >= 0 && std::abs(F - recAnchor) <= 16)
                                continue;
                            // --holdrecbox: the recorded attempt fired this very box on this very tick (--rectouch), so its rows hold the touch --
                            // when the rows are that attempt's own (formulaRecProvOk: --recprov names one attempt for the recording and the anchor, and the tick is within its reach;
                            // another attempt's rows hold nothing of this run's touch: a custom level's kit, recording of attempt 36 and anchor of 55, platform uid 3427 at t=4,043-4,046)
                            if (g_holdRecBox && g_recTouchGiven && formulaRecProvOk(t)
                                && slamBoxRecorded(g_recTouchBit, p.trig & (kTouchBits - 1), F)) {
                                g_statHoldRecBoxHits.fetch_add(1, std::memory_order_relaxed);
                                continue;
                            }
                            delay += std::min(60, t - F);
                        }
                        tt -= delay;
                    }
                    // --shiftdbg <uid>: print this uid's re-timing once. An
                    // instrument for the suspicion that the descent phase of
                    // the t=2776 spike (uid18118) is off from live by tens of
                    // ticks.
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
                    cx = s.cx; cy = s.cy; hw = s.hw; hh = s.hh; onv = recOnAt(sm, lo, tt);   // --offafter
                    rotv = s.rot;
                    // --moverextend (see seek): past the last row of a constant-velocity mover the
                    // retimed tick goes on at that velocity (this placement overwrites seek()'s).
                    if (i < extOk.size() && extOk[i] && lo + 1 == sm.size() && tt > s.t) {
                        const float k = (float)std::min(tt - s.t, extCap[i]);
                        cx = s.cx + extVx[i] * k;
                        cy = s.cy + extVy[i] * k;
                    }
                    // [2026-08-22 r104] **Interpolate linearly between rows.**
                    // grouptrace does not write changes under 0.05px, so an
                    // object moving at 0.045px/tick is recorded as "jumps
                    // 0.09px once every 2 ticks". GD moves smoothly, so holding
                    // (the staircase) is a systematic error of up to 0.09px and
                    // **cannot reproduce the tick at which contact breaks, not
                    // even to within 1 tick** (the lv22@6,129 family).
                    // Position and rot only. on (a boolean) and hw/hh stay
                    // stepped.
                    // --touchretime: not across the LEADING gap. Before the
                    // recording's first motion the object is at rest, and the
                    // row after the gap is already the first moved position --
                    // interpolating toward it from t=1 put lv22's block row at
                    // 344.70 one tick before GD moves it (t=6510, shift 5), which
                    // is exactly the landing tick.
                    const bool leadingGap = m
                                            && trigRecFire[i] >= 0
                                            && tt < trigRecFire[i];
                    // Only a step a uniform mover can make, per channel. Until
                    // the newer row's tick every channel stayed within kRecEps
                    // of the older row (else a row would have been written),
                    // so an object moving under kRecEps a tick arrives less
                    // than 2*kRecEps away. A larger step across a gap is an
                    // object that sat still and then jumped or set off fast,
                    // and holding the older row stays within kRecEps of it
                    // the whole time. 4002 platform uid 18238: rows at 20,066
                    // (y 2,068.5) and 20,711 (1,468.5) are a 600 px instant
                    // Move, which this spread over 645 ticks -- the model's
                    // platform hung 81 px above GD's and crushed the player.
                    // --recstall: ...nor across a fast mover's stop (recStallGap)
                    if (lo + 1 < sm.size() && !leadingGap && !recStallGap(sm, lo)) {
                        const DynSample& n = sm[lo + 1];
                        const int span = n.t - s.t;
                        if (span > 0 && tt > s.t) {
                            const double u = (double)(tt - s.t) / (double)span;
                            const auto lerp = [u](float a, float b) {
                                const double d = (double)b - (double)a;
                                return std::fabs(d) < 2.0 * kRecEps
                                           ? (float)((double)a + d * u) : a;
                            };
                            cx = lerp(s.cx, n.cx);
                            cy = lerp(s.cy, n.cy);
                            rotv = lerp(s.rot, n.rot);
                        }
                    }
                    if (lo + 1 < sm.size() && !leadingGap && tt > s.t && recSizeGapOk(sm, lo)) {   // --recinterpsize
                        const DynSample& n = sm[lo + 1];
                        const double u = (double)(tt - s.t) / (double)(n.t - s.t);
                        hw = recSizeLerp(s.hw, n.hw, u);
                        hh = recSizeLerp(s.hh, n.hh, u);
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
                    // (Unconditional since the 0.4.0 clean-up; it was --slideparts, on in every
                    // loop call that read a recording since d008ee2.) An object with two or more
                    // autonomous moves goes on from its last row by each move's own eased increment,
                    // every move from its own crossing -- the form stage 1' objects already
                    // take (below). The completion after this reads the moves as ONE, the
                    // summed offset over one duration from the recording's first motion,
                    // which is right for a single slide and not for a sequence.
                    bool partsDone = false;
                    // --ctlautotail: the same tail for a touch-controlled object whose chain places
                    // nothing (offset 0) and whose boxes this state has not entered, from the Moves
                    // the loader keeps for every object (partsAny; autoParts is empty when a touch
                    // controls it). A custom level's spike uid 139269: a zero-offset touch Rotate on
                    // its group, out of the player's reach, and four autonomous Moves -- (+300,0) and
                    // (+81,0) overlapping (2.12 px a tick), then two eased ones. Its rows end at the
                    // attempt's death, it held there, and GD killed on the next tick at 12,567.10
                    // where the held row reads 12,564.98. It starts from the last row, so a
                    // --moverextend tail placed above is replaced, not added to.
                    const bool ctlTail = m && !(mask & m).any() && fdx == 0.f
                                         && fdy == 0.f && autoParts[i].empty() && i < partsAny.size()
                                         && !partsAny[i].empty();
                    if (lo + 1 == sm.size() && tt > s.t && !recAutoObj
                        && (autoParts[i].size() >= 2 || ctlTail)
                        && !(i < rotSplit.size() && rotSplit[i])) {
                        const int tEnd = s.t + (t - tt);   // the last row on this state's clock
                        if (ctlTail) { cx = s.cx; cy = s.cy; }
                        const bool fromLast = (double)s.cx == cx && (double)s.cy == cy;   // --recendresid
                        for (const AutoPart& p : (ctlTail ? partsAny[i] : autoParts[i])) {
                            const int f = autoFireT(p.trig, mask, fireB);   // --boxarm
                            if (f < 0) continue;
                            auto eAt = [&](int tk) {
                                if (tk < f) return 0.0;
                                return p.dur > 0.0
                                    ? gdEase(p.ease, p.erate, (double)(tk - f) / p.dur)
                                    : instantE(tk, f);   // --instantlag
                            };
                            const double de = eAt(t) - eAt(tEnd);
                            cx += p.dx * de;
                            cy += p.dy * de;
                            {
                                const double dl = eAt(tEnd) - eAt(tEnd - 1);
                                tailLastDx += p.dx * dl;
                                tailLastDy += p.dy * dl;
                            }
                        }
                        partsDone = true;
                        if (fromLast) { partsTail = true; tailCx = cx; tailCy = cy; }
                    }
                    if (!partsDone && lo + 1 == sm.size() && tt > s.t && bdur > 0) {
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
                    // --recendtail: past the last row an armed object is its simulated rig (the offset to the last
                    // row is in the table), whatever the tails above made of it, and its lock follows this
                    // group's x rather than the track's.
                    if (i < rigTail.size() && !rigTail[i].x.empty()
                        && lo + 1 == sm.size() && tt > s.t && rigTail[i].t0 == s.t) {
                        const RigTail& R = rigTail[i];
                        const size_t k = std::min((size_t)(tt - R.t0), R.x.size() - 1);
                        cx = R.x[k]; cy = R.y[k]; rotv = R.r[k];
                        if (tt >= R.lockA && tt <= R.lockB) cx += px - xRefAt(tt);
                        rigTailed = true;
                        partsTail = false;
                        recEndAxis(i, (double)rotv);
                        if (objs[i].obbOk) {
                            hw = objs[i].bhw * std::fabs(objs[i].bc) + objs[i].bhh * std::fabs(objs[i].bs);
                            hh = objs[i].bhw * std::fabs(objs[i].bs) + objs[i].bhh * std::fabs(objs[i].bc);
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
                        // --slamdedup: the box the recording's own motion anchors on is the object's first touch (the
                        // earliest fire among the boxes it was touched by); a later, independent touch near it is
                        // not in the recording and keeps its step.
                        int fireMin = 1 << 30;
                        int nHit = 0;   // boxes the state touched that move this object (two on the same tick are both fireMin)
                        for (const AutoPart& q : touchParts[i]) {
                            if (!mask.test(q.trig)) continue;
                            const int Fq = g_touchFireT[q.trig & (kTouchBits - 1)];
                            if (Fq >= 0) { fireMin = std::min(fireMin, Fq); ++nHit; }
                        }
                        for (const AutoPart& p : touchParts[i]) {
                            if (p.dy >= 0.f) continue;
                            if (!mask.test(p.trig)) continue;
                            const int F = g_touchFireT[p.trig & (kTouchBits - 1)];
                            if (F < 0 || t < F + 5) continue;
                            if (std::fabs((double)g_touchBoxU[p.trig & (kTouchBits - 1)]
                                          - (double)objs[i].cx) >= 400.0)
                                continue;
                            // --slamdedup: a touch the recording already holds (its motion anchor within
                            // 8 ticks of the state's fire tick) is in the recording's own rows; the step
                            // would add the same move a second time (a custom level, t=5124, plate -9 / 24 ticks).
                            // Only with ONE touching box (nHit == 1): the recording holds one touch and the state has no
                            // other candidate to mistake it for.
                            // --slambox: by the box itself, from the recorded attempt's own record of what
                            // GD fired (slamBoxRecorded), in place of the tick-only rule above. A call the
                            // mod gives no --rectouch (the first, unanchored one) keeps the rule above.
                            const int pb = p.trig & (kTouchBits - 1);
                            const bool dup = (g_recTouchGiven)
                                ? slamBoxRecorded(g_recTouchBit, pb, F)
                                : (trigRecFire[i] >= 0 && nHit == 1 && F == fireMin && std::abs(F - recAnchor) <= 8);
                            if (g_dynDbg == objs[i].uid)
                                std::printf("slamdbg t=%d uid=%d box=%d F=%d recAnchor=%d recFire=%d rec=%d dy=%.3f dedup=%d\n",
                                            t, objs[i].uid, p.trig, F, recAnchor, trigRecFire[i],
                                            g_recTouchBit[(size_t)pb], (double)p.dy, dup ? 1 : 0);
                            if (dup) continue;
                            cx += p.dx;
                            cy += p.dy;
                        }
                    }
                    // The other half of the rotation-split decomposition. The
                    // samples carry `entry_C + R(theta)*rel` -- the orbit about
                    // a STATIONARY centre -- and the loader subtracted the
                    // centre's own translation out to get there ("recorded =
                    // orbit + the centre's own translation", at the fit check).
                    // Nothing added it back: the one path that would is behind
                    // g_rotCompute, which is false and which no flag sets, so
                    // every split object has been running without its
                    // translation since 7c6daa1. The split refuses any object
                    // that does not share its move controllers with its centre,
                    // so this object's own autoParts IS `C(t) - entry_C`, each
                    // controller on its own crossing.
                    if (i < rotSplit.size() && rotSplit[i]) {
                        for (const AutoPart& p : autoParts[i]) {
                            const int f = autoFireT(p.trig, mask, fireB);   // --boxarm
                            if (f < 0 || t < f) continue;
                            // --autostop: a halted Move holds where its Stop left it
                            const int te = g_autoStop ? std::min(t, autoStopT(p.trig)) : t;
                            if (te < f) continue;
                            const double e = (p.dur > 0.0)
                                ? gdEase(p.ease, p.erate, (double)(te - f) / p.dur)
                                : instantE(te, f);   // --instantlag
                            cx += p.dx * e;
                            cy += p.dy * e;
                        }
                    }
                    // --lockx (removed): an object whose only x mover is a lockToPlayerX Move takes its x
                    // from the lock, as the closed form below does, instead of from the
                    // recording. The recording is re-timed by the object's anchor, and when the
                    // anchor and recAnchor come from different controls (a custom level's uid 20017:
                    // anchor from the y Moves, recAnchor from another group's crossing, shift
                    // -80) the recorded x is the player's x 80 ticks ahead, then frozen at the
                    // recording's last row. y still comes from the recording.
                    // (always on: the switch --lockx is removed)
                    // --lockx (removed): see the recording arm in applyTriggers; the lock replaces a locked object's recorded x.
                    if (!rigTailed && aA >= 0 && autoLockTrig[i] >= 0 && autoLockXOnly(i)
                        && autoFireT(autoLockTrig[i], mask, fireB) >= 0) {   // --boxarm
                        AutoTrig& A = g_autoTrig[(size_t)autoLockTrig[i]];
                        const int lockT0 = A.fireT - A.delay;
                        int lockEnd = lockT0 + (int)autoLock[i];
                        if (g_autoStop) lockEnd = std::min(lockEnd, autoStopT(autoLockTrig[i]));
                        lockEnd = std::min(lockEnd, touchStopTick(mask, fireB, i < lockMover.size() ? lockMover[i] : A.uid));
                        cx = s0.cx;
                        if (t >= lockT0 && t <= lockEnd) {
                            A.lockLastX = px;
                            cx += (px - A.fireX) * lockMod(A);
                        } else if (t > lockEnd) {
                            cx += (A.lockLastX - A.fireX) * lockMod(A);
                        }
                        addExtraLocks(i, t, px, mask, fireB, cx);   // --multilock
                        if (g_dynDbg == objs[i].uid && (t % 40 == 0))
                            std::printf("lockxdbg t=%d uid=%d lockT0=%d lockEnd=%d fireX=%.3f "
                                        "px=%.3f cx=%.3f\n", t, objs[i].uid, lockT0, lockEnd,
                                        A.fireX, px, cx);
                    }
                    if (g_dynDbg == objs[i].uid && (t % 40 == 0))
                        std::printf("armdbg t=%d uid=%d arm=recording split=%d "
                                    "parts=%zu cx=%.3f cy=%.3f\n",
                                    t, objs[i].uid,
                                    (int)(i < rotSplit.size() ? rotSplit[i] : 0),
                                    autoParts[i].size(), cx, cy);
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
                        && autoFireT(autoLockTrig[i], mask, fireB) >= 0) {   // --boxarm
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
                        int lockEnd = lockT0 + (int)autoLock[i];
                        // --autostop: a Stop naming the lock's Move closes the window on
                        // its crossing tick (the lock still copies x on that tick).
                        if (g_autoStop) lockEnd = std::min(lockEnd, autoStopT(autoLockTrig[i]));
                        // --lockstop: ...and so does one in a touch chain this world fired.
                        lockEnd = std::min(lockEnd, touchStopTick(mask, fireB, i < lockMover.size() ? lockMover[i] : A.uid));
                        if (t >= lockT0 && t <= lockEnd) {
                            A.lockLastX = px;
                            cx += (px - A.fireX) * lockMod(A);
                        } else if (t > lockEnd) {
                            cx += (A.lockLastX - A.fireX) * lockMod(A);
                        }
                        addExtraLocks(i, t, px, mask, fireB, cx);   // --multilock
                    }
                    if (trigRecFire[i] >= 0 && sm.size() > 1) {
                        const int shift = (anchor >= 0) ? (anchor - recAnchor) : 0;
                        const int tt = t - shift;
                        size_t lo = 0, hi = sm.size();
                        while (lo + 1 < hi) {
                            const size_t mid = (lo + hi) / 2;
                            if (sm[mid].t <= tt) lo = mid; else hi = mid;
                        }
                        hw = sm[lo].hw; hh = sm[lo].hh; onv = recOnAt(sm, lo, tt);   // --offafter
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
                            if (!mask.test(p.trig)) continue;
                            const int F = g_touchFireT[p.trig & (kTouchBits - 1)];
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
                        if (g_dynDbg == objs[i].uid && (t % 40 == 0))
                            for (const AutoPart& p : autoParts[i])
                                std::printf("armdbg   part t=%d uid=%d trig=%d "
                                            "fireT=%d d=(%.1f,%.1f) dur=%.2f\n",
                                            t, objs[i].uid, p.trig,
                                            g_autoTrig[(size_t)p.trig].fireT,
                                            (double)p.dx, (double)p.dy, p.dur);
                        for (const AutoPart& p : autoParts[i]) {
                            const int f = autoFireT(p.trig, mask, fireB);   // --boxarm
                            if (f < 0 || t < f) continue;
                            // --autostop: a halted Move holds where its Stop left it
                            const int te = g_autoStop ? std::min(t, autoStopT(p.trig)) : t;
                            if (te < f) continue;
                            const double e = (p.dur > 0.0)
                                ? gdEase(p.ease, p.erate, (double)(te - f) / p.dur)
                                : instantE(te, f);   // --instantlag
                            cx += p.dx * e;
                            cy += p.dy * e;
                        }
                    }
                    // DIAGNOSTIC (not for landing): which branch served this
                    // object, and what its parts were clocked by.
                    if (g_dynDbg == objs[i].uid && (t % 40 == 0))
                        std::printf("armdbg t=%d uid=%d branch=%s m=%u "
                                    "parts=%zu recAutoObj=%d anchor=%d "
                                    "cy=%.3f\n", t, objs[i].uid,
                                    recAutoObj ? "recAuto"
                                        : (m || autoParts[i].empty())
                                            ? "collapsed" : "superpos",
                                    (unsigned)m.word(0), autoParts[i].size(),
                                    (int)recAutoObj, anchor, cy);
                    if (g_dynDbg == objs[i].uid && t == 11720)
                        for (const AutoPart& p : autoParts[i])
                            std::printf("armdbg   part trig=%d uid=%d cx=%.1f "
                                        "fireT=%d d=(%.2f,%.2f) dur=%.1f\n",
                                        p.trig, g_autoTrig[(size_t)p.trig].uid,
                                        g_autoTrig[(size_t)p.trig].cx,
                                        g_autoTrig[(size_t)p.trig].fireT,
                                        p.dx, p.dy, p.dur);
                }
            }
            // --shiftdbg <uid> also samples the object's EVALUATED position
            // every 20 ticks -- the belief the kill tests actually see, which
            // no amount of reasoning about the composition rules substitutes
            // for (the switch-band phantom survived three composition fixes
            // that were all aimed at the wrong mechanism).
            // ---- the ROTATION, after whatever produced the translation ------
            // A Rotate carries its group around a centre object, which no
            // offset describes: 806 of lv21's turned objects and 264 of lv22's
            // have no recording at all, so until this ran the model held them
            // PERFECTLY STILL while GD swung them. Applied here, after the
            // branches above, so it composes with any of them rather than
            // being duplicated into each.
            //
            // The centre is evaluated from its own entry plus its own
            // controllers rather than read out of objs[centreIdx]: this loop
            // has no order guarantee, so the centre may not have been placed
            // yet on this tick.
            //
            // Closed form, not the per-tick recurrence. The recurrence with a
            // float store is 0.0060 px against GD and this is 0.0142 px (see
            // RotSpec), but the recurrence needs a sequential carry and this
            // path is idempotent and may be entered with a jumped tick. 0.0142
            // against the 1.65-2.18 px a re-timed recording leaves is the
            // trade; the remaining eight thousandths wait for a table.
            if (g_rotCompute && !g_rotSpec.empty() && !rigTailed) {
                const auto rs = g_rotSpec.find(objs[i].uid);
                if (rs != g_rotSpec.end() && rs->second.anchor >= 0
                    && rs->second.centreIdx >= 0) {
                    const RotSpec& R = rs->second;
                    const int f = g_autoTrig[(size_t)R.anchor].fireT;
                    if (f >= 0 && t >= f) {
                        const size_t ci = (size_t)R.centreIdx;
                        double ccx, ccy;
                        if (samples[ci].size() > 1) {
                            // The centre has a recording of its own, and seek()
                            // has already placed it for this tick. Use it.
                            //
                            // Deriving it analytically instead was WRONG and
                            // deathref caught it: a centre whose motion is
                            // recorded rather than trigger-controlled has no
                            // autoParts, so the analytic form pinned it at its
                            // entry and the whole orbit hung off the wrong
                            // point. lv21_t13304 went PASS -> FAIL (died -436)
                            // -- an over-kill, a rotated hazard swung through
                            // the player because its centre never moved.
                            ccx = objs[ci].cx;
                            ccy = objs[ci].cy;
                        } else {
                            ccx = samples[ci][0].cx;
                            ccy = samples[ci][0].cy;
                            for (const AutoPart& p : autoParts[ci]) {
                                const int cf = autoFireT(p.trig, mask, fireB);   // --boxarm
                                if (cf < 0 || t < cf) continue;
                                const double e = (p.dur > 0.0)
                                    ? gdEase(p.ease, p.erate,
                                             (double)(t - cf) / p.dur)
                                    : instantE(t, cf);   // --instantlag
                                ccx += p.dx * e;
                                ccy += p.dy * e;
                            }
                        }
                        // sign: GD's positive turn is clockwise on screen, the
                        // opposite of a y-up matrix. Measured -1 on both
                        // subjects (lv21 uid15367, lv22 uid254).
                        const double th = -R.total
                            * gdEase(R.ease, R.erate, (double)(t - f) / R.durT)
                            * 3.14159265358979 / 180.0;
                        const double c = std::cos(th), s = std::sin(th);
                        const double rx = cx - ccx, ry = cy - ccy;
                        cx = ccx + c * rx - s * ry;
                        cy = ccy + s * rx + c * ry;
                    }
                }
            }
            if (g_shiftDbgUid >= 0 && objs[i].uid == g_shiftDbgUid
                && t % 20 == 0)
                std::printf("dynpos: uid=%d t=%d cx=%.2f cy=%.2f on=%d\n",
                            objs[i].uid, t, cx, cy, (int)onv);
            // --rottail: a placement that still holds the last row goes on turning with its Rotates from the
            // recording's last tick (rotTailMaxT + shiftUsed on the model's clock). The recorder writes a row
            // whenever anything changes, so an object whose last row is older stood as that row until then --
            // one that had not moved at all when the attempt died included: its turn starts past the end.
            bool rotTailed = rigTailed;   // --recendtail: already turned on, nothing below adds to it
            // --rottailmove: the tail turned the angle of an object a Move carries; the steady-step extension
            // below still extends its position, not its angle
            bool rotAngleOnly = false;
            if (!rigTailed && i < rotTail.size() && !rotTail[i].empty()) {
                const DynSample& L = sm.back();
                const bool moveCarried = i < rotTailMove.size() && rotTailMove[i];
                if ((moveCarried || ((double)L.cx == cx && (double)L.cy == cy)) && t - shiftUsed > rotTailMaxT
                    && rotTailFrom(i, rotTailMaxT + shiftUsed, t, cx, cy, rotv)) {
                    rotTailed = !moveCarried;
                    rotAngleOnly = moveCarried;
                    if (rotBoxW[i] > 0.f) {
                        const double th = (double)rotv * (3.14159265358979 / 180.0);
                        const double c = std::fabs(std::cos(th)), s = std::fabs(std::sin(th));
                        hw = 0.5 * ((double)rotBoxW[i] * c + (double)rotBoxH[i] * s);
                        hh = 0.5 * ((double)rotBoxW[i] * s + (double)rotBoxH[i] * c);
                    }
                    recEndAxis(i, (double)rotv);
                }
            }
            // --recendextend: a placement that still holds the last row goes on by the last steady step, on the
            // recording's own clock (t - shiftUsed). One that another tail moved keeps that tail's answer.
            if (!rotTailed) {
                const DynSample& L = sm.back();
                // --recendresid: ...or the parts tail alone moved it, and then the step minus the parts' share --
                // only on the ticks the extension covers, not held past them: the tail is the long-range answer
                // and the residual a straight line (a lock to the player, a turn) that a held offset would bend
                // for the rest of the level. A search reads a recording that ended at the last attempt's death
                // thousands of ticks earlier.
                const bool resid = partsTail && cx == tailCx && cy == tailCy
                                   && (t - shiftUsed) - L.t <= kRecEndExtendTicks;
                if (((double)L.cx == cx && (double)L.cy == cy) || resid)
                    if (const int k = recEndTicks(i, t - shiftUsed)) {
                        const double rdx = resid ? tailLastDx : 0.0, rdy = resid ? tailLastDy : 0.0;
                        cx += ((double)recVx[i] - rdx) * k; cy += ((double)recVy[i] - rdy) * k;
                        if (!rotAngleOnly) {   // --rottailmove: the tail holds the angle and the box
                            hw += (double)recVhw[i] * k; hh += (double)recVhh[i] * k;
                            rotv += recVr[i] * (float)k;
                            recEndAxis(i, (double)rotv);
                        }
                    }
            }
            // --areamove: an armed object's x and y are its base plus this state's area displacement, whatever the
            // recording or a tail made of them (the recording holds the recorded run's player's area pass).
            {
                double ax = 0.0, ay = 0.0, ahw = hw, ahh = hh, arot = std::nan("");
                if (areaPlace(i, ax, ay, &ahw, &ahh, &arot)) {
                    cx = ax;
                    cy = ay;
                    if (!std::isnan(arot)) {
                        hw = ahw;
                        hh = ahh;
                        rotv = (float)arot;
                        recEndAxis(i, arot);
                    }
                } else if (!m && i < areaAdd.size() && areaAdd[i] && !(trigRecFire[i] >= 0 && !autoClosed[i])) {
                    // --areaadd: the closed form's place plus the actions' displacement (a recording holds its own)
                    double adx = 0.0, ady = 0.0;
                    if (areaAddDisp(i, cx, adx, ady)) { cx += adx; cy += ady; }
                }
            }
            boxBase(i);   // --obbscale
            if (lastStep1) objs[i].dcx += cx - (double)objs[i].cx;   // --dcxfinal (constants.hpp): dcx from the final position, as dcy below
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
            // --boxoffown: ...unless an off box takes this object out and none has fired here yet.
            bool offBoxHeld = false;
            if (i < offBox.size() && !offBox[i].empty()) {
                offBoxHeld = true;
                if (fireB)
                    for (const uint8_t b : offBox[i])
                        if ((mask & touchBit((int)b)) && (int)fireB[b] <= t) {
                            offBoxHeld = false;
                            break;
                        }
            }
            if (!offBoxHeld && cur[i] < sm.size() && sm[cur[i]].t <= t
                && recOnAt(sm, cur[i], t - shiftUsed) == 0)   // --offafter: the row's own clock
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
            // --recendtail: past the recording's end, the last row's on/off and every switch that lands after
            // it. The recording's end, not the object's last row: the recorder writes a row whenever anything
            // changes, so an object whose last row is older stood as that row until the end, switched or not.
            if (i < tailOnEv.size() && !tailOnEv[i].empty()) {
                const int after = std::max(sm.back().t, tailRecEnd) + shiftUsed;
                if (t > after)
                    onv = resolveOnAfter(sm.back().on != 0, after, tailOnEv[i].data(), tailOnEv[i].size(),
                                         mask, fireB, t) ? 1 : 0;
            }
            // --touchonrec (removed): where this state and the recording's own attempt entered one of the boxes that switch
            // this object on different ticks (or only one of them entered it), the recording's on/off holds only
            // up to the first such tick; from there the switches land as this state takes them.
            if (g_recTouchEnteredGiven && fireB && i < tailOnEv.size() && i < tailOnBox.size()
                && tailOnBox[i] && !tailOnEv[i].empty()) {
                int split = std::numeric_limits<int>::max();
                for (const OnEvent& e : tailOnEv[i]) {
                    if (e.box < 0 || e.box >= kTouchBits) continue;
                    const int rec = g_recOwnEntry[(size_t)e.box];
                    int st = (mask & touchBit(e.box)) ? (int)fireB[e.box] : -1;
                    if (g_touchRecEntry[(size_t)e.box] > 0) st = g_touchRecEntry[(size_t)e.box];   // --touchonrecentry
                    if (st == rec) continue;
                    int a = std::numeric_limits<int>::max();
                    if (st >= 0) a = std::min(a, st);
                    if (rec >= 0) a = std::min(a, rec);
                    split = std::min(split, a);
                }
                if (split != std::numeric_limits<int>::max() && t > split) {
                    // from the tick before the split: a switch landing ON it is one of the two histories'
                    const int from = split - 1;
                    size_t lo = 0, hi = sm.size();
                    while (lo + 1 < hi) {
                        const size_t md = (lo + hi) / 2;
                        if (sm[md].t <= from) lo = md; else hi = md;
                    }
                    const bool at = recOnAt(sm, lo, from) != 0;
                    onv = resolveOnAfter(at, from, tailOnEv[i].data(), tailOnEv[i].size(), mask, fireB, t) ? 1 : 0;
                    if (g_dynDbg == objs[i].uid)
                        std::printf("touchonrec t=%d uid=%d split=%d on@split=%d -> %d\n", t, objs[i].uid, split,
                                    at ? 1 : 0, (int)onv);
                }
            }
            // --activators: this object's on/off is its switches', not the recording's.
            if (i < onEv.size() && !onEv[i].empty())
                onv = resolveOn(onInit[i] != 0, onEv[i].data(), onEv[i].size(), mask, fireB, t)
                          ? 1 : 0;
            // --tapdisarm: ...and, once a switch of this object's x-crossing triggers would have landed in a state
            // where a box had switched that trigger off, its switches' on/off without it (resolveOn skips it).
            if (i < disarmEv.size() && !disarmEv[i].empty() && fireB) {
                bool any = false;
                for (const OnEvent& e : disarmEv[i])
                    if (e.autoIdx >= 0 && (size_t)e.autoIdx < g_autoTrig.size()
                        && g_autoTrig[(size_t)e.autoIdx].fireT >= 0
                        && (double)g_autoTrig[(size_t)e.autoIdx].fireT + e.delay <= (double)t
                        && (autoDisarmed(e.autoIdx, mask, fireB)
                            // a Toggle that a ring arms (triggers.hpp, the toggle-ring chain) switches what it switches in the states that pressed the ring too -
                            // the recording the rows come from may be one where nobody pressed it
                            || (g_autoTrig[(size_t)e.autoIdx].offInit && g_autoTrig[(size_t)e.autoIdx].toggleRoot))) {
                        any = true;
                        break;
                    }
                // By group (resolveOnGroups), from every group on -- or, for an object the reset left off for a
                // reason its switches do not hold, the one-list form from that.
                if (any)
                    onv = (disarmInit[i] != 0
                               ? resolveOnGroups(disarmEv[i].data(), disarmEv[i].size(), mask, fireB, t)
                               : resolveOn(false, disarmEv[i].data(), disarmEv[i].size(), mask, fireB, t))
                              ? 1 : 0;
                if (any && g_dynDbg == objs[i].uid) {   // --dyndbg: which switches it took, which were stopped
                    std::printf("tapdisarm t=%d uid=%d init=%d ->%d:", t, objs[i].uid, (int)disarmInit[i], (int)onv);
                    for (const OnEvent& e : disarmEv[i]) {
                        if (e.autoIdx >= 0 && (size_t)e.autoIdx < g_autoTrig.size()) {
                            const AutoTrig& A = g_autoTrig[(size_t)e.autoIdx];
                            if (A.fireT < 0 || A.fireT > t) continue;
                            std::printf(" u%d@%d:%d%s", A.uid, A.fireT, (int)e.on,
                                        autoDisarmed(e.autoIdx, mask, fireB) ? "X" : "");
                            for (const AutoDisarm& d : g_autoDisarm[(size_t)e.autoIdx])
                                if (d.box >= 0 && d.box < kTouchBits && (mask & touchBit(d.box)))
                                    std::printf("(b%d@%d:%d)", d.box, (int)fireB[d.box], (int)d.on);
                        } else if (e.box >= 0 && (mask & touchBit(e.box)))
                            std::printf(" box%d@%d:%d", e.box, (int)fireB[e.box], (int)e.on);
                    }
                    std::printf("\n");
                }
            }
            // --boxoff (removed): out once a box that switches it off has fired in this group's world.
            if (i < offBox.size() && fireB)
                for (const uint8_t b : offBox[i])
                    if ((mask & touchBit((int)b)) && (int)fireB[b] <= t) { onv = 0; break; }
            // --onlag: a hazard (bucket NEAR, not a solid) that the recording switches ON takes effect the tick after the
            // row, as the portal / speed read above does, and on the tick after that the sweep leaves out the previous
            // tick's end position (Obj::onPrev). Whatever produced `onv` above, the recording's row dates the edge.
            if (onv && bucket[i] == NEAR && objs[i].type != 0
                && cur[i] > 0 && cur[i] < sm.size() && sm[cur[i]].on && !sm[cur[i] - 1].on) {
                if (sm[cur[i]].t == t) onNewV = 1;
                else if (sm[cur[i]].t == t - 1) onPrevV = 0;
            }
            // --padonlate (removed), for a portal / speed portal whatever put its ON here (the unmoved-object rule above only reaches an object no Move and no Touch box acts on, and the activators /
            // the recording's rows after it overwrite what it decided): the row that switches it ON lands on the tick after it. The recorder writes the row after the tick's trigger pass,
            // which comes after the collisions, so a portal a Toggle or a Touch box switched on at row t is still off for tick t's pass. [M] a custom level t0=44601: teleport portal
            // uid 61624 (id 747, group 1, rot 90, a Touch box's Toggle, first row on at t=44,744): GD's cube overlaps it by 1.1 px from 44,744 and teleports in the pass of 44,745, the
            // model at 44,744 (dy 1505.6 there); a custom level t0=7001 uid 13170: the model at 7,126, GD at 7,127.
            // A PAD switched on the same way (a Toggle's group, the pad under an autonomous control) is the same: the recorded-row rule for pads below (bucket PAD) never reaches an object a trigger
            // controls, so it was live in its own row's pass, one step early. [M] a custom level t0=9801: pad uid 15328 (group 51, the Toggle 19420 at x 14985 turns it on), first ON row t=10054:
            // GD's cube is launched in the pass of 10055, the model's at 10054.
            // (always on: the switch --padonlate is removed)
            // --padonlate (removed): a recorded ON lands the tick after its row for a PAD too (seek; the PORT / SPEED rule in place() does it for those two). The recorder writes
            // the row after the tick's trigger pass, which comes after the collisions, so a pad a Toggle switched on at row t is still off for tick t's pass.
            // A custom level (a custom level) t=5,434: 12 pads (id 35, turned 45 deg, group 1) are off in the first row and on in the row of 5,434; GD's cube
            // meets them in the pass of 5,435 (padseed label 5,434; BOOST and vy 16 on the row of 5,435), the model's on tick 5,434, one tick early, with the
            // gravity ring's flip still to come.
            if (onv && (bucket[i] == PORT || bucket[i] == SPEED || bucket[i] == PAD)
                && cur[i] > 0 && cur[i] < sm.size() && sm[cur[i]].on && !sm[cur[i] - 1].on && sm[cur[i]].t == t)
                onv = 0;
            // A SOLID the recording switches on at row t is the same: the row is written after the tick's trigger pass, the solids are met before it, so it is first met in the pass of t + 1.
            // [M] a custom level t0=9401: block uid 99889 (30 x 30, group 2708) has its first on row at 9,550; GD's robot is first seated on it (y 3465.9236, the block's top 3450.9236 + 15) at
            // 9,551, the model at 9,550. Only the one witness (the other solids that start off and are switched on by rows were not looked at).
            if (onv && objs[i].type == 0 && cur[i] > 0 && cur[i] < sm.size() && sm[cur[i]].on && !sm[cur[i] - 1].on && sm[cur[i]].t == t)
                onv = 0;
            // --zerosaw: a hazard whose rect is empty at this tick (scale 0) hurts nothing
            if (onv && bucket[i] == NEAR && objs[i].type != 0 && objs[i].hw <= 0.f && objs[i].hh <= 0.f)
                onv = 0;
            // --trigsupply-all (T29): no recording behind it -> the shadow's on/off for this tick
            if (g_supplyOnHook && sm.size() <= 1 && objs[i].uid >= 0) {
                int so = 0;
                if (g_supplyOnHook(objs[i].uid, so)) { onv = (uint8_t)(so ? 1 : 0); onNewV = 0; onPrevV = 1; }
            }
            objs[i].onPrev = onPrevV;   // --onlag
            objs[i].onNew = onNewV;
            on[i] = onv;
            objs[i].recOn = onv;   // --holdtap (object.hpp)
            if (objs[i].pendBox >= 0)   // --pressfresh (Obj::pendOff)
                objs[i].pendOff = !onv && !(mask & touchBit(objs[i].pendBox));
            if (g_geoDumpF && t >= g_geoT0 && t <= g_geoT1 && (t - g_geoT0) % g_geoStride == 0)
                std::fprintf(g_geoDumpF, "%d,%d,%.3f,%.3f,%.2f,%.2f,%d\n", t, objs[i].uid, cx, cy, hw, hh, (int)onv);
            if (g_dynDbg == objs[i].uid)
                std::printf("dyntrig t=%d uid=%d fired=%d m=%08x cx=%.3f "
                            "cy=%.3f hw=%.1f hh=%.1f on=%d\n",
                            t, objs[i].uid, (int)fired, (unsigned)m.word(0), cx, cy,
                            hw, hh, (int)onv);
            // --shiftstat: `shiftstat:` once per object the first time it is
            // placed (this is the DENOMINATOR -- the objects a collision
            // consumes; decoration never reaches Dynamics at all), and
            // `shiftnz:` the first time its re-timing is non-zero.
            //
            // TWO LINES ON PURPOSE. The first placement happens long before the
            // controlling trigger fires, and until it does `anchor` is -1 and
            // the shift reads 0 -- sampling only there says "every object is at
            // shift 0", which is how the first version of this counter reported
            // 2,094 of 2,094 while --shiftdbg had already shown uid 434 at
            // shift=1. An object that never prints a `shiftnz:` line is the
            // real shift-0 case.
            if (g_shiftStat) {
                if (shiftSaid.size() != objs.size())
                    shiftSaid.assign(objs.size(), 0);
                if (!(shiftSaid[i] & 1)) {
                    shiftSaid[i] |= 1;
                    std::printf("shiftstat: uid=%d bucket=%d t=%d\n",
                                objs[i].uid, (int)bucket[i], t);
                }
                if (shiftUsed != 0 && !(shiftSaid[i] & 2)) {
                    shiftSaid[i] |= 2;
                    std::printf("shiftnz: uid=%d bucket=%d shift=%d t=%d "
                                "anchor=%d recAnchor=%d\n",
                                objs[i].uid, (int)bucket[i], shiftUsed, t,
                                anchor, recAnchor);
                }
            }
            turnedBox(objs[i], bucket[i], (double)rotv,
                      i < everRot.size() && everRot[i] != 0,
                      g_obbScale && i < baseOhw.size() ? (double)baseOhw[i] : 0.0, g_obbScale && i < baseOhh.size() ? (double)baseOhh[i] : 0.0,
                      boxScale(i, bucket[i], (double)hw, (double)hh));
            carryTpY(i, cy - (double)baseCy[i]);   // --tpmove (removed)
            // a moving SLOPE carries its surface line (same rule as seek())
            if (objs[i].slope) {
                const double dy = cy - (double)baseCy[i];
                objs[i].sy0 = (double)baseSy0[i] + dy;
                objs[i].sy1 = (double)baseSy1[i] + dy;
            }
        }
        };
        const size_t nIter = idxs ? idxs->size() : objs.size();
        // Whether the pool is woken is decided by the list the call had BEFORE the quiet objects were left out of it (sq.walk, K1): a level whose non-pure list sits just above kParMin
        // (official lv22: 522 moving objects, every one non-pure) fell below it with the shorter list, and the same-tick placements of every group went back to this one thread.
        // The call then placed fewer objects (CPU of lv22's first call x 0.75) and took LONGER (wall 7.59 -> 8.74 s on a quiet machine: prepare's seek 1,172 -> 1,657 ms,
        // applyTriggers 610 -> 771 ms), because this thread is the search's serial section while the workers wait. With the old decision the shorter list is split over the pool as the
        // longer one was. kParMin itself is not moved.
        const size_t nWork = !idxs ? (samePure ? nNonPure : objs.size()) : (idxs == &sq.walk ? inc.nonPure.size() : nIter + (samePure ? 0 : inc.firstCut));
        forRunsN(par, nWork, nIter,
                 [&](size_t b, size_t e) { place(b, e, false); });
        if (par) place(0, nIter, true);
    }

    // Put every object where it was at tick t. Cursors move forward with the
    // layer loop, so this is O(1) amortised; a backwards jump (--start, or the
    // witness resim starting over) rewinds and re-scans, which happens once.
    //
    // ROW t IS THE RIGHT ROW, and this is the note that stops the next person
    // re-deriving otherwise. GJBaseGameLayer::update runs processCommands
    // (0x238090) -> processMoveActionsStep (0x238388) -> checkCollisions
    // (0x23846e), and the mod's recorder writes its row inside the FIRST of
    // those, so the row written as T holds the objects from before that update's
    // move. It is tempting to conclude that a collision therefore saw row T+1.
    // It did not: the player state that collision produces does not appear in
    // the dump until row T+2 either, so the two offsets cancel and the pairing
    // is (player row T, object row T).
    //
    // Measured twice on 2026-09-03, both against the pairing being anything else
    // (a --dynphase flag that shifted the row by one was built, run and reverted):
    //   * lv19 t=20,601. A ball rides a surface descending 0.062 px/tick
    //     (uid 14011, cy 210.178, h 15): row t gives 210.178 + 7.5 + 15 =
    //     232.678, which is GD's dump to the last digit, for eleven ticks. The
    //     shifted row drops the support on the first tick and falls for five.
    //   * lv22 uid 434's firing edge at t=777, swept in GD at 0.05 px (the
    //     earlier 0.5 px sweep could not tell the two apart, which is how the
    //     shifted row briefly looked better):
    //         dy=0     GD (35.502, 35.552)   row t: 35.541 INSIDE   row t+1: 35.859
    //         dy=43.09 GD (32.052, 32.102)   row t: 32.130 (0.028)  row t+1: 31.616 (0.44)
    // The 0.028 px that is left at dy=43.09 is a residual of the portal
    // predicate, not of the row: see the two-gate contact shape.
    // A search that steps several lineages tick by tick through this one object (searchMontecarlo).
    // What one tick carries into the next is each object's final cy (seek's 1-tick advance reads it
    // for dcy and prevCy); everything else seek rewrites from the recording. So a lineage's world is
    // put back by restoring those and naming the tick they were placed for. forgetPlacement: the
    // next seek is a jump, as after an anchor, whatever was placed last.
    void forgetPlacement() { lastT = 1 << 30; incInvalidate(); }   // later than any tick: the next seek goes back
    void saveCarry(std::vector<double>& cy) const {
        cy.resize(objs.size());
        for (size_t i = 0; i < objs.size(); ++i) cy[i] = objs[i].cy;
    }
    void loadCarry(const std::vector<double>& cy, int tPrev) {
        for (size_t i = 0; i < objs.size() && i < cy.size(); ++i) objs[i].cy = cy[i];
        lastT = tPrev;
        incInvalidate();   // the carried cy is not the fixed point of every object
    }
    // The per-object cursors back to the first row (a State file loaded into this world, statefile.hpp). A forward seek only
    // ever advances a cursor, so one left where a later tick put it (the search ran past the tick
    // the state was saved at) would read a row too late; from 0 the seek finds the same row an
    // unbroken walk would have.
    void resetCursors() { std::fill(cur.begin(), cur.end(), (size_t)0); incInvalidate(); }
    // One object's place for tick t: seek's per-object body, run for every object of a full pass and for the awake
    // ones of an incremental one. `back`: t is before the last tick placed; `step1`: it is the next one.
    void seekOne(size_t i, int t, bool back, bool step1) {
        {
            const auto& sm = samples[i];
            if (sm.empty()) return;
            size_t c = back ? 0 : cur[i];
            if (back && i < smSorted.size() && smSorted[i]) {
                // a backward seek over rows in t order: the walk from row 0 below stops before the first row after row 0 that is later
                // than t, which upper_bound finds (a recording of 113M rows walked every object from row 0 at every anchor)
                c = (size_t)(std::upper_bound(sm.begin() + 1, sm.end(), t, [](int v, const DynSample& r) { return v < r.t; }) - sm.begin()) - 1;
                if (g_dynSeekCheck) {
                    size_t w = 0;
                    while (w + 1 < sm.size() && sm[w + 1].t <= t) ++w;
                    ++g_dynSeekChecks;
                    if (w != c) { ++g_dynSeekDiff; c = w; }
                }
            } else
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
            if (i < prevCy.size()) {
                prevCy[i] = step1 ? objs[i].cy : (float)s.cy;
                objs[i].pcy = step1 ? prevCy[i] : __builtin_nanf("");
            }
            const float oldCx = objs[i].cx;   // --shadowdcx: dcx beside dcy
            objs[i].dcx =
                step1 ? ((double)s.cx - (double)oldCx)
                      : (c > 0 && sm[c].t == t && sm[c - 1].t == t - 1)
                            ? ((double)sm[c].cx - (double)sm[c - 1].cx)
                            : 0.0;
            objs[i].dcy =
                step1 ? ((double)s.cy - (double)objs[i].cy)
                      : (c > 0 && sm[c].t == t && sm[c - 1].t == t - 1)
                            ? ((double)sm[c].cy - (double)sm[c - 1].cy)
                            : 0.0;
            objs[i].cx = s.cx; objs[i].cy = s.cy;
            // --recinterp: between two rows at most kRecInterpGap ticks apart,
            // the object was moving the whole time -- the recorder simply did
            // not write the ticks whose step was under its 0.05 px threshold
            // (grouptrace kEps). Holding the older row made a slow mover step
            // every other tick: lv22 uid5825 at t=6,118..6,124 has rows at
            // 6,118 / 6,119 / 6,121 / 6,123 while GD's rider moves every tick.
            // A longer gap is a real stop and keeps the hold.
            if (s.t < t && c + 1 < sm.size()
                && sm[c + 1].t - s.t <= kRecInterpGap
                && !recStallGap(sm, c)) {   // --recstall
                const DynSample& n = sm[c + 1];
                const double u = (double)(t - s.t) / (double)(n.t - s.t);
                objs[i].cx = (float)((double)s.cx + u * ((double)n.cx - (double)s.cx));
                objs[i].cy = (float)((double)s.cy + u * ((double)n.cy - (double)s.cy));
                if (step1 && i < prevCy.size())
                    objs[i].dcy = (double)objs[i].cy - (double)prevCy[i];
                if (step1) objs[i].dcx = (double)objs[i].cx - (double)oldCx;
            } else if (g_recInterpSlow && s.t < t && recGapLikeNeighbours(sm, c)
                       && sm[c + 1].t - s.t > kRecInterpGap) {   // --recinterpslow (a stall keeps its hold)
                const DynSample& n = sm[c + 1];
                const double u = (double)(t - s.t) / (double)(n.t - s.t);
                const auto lerp = [u](float a, float b) {
                    const double d = (double)b - (double)a;
                    return std::fabs(d) < 2.0 * kRecEps ? (float)((double)a + d * u) : a;
                };
                objs[i].cx = lerp(s.cx, n.cx);
                objs[i].cy = lerp(s.cy, n.cy);
                if (step1 && i < prevCy.size())
                    objs[i].dcy = (double)objs[i].cy - (double)prevCy[i];
                if (step1) objs[i].dcx = (double)objs[i].cx - (double)oldCx;
            }
            // --moverextend: past the recording's last row (the attempt's death) a constant-velocity mover
            // goes on (a custom level's uid 139041: rows end at t=10,272, +1.4707 px/tick; GD's spike is at
            // 12,339.088 on 10,273 and kills a mini wave the held row misses by 1.46 px).
            const bool mvExt = i < extOk.size() && extOk[i]
                               && c + 1 == sm.size() && t > s.t;
            if (mvExt) {
                const float k = (float)std::min(t - s.t, extCap[i]);
                objs[i].cx = s.cx + extVx[i] * k;
                objs[i].cy = s.cy + extVy[i] * k;
                if (step1 && i < prevCy.size())
                    objs[i].dcy = (double)objs[i].cy - (double)prevCy[i];
                if (step1) objs[i].dcx = (double)objs[i].cx - (double)oldCx;
            }
            float hwNow = s.hw, hhNow = s.hh;
            if (s.t < t && recSizeGapOk(sm, c)) {   // --recinterpsize
                const DynSample& n = sm[c + 1];
                const double u = (double)(t - s.t) / (double)(n.t - s.t);
                hwNow = recSizeLerp(s.hw, n.hw, u);
                hhNow = recSizeLerp(s.hh, n.hh, u);
            }
            double rotNow = (double)s.rot;
            // --recendextend: past the last row by the last steady step, where --moverextend did not place it.
            if (!mvExt && c + 1 == sm.size())
                if (const int k = recEndTicks(i, t)) {
                    objs[i].cx = s.cx + recVx[i] * (float)k;
                    objs[i].cy = s.cy + recVy[i] * (float)k;
                    hwNow += recVhw[i] * (float)k; hhNow += recVhh[i] * (float)k;
                    rotNow += (double)recVr[i] * k;
                    if (step1 && i < prevCy.size())
                        objs[i].dcy = (double)objs[i].cy - (double)prevCy[i];
                }
            {   // --facecarry: this tick's change of the half-height, as dcy is this tick's change of the centre
                const double hhBefore = (double)objs[i].hh;
                objs[i].dhh = step1 ? ((double)hhNow - hhBefore)
                                    : (c > 0 && sm[c].t == t && sm[c - 1].t == t - 1)
                                          ? ((double)sm[c].hh - (double)sm[c - 1].hh)
                                          : 0.0;
            }
            boxBase(i);   // --obbscale
            objs[i].hw = hwNow; objs[i].hh = hhNow;
            objs[i].onPrev = 1;
            objs[i].onNew = 0;
            on[i] = recOnAt(sm, c, t);   // --offafter
            if (bucket[i] == PAD && s.on && s.t == t && c > 0 && !sm[c - 1].on)   // --padonlate (removed): the row that switches a pad ON lands on the tick after it
                on[i] = 0;
            if (on[i] && bucket[i] == NEAR && objs[i].type != 0 && objs[i].hw <= 0.f && objs[i].hh <= 0.f)
                on[i] = 0;   // --zerosaw
            // --onlag: a row that switches the object ON takes effect the tick after it, as the OFF row does
            // (--offafter): the game's collision list is rebuilt after the toggle, so the object is not there on the
            // row's own tick, and on the tick after it the sweep starts at its sub-steps, not at the previous
            // tick's end position. A custom level t=16,381: a saw toggled on 22.8 px from a mini wave that GD
            // lets through at that tick and, 1.95 px later, tests only from the end position.
            if (bucket[i] == NEAR && objs[i].type != 0 && s.on && c > 0 && !sm[c - 1].on) {
                if (s.t == t) objs[i].onNew = 1;
                else if (s.t == t - 1) objs[i].onPrev = 0;
            }
            objs[i].recOn = s.on;   // --holdtap (object.hpp)
            objs[i].pendOff = 0;   // --pressfresh: applyTriggers decides it per group
            turnedBox(objs[i], bucket[i], rotNow,
                      i < everRot.size() && everRot[i] != 0,
                      g_obbScale && i < baseOhw.size() ? (double)baseOhw[i] : 0.0, g_obbScale && i < baseOhh.size() ? (double)baseOhh[i] : 0.0,
                      boxScale(i, bucket[i], (double)hwNow, (double)hhNow));
            obbLatch(i, t, rotNow);   // --rotobblatch
            // --dynobbrot: the hazard's turned box at this row's angle. objs[i].rot is still the
            // placed angle (turnedBox writes it for portals and speed portals only), and GD's
            // rotation is clockwise, so the axis turns by minus the difference.
            if (g_dynObbRot && objs[i].obbOk && i < everRot.size() && everRot[i]
                && i < obbBc0.size()) {
                const double a = -(rotNow - objs[i].rot) * (3.14159265358979 / 180.0);
                const double ca = std::cos(a), sa = std::sin(a);
                objs[i].bc = obbBc0[i] * ca - obbBs0[i] * sa;
                objs[i].bs = obbBc0[i] * sa + obbBs0[i] * ca;
            }
            if (objs[i].slope) {
                const double dy = (double)objs[i].cy - (double)baseCy[i];
                objs[i].sy0 = (double)baseSy0[i] + dy;
                objs[i].sy1 = (double)baseSy1[i] + dy;
            }
            carryTpY(i, (double)objs[i].cy - (double)baseCy[i]);   // --tpmove (removed)
            if (g_dynDbg == objs[i].uid)
                std::printf("dynseek t=%d uid=%d cur=%zu/%zu cy=%.3f "
                            "(sample t=%d) at=%p\n",
                            t, objs[i].uid, c, sm.size(), (double)objs[i].cy,
                            s.t, (const void*)&objs[i]);
        }
    }

    // ---- incremental placement (--incplace) ----------------------------------------------------------------------
    //
    // seekOne rewrites an object from the recording row in force at tick t. Written again where nothing it reads has
    // changed it writes the bytes the object already holds, so after a pass that left every object at that fixed
    // point, a one-tick advance only has to place the objects it can change:
    //  * the non-pure ones and the pure ones an autonomous trigger anchors (applyTriggers places those from
    //    the level-wide clocks, every tick) and the few seek cannot reason about (a latch on the tick, an
    //    extension without a short end): `always`;
    //  * every other object, on the tick of one of its rows and the tick after it (`on` lands a tick later, and dcy and
    //    prevCy settle one tick after a move), and on every tick inside a gap the recording interpolates across or
    //    past its last row while an extension goes on: the events, from the rows alone.
    // Between those an object is in a row's interval with no t in anything it computes: cx, cy, hw, hh, rot and on are
    // the row's, dcy is 0 and prevCy is cy, and turnedBox, the slope line and the teleport target are functions of
    // those (turnedBox is idempotent: a second call with the same box and angle writes what the first did).
    // The first full pass may leave dcy / prevCy off the fixed point (it came from a jump, or from a lineage's carried
    // cy), so skipping starts at the third tick of a run of one-tick advances: the second full pass settles it.
    // Every way the state could be moved from outside clears `settled` (loadCarry, forgetPlacement, a new call's
    // generation, a change of flags), and --incplacecheck compares the incremental result with the full one.
    uint32_t incSig() const {
        return (g_recInterpSlow ? 1u : 0u) | (g_recInterpSize ? 2u : 0u) | (4u)
               | (g_moverExtendVel ? 8u : 0u) | (16u) | (g_offAfter ? 32u : 0u)
               | (g_dynObbRot ? 64u : 0u) | (g_incAnchor ? 128u : 0u) | ((uint32_t)objs.size() << 8);
    }
    // The same conditions applyTriggers calls `quiet`, plus the parallel loop's: no print flag, no object named
    // uid -1 (--dyndbg's default).
    bool incOn() const {
        return g_incPlace && g_dynDbg == -1 && g_shiftDbgUid < 0 && !g_shiftStat
               && !(g_rotCompute && !g_rotSpec.empty()) && !anyUidM1;
    }
    void incInvalidate() { inc.settled = false; inc.lastFull = false; inc.dKnown = false; inc.atT = INT_MIN; inc.pKnown = false; sq.lastT = INT_MIN; }   // (sq: the quiet objects' run of calls ends too)
    // ---- --incanchor: the closed-form anchored objects ----
    // An object an autonomous trigger moves by a closed form (applyTriggers' superposition branch, or its collapsed
    // one) is written from: the position of its first row plus each Move's eased offset at (t - the Move's fireT);
    // size, angle and on/off from its rows (re-timed to the controller's fire tick once it has fired) and from the
    // cursor seek keeps on the tick's own clock. Every one of those is constant outside a span the dates and the rows
    // give, so outside it a placement writes the bytes the object holds. The spans are padded: a move starts to show
    // a tick after its date, ends a tick after its last step, and dcy / prevCy settle one tick after that.
    static int incDurPad(double d) { return (int)std::min(std::ceil(std::max(d, 0.0)), 1.0e7) + 3; }
    bool incAnchorClass(size_t i, bool atNoop) const {
        if (atNoop || !isPure(i)) return false;
        return incAnchorBody(i);
    }
    // K1: an object that is non-pure for the supply's on/off clause alone, has one row (so its on/off is the supply's whenever the placement reaches the hook) and that the anchored slots would
    // take were it pure. Its place is the same closed form, so outside a slot's span a placement writes the bytes it holds; what the clause adds, its on/off, is Dynamics::sqHead's to watch.
    bool k1SlotClass(size_t i) const {
        return g_supplyOnHook && g_supplyOnSyncHook && g_incAnchor && !isPure(i) && isPureButSupply(i) && i < samples.size() && samples[i].size() == 1 && incAnchorBody(i);
    }
    bool incAnchorBody(size_t i) const {
        if (i >= autoAnchor.size() || autoAnchor[i] < 0 || (size_t)autoAnchor[i] >= g_autoTrig.size()) return false;
        if (i >= samples.size() || samples[i].empty() || i >= trigRecFire.size() || i >= autoClosed.size()
            || i >= autoDur.size() || i >= autoParts.size())
            return false;
        if (i >= recLag.size() && i >= autoLag.size()) return false;
        if (i < obbLatchT.size() && obbLatchT[i] >= 0) return false;
        if (i < rotTail.size() && !rotTail[i].empty()) return false;
        if (i < offBox.size() && !offBox[i].empty()) return false;
        // [R] Its switches past the recording's end (--recendtail, applyTriggersImpl): past `after` = max(its last row, tailRecEnd) the on/off is resolveOnAfter's, which reads the tick and, a
        // switch at a time, a box's fire tick of the group, or a trigger's fire tick (g_autoTrig[].fireT) and whether a box of the group disarmed it (autoDisarmed), or a fixed tick. With no box
        // and no disarmable trigger among them the answer is a function of the tick and of dates alone, and it changes only at after + 1 and at each switch's tick ceil(date + delay + lag): the
        // slot holds those ticks (incAddSlot: pSwTrig / pSwAt), is placed around each (incPActive) and reads the dates as it reads its controller's (pDep). The shift of the recording's clock
        // that `after` takes is 0 here: it is the recording-driven branch's, which is not this class (below).
        if (i < tailOnEv.size())
            for (const OnEvent& e : tailOnEv[i]) {
                if (e.box >= 0) return false;
                if (e.autoIdx >= 0 && (size_t)e.autoIdx < g_autoDisarm.size() && !g_autoDisarm[(size_t)e.autoIdx].empty()) return false;
            }
        if (i < tailOnBox.size() && tailOnBox[i]) return false;
        if (i < rotSplit.size() && rotSplit[i]) return false;
        if (i < rigTail.size() && !rigTail[i].x.empty()) return false;
        if (i < extOk.size() && extOk[i]) return false;
        if (i < recOk.size() && recOk[i]) return false;
        if (objs[i].pendBox >= 0) return false;
        if (trigRecFire[i] >= 0 && !autoClosed[i]) return false;   // the recording-driven branch
        for (const AutoPart& p : autoParts[i])
            if (p.trig < 0 || (size_t)p.trig >= g_autoTrig.size()) return false;
        return true;
    }
    void incAddSlot(size_t i) {
        IncState::PSlot s;
        s.obj = (uint32_t)i;
        if (inc.pSlotOf.size() <= i) inc.pSlotOf.resize(objs.size(), -1);
        inc.pSlotOf[i] = (int32_t)inc.pSlot.size();
        s.anchor = autoAnchor[i];
        const auto& sm = samples[i];
        s.rows = trigRecFire[i] >= 0 && sm.size() > 1;
        s.rowT1 = sm.size() > 1 ? sm[1].t : sm[0].t;
        s.rowTL = sm.back().t;
        s.e0 = sm[0].t - 1;
        s.e1 = sm[0].t + 2;
        if (sm.size() > 1) { s.u0 = sm[1].t - 2; s.u1 = sm.back().t + 3; }
        s.p0 = (uint32_t)inc.pPartTrig.size();
        if (autoParts[i].empty()) {
            inc.pPartTrig.push_back(s.anchor);
            inc.pPartDur.push_back(incDurPad(autoDur[i]));
        } else {
            for (const AutoPart& p : autoParts[i]) {
                inc.pPartTrig.push_back(p.trig);
                inc.pPartDur.push_back(incDurPad(p.dur));
            }
        }
        s.p1 = (uint32_t)inc.pPartTrig.size();
        if (inc.pDep.size() != g_autoTrig.size()) inc.pDep.assign(g_autoTrig.size(), {});
        std::vector<int> deps{s.anchor};
        s.w0 = (uint32_t)inc.pSwTrig.size();
        if (i < tailOnEv.size() && !tailOnEv[i].empty()) {
            inc.pSwTrig.push_back(-1);
            inc.pSwAt.push_back((double)std::max(sm.back().t, tailRecEnd) + 1.0);   // the first tick the switches answer for
            for (const OnEvent& e : tailOnEv[i]) {
                const double lag = offAfterLag(e);
                if (e.autoIdx >= 0) {
                    if ((size_t)e.autoIdx >= g_autoTrig.size()) continue;   // (resolveOnAfter passes it by)
                    inc.pSwTrig.push_back(e.autoIdx);
                    inc.pSwAt.push_back(e.delay + lag);
                    deps.push_back(e.autoIdx);
                } else if (e.at >= 0) {
                    inc.pSwTrig.push_back(-1);
                    inc.pSwAt.push_back(std::ceil((double)e.at + e.delay + lag));
                }
            }
        }
        s.w1 = (uint32_t)inc.pSwTrig.size();
        for (uint32_t q = s.p0; q < s.p1; ++q) {
            const int b = inc.pPartTrig[q];
            deps.push_back(b);
            if (g_autoStop && (size_t)b < g_autoStopBy.size())
                for (const int st : g_autoStopBy[(size_t)b]) deps.push_back(st);
        }
        std::sort(deps.begin(), deps.end());
        deps.erase(std::unique(deps.begin(), deps.end()), deps.end());
        for (const int b : deps)
            if (b >= 0 && (size_t)b < inc.pDep.size()) inc.pDep[(size_t)b].push_back((uint32_t)inc.pSlot.size());
        inc.pSlot.push_back(s);
    }
    // the span the current dates put the slot's changes in
    void incPRecompute(IncState::PSlot& s) const {
        int lo = INT_MAX, hi = INT_MIN;
        const int F = g_autoTrig[(size_t)s.anchor].fireT;
        if (F >= 0) {
            lo = F - 2;
            hi = F + 3;
            if (s.rows) {
                const size_t o = s.obj;
                int recAnchor = trigRecFire[o] - (o < autoLag.size() ? autoLag[o] : recLag[o]);
                if (o < anchorUnrec.size() && anchorUnrec[o]) recAnchor = F;
                else if (o < recAnchorX.size() && recAnchorX[o] >= 0) recAnchor = recAnchorX[o];
                const int shift = F - recAnchor;
                lo = std::min(lo, s.rowT1 + shift - 2);
                hi = std::max(hi, s.rowTL + shift + 3);
            }
        }
        for (uint32_t q = s.p0; q < s.p1; ++q) {
            const int f = g_autoTrig[(size_t)inc.pPartTrig[q]].fireT;
            if (f < 0) continue;
            lo = std::min(lo, f - 2);
            hi = std::max(hi, f + inc.pPartDur[q]);
        }
        s.d0 = lo;
        s.d1 = hi;
    }
    // every object has just been placed under the current dates
    void incPSyncAll() {
        if (inc.pSlot.empty()) return;
        ++pSlotMut;
        inc.pFire.resize(g_autoTrig.size());
        for (size_t b = 0; b < g_autoTrig.size(); ++b) inc.pFire[b] = g_autoTrig[b].fireT;
        for (IncState::PSlot& s : inc.pSlot) { incPRecompute(s); s.force = INT_MIN; }
        inc.pKnown = true;
    }
    // the slots that read a date which is not the one the objects were last placed under: placed now and on the next tick.
    // Their objects, ascending, are left in inc.pWoke. Returns how many.
    size_t incPWake(int t) {
        inc.pWoke.clear();
        if (inc.pSlot.empty()) return 0;
        ++g_incStats.pWakes;
        if (!inc.pKnown || inc.pFire.size() != g_autoTrig.size() || inc.pDep.size() != g_autoTrig.size()) {
            incPSyncAll();   // (sizes the dates)
            ++pSlotMut;
            for (IncState::PSlot& s : inc.pSlot) { s.force = t + 1; inc.pWoke.push_back(s.obj); }
            g_incStats.pWokeObjs += (long long)inc.pWoke.size();
            return inc.pWoke.size();
        }
        std::vector<uint32_t> slots;
        for (size_t b = 0; b < g_autoTrig.size(); ++b) {
            if (g_autoTrig[b].fireT == inc.pFire[b]) continue;
            inc.pFire[b] = g_autoTrig[b].fireT;
            for (const uint32_t q : inc.pDep[b]) slots.push_back(q);
        }
        if (slots.empty()) return 0;
        ++pSlotMut;
        std::sort(slots.begin(), slots.end());
        slots.erase(std::unique(slots.begin(), slots.end()), slots.end());
        for (const uint32_t q : slots) {
            IncState::PSlot& s = inc.pSlot[q];
            incPRecompute(s);
            s.force = t + 1;
            inc.pWoke.push_back(s.obj);
        }
        g_incStats.pWokeObjs += (long long)inc.pWoke.size();
        return inc.pWoke.size();
    }
    // the tick switch q of a slot lands on under the current dates; INT_MIN = its trigger has not fired
    int incPSwTick(uint32_t q) const {
        const int b = inc.pSwTrig[q];
        if (b < 0) return (int)inc.pSwAt[q];
        const int f = g_autoTrig[(size_t)b].fireT;
        return f < 0 ? INT_MIN : (int)std::ceil((double)f + inc.pSwAt[q]);
    }
    // [R] A switch landing at te changes resolveOnAfter's answer at the first tick t with te <= t (its `te > t` test), which is c = ceil(te), and at no other: placed at c. A placement of tick t
    // is also what the tick after it holds as its previous tick (the previous-place pass places at t - 1, the step is taken from the bytes of t - 1), so c + 1 is the first tick whose both
    // sides have the new answer: placed at c + 1 as well. These are the two ticks the switches of an object with no controller are given (incBuild: c, c + 1 and after + 1, after + 2).
    static constexpr int kPSwLo = 0, kPSwHi = 1;
    bool incPActive(const IncState::PSlot& s, int t) const {
        if (s.force >= t || (t >= s.e0 && t <= s.e1) || (t >= s.u0 && t <= s.u1) || (t >= s.d0 && t <= s.d1)) return true;
        return incPSwActive(s, t);
    }
    bool incPSwActive(const IncState::PSlot& s, int t) const {
        for (uint32_t q = s.w0; q < s.w1; ++q) {
            const int c = incPSwTick(q);
            if (c != INT_MIN && (long long)t >= (long long)c - kPSwLo && (long long)t <= (long long)c + kPSwHi) return true;
        }
        return false;
    }
    static constexpr int kIncBuildAfter = 32;   // one-tick advances before the index is built (it takes ~3 ms at 8,400 objects)
    void incBuild() {
        ++g_incStats.builds;
        ++incBuilds;
        const auto tb0 = std::chrono::steady_clock::now();
        inc = IncState();
        const size_t n = objs.size();
        inc.gen = g_dynPlaceGen;
        inc.sig = incSig();
        constexpr int kMaxExt = 4096;       // an extension longer than this is placed on every tick
        constexpr int kMaxGap = 4096;       // ...and so is an interpolated gap longer than this
        constexpr int64_t kMaxSpan = 1 << 24;
        std::vector<std::pair<int, uint32_t>> ev;
        std::vector<std::pair<int, double>> dEvs;
        int lo = INT_MAX, hi = INT_MIN;
        for (size_t i = 0; i < n; ++i) {
            const bool pure = isPure(i);
            const bool atNoop = pure && i < autoAnchor.size() && autoAnchor[i] < 0;
            if (!pure) inc.nonPure.push_back((uint32_t)i);
            if (!atNoop) inc.atList.push_back((uint32_t)i);
            if (i >= samples.size() || samples[i].empty()) continue;   // seekOne has nothing to do for it
            const auto& sm = samples[i];
            // An autonomous object that no recorded motion anchors (trigRecFire < 0) and none of whose moves goes
            // anywhere is placed by applyTriggers' closed form at its first row, whichever tick it is and whether or
            // not its trigger has fired (fired adds 0 * ease); like the uncontrolled ones it changes only where its
            // rows do (its `on` follows the row in force). What stands in the way of that is listed here.
            bool isC = false, isD = false;
            if (!atNoop && pure && i < autoAnchor.size() && autoAnchor[i] >= 0 && i < trigRecFire.size()
                && trigRecFire[i] < 0 && i < autoDx.size() && i < autoDy.size() && autoDx[i] == 0.f
                && autoDy[i] == 0.f && i < autoParts.size() && (i >= rotTail.size() || rotTail[i].empty())
                && (i >= offBox.size() || offBox[i].empty()) && objs[i].pendBox < 0) {
                isC = true;
                for (const AutoPart& p : autoParts[i])
                    if (p.dx != 0.f || p.dy != 0.f) { isC = false; break; }
                // switches past the recording's end (--recendtail): x-crossing and fixed-tick ones only (a pure object has
                // no touch switch: tailOnBox)
                if (isC && i < tailOnEv.size() && !tailOnEv[i].empty()) {
                    isD = true;
                    for (const OnEvent& e : tailOnEv[i])
                        if (e.box >= 0) { isC = isD = false; break; }
                }
            }
            bool alwaysI = (!atNoop && !isC) || (i < obbLatchT.size() && obbLatchT[i] >= 0);
            const size_t evBegin = ev.size();
            if (alwaysI && g_incAnchor && (incAnchorClass(i, atNoop) || k1SlotClass(i))) {
                incAddSlot(i);
                continue;
            }
            if (!alwaysI) {
                const size_t m = sm.size();
                for (size_t k = 0; k < m && !alwaysI; ++k) {
                    const int Tk = sm[k].t;
                    ev.push_back({Tk, (uint32_t)i});
                    ev.push_back({Tk + 1, (uint32_t)i});
                    if (k + 1 < m) {
                        const int gap = sm[k + 1].t - Tk;
                        // a superset of where seek interpolates a position or a size (it also takes recStallGap into
                        // account; here a stalled gap is simply placed on every tick as well)
                        const bool vol = gap <= kRecInterpGap || (g_recInterpSlow && recGapLikeNeighbours(sm, k))
                                         || recSizeGapOk(sm, k);
                        if (vol && gap > 2) {
                            if (gap > kMaxGap) { alwaysI = true; break; }   // (an interpolation this long is a rare thing)
                            for (int v = Tk + 2; v < sm[k + 1].t; ++v) ev.push_back({v, (uint32_t)i});
                        }
                    } else {
                        int ext = 0;
                        if (i < extOk.size() && extOk[i])
                            ext = std::max(ext, extCap[i]);
                        if (i < recOk.size() && recOk[i]) ext = std::max(ext, kRecEndExtendTicks);
                        if (ext > kMaxExt) { alwaysI = true; break; }
                        for (int v = Tk + 2; v <= Tk + ext + 1; ++v) ev.push_back({v, (uint32_t)i});
                    }
                }
                if (!alwaysI && isD) {
                    // resolveOnAfter takes over from tick `after` on; a fixed-tick switch lands where it says
                    const int after = std::max(sm.back().t, tailRecEnd);
                    ev.push_back({after + 1, (uint32_t)i});
                    ev.push_back({after + 2, (uint32_t)i});
                    for (const OnEvent& e : tailOnEv[i]) {
                        const double lag = offAfterLag(e);
                        if (e.autoIdx >= 0) {
                            if ((size_t)e.autoIdx < g_autoTrig.size()) dEvs.push_back({e.autoIdx, e.delay + lag});
                        } else if (e.at >= 0) {
                            const int c = (int)std::ceil((double)e.at + e.delay + lag);
                            ev.push_back({c, (uint32_t)i});
                            ev.push_back({c + 1, (uint32_t)i});
                        }
                    }
                }
            }
            if (alwaysI) {
                ev.resize(evBegin);
                inc.always.push_back((uint32_t)i);
            } else {
                if (isD) inc.dList.push_back((uint32_t)i);
                for (size_t q = evBegin; q < ev.size(); ++q) {
                    lo = std::min(lo, ev[q].first);
                    hi = std::max(hi, ev[q].first);
                }
            }
        }
        std::sort(dEvs.begin(), dEvs.end());
        dEvs.erase(std::unique(dEvs.begin(), dEvs.end()), dEvs.end());
        inc.dEv = dEvs;
        for (const auto& d : dEvs)
            if (inc.dTrigs.empty() || inc.dTrigs.back() != d.first) inc.dTrigs.push_back(d.first);
        if (!ev.empty()) {
            if ((int64_t)hi - (int64_t)lo > kMaxSpan) { ++g_incStats.buildFail; inc.failed = true; return; }   // valid stays false
            std::sort(ev.begin(), ev.end());
            ev.erase(std::unique(ev.begin(), ev.end()), ev.end());
            inc.evT0 = lo;
            inc.evT1 = hi;
            inc.evStart.assign((size_t)(hi - lo) + 2, 0);
            for (const auto& e : ev) ++inc.evStart[(size_t)(e.first - lo) + 1];
            for (size_t q = 1; q < inc.evStart.size(); ++q) inc.evStart[q] += inc.evStart[q - 1];
            inc.evObj.reserve(ev.size());
            for (const auto& e : ev) inc.evObj.push_back(e.second);
        }
        inc.valid = true;
        inc.wUntil.clear(); inc.wList.clear(); inc.wSorted = true;
        { long long n = 0; for (const IncState::PSlot& s : inc.pSlot) n += s.w1 > s.w0; g_incStats.pSwSlots = n; }
        if (g_phaseProf) {   // what the placed-every-tick objects are made of (measurement)
            size_t nonPure = 0, rec = 0, noRec = 0, closed = 0, p0 = 0, p1 = 0, p2 = 0, tails = 0, ext = 0, rt = 0, off = 0,
                   latch = 0, tailOn = 0, split = 0, nrow1 = 0, simple = 0;
            for (const uint32_t u : inc.always) {
                if (!isPure(u)) { ++nonPure; continue; }
                const bool hasRec = u < trigRecFire.size() && trigRecFire[u] >= 0 && u < samples.size() && samples[u].size() > 1;
                (hasRec ? rec : noRec)++;
                const bool cl = u < autoClosed.size() && autoClosed[u];
                closed += cl;
                const size_t np = u < autoParts.size() ? autoParts[u].size() : 0;
                (np == 0 ? p0 : np == 1 ? p1 : p2)++;
                const bool rtl = u < rotTail.size() && !rotTail[u].empty();
                const bool ofb = u < offBox.size() && !offBox[u].empty();
                const bool lat = u < obbLatchT.size() && obbLatchT[u] >= 0;
                const bool ton = u < tailOnEv.size() && !tailOnEv[u].empty();
                const bool spl = u < rotSplit.size() && rotSplit[u];
                const bool ex = (u < extOk.size() && extOk[u]) || (u < recOk.size() && recOk[u]);
                rt += rtl; off += ofb; latch += lat; tailOn += ton; split += spl; ext += ex;
                tails += (u < rigTail.size() && !rigTail[u].x.empty());
                if (u < samples.size() && samples[u].size() <= 1) ++nrow1;
                if (hasRec && !cl && !rtl && !ofb && !lat && !ton && !spl && !(u < rigTail.size() && !rigTail[u].x.empty())) ++simple;
            }
            std::printf("incbuild: always=%zu: non-pure %zu | pure: with recording %zu (simple %zu), without %zu; closed %zu; autoParts 0/1/2+ = %zu/%zu/%zu; "
                        "rotTail %zu offBox %zu latch %zu tailOn %zu split %zu rigTail %zu ext %zu 1-row %zu\n",
                        inc.always.size(), nonPure, rec, simple, noRec, closed, p0, p1, p2, rt, off, latch, tailOn, split, tails, ext, nrow1);
        }
        g_incStats.buildMs +=std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tb0).count();
        if (g_phaseProf)
            std::printf("incbuild: %zu objects: %zu non-pure, %zu in applyTriggers' first walk, %zu placed on every tick, "
                        "%zu d-class (%zu switches on %zu triggers), %zu row events\n",
                        n, inc.nonPure.size(), inc.atList.size(), inc.always.size(), inc.dList.size(), inc.dEv.size(),
                        inc.dTrigs.size(), inc.evObj.size());
    }
    // --phaseprof: what the objects placed on every tick (inc.always) are doing, per tick of the call. Measurement only.
    // `near` is the object's box inside the group's window (+-60); `ahead` is farther on, `behind` is left of it.
    // "never" = behind, and nothing it can still do brings it back: pure (no lock / formula / touch), its recording's
    // rightmost row and the sum of its Moves' positive dx put its box left of the window.
    std::vector<float> cenCx, cenCy, cenMaxX;
    std::vector<uint8_t> cenOn, cenCatch;
    int cenT = INT_MIN;
    void censusAlways(int t, double wLo, double wHi) {
        if (!inc.valid || cenT == t) return;
        const bool shadowOk = cenT == t - 1 && cenCx.size() == objs.size();
        if (cenCx.size() != objs.size()) {
            cenCx.assign(objs.size(), 0.f); cenCy.assign(objs.size(), 0.f); cenOn.assign(objs.size(), 0);
            cenMaxX.assign(objs.size(), 0.f); cenCatch.assign(objs.size(), 0);
            for (size_t i = 0; i < objs.size(); ++i) {
                float mx = objs[i].cx;
                if (i < samples.size()) for (const DynSample& s : samples[i]) mx = std::max(mx, s.cx);
                bool catches = !isPure(i) || (i < rotTail.size() && !rotTail[i].empty());
                if (i < autoParts.size())
                    for (const AutoPart& p : autoParts[i]) {
                        mx += std::max(0.f, p.dx);
                        if (p.mover != 0 || p.tmode != 0) catches = true;
                    }
                cenMaxX[i] = mx;
                cenCatch[i] = catches ? 1 : 0;
            }
        }
        cenT = t;
        CenStats& c = g_cen;
        for (const uint32_t u : inc.always) {
            const Obj& o = objs[u];
            const bool moved = shadowOk && (o.cx != cenCx[u] || o.cy != cenCy[u] || on[u] != cenOn[u]);
            const int zone = (o.cx + o.hw >= wLo - 60 && o.cx - o.hw <= wHi + 60) ? 0 : (o.cx - o.hw > wHi + 60) ? 1 : 2;
            if (shadowOk) {
                ++c.n; ++c.zone[zone][moved ? 1 : 0];
                if (zone == 2 && !cenCatch[u] && (double)cenMaxX[u] + o.hw + 60 < wLo - 60) ++c.never[moved ? 1 : 0];
                if (zone == 2 && cenCatch[u]) ++c.behindCatch[moved ? 1 : 0];
                if (!moved) {
                    const int aa = u < autoAnchor.size() ? autoAnchor[u] : -1;
                    if (aa >= 0 && (size_t)aa < g_autoTrig.size() && g_autoTrig[(size_t)aa].fireT >= 0) ++c.idleFired;
                    else ++c.idleUnfired;
                }
            }
            cenCx[u] = o.cx; cenCy[u] = o.cy; cenOn[u] = on[u];
        }
        if (shadowOk) ++c.ticks;
    }
    // dFire / dNext / atT as of a placement of every d-class object at tick t (a full walk, or a wake)
    void incNotedPlaced(int t) {
        inc.dFire.resize(inc.dTrigs.size());
        for (size_t q = 0; q < inc.dTrigs.size(); ++q) inc.dFire[q] = g_autoTrig[(size_t)inc.dTrigs[q]].fireT;
        int nx = INT_MAX;
        for (const auto& d : inc.dEv) {
            const int ft = g_autoTrig[(size_t)d.first].fireT;
            if (ft < 0) continue;
            const double c = std::ceil((double)ft + d.second);
            if (c > (double)t && c < (double)nx) nx = (int)c;
        }
        inc.dNext = nx;
        inc.dKnown = true;
        inc.atT = t;
    }
    // The objects applyTriggers' first placement of tick t walks. After an incremental seek of the same tick and an
    // unbroken run of first placements, what that seek placed (the d-class objects join when one of their
    // fireT has changed or a switch of theirs takes effect now); otherwise every object that is not a no-op.
    const std::vector<uint32_t>* incAtFirstList(int t) { return incWithWrote(incAtFirstList1(t), t, inc.wBufF); }   // (a later group of the tick: the objects written since the seek's list)
    const std::vector<uint32_t>* incAtFirstList1(int t) {
        const bool incTick = inc.procT == t && (inc.atT == t || inc.atT == t - 1) && inc.dKnown;
        inc.firstCut = 0;
        if (!incTick) { incNotedPlaced(t); incPSyncAll(); return &inc.atList; }
        const std::vector<uint32_t>* l = sqWithTodoA(incAtFirstList0(t));
        if (!inc.procShort) return l;
        // proc was made without the stable quiet objects of the seek's call (incProcList). This call passes by the same ones or fewer: the ones of the set that are to do now and were not
        // then join; a call that passes none by (or has too many to do) takes the whole set back.
        static const bool qchk = std::getenv("QLISTCHECK") != nullptr;
        if (sqShortOk()) {
            sqTodoAlways(sq.tNow);
            inc.firstCut = inc.procCut;
            if (sq.tNow != inc.procTodo) {
                inc.procF.clear();
                std::set_union(l->begin(), l->end(), sq.tNow.begin(), sq.tNow.end(), std::back_inserter(inc.procF));
                l = &inc.procF;
            }
            if (qchk) sqListCheck(*l, "applyTriggers", t);
            return l;
        }
        inc.procF.clear();
        std::set_union(l->begin(), l->end(), inc.always.begin(), inc.always.end(), std::back_inserter(inc.procF));
        return &inc.procF;
    }
    const std::vector<uint32_t>* incAtFirstList0(int t) {
        inc.atT = t;
        // --incanchor: a later group of the tick reads other dates than the group the seek placed for
        const size_t pw = incPWake(t);
        bool wake = false;
        if (!inc.dList.empty()) {
            wake = t >= inc.dNext;
            for (size_t q = 0; !wake && q < inc.dTrigs.size(); ++q)
                wake = g_autoTrig[(size_t)inc.dTrigs[q]].fireT != inc.dFire[q];
        }
        if (!wake && pw == 0) return &inc.proc;
        if (wake) incNotedPlaced(t);
        std::vector<uint32_t> extra;
        if (wake && pw > 0)
            std::set_union(inc.dList.begin(), inc.dList.end(), inc.pWoke.begin(), inc.pWoke.end(), std::back_inserter(extra));
        else if (wake)
            extra = inc.dList;
        else
            extra = inc.pWoke;
        inc.procD.clear();
        std::set_union(inc.proc.begin(), inc.proc.end(), extra.begin(), extra.end(), std::back_inserter(inc.procD));
        return &inc.procD;
    }
    // Per object: its rows are in t order, so seekOne's backward seek may search them. Filled on the first backward seekFull (one pass over the rows,
    // before the parallel runs), after the loader has finished the rows; empty = not known yet, and the walk is used.
    std::vector<uint8_t> smSorted;
    void seekSortedPrep() {
        if (smSorted.size() == samples.size()) return;
        smSorted.assign(samples.size(), 1);
        for (size_t i = 0; i < samples.size(); ++i)
            for (size_t k = 1; k < samples[i].size(); ++k)
                if (samples[i][k].t < samples[i][k - 1].t) { smSorted[i] = 0; break; }
    }
    void seekFull(int t, bool back, bool step1, bool par) {
        ++g_incStats.seekFull;
        if (back) seekSortedPrep();
        forRuns(par, objs.size(), [&](size_t b, size_t e) {
            for (size_t i = b; i < e; ++i) { if (sq.active && sq.skip[i]) continue; seekOne(i, t, back, step1); }   // (a quiet object: sqHead)
        });
        if (back && g_dynSeekCheck)
            std::printf("dynseek check: %lld backward seeks searched and walked, %lld differ\n", g_dynSeekChecks.load(), g_dynSeekDiff.load());
    }
    // the list `l` and the anchored objects the quiet rule leaves to do (sqGateAnchored), ascending
    const std::vector<uint32_t>* sqWithTodoA(const std::vector<uint32_t>* l) {
        if (!sq.active || sq.todoA.empty()) return l;
        inc.procE.clear();
        std::set_union(l->begin(), l->end(), sq.todoA.begin(), sq.todoA.end(), std::back_inserter(inc.procE));
        return &inc.procE;
    }
    // always + the tick's events, ascending (the two sets are disjoint)
    void incProcList(int t) {
        SqTimer tmP(g_sqStats.nsProc, g_phaseProf);
        incProcList0(t);
        if (sq.active && !sq.todoA.empty()) {
            std::vector<uint32_t>& p = inc.proc;
            std::vector<uint32_t> merged;
            merged.reserve(p.size() + sq.todoA.size());
            std::set_union(p.begin(), p.end(), sq.todoA.begin(), sq.todoA.end(), std::back_inserter(merged));
            p.swap(merged);
        }
        if (inc.procShort) {   // the eligible objects of the placed-every-tick set that are to do (sqShortOk)
            sqTodoAlways(inc.procTodo);
            if (!inc.procTodo.empty()) {
                std::vector<uint32_t>& p = inc.proc;
                std::vector<uint32_t> merged;
                merged.reserve(p.size() + inc.procTodo.size());
                std::set_union(p.begin(), p.end(), inc.procTodo.begin(), inc.procTodo.end(), std::back_inserter(merged));
                p.swap(merged);
            }
            static const bool qchk = std::getenv("QLISTCHECK") != nullptr;
            if (qchk) sqListCheck(inc.proc, "seek", t);
        }
        if (incWithWrote(&inc.proc, t, inc.wBuf) != &inc.proc) inc.proc.swap(inc.wBuf);   // the objects the supply wrote (incSupplyWrote)
    }
    void incProcList0(int t) {
        std::vector<uint32_t>& p = inc.proc;
        p.clear();
        inc.procShort = sqShortOk();
        const std::vector<uint32_t>& alw = inc.procShort ? sq.alwHard : inc.always;
        inc.procCut = inc.procShort ? inc.always.size() - sq.alwHard.size() : 0;
        if (t < inc.evT0 || t > inc.evT1 || inc.evStart.empty()) {
            p = alw;
        } else {
            const uint32_t* a = inc.evObj.data() + inc.evStart[(size_t)(t - inc.evT0)];
            const uint32_t* ae = inc.evObj.data() + inc.evStart[(size_t)(t - inc.evT0) + 1];
            std::merge(alw.begin(), alw.end(), a, ae, std::back_inserter(p));
        }
        if (inc.pSlot.empty()) return;
        // --incanchor: the anchored objects whose changes this tick can reach (and the ones whose dates just changed)
        incPWake(t);
        std::vector<uint32_t>& selOwn = inc.procD;
        const bool shared = sq.active && sq.selOk && sq.selT == t;   // K1: sqGateAnchored has made this list from the same slots a moment ago (nothing in between writes them)
        if (!shared || sq.check) {
            selOwn.clear();
            for (const IncState::PSlot& s : inc.pSlot)
                if (incPActive(s, t)) { selOwn.push_back(s.obj); if (!shared && s.w1 > s.w0 && g_phaseProf && incPSwActive(s, t)) ++g_incStats.pSwListed; }
            if (shared) g_sqStats.selChecked.fetch_add(1, std::memory_order_relaxed);
            if (shared && selOwn != sq.sel) {
                std::printf("k1check: the slots' list sqGateAnchored made for t=%d (%zu) is not the one the seek makes (%zu)\n", t, sq.sel.size(), selOwn.size());
                std::fflush(stdout);
                std::abort();
            }
        }
        const std::vector<uint32_t>& sel = shared ? sq.sel : selOwn;
        ++g_incStats.pTicks;
        g_incStats.pPlaced += (long long)sel.size();
        g_incStats.pSlots += (long long)inc.pSlot.size();
        std::vector<uint32_t> merged;
        merged.reserve(p.size() + sel.size());
        std::merge(p.begin(), p.end(), sel.begin(), sel.end(), std::back_inserter(merged));
        p.swap(merged);
    }
    void seekInc(int t, bool par) {
        incProcList(t);
        inc.procT = t;
        const std::vector<uint32_t>& p = inc.proc;
        ++g_incStats.seekInc;
        g_incStats.procInc += (long long)p.size();
        g_incStats.objsInc += (long long)objs.size();
        forRunsN(par, p.size() + inc.procCut, p.size(), [&](size_t b, size_t e) {   // (the pool is woken as for the list with the whole set: see applyTriggersImpl's nWork)
            for (size_t k = b; k < e; ++k) { if (sq.active && sq.skip[p[k]]) continue; seekOne(p[k], t, false, true); }   // (a quiet object: sqHead)
        });
        inc.lastFull = false;
    }
    // ---- --incplacecheck: the incremental result against the plain one, from the same state ----
    struct IncSnap {
        std::vector<unsigned char> objs;
        std::vector<float> prevCy;
        std::vector<uint8_t> on;
        std::vector<size_t> cur;
        std::vector<double> lockX;
        uint64_t pureEpoch = 0;
        int pureT = 0;
        std::vector<int> pureFire;
    };
    static_assert(std::is_trivially_copyable<Obj>::value, "incplacecheck copies objs bytes");
    void incTake(IncSnap& s) const {
        s.objs.resize(objs.size() * sizeof(Obj));
        if (!objs.empty()) std::memcpy(s.objs.data(), objs.data(), s.objs.size());
        s.prevCy = prevCy; s.on = on; s.cur = cur;
        s.lockX.resize(g_autoTrig.size());
        for (size_t k = 0; k < g_autoTrig.size(); ++k) s.lockX[k] = g_autoTrig[k].lockLastX;
        s.pureEpoch = pureEpoch; s.pureT = pureT; s.pureFire = pureFire;
    }
    void incPut(const IncSnap& s) {
        if (!objs.empty()) std::memcpy(objs.data(), s.objs.data(), s.objs.size());
        prevCy = s.prevCy; on = s.on; cur = s.cur;
        for (size_t k = 0; k < g_autoTrig.size() && k < s.lockX.size(); ++k) g_autoTrig[k].lockLastX = s.lockX[k];
        pureEpoch = s.pureEpoch; pureT = s.pureT; pureFire = s.pureFire;
    }
    // Compare what two runs left (objs bytes, prevCy, on); returns the number of differing objects and prints a few.
    // [R] The comparison is made after applyTriggers. In a call that ran the previous-place pass, groupPrevApply comes next (model.hpp prepareGroup) and writes dcx, dcy and pcy of every
    // non-pure object from its place (compared here) and the pass's (made before both arms): what the two arms hold in those three fields at this point is not what the call leaves. They do
    // differ there for an object at rest the lists leave out: the plain seek writes dcx = cx - (float)cx (seekOne's old place is a float), the lists leave the 0 the last groupPrevApply
    // wrote, and this call's writes 0 over both ([M] a custom level, anchor 3012: uid 41645 from t=15255, dcx -6.4e-05 against 0, in a call with the pass pending, the outputs the same). So in
    // such a call the three fields of a non-pure object are not compared; a call without the pass compares them as before.
    size_t incDiff(const IncSnap& a, const IncSnap& b, const char* what, int t) const {
        size_t bad = 0;
        const size_t S = sizeof(Obj);
        for (size_t i = 0; i < objs.size(); ++i) {
            bool objSame = std::memcmp(&a.objs[i * S], &b.objs[i * S], S) == 0;
            if (!objSame && gpPending && lastStep1 && i < gpCx.size() && !isPure(i)) {
                Obj xa, xb;
                std::memcpy(&xa, &a.objs[i * S], S);
                std::memcpy(&xb, &b.objs[i * S], S);
                xb.dcx = xa.dcx; xb.dcy = xa.dcy; xb.pcy = xa.pcy;
                objSame = std::memcmp(&xa, &xb, S) == 0;
                if (objSame) ++g_incStats.gpStep;
            }
            const bool same = objSame
                              && (i >= a.prevCy.size() || a.prevCy[i] == b.prevCy[i] || (a.prevCy[i] != a.prevCy[i] && b.prevCy[i] != b.prevCy[i]))
                              && (i >= a.on.size() || a.on[i] == b.on[i]);
            if (same) continue;
            if (++bad <= 5) {
                Obj oa, ob;
                std::memcpy(&oa, &a.objs[i * S], S);
                std::memcpy(&ob, &b.objs[i * S], S);
                size_t byteAt = 0;
                for (size_t q = 0; q < S; ++q)
                    if (a.objs[i * S + q] != b.objs[i * S + q]) { byteAt = q; break; }
                std::printf("incplacecheck: MISMATCH %s t=%d obj=%zu uid=%d byte=%zu inc(cx %.4f cy %.4f hw %.4f hh %.4f dcy %.4f "
                            "prevCy %.4f on %d) plain(cx %.4f cy %.4f hw %.4f hh %.4f dcy %.4f prevCy %.4f on %d)\n",
                            what, t, i, oa.uid, byteAt, oa.cx, oa.cy, oa.hw, oa.hh, oa.dcy,
                            i < a.prevCy.size() ? (double)a.prevCy[i] : 0.0, i < a.on.size() ? (int)a.on[i] : -1,
                            ob.cx, ob.cy, ob.hw, ob.hh, ob.dcy, i < b.prevCy.size() ? (double)b.prevCy[i] : 0.0,
                            i < b.on.size() ? (int)b.on[i] : -1);
            }
        }
        return bad;
    }
    // The C / D class objects are only right as a seek-then-applyTriggers pair (applyTriggers overwrites what seek wrote), so
    // with triggers in the level the seek is checked together with the tick's first applyTriggers: seekChecked keeps
    // the state from before the seek and applyTriggers (the wrapper above) compares the two ways of doing both.
    IncSnap chkPre;
    int chkT = INT_MIN;
    void seekChecked(int t, bool par) {
        if (anyTrig || anyAuto) {
            incTake(chkPre);
            chkT = t;
            seekInc(t, par);
            return;
        }
        IncSnap pre, a, b;
        incTake(pre);
        seekInc(t, par);
        incTake(a);
        incPut(pre);
        seekFull(t, false, true, par);
        incTake(b);
        ++g_incStats.checked;
        g_incStats.mismatch += (long long)incDiff(a, b, "seek", t);
        inc.lastFull = false;
    }
    // `atFollows`: applyTriggers is called for this tick right after (prepareGroup does). Only then may a one-tick
    // advance leave objects alone: a C / D object is the closed form's (applyTriggers wrote it) and a plain seek would
    // have put the recording's row on it, so what a seek that no applyTriggers follows leaves is its own and every
    // object takes the plain pass (the other frames' levels, the anchor's first seek).
    void seek(int t, bool atFollows = false) {
        if (objs.empty() || t == lastT) return;
        if (extOk.size() != objs.size()) prepareExtend();
        if (recOk.size() != objs.size()) prepareRecEnd();
        // Every object is about to be rewritten, so whatever applyTriggers placed
        // is stale from here on (see pureEpoch).
        ++seekEpoch;
        if (byBucketN != objs.size()) rebuildBuckets();
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
        // --dynobbrot: keep the loaded axes before the first turn overwrites them (outside the
        // split loop below, which would race on the resize).
        if (g_dynObbRot && obbBc0.size() != objs.size()) {
            obbBc0.resize(objs.size()); obbBs0.resize(objs.size());
            for (size_t i = 0; i < objs.size(); ++i) { obbBc0[i] = objs[i].bc; obbBs0[i] = objs[i].bs; }
        }
        // Each object reads its own recording and writes its own slots, so the
        // loop splits (forRuns) -- unless --dyndbg could print from it.
        const bool par = g_dynDbg == -1 && !anyUidM1;
        inc.procT = INT_MIN;
        if (!incOn()) {
            inc.valid = false;
            incInvalidate();
            seekFull(t, back, step1, par);
            return;
        }
        if (!atFollows) {
            incInvalidate();
            seekFull(t, back, step1, par);
            return;
        }
        if (inc.gen != g_dynPlaceGen || inc.sig != incSig()) {
            inc = IncState();
            inc.gen = g_dynPlaceGen;
            inc.sig = incSig();
        }
        if (!inc.valid) {
            // The index costs a few ms on a big level; a call that runs only a few ticks never builds it.
            inc.runLen = step1 ? inc.runLen + 1 : 0;
            if (inc.runLen < kIncBuildAfter || inc.failed) {
                seekFull(t, back, step1, par);
                return;
            }
            incBuild();
            incInvalidate();
        }
        if (inc.valid && inc.settled && step1) {
            if (g_incCheck) seekChecked(t, par); else seekInc(t, par);
            return;
        }
        seekFull(t, back, step1, par);
        // two full passes in a row, the second a one-tick advance: every object is at the fixed point of its interval
        inc.settled = inc.valid && step1 && inc.lastFull;
        inc.lastFull = true;
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
    // --rotspeed: ...and the SPEED portals, whose pass has the same oriented test.
    // --ringturn (removed): ...and the RINGS, redrawn every tick (constants.hpp): a square, so the half comes out
    // of the bound at any angle, and a Scale may change it while the ring turns. A right angle, or a
    // ring stretched out of square, is tested as its bound, as before.
    // --obbscale: remember the loaded halves of object i before a placement overwrites them (hw / hh become the recording's bound), and the uniform factor the recording's bound has taken
    // since (a Scale trigger shrinks or grows a rect about its centre; a frame turn swaps the bound's halves, which the uniformity test refuses). 1 = no change known.
    void boxBase(size_t i) {
        if (!g_obbScale || i >= baseHw.size() || baseHw[i] >= 0.f) return;
        baseHw[i] = objs[i].hw; baseHh[i] = objs[i].hh; baseOhw[i] = (float)objs[i].ohw; baseOhh[i] = (float)objs[i].ohh;
    }
    double boxScale(size_t i, uint8_t bucketOf, double hwNow, double hhNow) const {
        if (!g_obbScale || (bucketOf != PORT && bucketOf != SPEED) || i >= baseHw.size() || baseHw[i] <= 0.5f || baseHh[i] <= 0.5f) return 1.0;
        const double kw = hwNow / (double)baseHw[i], kh = hhNow / (double)baseHh[i];
        return (kw > 0.05 && std::fabs(kw - kh) <= 0.01) ? kw : 1.0;
    }    static void turnedBox(Obj& o, uint8_t bucketOf, double rotDeg,
                          bool everRot = false, double ohw0 = 0.0, double ohh0 = 0.0, double kScale = 1.0) {
        if (bucketOf == ORB) {
            o.rotTurned = everRot ? 1 : 0;
            // (always on: the switch --ringturn is removed)
            // --ringturn (removed): a RING its group turns is tested as its turned box too (dynamics.hpp turnedBox), redrawn
            // every tick from the recording: the angle, and the size out of the bound, since a Scale can grow it
            // while it turns. A ring is square, so its half is the bound's half over |cos| + |sin| at any angle. The
            // bound was what the ring passes tested, and m_touchedRings' order (updateTouchRings) with it. A custom
            // level: a red ring a Rotate turns through 238..226 degrees while a Scale grows it 1.4x..1.5x sits beside
            // a still red ring. On GD's own positions the bound meets the player at t=711 and the still ring at 717,
            // so the model fired the turned ring on the press at 733; the turned box meets it at 717 as well, the tie
            // goes to the lower uid, and GD fired the still one at 733 and the turned one at 755.
            if (std::fabs(o.hw - o.hh) > 0.01) return;
            const double om = std::fabs(std::fmod(rotDeg, 90.0));
            if (om <= 0.5 || om >= 89.5) {
                o.oriented = 0;
                return;
            }
            const double th = rotDeg * 3.14159265358979 / 180.0;
            const double k = std::fabs(std::cos(th)) + std::fabs(std::sin(th));
            o.rot = rotDeg;
            o.oriented = 1;
            o.ohw = o.ohh = o.hw / k;
            o.rc = std::cos(-th); o.rs = std::sin(-th);
            return;
        }
        if (bucketOf == PAD) {
            // (always on: the switch --padturn is removed)
            // --padturn (removed): a PAD its group turns off a right angle is tested as its turned box (dynamics.hpp
            // turnedBox), as GD's collision pass does for an oriented object: the bounding rect AND the two-way SAT of the
            // player's oriented box with the pad's. The loader sets `oriented` from the placed angle only, so a pad placed at
            // a right angle (rot 180) and nudged a fraction of a degree by a Rotate was tested as its bound. A custom level
            // t=8,775: a flipped cube meets a pink pad hung under a moving platform (placed rot -180, turned to -179.29, 25 x 5);
            // the model fires it on the first tick the bound meets the cube's square and GD one tick later, when the cube's
            // turned box reaches the slab -- GD's own state put into the game at the tick before (y raised by 1, 2, 3 or 4)
            // fires it at 8,775, left as it was at 8,776, lowered by 1 at 8,777: the SAT of the two oriented boxes
            // (side 30.5 or more, the player's angle clockwise) and not an axis-aligned test at any height. The nominal
            // size comes back out of the bound and the angle, as for the portals.
            if (o.oriented) {   // placed off a right angle: only the angle is redrawn
                o.rot = rotDeg;
                const double thp = rotDeg * 3.14159265358979 / 180.0;
                o.rc = std::cos(-thp); o.rs = std::sin(-thp);
                return;
            }
            const double omp = std::fabs(std::fmod(rotDeg, 90.0));
            if (omp <= 0.01 || omp >= 89.99) return;   // a right angle: the bound is the shape
            const double thp = rotDeg * 3.14159265358979 / 180.0;
            const double cp = std::fabs(std::cos(thp)), sp = std::fabs(std::sin(thp));
            const double detp = cp * cp - sp * sp;
            if (std::fabs(detp) < 0.1) return;
            const double Wp = ((double)o.hw * cp - (double)o.hh * sp) / detp;
            const double Hp = ((double)o.hh * cp - (double)o.hw * sp) / detp;
            if (Wp <= 0.5 || Hp <= 0.5) return;
            o.rot = rotDeg;
            o.oriented = 1;
            o.ohw = Wp; o.ohh = Hp;
            o.rc = std::cos(-thp); o.rs = std::sin(-thp);
            return;
        }
        // (always on: the switch --rotspeed is removed)
        // --rotspeed: a SPEED portal its group turns is tested as the turned box too (dynamics.hpp,
        // turnedBox), as the mode and gravity portals are. The recording's w/h are the turned box's
        // axis-aligned bound, and the speed pass tested that bound. Measured on a custom level: a 2x
        // portal (51x56) turning through 331-333 degrees records a 71.7x73.7 bound; GD switches the speed
        // at x 43.3 from its centre and not at 45.2, the bound's test at 49.1 -- three ticks early, every
        // attempt through it. On by default since 2026-10-06; --no-rotspeed is the off arm.
        // A turned HAZARD: GD builds its box (GameObject::updateOrientedBox, OBB2D) from the object's own size times its scale and
        // from getRotation(), every time the box is dirty -- not from the bound -- so the angle the rows carry is the box's angle.
        // This function left it at the angle the call's objects were dumped at (not a PORT / SPEED). a custom level, hazards uid 16075 / 16076
        // (id 3610): a Scale turns them from -49 at the anchor to -46.116 at t=14815 (the box's size: shadow_step.hpp). Only with an objrects that has
        // the start scale (gdev::g_objrectsStartScale: the box is then scaled too); an older one stays as it was.
        if (bucketOf == NEAR && o.type == 2 && o.oriented && o.radius <= 0.0 && everRot && gdev::g_objrectsStartScale) {
            const double th = rotDeg * 3.14159265358979 / 180.0;
            o.rot = rotDeg;
            o.rc = std::cos(-th); o.rs = std::sin(-th);
            return;
        }
        if (bucketOf != PORT && bucketOf != SPEED) return;
        o.rot = rotDeg;
        // [2026-08-18] **Do not throw away the real box built from w0/h0 at
        // load.** This used to drop oriented to 0 on every seek and rebuild it
        // by inverting the bounding box, and since the inversion does not work
        // near 45 degrees (|cos2ﾎｸ| < 0.1), grouped portals alone were **firing
        // on the bounding box**. Measured on lv21 uid24194 (id111 UFO portal,
        // rot=43, w0/h0 = 34x86, bound 167x172): GD switches at t=18,087
        // (x=23,929), but the model did so at the bound's left edge at t=18,075
        // (x=23,910) -- 12 ticks / 19px early. Turning **does not change the
        // box's size**, so only the angle needs to be redrawn.
        if (o.oriented) {
            if (g_obbScale && ohw0 > 0.0 && ohh0 > 0.0) { o.ohw = ohw0 * kScale; o.ohh = ohh0 * kScale; }   // --obbscale: a Scale trigger has resized the box (see boxScale)
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
        if (om <= 0.5 && everRot && o.hw > 0.5 && o.hh > 0.5) {
            o.oriented = 1;
            o.ohw = o.hw; o.ohh = o.hh;   // axis aligned: the bound IS the box
            // ...but ohw/ohh are the UNTURNED box's halves, and at an odd quarter turn (`om`, the
            // angle mod 90, is near 0 at 90 and 270 degrees too) the bound's halves are that box's
            // swapped. Kept unswapped, the box was turned twice -- here and on every redraw (the
            // oriented branch above turns it by the whole angle). On custom level C the
            // gravity portal uid 28482 (id 11, 25x75 at scale 0.5, rot 90, group 1) is 37.5 x 12.5
            // in GD's getObjectRect, and GD flips both bodies on a 0.7-1.1 px corner contact at
            // x~5,075, y~905 (t=3,910/3,912); the model tested 12.5 x 37.5, missed it, and every
            // plan through there died at t~4,024 while the recorder patched vy tick by tick.
            // --no-rotport tests the bound as an AABB and matches GD there; so does this swap.
            if (((long long)std::llround(rotDeg / 90.0)) & 1)
                std::swap(o.ohw, o.ohh);
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
            for (DynSample& s : samples[i].mut()) {
                double u, v;
                toFrame(f, (double)s.cx, (double)s.cy, u, v);
                s.cx = (float)u; s.cy = (float)v;
                if (f & 1) std::swap(s.hw, s.hh);
            }
            turnObj(objs[i], f);
            baseCy[i] = objs[i].cy;
            if (i < baseTpY.size()) baseTpY[i] = objs[i].tpY;
            baseSy0[i] = (float)objs[i].sy0;
            baseSy1[i] = (float)objs[i].sy1;
            rotVec(trigDx[i], trigDy[i]);
            rotVec(autoDx[i], autoDy[i]);
            for (AutoPart& p : autoParts[i]) rotVec(p.dx, p.dy);
            for (AutoPart& p : touchParts[i]) {
                rotVec(p.dx, p.dy);
                // --movetarget: the destination turns with the frame, and in an
                // odd frame a one-axis move is on the other axis.
                rotVec(p.tdx, p.tdy);
                if ((f & 1) && (p.tmode == 2 || p.tmode == 3))
                    p.tmode = (uint8_t)(5 - p.tmode);
            }
        }
        std::fill(cur.begin(), cur.end(), (size_t)0);
        lastT = -1;
        ++seekEpoch;
    }

    // `sup` (prepareGroup, frame 0 only): the supplied movers whose shadow place is in the group's window. The ones this bucket holds that the model's own place left out of [x0, x1] are added at the end,
    // where the shadow has them -- supplyPlaceMovers then places them. Objects already collected by their own place are not touched, so with no hits the list is exactly what it was.
    void collect(uint8_t b, double x0, double x1,
                 std::vector<const Obj*>& out, const SupHits* sup = nullptr) const {
        SqTimer tmC(g_sqStats.nsCollect, g_phaseProf);
        // Same objects, same order, fewer visits (see byBucket). --dyndbg prints
        // one line per object of every bucket, so it keeps the full walk.
        if (g_dynDbg < 0 && byBucketN == objs.size() && b < byBucket.size()) {
            // --pressfresh (Obj::pendOff); --holdtap: the state decides (State::tog, in the shadow's tick)
            auto keeps = [&](uint32_t i) { if (!on[i] && !objs[i].pendOff && !(objs[i].holdTapX > 0.f)) return false; const Obj& o = objs[i]; return !(o.cx + o.hw < x0 || o.cx - o.hw > x1); };
            if (sqIxUsable(b)) {   // the stable quiet objects by their cell (sqIx*), the rest one by one; the same tests, the scan's order
                static const bool chk = std::getenv("COLLECTCHECK") != nullptr;   // read once
                thread_local std::vector<uint32_t> got;
                got.clear();
                long long looked = 0;
                const Quiet::Ix& X = sq.ix;
                if (b < X.cell.size()) {
                    if (!X.cell[b].empty())
                        for (int c = sqIxCellOf(x0 - kIxHw), c1 = sqIxCellOf(x1 + kIxHw); c <= c1; ++c) for (const uint32_t i : X.cell[b][(size_t)c]) { ++looked; if (keeps(i)) got.push_back(i); }
                    for (const uint32_t i : X.wide[b]) { ++looked; if (keeps(i)) got.push_back(i); }
                }
                for (const uint32_t k : sq.todo) { const uint32_t i = sq.s[k]; if (bucket[i] != b) continue; ++looked; if (keeps(i)) got.push_back(i); }
                for (const uint32_t i : X.hard[b]) { ++looked; if (keeps(i)) got.push_back(i); }
                std::sort(got.begin(), got.end());
                g_sqStats.colIx.fetch_add(1, std::memory_order_relaxed); g_sqStats.colLooked.fetch_add(looked, std::memory_order_relaxed);
                if (chk) {
                    size_t a = 0; bool same = true;
                    for (const uint32_t i : byBucket[b]) { if (!keeps(i)) continue; if (a >= got.size() || got[a] != i) { same = false; break; } ++a; }
                    if (same && a != got.size()) same = false;
                    g_sqStats.colChecked.fetch_add(1, std::memory_order_relaxed);
                    if (!same) {
                        std::printf("collectcheck: MISMATCH t=%d bucket %d window %.3f..%.3f: the index gives %zu objects, the scan differs at place %zu\n", lastT, (int)b, x0, x1, got.size(), a);
                        std::fflush(stdout);
                        std::abort();
                    }
                }
                for (const uint32_t i : got) out.push_back(&objs[i]);
            } else {
                g_sqStats.colScan.fetch_add(1, std::memory_order_relaxed);
                for (const uint32_t i : byBucket[b]) if (keeps(i)) out.push_back(&objs[i]);
            }
            if (sup)
                for (const auto& h : *sup) {
                    const auto it = uidIdx.find(h.first);
                    if (it == uidIdx.end()) continue;
                    const uint32_t i = it->second;
                    if (bucket[i] != b) continue;
                    if (!on[i] && !objs[i].pendOff && !(objs[i].holdTapX > 0.f)) continue;
                    const Obj& o = objs[i];
                    if (!(o.cx + o.hw < x0 || o.cx - o.hw > x1)) continue;   // collected above by its own place
                    if (h.second + o.hw < x0 || h.second - o.hw > x1) continue;
                    out.push_back(&o);
                    ++g_supHitAdded;
                }
            return;
        }
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
            if (bucket[i] != b || (!on[i] && !o.pendOff && !(o.holdTapX > 0.f))) continue;
            if (o.cx + o.hw < x0 || o.cx - o.hw > x1) continue;
            out.push_back(&o);
        }
    }
};

}  // namespace dp


