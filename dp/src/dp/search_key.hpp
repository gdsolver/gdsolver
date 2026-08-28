#pragma once
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
// same x on the build without the bits. It is a pre-existing break; see
// docs/findings.md.) Reverted for having no measured benefit, on the same
// grounds as the `s.action` experiment recorded at the end of the key below.
// What actually happens at that portal is in docs/findings.md: the touching
// branch exists, the driver just never re-anchors far enough back to take it.

// GD's `upsideDown` for a state whose `flip` is the model's frame-local sign.
// `toFrame` gives frame 1 v = +X and frame 3 v = -X, so frame 3 is the mirror.
// Measured on lv22 with the trace's own flip/frame columns against GD's dump:
// the two agree everywhere up to t=4,664 and are opposite for the whole frame-3
// section. Only the places that read or write GD's own statement -- the gravity
// portals -- convert; the physics keeps using `flip`.
inline uint8_t gdUpOf(const State& s) {
    return (s.frame == 3) ? (uint8_t)!s.flip : s.flip;
}

inline uint64_t keyOf(const State& s) {
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
    const int32_t xq = (int32_t)std::lround(s.xAbs * 4.0);
    return ((uint64_t)(uint32_t)yq << 32) ^ ((uint64_t)(uint32_t)xq << 18)
           ^ ((uint32_t)vq << 6)
           ^ ((uint64_t)s.mini << 5)
           // ringHold too: two states that differ only in "has this hold
           // already spent its ring" answer the next press differently.
           ^ ((uint64_t)s.ringHold << 4)
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
           ^ ((uint64_t)(uint32_t)(int32_t)std::lround(s.bandFloor) << 46)
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
           // Slope ride counter (see State::slopeT): two riders at the same
           // (x, y, vy) that landed on the ride at different ticks launch with
           // different exit impulses, so they are not interchangeable. Gated
           // on onSlope so every non-slope key stays bit-identical; the value
           // saturates at 24, so this costs at most 25 cells per riding cell
           // (in practice riders at the same x almost always share it).
           ^ (s.onSlope ? ((uint64_t)s.slopeT << 47) : 0)
           ^ ((s.dual && s.onSlope2) ? ((uint64_t)s.slopeT2 << 12) : 0)
           // The flap buffered on the portal's tick (State::pFlap). Same (y,vy)
           // but different behaviour next tick, so it goes in the key.
           ^ (s.pFlap ? 0x9E3779B97F4A7C15ull : 0)
           // [D9] Leaving a surface in a rotated frame (State::pBallOff). Same
           // (y,vy), but whether -1.000 is written next tick differs.
           ^ (s.pBallOff ? 0xC2B2AE3D27D4EB4Full : 0)
           // [r102] one-shot skip of the terminal clamp (State::pNoTerm)
           ^ (s.pNoTerm ? 0x94D049BB133111EBull : 0)
           // [2026-08-25] GD's velocity-limit exemption (State::boost). Same
           // (y,vy), but whether the swing's terminal clamp applies differs
           // (a boosted state passes 8 and keeps accelerating; an unboosted
           // one sits pinned there).
           ^ (s.boost ? 0xA0761D6478BD642Full : 0)
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
           ^ ((uint64_t)s.fgArm << 63)
           // [r52] The frame-change bit goes in the key too (it is set only on
           // the tick after the change, so the partition granularity barely
           // moves)
           ^ (s.frameChg ? 0x27D4EB2F165667C5ull : 0)
           // [r66] Non-zero only while pushed. The release vy is a function of
           // ceilT, so it goes in the key
           ^ (s.ceilT ? (0x165667B19E3779F9ull
                         * (uint64_t)(s.ceilT | ((uint32_t)s.ceilM4 << 8)))
                      : 0)
           ^ ((uint64_t)s.held << 1) ^ s.grounded;
}

}  // namespace dp
