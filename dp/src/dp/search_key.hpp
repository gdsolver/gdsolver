#pragma once
#include <bit>
#include "dp/thread_pool.hpp"

namespace dp {

// Suffix maximum of every surface top at or ahead of an x bucket. A player that
// is airborne, upside down, and already above everything left in the level can
// never land again -- it falls upward forever. That is not a death in GD, so
// nothing pruned it and the frontier filled up with escapees: on lv9 all 56
// surviving states at t=7370 were at y=511..697 climbing out, while the states
// in the actual corridor had died. g_yBound only caught them 120 px above the
// level, far too late to keep the corridor branches alive.
inline std::vector<double> g_topAhead;   // per 30 px bucket
inline double g_bucketX0 = 0.0;
inline double topAheadAt(double x) {
    if (g_topAhead.empty()) return 1e9;
    long long i = (long long)((x - g_bucketX0) / 30.0);
    if (i < 0) i = 0;
    if (i >= (long long)g_topAhead.size()) return -1e9;
    return g_topAhead[(size_t)i];
}
inline std::vector<double> g_topAheadDyn;   // per 30 px bucket, on g_topAhead's grid
inline double topAheadDynAt(double x) {
    if (g_topAheadDyn.empty()) return -1e18;
    long long i = (long long)((x - g_bucketX0) / 30.0);
    if (i < 0) i = 0;
    if (i >= (long long)g_topAheadDyn.size()) return -1e18;
    return g_topAheadDyn[(size_t)i];
}
// Rotated-frame version. frame 0 is the same as above. The right thing is to
// DO THE SAME TEST IN THE FRAME'S COORDINATES, not to disable it: disabled, the
// "can never come back" branches were left unpruned and squeezed the frontier
// at lv22's cold entry (measured 2026-08-15).
inline double topAheadAtF(int f, double x) {
    if ((f & 3) == 0) return topAheadAt(x);
    const auto& slot = g_frameLv[(size_t)(f & 3)];
    if (!slot || slot->topAhead.empty()) return 1e9;
    long long i = (long long)((x - slot->bucketX0) / 30.0);
    if (i < 0) i = 0;
    if (i >= (long long)slot->topAhead.size()) return -1e9;
    return slot->topAhead[(size_t)i];
}
// For the --slopedbg `escapee:` print: the bucket topAheadAtF reads and the
// table's length (i >= n: it answered -1e9 from past the end; n == 0: no table).
inline void topAheadIndex(int f, double x, long long& i, long long& n) {
    const std::vector<double>* t = nullptr;
    double x0 = 0.0;
    if ((f & 3) == 0) {
        t = &g_topAhead;
        x0 = g_bucketX0;
    } else {
        const auto& slot = g_frameLv[(size_t)(f & 3)];
        if (slot) { t = &slot->topAhead; x0 = slot->bucketX0; }
    }
    n = t ? (long long)t->size() : 0;
    i = (t && n) ? std::max(0LL, (long long)((x - x0) / 30.0)) : -1;
}
// ship dedupe granularity (cube is always 0.5px / 0.1). Ship layers are the
// only ones that saturate the cap, so these two knobs set the runtime.
inline double g_shipYq = 0.5;   // multiplier: 0.5 -> 2px bins
inline double g_shipVq = 2.5;   // multiplier: 2.5 -> 0.4 bins
// cube/ball dedupe granularity. Fixed at 2.0 / 10.0 (0.5 px, 0.1 vy) until lv9
// turned up a 7 px gap the frontier could not thread -- worth being able to
// sharpen it there without paying for it on every level.
inline double g_cubeYq = 2.0;
inline double g_cubeVq = 10.0;
// Dedupe multiplier for a dual's second body while it is in open air (see
// stepBoth / keyOf). 0.125 = 4 px / 0.8 vy bins instead of 0.5 px / 0.1.
inline double g_dualFreeQ = 0.125;
// --keyxq / --keyxqfly (default 4.0 = the 0.25 px x bucket): the x-phase bucket multiplier for
// the ground modes (cube, ball, robot, spider) and for the flying ones (ship, UFO, wave, swing).
// The x term keeps the snap phases apart (see keyOf); --searchcensus measured it as the largest
// single divider of the frontier (56% of cube cells, 59% of ship cells).
inline constexpr double kDefKeyXq = 4.0;
inline double g_keyXq = kDefKeyXq;
inline double g_keyXqFly = kDefKeyXq;

// TRIED AND REVERTED (2026-08-03): one bit per nearby portal in the key,
// saying "is this state lined up to fire it right now" (the step's own y-box
// test plus "would it change anything"). The proposal came from lv18's
// x=20,386.6 (70% of the level), where the plan sits at y=321.000 for 47 ticks
// with the player's bottom at 312.00 against a RegularSize portal whose top
// edge is 311.99 -- it misses the portal by 0.01 px and stays mini. The theory
// was that the two worlds (touch / miss) are both still mini when they split,
// differ by 0.01 px in y, and land in the same 2 px ship bin, so the dedupe
// kills the branch before `mini` can ever tell them apart.
// MEASURED, and the theory is wrong: the branch is NOT being killed. At the
// anchor t=14,200 (x=20,183, from the driver's own GD dump) the frontier at
// t=14,500 already holds 416 states that took the portal stack WITHOUT the
// bits; with them it holds 312, and both builds report SOLVED to x=29,151 from
// there. Cold from t=0 both builds die at the same tick and the same x
// (t=18,330, x=24,721.0) and the arena grows 0.25%.
// (The 17-level cold regression it was run against came back 16/17 with lv9
// STUCK at x=21,225, but that is NOT this change's doing -- lv9 fails at the
// same x on the build without the bits. It is a pre-existing break.)
// Reverted for having no measured benefit, on the same grounds as the
// `s.action` experiment recorded at the end of the key below. What actually
// happens at that portal: the touching branch exists, the driver just never
// re-anchors far enough back to take it.

// GD's `upsideDown` for a state whose `flip` is the model's frame-local sign.
// `toFrame` gives frame 1 v = +X and frame 3 v = -X, so frame 3 is the mirror.
// Measured on lv22 with the trace's own flip/frame columns against GD's dump:
// the two agree everywhere up to t=4,664 and are opposite for the whole frame-3
// section. Only the places that read or write GD's own statement -- the gravity
// portals -- convert; the physics keeps using `flip`.
inline uint8_t gdUpOf(const State& s) {
    return (s.frame == 3) ? (uint8_t)!s.flip : s.flip;
}

// A HELD state near a toggle block it has not fired (g_pressWin, triggers.hpp). Frame 0 only:
// the windows are frame-0 x ranges, and the one block that needs this (SubZero 4003's uid 4001)
// sits in frame 0.
inline bool nearPressBox(const State& s) {
    if (g_pressWin.empty() || s.frame != 0) return false;
    const double x = (double)s.xAbs;
    for (const PressWin& w : g_pressWin)
        if (x >= w.lo && x <= w.hi && !s.trig.test(w.bit)) return true;
    return false;
}
// ...and the key term for it. Inside a block's window, a held state that has not fired it and a
// state that HAS are each their own node: the second matters as much as the first, because the
// tick a held child enters the block its sibling that let go does not, and the layer is only
// partitioned by `trig` from the NEXT tick -- within the tick both share the parent's dedupe, and
// the one that fired used to merge into the one that did not (measured on 4003: 1,003 entries at
// t=7,750, not one of them alive at t=7,752). 0 for a released state that has not fired it, so
// those keys stay bit-identical.
inline uint64_t pressKey(const State& s) {
    if (g_pressWin.empty() || s.frame != 0) return 0;
    const double x = (double)s.xAbs;
    uint64_t k = 0;
    for (const PressWin& w : g_pressWin) {
        if (x < w.lo || x > w.hi) continue;
        if (s.trig.test(w.bit)) k ^= 0x5851F42D4C957F2Dull * (uint64_t)(w.bit + 1);
        else if (s.action) k ^= 0xE7037ED1A0B428DBull * (uint64_t)(w.bit + 1);
    }
    return k;
}

// `t` is the tick the state belongs to. It is a parameter rather than a field
// because dedupe runs per tick layer, so every state compared against another
// shares it -- the key stays consistent within the layer, which is the only
// place it is ever used. Carrying it in the state instead would add a field
// that has to be maintained every tick and that can only ever hold the layer's
// own t: redundant, and a second place for the key's meaning to live.
//
// WHOEVER STORES A KEY MUST STORE ITS TICK. refwatch keeps a reference key
// across ticks; comparing it against a key built at a different t silently
// fails to match and reads as "the reference left the frontier" when nothing
// left it. The two call sites there rebuild the reference key at the tick they
// compare at, for that reason.
// Whether the flight band clamps this state: the modes whose clamps read it (step.hpp's
// ship/UFO/swing, ball/spider and wave branches), for either body of a dual. A cube and a robot
// never read it, and GD's pmin/pmax there are camera state that carries the run's history, so two
// flights of one plan record different values (SubZero 4001, 2026-09-26). The loop records the band
// only around these modes (repair.hpp) and the key and the cap classes use it only here.
inline bool bandClamps(const State& s) {
    auto m = [](int k) { return k == 1 || k == 2 || k == 3 || k == 4 || k == 6 || k == 7; };
    return m(s.mode) || (s.dual && m(s.mode2));
}
// The model's dedupe key of a state (everything but the route's part), and the route's part under
// --coins. A search keys a state by modelKeyOf ^ its rules' keyBits (search_dp.hpp CurrentRules,
// which takes coinKeyBits); keyOf is the two together, for what keys a state outside a search
// (the census, the dumps). Every term is xor-ed in, so the split changes no key.
// --coins: the collected set (State::coins). Same argument as the trigger mask -- a state that took
// the coin and one that flew through the same cell without it answer the rest of the level
// differently, and only one of them can ever reach the goal. Gated on non-zero so every run without
// the flag keeps a bit-identical key; with it, only the cells inside a coin's window ever split. Its
// own multiplier: with the arm bit's, coins == 1 and an armed state would cancel to the key of
// neither.
inline uint64_t coinKeyBits(const State& s) {
    return s.coins ? ((uint64_t)s.coins * 0x2545F4914F6CDD1Dull) : 0;
}
inline uint64_t modelKeyOf(const State& s, long long t) {
    // dedupe key: exact enough to keep distinct behaviours, coarse enough to
    // saturate. cube y 0.5px / vy 0.1; ship coarser (y 1px / vy 0.2) or the
    // per-layer set is ~100k cells and the run needs >10GB
    // The coarse grid belongs to the FLYING modes, not to the ship alone. This
    // used to test `mode == 1`, so the UFO (3) got the CUBE's bins -- 0.5 px and
    // 0.1 vy against the ship's 4 px and 1.0, i.e. 80x the cells for a mode that
    // covers the same continuous y range for the same reason. Same bug class as
    // the dual's second body below, and it sits on lv16's critical path: both of
    // its late walls (x=17,940 and x=18,087) are `mode=3`.
    // Ball (2) is deliberately NOT here: it is a ground mode and the cube's bins
    // are the right ones for it. Wave (4) is left out too -- its vy only ever
    // takes +-4*dx, so the vy bins cost it nothing, and lv17 already clears.
    const bool flying = (s.mode == 1 || s.mode == 3);
    const double ys = flying ? g_shipYq : g_cubeYq;
    const double vs = flying ? g_shipVq : g_cubeVq;
    const int32_t yq = (int32_t)std::lround(s.y * ys);
    const int32_t vq = (int32_t)std::lround(s.vy * vs);
    // `flip` MUST be in the key. It was missing, so a player resting on a floor
    // and one resting on a ceiling at the same y with vy = 0 hashed to the same
    // cell and the dedupe threw one of them away. Harmless-looking for the cube
    // (its two orientations rarely share a y) but fatal for the ball, whose ONLY
    // action is to flip: measured on lv10 t=2000, 112 children were born, ZERO
    // died, and 56 were merged away -- the frontier collapsed 56 -> 1 purely
    // through this collision, and the search then had a single forced route.
    // `mini` is in the key for the same reason as `flip`: two players at the
    // same y and vy but different sizes stand on different surfaces and die to
    // different boxes, so they are not interchangeable.
    // xAbs MUST be in the key too. Two states at the same y and vy but a
    // different x PHASE are not interchangeable: x only ever differs by whole
    // stair snaps (+0.3 .. +1.0 px), and one of those decides whether a landing
    // falls on tick n or n+1 -- which decides everything downstream.
    // Measured on lv11 x=24,863: the level is passable only by landing at
    // 24863.4 and jumping on the very next tick (clearing the spike at 24,885 by
    // 1.5 px). GD's own phase puts that landing 0.6 px later and the jump then
    // misses. The search had BOTH phases available and collapsed them into one,
    // so it never had the choice. The bucket is 0.25 px, far finer than a snap
    // and far coarser than the float noise, so it costs almost no extra states.
    // (--keyxq / --keyxqfly; both 4.0 unless given, which is this expression exactly)
    const bool flyX = (s.mode == 1 || s.mode == 3 || s.mode == 4 || s.mode == 7);
    const int32_t xq = (int32_t)std::lround(s.xAbs * (flyX ? g_keyXqFly : g_keyXq));
    const bool p2Action = s.action2 == 1 || (s.action2 == 2 && s.action);
    uint64_t k = ((uint64_t)(uint32_t)yq << 32) ^ ((uint64_t)(uint32_t)xq << 18)
           ^ ((uint32_t)vq << 6)
           ^ ((uint64_t)s.mini << 5)
           // ringHold too: two states that differ only in "has this hold
           // already spent its ring" answer the next press differently.
           ^ ((uint64_t)s.ringHold << 4)
           // ...and `pressSpent`, which is the same question for EVERY consumer
           // rather than rings alone, so two states that agree on ringHold can
           // still answer the next ring differently (a grounded jump spent the
           // press without touching a ring). Mixed like mode2/mini2 rather than
           // taking a bit: gated on the difference, so every key where the two
           // agree -- all of them until a grounded impulse happens under a held
           // button -- stays bit-identical to before.
           ^ ((s.pressSpent != s.ringHold)
                  ? ((uint64_t)s.pressSpent * 0x9E3779B97F4A7C15ull) : 0)
           // In a dual, the second half is part of the state: two states that
           // agree on the first player but not the second are not the same
           // node.
           // Its bins are the SAME ones the first half gets -- `ys`/`vs`, which
           // depend on the mode. They used to be hardcoded to the CUBE's
           // (g_cubeYq / g_cubeVq), so in a dual SHIP section the passenger was
           // keyed 8x finer in y and 10x finer in vy than the pilot (the driver
           // runs ships at 0.25/1.0 against the cube's 2.0/10.0). An 80x finer
           // grid on the body that is not being steered is exactly what makes
           // the frontier the product of two state sets and pins it at the cap.
           // lv16's wall at x=13,295 sits inside a dual SHIP section.
           // ...times a further factor while that body is in open air. The two
           // halves share the input, so a drifting half carries no decision
           // (see stepBoth). It is still in the key -- the body comes back and
           // its position matters again -- only the bins are wider.
           ^ (s.dual ? ((uint64_t)(uint32_t)(int32_t)std::lround(
                            s.y2 * (s.freeHalf ? ys * g_dualFreeQ : ys)) << 40)
                     : 0)
           ^ (s.dual ? ((uint64_t)(uint32_t)(int32_t)std::lround(
                            s.vy2 * (s.freeHalf ? vs * g_dualFreeQ : vs)) << 24)
                     : 0)
           ^ ((uint64_t)s.dual << 7) ^ ((uint64_t)s.flip2 << 6)
           ^ ((uint64_t)s.mode << 3) ^ ((uint64_t)s.flip << 2)
           // The SECOND BODY'S mode, on the ticks it differs from the first's
           // (State::mode2). Two pairs whose halves are in different modes
           // answer the next tick differently, so they must not merge. Gated on
           // the difference so every key where the pair agrees -- all of them
           // outside the one-tick window a mode portal opens between the halves
           // -- stays bit-identical to before.
           ^ ((s.dual && s.mode2 != s.mode)
                  ? ((uint64_t)s.mode2 * 0xFF51AFD7ED558CCDull) : 0)
           // ...and its SIZE, on the ticks that differs (State::mini2). Same
           // argument as `mini` above -- two bodies of different extents stand
           // on different surfaces and die to different boxes -- and the same
           // gate, so every key where the halves agree stays bit-identical.
           ^ ((s.dual && s.mini2 != s.mini)
                  ? ((uint64_t)s.mini2 * 0xC4CEB9FE1A85EC53ull) : 0)
           // ...and the BAND, for the same reason as flip: two states at the
           // same y and vy that came in through different mode portals are
           // clamped by different ceilings and floors, so they are not
           // interchangeable (lv1's two lanes into its last ship section).
           // Only for a body the band clamps (bandClamps): a cube or a robot never
           // reads it, and the next mode portal sets it again, so two such states
           // with different bands are the same state. Keyed there, the band's
           // camera noise split cells that are one (see bandClamps).
           ^ (bandClamps(s) ? ((uint64_t)(uint32_t)(int32_t)std::lround(s.bandFloor) << 46)
                            : 0)
           // ...and in BRANCH A the band's HEIGHT is animating rather than
           // fixed, so two states at the same y and vy that reached this tick
           // at different points of the tween are clamped by different
           // ceilings -- the same argument as the floor above, and the spread
           // is large (322 against the mode's 270 or 300, before the zoom
           // divides it). What goes in is the resulting numerator in whole
           // pixels, which is the quantity the ceiling is built from and is
           // computable from the state alone.
           // Gated on branch A, so every key outside it stays bit-identical:
           // all of lv1-21, every dump without the Static Camera columns, and
           // lv22 itself wherever the mode holds the band.
           // WHAT THIS COSTS IS NOT MEASURED. A new key dimension fragments
           // the arena, and the only instrument for that is a cold run.
           ^ (((s.bandBranch & 3) == 1 && bandClamps(s))
                  ? ((uint64_t)(uint32_t)(int32_t)std::lround(
                        kBandRetracted
                        - (kBandRetracted - ((double)s.bandCeil
                                             - (double)s.bandFloor))
                          * bandProgress(s.bandAnim))
                     * 0x9E3779B97F4A7C15ull)
                  : 0)
           // TRIED AND REVERTED (2026-08-03): adding `s.action` here, because the
           // cube's jump is an EDGE (`groundedNow && input && !s.action`) and two
           // grounded states differing only in `action` are not interchangeable.
           // It DID add states (arena 57,988 -> 83,068, +43%) but lv18's frontier
           // at the wall was identical (same alive, same y, same death tick), so
           // it costs memory for no reach. Re-try only with a measured benefit.
           // The ROBOT's remaining hover budget is part of the state: two
           // airborne robots at the same y and vy answer "keep holding" with
           // completely different trajectories if one has 60 ticks of float
           // left and the other has none. Without this the dedupe keeps
           // whichever arrived first and the hover is silently unavailable.
           ^ ((uint64_t)s.rHover << 56)
           // ...and while that budget is live, whether the button is STILL
           // DOWN is part of the state too. The robot's hover reads the
           // previous tick's input (GD runs buttons after the update), so the
           // held child and the released child of the same jump have identical
           // y and vy on the tick they are born -- the dedupe kept exactly one,
           // the released one, and the hover stopped after a single tick.
           // Measured on lv19 t=1,522: both children come out (166.258, 5.590),
           // the next tick is (167.472, 5.396) for both, and the frontier tops
           // out at y=184 -- the plain ballistic apex of a 5.59 jump. The robot
           // could never climb the 30 px onto the walkway at x=2,148 and every
           // cold run died at x=2,187.
           // Gated on rHover so that every other mode's key is bit-identical
           // (adding `action` unconditionally was tried in 2026-08-03 and cost
           // 43% more states for no reach).
           // ...and the same for a DASH, which is held in exactly the same way
           ^ ((uint64_t)s.dashing << 54)
           ^ ((uint64_t)((s.rHover || s.dashing) ? s.action : 0) << 55)
           ^ ((s.dashing && s.dashSlope != 0.f)
                  ? (uint64_t)std::bit_cast<uint32_t>(s.dashSlope) * 0xA0761D6478BD642Full : 0)
           ^ ((s.dual && s.rHover2)
                  ? (((uint64_t)s.rHover2 << 1) | (s.mode2 == 5 && p2Action))
                        * 0x8CB92BA72F3D8DD7ull : 0)
           // --dualdash: ...and the second body's dash, which its own step now keeps (0 off)
           ^ ((s.dual && s.dashing2)
                  ? (0x5B2E7A1D93C4F086ull ^ ((uint64_t)p2Action << 31)) : 0)
           ^ ((s.dual && s.dashing2 && s.dashSlope2 != 0.f)
                  ? (uint64_t)std::bit_cast<uint32_t>(s.dashSlope2) * 0xE7037ED1A0B428DBull : 0)
           // --p2born: ...and a second body born holding nothing whose plan level has not yet shown a release
           ^ ((g_p2Born && s.dual && s.action2 == 3) ? 0x6E3B95C1A7D204F8ull : 0)
           // --dualinlat: ...and in a dual whose second body takes the button a tick after the first
           // (ship or UFO against anything else), the two buttons it has just seen are part of the state
           ^ ((g_dualInLat && s.dual && (s.mode2 == 1 || s.mode2 == 3) && !(s.mode == 1 || s.mode == 3))
                  ? (0x3C79AC492BA7B653ull ^ ((uint64_t)s.action << 29)
                     ^ ((uint64_t)p2Action << 30))
                  : 0)
           // --revcarry: ...and the correction a reverse ring still owes (State::revOff), in 0.25 px buckets: two states alike
           // in everything else run to different x. 0 for every state that never met such a ring, so those keys stay as they were.
           ^ (s.revOff != 0.f
                  ? ((uint64_t)(uint32_t)(int32_t)std::lround((double)s.revOff * 4.0) * 0xA24BAED4963EE407ull)
                  : 0)
           // --p2xoff: ...and how far the second body runs ahead of the first (State::dx2), in
           // 0.25 px buckets. Two pairs alike in everything else meet different objects with
           // their second body. 0 without the flag and in every pair that never separated, so
           // those keys stay bit-identical.
           ^ ((s.dual && s.dx2 != 0.f)
                  ? ((uint64_t)(uint32_t)(int32_t)std::lround((double)s.dx2 * 4.0)
                     * 0xD6E8FEB86659FD93ull)
                  : 0)
           // ...and near a TOGGLE BLOCK it has not fired (g_pressWin), for the
           // same reason: the block fires on the parent's button, so the held
           // child and the released one of an airborne cube -- same y, same vy
           // -- answer the next tick differently, and without this the dedupe
           // kept whichever came first, which is the released one. Measured on
           // SubZero 4003 (2026-09-24): from t=6,958 the search passed the block
           // at uid 4001 with no state ever entering it, while the same route
           // with the button held through it enters it at t=7,762. Every key
           // away from such a block, and every released key near one that has
           // not fired it, stays bit-identical (see pressKey).
           ^ pressKey(s)
           // --airpress: a cube's press buffer (see g_airPress), in the air AND
           // on the ground: a cube standing with the button held jumps on the
           // next tick's physics pass, one that let go needs a fresh press (the
           // rising-edge pass), so the two are different futures either way.
           // Only a set buffer changes the key, and only under the flag.
           ^ ((g_airPress && s.mode == 0 && s.jumpBuf) ? 0x6C62272E07BB0142ull : 0)
           // Slope ride counter (see State::slopeT): two riders at the same
           // (x, y, vy) that landed on the ride at different ticks launch with
           // different exit impulses, so they are not interchangeable. Gated
           // on onSlope so every non-slope key stays bit-identical; the value
           // saturates at 24, so this costs at most 25 cells per riding cell
           // (in practice riders at the same x almost always share it).
           ^ (s.onSlope ? ((uint64_t)s.slopeT << 47) : 0)
           ^ ((s.dual && s.onSlope2) ? ((uint64_t)s.slopeT2 << 12) : 0)
           // ...and whether the ride LANDED, for the same reason the counter is
           // here: it decides whether leaving the ramp launches at all (see
           // State::rideLanded), so two riders alike in (x, y, vy) but unlike in
           // this are not interchangeable. Folding them would let the search
           // plan a launch off a ride that never landed. Gated on onSlope so
           // every non-riding key stays bit-identical, and one bit wide.
           // Carried as a distinct constant rather than a bit position: bit 46
           // already belongs to bandFloor above, and an XOR into occupied bits
           // can cancel, quietly folding two states the key is meant to keep
           // apart. pFlap below uses the same idiom for the same reason.
           ^ ((s.onSlope && s.rideLanded) ? 0xD1B54A32D192ED03ull : 0)
           ^ ((s.dual && s.onSlope2 && s.rideLanded2)
                  ? 0xA24BAED4963EE407ull : 0)
           // The flap buffered on the portal's tick (State::pFlap). Same (y,vy)
           // but different behaviour next tick, so it goes in the key.
           ^ (s.pFlap ? 0x9E3779B97F4A7C15ull : 0)
           // ...and the spider's handed-over press (State::pSpiderTap), same reason
           ^ (s.pSpiderTap ? 0xC2B2AE3D27D4EB4Full : 0)
           // [D9, REMOVED 2026-09-03] `pBallOff` had a term here for the same
           // reason pFlap does. The rule it keyed is gone (see State), and
           // dropping the term is bit-identical for every state that ever
           // existed: it contributed only when the flag was set, and the flag
           // was never set anywhere in the 22 verified solutions.
           // [r102] one-shot skip of the terminal clamp (State::pNoTerm)
           ^ (s.pNoTerm ? 0x94D049BB133111EBull : 0)
           // [2026-08-25] GD's velocity-limit exemption (State::boost). Same
           // (y,vy), but whether the swing's terminal clamp applies differs
           // (a boosted state passes 8 and keeps accelerating; an unboosted
           // one sits pinned there).
           ^ (s.boost ? 0xA0761D6478BD642Full : 0)
           // ...and the second body's (State::boost2, per-half since
           // 2026-09-06). Gated on `dual` so that every single-player key is
           // bit-identical; a dual pair whose halves differ in the exemption
           // -- one of them off a ramp, the other not -- is two different
           // world lines from the next tick onward. The corpus reaches it on
           // lv16 t=8,913 only, where BOTH halves carry it, so the term costs
           // that level nothing and every other level zero.
           ^ ((s.dual && s.boost2) ? 0x7B7D159C79E2A32Full : 0)
           // --a1clatch: GD's +0xa1c (State::a1cLatch). Same (y,vy), but whether the next
           // ramp exit launches or releases differs. 0 without the flag, so every key is
           // bit-identical there; the second body's bit only counts in a dual.
           ^ ((s.a1cLatch & 1u) ? 0x6A09E667F3BCC909ull : 0)
           ^ ((s.dual && (s.a1cLatch & 2u)) ? 0xBB67AE8584CAA73Bull : 0)
           // --ballscaleair: GD's +0x728 (State::ball728). Same (y,vy), but whether a size portal
           // re-stakes the ball's spin differs. 0 without the flag; the second body's only in a dual.
           ^ ((uint64_t)(s.ball728 & 3u) * 0xA54FF53A5F1D36F1ull)
           ^ (s.dual ? (uint64_t)((s.ball728 >> 2) & 3u) * 0x510E527FADE682D1ull : 0)
           // --lawcontact (always on since the 2026-10 slope clean-up): a law seat last tick
           // (State::seatT > 0) makes this tick a continuing contact.
           ^ ((s.seatT > 0) ? 0x3C6EF372FE94F82Bull : 0)
           // [r93] The slope-exit launch of a ride a warp interrupted
           // (State::pExitVy). Same (y,vy), but whether the launch comes out
           // next tick differs.
           ^ (s.pExitVy != 0.f
                  ? (uint64_t)(int64_t)((double)s.pExitVy * 1000.0)
                        * 0xD6E8FEB86659FD93ull
                  : 0)
           // [night 3] The ramp the ride started on (State::slopeUid0). Same
           // (y,vy), but whether the seam clamp fires differs.
           ^ (s.onSlope ? ((uint64_t)(uint32_t)s.slopeUid0 * 0x9E3779B1ull)
                        : 0)
           // [2026-08-20] The ramp currently ridden (State::slopeUidNow). Used
           // for the "do not transfer" gate while held by a flat actual object.
           ^ (s.onSlope ? ((uint64_t)(uint32_t)s.slopeUidNow * 0x85EBCA77ull)
                        : 0)
           // The flip-on-head-hit arm is world state, not position: two cubes
           // at the same (y, vy) answer a ceiling completely differently
           // depending on it, so the dedupe must not merge them. 0 in every
           // level without an id-2866 object, so every existing key is
           // bit-identical.
           // --fgarmlive turns fgArm into GD's countdown (0..kArmTicks), and
           // `<< 63` throws away everything above 1: two armed cubes with 1 and
           // 2 ticks left would get the same key while answering a ceiling two
           // ticks from now differently. So the flag switches the term to a
           // multiply. Without the flag the byte is still 0/1 and the shift is
           // what it always was, so every key is bit-identical.
           // The riding box's renewal uses the same countdown (and reads its fresh value),
           // so the term is always the multiply.
           ^ ((uint64_t)s.fgArm * 0xBF58476D1CE4E5B9ull)
           // --upsidecoyote: a cube whose og is still set can jump from mid-air next tick and
           // one at the same (y, vy) without it cannot. Set only under the flag, so every
           // other key is bit-identical.
           ^ (s.ogLinger ? 0x3C6EF372FE94F82Bull : 0)
           // --holdlatch: a held button a ball's tap has spent does not thrust a ship, and one
           // that is still live does. Set only under the flag.
           ^ (s.holdDead ? 0x1F83D9ABFB41BD6Bull : 0)
           // ...and the id-1859 ceiling arm WOULD be the same kind of world
           // state, for the same reason: two cubes at the same (y, vy) answer
           // the ceiling above them with a bonk or with a death depending on
           // it. The bit is out again with the gate it served (see the note at
           // the bonk gate in step.hpp) -- keying a distinction the physics no
           // longer makes would only fragment the dedupe. It comes back with
           // 004b, in this exact form:
           //     ^ ((s.armT < kArmTicks) ? 0xFF51AFD7ED558CCDull : 0)
           // Only the ARMED/not bit, never the counter -- the two ticks it
           // holds behave alike -- and 0 in every level without an id-1859, so
           // every existing key stays bit-identical.
           // --bonkarm puts the gate back, so it puts the bit back with it.
           ^ ((s.armT < kArmTicks) ? 0xFF51AFD7ED558CCDull : 0)
           // --ridebox: inside a riding 1859's ride the two armed ticks do NOT behave
           // alike -- a fresh arm (armT 0) is renewed on the next tick and a decaying one
           // lapses -- so the fresh value gets its own term. Set only under the flag.
           ^ ((s.armT == 0) ? 0x2545F4914F6CDD1Dull : 0)
           // The spider's teleport age while it still spares a side kill (State::spiderJumpT):
           // two spiders at the same (y, vy) answer a wall inside their box differently for the
           // ticks that are left. 0 outside the window, so every other key is bit-identical.
           ^ ((s.spiderJumpT < spiderJumpGraceTicks())
                  ? ((uint64_t)(s.spiderJumpT + 1) * 0x94D049BB133111EBull) : 0)
           // (The collected coins are the route's part: coinKeyBits.)
           // The pickup items -- but as a COUNT, not as the set. What
           // reads them is a Count trigger, which compares the number; and the
           // pickups are passed in x order, so two states at the same x that
           // took different ones have the same ones left to take. Keying the
           // set instead splits every cell into up to 2^n worlds (lv21 has 11
           // pickups) and the frontier fills with lineages that are the same
           // world -- measured as a fourfold slowdown against the same level
           // without coins.
           ^ (s.items ? ((uint64_t)popCount32(s.items) * 0xD1B54A32D192ED03ull) : 0)
           // ...and the counting tap's presses, a number for the same reason.
           ^ (s.taps ? ((uint64_t)s.taps * 0x9FB21C651E98DF25ull) : 0)
           // --tap2: the other counting taps' presses, the same (0 without the flag)
           ^ (s.tapsX[0] ? ((uint64_t)s.tapsX[0] * 0xA24BAED4963EE407ull) : 0)
           ^ (s.tapsX[1] ? ((uint64_t)s.tapsX[1] * 0x9FB21C651E98DF25ull * 3) : 0)
           ^ (s.tapsX[2] ? ((uint64_t)s.tapsX[2] * 0xD1B54A32D192ED03ull * 5) : 0)
           // [r52] The frame-change bit goes in the key too (it is set only on
           // the tick after the change, so the partition granularity barely
           // moves)
           ^ (s.frameChg ? 0x27D4EB2F165667C5ull : 0)
           // [r66] Non-zero only while pushed. The release vy is a function of
           // ceilT, so it goes in the key
           ^ (s.ceilT ? (0x165667B19E3779F9ull
                         * (uint64_t)(s.ceilT | ((uint32_t)s.ceilM4 << 8)))
                      : 0)
           // `held` is keyed only for a body whose next tick reads it or writes it from the button -- ship, UFO,
           // wave and swing (and every dual) -- not in every mode (formerly --heldkeyread). Only the wave and
           // flight branches write `held` (without --heldall), so in a cube, ball, robot or spider it is
           // whatever the last flight section left, read by nothing, and it split those cells: measured by
           // --searchcensus over the 22 official levels and 7 customs, 15% of cube cells, 21% of ball and 6% of
           // spider cells were kept apart by `held` alone.
           ^ ((s.dual || s.mode == 1 || s.mode == 3 || s.mode == 4
               || s.mode == 7)
                  ? ((uint64_t)s.held << 1) : 0)
           ^ s.grounded;
    if (s.dual) {
        const unsigned press2 = (s.pressSpent2 != s.pressSpent)
                               | ((s.ringHold2 != s.ringHold) << 1)
                               | ((s.holdDead2 != s.holdDead) << 2)
                               | ((g_dualJumpBuf && s.jumpBuf2) << 3);
        k ^= (uint64_t)press2 * 0xD1342543DE82EF95ull;
        if (s.mode2 == 4) {
            const bool held2 = (g_twoPlayer && g_p2Held ? p2Action : s.held != 0)
                               && !(g_waveHoldDead && s.holdDead2);
            if (held2 != (s.held != 0)) k ^= 0x589965CC75374CC3ull;
        }
    }
    if (g_wavePortalRot && g_wavePortalSize) {
        if (s.waveResized && s.mode == 4 && s.vy != 0.f && (s.mini || !g_wavePortalRotMini))
            k ^= 0x3F84D5B5B5470917ull;
        if (s.dual && s.waveResized2 && s.mode2 == 4 && s.vy2 != 0.f && (s.mini2 || !g_wavePortalRotMini))
            k ^= 0x9216D5D98979FB1Bull;
    }
    // Which gravity portals this state has spent (State::portalLatch). Unlike
    // `trig` this is not partitioned into the group -- a spent portal moves no
    // geometry, so both states want the same windows -- and the separation has
    // to happen here or two states that disagree about a portal ahead will
    // merge and the survivor gets the wrong future.
    // Zero until a gravity portal is actually crossed, and once crossed it is
    // usually the SAME mask for every state in the layer: an xor by one
    // constant is a bijection, so those layers keep their dedupe exactly. Only
    // a layer that genuinely disagrees pays anything, which is the case the bit
    // exists for. Levels with no gravity portals keep bit-identical keys.
    // Word 0 is mixed EXACTLY as the uint32 was (the value is the same number while
    // a level has at most 32), so those levels keep bit-identical keys; the higher
    // words are mixed only when non-zero, each under its own rotation.
    if (s.portalLatch.word(0))
        k ^= s.portalLatch.word(0) * 0xFF51AFD7ED558CCDull;
    if (s.portalLatch2.word(0))
        k ^= s.portalLatch2.word(0) * 0xC4CEB9FE1A85EC53ull;
    for (int w = 1; w < GravLatch::kWords; ++w) {
        const unsigned r = (unsigned)(7 * w);
        if (const uint64_t v = s.portalLatch.word(w))
            k ^= ((v * 0xFF51AFD7ED558CCDull) << r) | ((v * 0xFF51AFD7ED558CCDull) >> (64 - r));
        if (const uint64_t v = s.portalLatch2.word(w))
            k ^= ((v * 0xC4CEB9FE1A85EC53ull) << r) | ((v * 0xC4CEB9FE1A85EC53ull) >> (64 - r));
    }
    // Per-box fire ticks, for the boxes whose chain is STILL MOVING.
    //
    // `trig` itself is not in the key -- the layer is partitioned by it, the
    // same way `dx` is -- so two states in the same group already agree on
    // WHICH boxes they have entered. What they can still disagree about is
    // WHEN, and that only matters while the motion a box started is running:
    // once it has come to rest the two are in the same world again and must
    // merge, or the frontier splits forever on a difference that has stopped
    // existing.
    //
    // Quantised to 4 ticks. Within a layer `t` is a constant, so quantising
    // the fire tick and quantising the elapsed time differ by a fixed offset
    // and are the same partition -- the simpler one is used. 4 ticks is the
    // knob if the frontier turns out too wide; move it only against an A/B,
    // because a coarser bucket merges states that are genuinely at different
    // points of the same move.
    //
    // Costs nothing where nothing moves: levels whose touch chains move
    // nothing have g_touchMoveTicks all zero, so the loop adds nothing and
    // their keys stay bit-identical.
    if (s.trig) {
        // kTouchBits, not 32: b indexes a touch BOX, and s.fireB is sized by
        // the constant. At a width of 64 the old cap left boxes 32..63 out of
        // the key's "is this box still moving" term, so two states that differ
        // only in a high box's motion merged.
        const size_t n = std::min<size_t>(g_touchMoveTicks.size(),
                                          (size_t)kTouchBits);
        for (size_t b = 0; b < n; ++b) {
            if (!s.trig.test(b)) continue;
            const long long moving = g_touchMoveTicks[b];
            if (moving <= 0) continue;
            if (t - (long long)s.fireB[b] >= moving) continue;   // at rest
            // --keycensus: WHICH boxes are splitting the frontier. The cost of
            // the touch window lands here and nowhere else -- a box in the
            // window is a box the key divides on for as long as it is moving --
            // so widening to 64 cost lv22 the solve (41 iterations to 201) even
            // though it fixed the coverage. If a few boxes account for the
            // splitting, the selection can be tightened without giving the
            // coverage back. Counted, not inferred.
            if (g_keyCensus) ++g_keyCount[b];
            k ^= ((uint64_t)(s.fireB[b] >> 2) * 0x9E3779B97F4A7C15ull)
                 ^ ((uint64_t)(b + 1) * 0xBF58476D1CE4E5B9ull);
        }
    }
    // --ctrltouch: a branch whose controls a touched Options trigger switched off answers no press,
    // so it is not the one beside it that can still jump (State::ctrlT), and near a box a held button
    // switches off the button is part of the state (nearCtrlHoldBox). 0 keeps every key as it was.
    if (s.ctrlT) k ^= 0x2545F4914F6CDD1Dull;
    if (s.action && nearCtrlHoldBox((double)s.xAbs)) k ^= 0x9FB21C651E98DF25ull;
    // --gravtouch: a branch that fired a touch-triggered gravity trigger falls at another rate (State::gravTouch)
    if (s.gravTouch) k ^= (uint64_t)s.gravTouch * 0xD6E8FEB86659FD93ull;
    // --tptouch: ...and one whose last touched teleport box will not fire again (State::tpTouch)
    if (s.tpTouch) k ^= (uint64_t)s.tpTouch * 0x94D049BB133111EBull;
    // --tpback: ...and one a teleport has left behind its farthest x, whose crossings wait for it (State::tpBack)
    if (s.tpBack) k ^= (uint64_t)s.tpBack * 0xC2B2AE3D27D4EB4Full;
    // --stopjump: a branch whose button a Player Control trigger released presses nothing until the plan lets go
    // (State::pcRel), and one whose last touched box will not fire again (State::pcTouch)
    if (s.pcRel) k ^= 0x6C8E9CF570932BD5ull;
    if (s.pcTouch) k ^= (uint64_t)s.pcTouch * 0xA24BAED4963EE407ull;
    return k;
}
inline uint64_t keyOf(const State& s, long long t) { return modelKeyOf(s, t) ^ coinKeyBits(s); }

// --groupfire (off): the search's speed groups are split by WHEN each box whose chain is still
// moving was entered, not only by which boxes were.
//
// A group places its moving geometry once for every member, from the LATEST fire tick of each
// box (gFireB, cli.hpp). That is conservative for a door, which is less open the later it was
// punched, and the opposite for a box that brings something DOWN onto the player: the member that
// punched it first is shown the object where the last one would have it. Measured on lv22's switch
// band at 1x (2026-09-28, anchor t=2,423): a plan that grazed the red box at x=3,401 passed the
// search alive, while its own witness walk -- the same inputs, placed from the state's own fire
// tick -- died on the ceiling at t=2,756, and GD killed it at 2,754.
//
// The key already separates these states (the fire-tick term above), so splitting the group merges
// nothing new; it only stops them sharing one placement. Exact ticks, not the key's 4-tick bucket,
// which would leave up to three ticks of the same error. A box at rest drops out, as it does from
// the key: its chain has finished the same way for everyone.
inline bool g_groupFire = false;
// --groupxsplit (on by default since 2026-10-07, together with --rampendclick; --no-groupxsplit is
// the off arm): a speed group is also split where its members' x leave a gap across which
// the two parts' object windows cannot meet, so each part builds its windows from its own x span.
// The widest window margin is the speed portals' 80 px (cli.hpp, the group's windows), so the
// windows [lo - 80, hi + dx + 80] of two parts are disjoint exactly when the gap between them is
// wider than 2 * 80 + |dx| (kGroupWinMargin). Nothing narrower is split: members that could still
// share an object stay one group, as before.
//
// The group key holds dx but not x, on the reasoning that members of one speed differ only by
// stair snaps. That stops holding once two populations have taken different speed histories and
// then come back to the same dx: they share the key, one window spans both, and every step walks
// the objects of the whole gap. Measured on lv16's heavy call (--start 3961, all flags): a lineage
// that the --ufolawflap flap kept alive ran at 1.30 while the rest ran at 1.61, both then took the
// 1.05 portal, and from t=19,500 one group spanned 1,700 px (x 31,374, xlo 29,663); those 1,500
// ticks cost 74 s against 5 s without the flag, with 4.8% more steps in the whole call.
//
// Each part's window still covers every member (it is built from that part's own lo..hi), so no
// state loses an object it could reach. What changes when a split happens is the group-level
// bookkeeping that is per part now: the latest fire tick and lock offset each part places its
// geometry from, the order the parts are emitted in, and their separate dedupe maps. Off, or on
// with no gap over the threshold, every state is part 0 and the layer is processed as before.
inline bool g_groupXSplit = true;
// --search dp|beam|montecarlo: the search a call's answer comes from (search_dp.hpp). 0 = the layer
// DP (searchLayers, the default), 1 = the plain beam (searchBeam, a prototype for the yardstick),
// 2 = Monte Carlo rollouts (searchMontecarlo, the reference search that draws random numbers).
// --beam B: the beam's width. --mcrollouts / --mcwindow / --mctoggles: a Monte Carlo round's shape;
// --mcsteps its budget in steps (what keeps it reproducible), --mcsafety the wall-clock safety valve
// in seconds. --mcstall K: give up when K rollouts in a row commit nothing deeper (0 = never) -- a
// dead end then returns in K rollouts, not in the whole step budget; counted in rollouts, so the
// stop stays reproducible. 200000 (measured 2026-10-05 on 29 dead-end calls of loop runs): the
// slowest dead end, about 770 us a rollout, then returns in about 155 s, a quarter of the valve.
// It costs one call measured that solved after 416916 rollouts without progress; 500000 kept it
// but left that dead end at 387 s, too near the valve. --mccall N (a measurement): draw the random
// sequence of the run's call N, to repeat one call of a loop run from leveldp alone (-1 = this
// process's own count).
// --seed: the seed for leveldp alone (in the mod the run's seed, cfg dpseed, is used).
inline int g_searchMethod = 0;
inline int g_beamWidth = 2000;
inline int g_mcRollouts = 300;
inline int g_mcWindow = 1000;
inline int g_mcToggles = 30;
inline long long g_mcSteps = 30000000;
inline double g_mcSafetyS = 600.0;
inline long long g_mcStall = 200000;
inline long long g_mcCall = -1;
inline long long g_searchSeed = 0;
// --witnessauto (a measurement, default off): the witness walk fires the autonomous triggers on its
// own crossing, as --replay does, from their state before the search -- instead of reading the firing
// the search decided from its frontier's leader (model.hpp fireAutoTriggers), which can be ticks
// early for a lineage behind the leader (lv20: t=4220 against the plan's own 4225).
// g_rolloutFiresAuto is the switch cliMainOnce sets around that walk (model.hpp rollout).
inline bool g_witnessAuto = false;
inline bool g_rolloutFiresAuto = false;
constexpr double kGroupWinMargin = 80.0;    // the widest of the group's window margins (speeds)
inline long long g_groupXSplitLayers = 0;   // layers where some group split (printed at the end)
inline int g_groupXSplitMax = 0;            // the most parts one group split into
inline uint64_t groupFireSig(const State& s, long long t) {
    if (!g_groupFire || !s.trig) return 0;
    uint64_t k = 0;
    const size_t n = std::min<size_t>(g_touchMoveTicks.size(), (size_t)kTouchBits);
    for (size_t b = 0; b < n; ++b) {
        if (!s.trig.test(b)) continue;
        const long long moving = g_touchMoveTicks[b];
        if (moving <= 0 || t - (long long)s.fireB[b] >= moving) continue;
        uint64_t h = ((uint64_t)(b + 1) << 32) ^ (uint64_t)s.fireB[b];
        h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ull;
        h = (h ^ (h >> 27)) * 0x94D049BB133111EBull;
        k ^= h ^ (h >> 31);
    }
    return k;
}

// ---- --statecross: the late crossings a state dates itself (State::xcId / xcLag) ----------------------------
// The search fires an autonomous x-crossing trigger on the frontier leader's crossing (model.hpp fireAutoTriggers); a
// group that crosses it later used to be dated by working its present speed back from its x (dynamics.hpp
// autoOwnApply), which is wrong whenever the speed was not the same since (a time-warp ramp, a portal after the
// trigger): measured on a heavy call, 65 ticks late on a trigger whose movers run 3,600 ticks. Here the state notes how many
// ticks after the leader it crossed, when it makes the crossing, in units of g_crossLagUnit. A crossing within g_crossMinLag
// ticks of the leader's is not noted (the leader's date stands: the group would otherwise split on every tick of x-phase),
// nor is one of a trigger that moves nothing. The leader's date is kept in g_crossFire: the search overrides AutoTrig::fireT
// per group (autoOwnApply) while the children of that group are stepped.
inline bool g_stateCross = true;   // (on since 2026-10-21; --no-statecross is the off arm)
inline int g_crossMinLag = 3;     // --crossminlag: ticks late below which a crossing reads the leader's date
inline int g_crossLagUnit = 4;     // --crosslagunit: the unit of a record (State::xcLag), and so the grain of the group split
inline std::vector<double> g_crossCx;      // g_autoTrig[i].cx (g_autoTrig is sorted by it)
inline std::vector<uint8_t> g_crossMoves;  // does trigger i move anything
inline std::vector<int> g_crossFire;       // g_autoTrig[i].fireT as the leader dated it, this layer
inline std::atomic<long long> g_crossNoted{0}, g_crossOver{0};   // --phaseprof (the step runs on the pool)
// --crosscx (default off): triggers standing at one x are crossed on one tick, so a state's lateness is the same for all of them; a record
// is kept per x (the first moving trigger there, its head), not per trigger. A heavy call's states lost half of their records on 16 slots
// (513,585 of 1,143,594 on a level with several triggers at each of a dozen x in a row) and the lost ones were dated by the old reading,
// which is not what the walk (dated by each state's own crossing) does.
inline bool g_crossCxHeads = false;
// --crossbehind (default off): a group whose x has not reached a trigger sees it as not fired, as autoOwnApply has it. This function left the leader's date on such a group (a trigger with no record is "crossed with the leader"),
// so a group still behind a trigger that the leader had fired saw its movers already moving: on a heavy call the search's groups 290 px behind the leader had a hazard moved 5.4 px away that the walk (dated by each state's own
// crossing) still had in place, and the plan they chose died on it in the walk at a 0.4 px edge.
inline bool g_crossBehind = true;   // (on since 2026-10-22; --no-crossbehind is the off arm)
inline std::vector<int> g_crossHead;      // the head of trigger i's x (itself when it is the head), -1 when it moves nothing
inline std::vector<double> g_crossRun;    // per head: how long the movers of its x run from the head's date
inline void autoRunEnsure() {
    if (g_autoRun.size() == g_autoTrig.size()) return;
    g_autoRun.assign(g_autoTrig.size(), 0.0);
    for (size_t i = 0; i < g_autoTrig.size(); ++i)
        for (const TrigCtl& c : g_autoTrig[i].ctl)
            g_autoRun[i] = std::max(g_autoRun[i], c.durTicks + (double)c.spawnLag + c.lockTicks);
}
inline void stateCrossPrepare() {
    g_crossCx.resize(g_autoTrig.size());
    g_crossMoves.resize(g_autoTrig.size());
    g_crossFire.assign(g_autoTrig.size(), -1);
    for (size_t i = 0; i < g_autoTrig.size(); ++i) {
        g_crossCx[i] = g_autoTrig[i].cx;
        bool mv = false;
        for (const TrigCtl& c : g_autoTrig[i].ctl)
            if (std::fabs(c.dx) > 0.001f || std::fabs(c.dy) > 0.001f || c.lockTicks > 0.0 || c.tmode != 0 || c.orbit) {
                mv = true;
                break;
            }
        g_crossMoves[i] = mv ? 1 : 0;
    }
    autoRunEnsure();
    g_crossHead.assign(g_autoTrig.size(), -1);
    g_crossRun.assign(g_autoTrig.size(), 0.0);
    int head = -1;
    for (size_t i = 0; i < g_autoTrig.size(); ++i) {
        if (!g_crossMoves[i]) continue;
        if (!(g_crossCxHeads && head >= 0 && g_crossCx[(size_t)head] == g_crossCx[i])) head = (int)i;
        g_crossHead[i] = head;
        g_crossRun[(size_t)head] = std::max(g_crossRun[(size_t)head],
                                            g_autoRun[i] + (double)std::max(0, g_autoTrig[i].delay - g_autoTrig[(size_t)head].delay));
    }
}
// the leader's dates, taken once a layer after the leader has fired (before any group overrides them)
inline void stateCrossSyncFire() {
    for (size_t i = 0; i < g_autoTrig.size() && i < g_crossFire.size(); ++i) g_crossFire[i] = g_autoTrig[i].fireT;
}
// the effect tick the record in slot k of `s` dates trigger i at
inline int xcOwn(const State& s, int k) {
    const size_t id = (size_t)s.xcId[k] - 1;
    return g_crossFire[id] + (int)s.xcLag[k] * g_crossLagUnit;
}
// is the record in slot k still needed at tick t (its movers have not finished)?
inline bool xcLive(const State& s, int k, long long t) {
    if (!s.xcId[k]) return false;
    const size_t id = (size_t)s.xcId[k] - 1;
    if (id >= g_crossRun.size() || id >= g_crossFire.size() || g_crossFire[id] < 0) return false;
    return (double)(t - (long long)xcOwn(s, k)) <= g_crossRun[id] + 2.0;
}
// the signature that splits groups by what their members dated themselves (0 for a state with no live record)
inline uint64_t xcSig(const State& s, long long t) {
    uint64_t k = s.xcOver ? 0x9E3779B97F4A7C15ull : 0;
    for (int j = 0; j < kCrossSlots; ++j) {
        if (!xcLive(s, j, t)) continue;
        uint64_t h = ((uint64_t)s.xcId[j] << 32) ^ (uint64_t)s.xcLag[j];
        h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ull;
        h = (h ^ (h >> 27)) * 0x94D049BB133111EBull;
        k ^= h ^ (h >> 31);
    }
    return k;
}
// `s` (tick t) has just been stepped from `from` (tick t-1): note the fired triggers whose x it crossed on this step, late.
inline void stateCrossNote(State& s, const State& from, long long t) {
    if (g_crossCx.empty() || (int)s.frame != 0 || s.rev || (int)from.frame != 0 || from.rev) return;
    const double x0 = (double)from.xAbs, x1 = (double)s.xAbs;
    if (!(x1 > x0)) return;
    size_t i = (size_t)(std::upper_bound(g_crossCx.begin(), g_crossCx.end(), x0) - g_crossCx.begin());
    for (; i < g_crossCx.size() && g_crossCx[i] <= x1; ++i) {
        const int fire = g_crossFire[i];
        if (fire < 0 || !g_crossMoves[i] || g_crossHead[i] != (int)i) continue;   // not fired yet: the leader fires it on this very crossing
        const int late = (int)t + g_autoTrig[i].delay - fire;
        if (late < g_crossMinLag) continue;
        const int lag = std::min(255, (late + g_crossLagUnit / 2) / g_crossLagUnit);
        int slot = -1;
        for (int k = 0; k < kCrossSlots && slot < 0; ++k)
            if (s.xcId[k] == (uint16_t)(i + 1)) slot = k;
        for (int k = 0; k < kCrossSlots && slot < 0; ++k)
            if (!xcLive(s, k, t)) slot = k;   // free, or its movers are done
        if (slot < 0) {                        // all still running: the one with the least left makes way, if this one has more
            const double mine = g_crossRun[i];
            double least = 1e300;
            int lk = -1;
            for (int k = 0; k < kCrossSlots; ++k) {
                const double left = g_crossRun[(size_t)s.xcId[k] - 1] - (double)(t - (long long)xcOwn(s, k));
                if (left < least) { least = left; lk = k; }
            }
            s.xcOver = 1;
            g_crossOver.fetch_add(1, std::memory_order_relaxed);
            if (mine > least) slot = lk; else continue;
        }
        s.xcId[slot] = (uint16_t)(i + 1);
        s.xcLag[slot] = (uint8_t)lag;
        g_crossNoted.fetch_add(1, std::memory_order_relaxed);
    }
}
// autoOwnApply for a group whose members share their records (xcSig is in the group's signature): each recorded crossing
// is the group's own date; a trigger with no record was crossed with the leader (to within g_crossMinLag), and a state that
// lost a record is dated by the old reading.
inline void autoOwnApplyCross(AutoOwnGuard& G, double gx, double gdx, long long t, const State& rep) {
    if (!(gdx > 0.0)) return;
    autoRunEnsure();
    for (size_t i = 0; i < g_autoTrig.size(); ++i) {
        AutoTrig& A = g_autoTrig[i];
        if (A.fireT < 0) continue;
        int own = -1;
        int e = -1;
        const int hd = i < g_crossHead.size() ? g_crossHead[i] : -1;   // --crosscx: the record of this trigger's x
        if (hd >= 0)
            for (int k = 0; k < kCrossSlots; ++k)
                if (rep.xcId[k] == (uint16_t)(hd + 1)) e = k;
        if (e >= 0) {
            own = A.fireT + (int)rep.xcLag[e] * g_crossLagUnit;
        } else if (rep.xcOver && gx >= A.cx) {
            const long long k = (long long)std::floor((gx - A.cx) / gdx);
            own = std::min((int)(t - 1 - k) + A.delay, A.fireT + A.fireSpreadT);
        } else if (g_crossBehind && gx < A.cx) {
            if (g_autoOwnPre && g_autoOwnT0 >= 0 && (long long)A.fireT <= g_autoOwnT0) continue;   // --autoownpre (dynamics.hpp)
            G.saved.push_back({i, A.fireT});
            A.fireT = -1;   // this group has not reached it
            continue;
        } else {
            continue;
        }
        if (own <= A.fireT) continue;
        if ((double)(t - own) > g_autoRun[i] + 2.0) continue;
        if (g_trigDbgT >= 0 && t == g_trigDbgT)
            std::printf("autoown t=%lld gx=%.1f gdx=%.3f trig=%zu cx=%.1f fireT %d -> %d (%s) run=%.0f\n", t, gx, gdx, i, A.cx,
                        A.fireT, own, e >= 0 ? "own record" : "old reading", g_autoRun[i]);
        G.saved.push_back({i, A.fireT});
        A.fireT = own;
    }
}
}  // namespace dp
