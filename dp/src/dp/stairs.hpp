#pragma once
#include "dp/level.hpp"

namespace dp {

// **Stair snap** -- what the old "+1.0 px on every landing" was really modelling.
//
// The +1 px was applied on EVERY airborne->grounded transition, but GD only did
// it on 24 of lv3's 148 landings, and not always by 1.0 (0.30 and 0.40 also
// occur). The real mechanism is PlayerObject::checkSnapJumpToObject, confirmed
// by hooking the function itself (cfg `snaptrace=1`, data/snaptrace_lv3.txt):
//
//   - called from the cube landing branch of collidedWithObjectInternal, so it
//     runs EVERY grounded tick, once per solid the hitbox touches -- not just on
//     the landing tick. Measured: 3000 calls over t=1231..18543 on lv3.
//   - if the object differs from the previously snapped one, and the centre-to-
//     centre (dx, dy) matches one of three stair patterns, GD nudges x so the
//     player keeps its offset relative to the object it is standing on:
//         x <- clamp(obj.cx + snapDistance, x +/- threshold)
//   - snapDistance is then refreshed to (x_after_snap - obj.cx), every call.
//
// Measured separation on lv3: 25 calls moved x, and their (dx, dy) were only
// (+120,+30), (+150,-30), (+90,+60). Of the 2975 that did not move x, ZERO had
// any of those three -- the gate is exact, not statistical. The clamp formula
// reproduced all 25 magnitudes with 0 mismatches, and the snapDistance update
// matched 3000/3000. This is why vy and penetration depth could never separate
// the cases: the condition is horizontal and depends on the PREVIOUS object.
struct StairParams {
    double threshold, little, down, big;
};
// GD accumulates the player's x as a **float, in absolute level coordinates**,
// one add per physics step. It does NOT evaluate x0 + t*dx. That matters:
// float32's spacing at x ~ 8,000 is ~0.001, so re-rounding the same add every
// tick biases the running total in a way a double closed form cannot follow.
//
// Measured on lv4 (1,269 sampled ticks over t=768..7173, snap offsets removed):
//   double closed form  x0 + t*1.298250   -> max |error| 0.2096 px
//   float32 accumulation from absolute x  -> max |error| 0.0043 px
// A 50x improvement, and it is the residual that was left after the stair snap.
// 0.2 px sounds harmless but it is 0.16 tick, which is enough to flip the tick
// on which the cube walks off a ledge -- and that was the actual lv4 failure.
//
// NOTE the "from absolute x" part: starting the accumulator at 0 and adding an
// offset afterwards scores WORSE than the double form (0.2858), because the
// rounding depends on the magnitude. Re-anchored runs must therefore seed the
// accumulator with GD's real x, which --start already supplies.
// dxF is the CURRENT speed's per-tick advance. It is a property of the level
// position, not of the state: the whole search shares one x timeline (the
// near-object window, the ceiling switching and the goal test are all built
// from it), so the speed is switched when the shared accumulator reaches the
// portal rather than per state. That is exact whenever the player actually
// passes through the portal, which is how official levels place them; a plan
// that flew OVER one would keep GD's old speed and this model would not.
inline float advanceX(float x, float dxF) { return (float)(x + dxF); }

// --snaplog <file>: mirror of the MOD's `snaptrace=1` output, emitted from the
// witness re-simulation only (never from the search, which would drown it).
// Having BOTH sides speak the same format is what makes the diff possible --
// deducing the discrepancy from x alone cost more than adding this did.
inline std::ofstream* g_snapOut = nullptr;
// Selected by player speed and size. The speed rows come from the decompiled
// function (camila314/gdp); only the 0.9 row is verified against our own
// measurement, because lv1-14 contain no speed portal. Rows other than 0.9 are
// therefore UNVERIFIED -- levels with speed portals must be re-checked with
// `snaptrace=1` before their results are trusted.
inline StairParams stairParamsFor(double dxPerTick, double size) {
    const bool full = std::fabs(size - 1.0) < 1e-3;
    if (dxPerTick < 1.15) return {1.0, 90.0, 120.0, 60.0};            // 0.7
    if (dxPerTick < 1.45) return {1.0, full ? 120.0 : 90.0, 150.0, 90.0};   // 0.9
    if (dxPerTick < 1.78) return {2.0, full ? 150.0 : 90.0, 195.0, 120.0};  // 1.1
    if (dxPerTick < 2.17) return {2.0, 90.0, 225.0, 135.0};           // 1.3
    return full ? StairParams{2.0, 180.0, 225.0, 135.0}
                : StairParams{1.0, 120.0, 150.0, 90.0};               // default
}

}  // namespace dp
