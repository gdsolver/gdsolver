#pragma once
#include "dp/level.hpp"

namespace dp {

;
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

}  // namespace dp
