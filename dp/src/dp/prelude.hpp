#pragma once

// Standard-library and model-table includes shared by every dp/ header.
// (Stage A-2 mechanical split of leveldp.cpp; the chain of includes keeps
// the translation unit in its historical order.)

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <set>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "models/ship_model.hpp"
#include "models/ufo_model.hpp"
#include "models/ship_params.hpp"

namespace dp {

// ============================================================================
// HOW MANY TOUCH BOXES A STATE CAN CARRY. The mask type, State::trig,
// State::fireB, the loader's cap, the proximity tables and the --needtrig bit
// range derive from it.
//
// THIS COMMENT USED TO SAY "change kTouchBits and nothing else". IT WAS WRONG,
// and it cost two sessions a day of measurement on 2026-09-21: both raised the
// width, both measured lv22 failing, and neither result was about the width.
// level_loader.hpp built TrigOf::mask as a uint32_t and set it with
// `(uint32_t)1 << b` over a touch BOX INDEX, so at a width of 64 the shift was
// undefined for b >= 32 and wrapped mod 32 on x86 -- boxes 32..63 answered for
// boxes 0..31 while dyn.trigMask, the consumer, was already TouchMask. The
// producer truncated and the widening into the consumer said nothing.
//
// So before believing any measurement that moves this constant: grep the tree
// for `uint32_t` next to a mask, for the literal 32, and for loops over bit
// indices, and READ each hit. Not every 32 is this constant -- the rotation
// queue's cursor (frames.hpp, paired with step.hpp's `idx < 32`) and a gravity
// portal's bit index are their own limits and must NOT be converted. The grep
// is a list to read, not a list to rewrite.
//
// IT LIVES HERE, in the root of the include chain, because the mask is held in
// three places that see each other only through this file: State (state.hpp),
// the recording's per-object masks (dynamics.hpp) and the run's outcome
// (progress.hpp). Putting it next to the touch triggers themselves was tried
// and does not compile -- two of those three are included first.
//
// Why it is a compile-time number: State is a POD copied for every node the
// search keeps, initialised positionally in places, sized by a static_assert,
// parsed out of --start and carried in the anchor payload. A runtime width
// would put an indirection in the hottest structure in the program.
//
// Why widening is not the whole answer: on the one corpus level that exceeds
// this, 152 boxes reduce to 61 once the ones that cannot reach the player are
// dropped (--trigrelevant), and before that filter existed 24 of these 32 bits
// went to boxes that only tint their group while 52 that move floors were
// discarded. Narrow first, then widen.
//
// UNMEASURED FOR CUSTOM LEVELS: the corpus tops out at 61 after filtering, and
// the two custom levels with dumps in the lab have empty trigger dumps, so
// nothing here says what a custom level needs. The loader still caps, and a run
// that hits the cap prints so.
// [2026-09-20, coin routing] 64 WAS TRIED AND IS HELD, NOT REJECTED. It fixes
// lv22's coverage outright (61 relevant, 61 kept, maxKeptX 3,675 -> 20,111) and
// the level then fails to solve. Measured with the variable isolated, same tree
// and flags, one Wine worker each:
//
//   32   lv22 CLEARED  41 iterations,  962 s, deepest x=21,897
//   64   lv22 stuck   201 iterations, 3516 s, deepest x=10,755
//
// WHY IT FAILS IS NOT ESTABLISHED, and the first answer written here was wrong.
// It said the cost lands in the dedupe key, which divides on every box in the
// window while that box is moving (search_key.hpp). The same run refutes that:
// at 32 and 64 the key divides 2,891,350 and 3,798,737 times -- 31% more -- yet
// maxAlive is 5,518 against 5,513, capHits 14,439 against 14,465, and BOTH
// SOLVE. A single anchorless search is nearly indifferent to the width. The
// 41 -> 201 happens in the repair loop.
//
// THE AUTO-WINDOW GATE IS NOT THE MECHANISM, and this file said it was. The
// gate's threshold really does move with the width -- the `plain` probe in
// cli.hpp is capped at kTouchBits, so it fires above x0 2,483 at 32 and above
// 4,187 at 64 -- but on lv22 at 64 THE GATE CHANGES NOTHING IT LOADS. 61
// relevant boxes fit in 64, so nothing is ever dropped, and the fromX argument
// (the only thing winTouch feeds) has nothing left to select:
//
//   width  anchor    gate     relevant  kept  droppedRelevant  maxKeptX
//     64    3,000    silent      61      61          0          20,111
//     64   20,200    fires       61      61          0          20,111
//     32    3,000    fires       61      32         29           7,875
//     32   20,200    fires       61      32         29          20,111
//
// The bottom pair is the positive control: at a width where the set cannot fit,
// the loaded set does move with the anchor, so the top pair is a real null and
// not an instrument that reports nothing. (Coin routing off, so this is the
// population the 41/201 arms above actually ran on.)
//
// WHAT THE TABLE SAYS INSTEAD INVERTS THE OBVIOUS READING. At 32 every call on
// this level runs with 29 relevant boxes missing and the level SOLVES; at 64
// every call has the whole world and it does not. Width is not costing the
// search something. It is SHOWING the model geometry the narrow run was blind
// to -- and lv22 carries twelve Stop triggers (1616) that this model does not
// implement at all. The cheap next test is static, with no run in it: are the
// 29 boxes a width of 32 drops the ones whose chains reach what is unmodelled?
//
// AND THE 41 -> 201 DOES NOT SAY WHICH BUILD FAILED. Its number is quoted in
// the message of the commit that fixed seven width sites -- three of them
// truncate every box above bit 31 onto bit 31 inside markTouched, a fault that
// is a no-op at 32 and corrupts every tick at 64 -- so the run finished before
// that commit landed, but whether its exe already carried the fixes cannot be
// recovered: the exe is gone, and the same message retracts an EARLIER 64-bit
// number on exactly these grounds, which cuts the other way. Reading the
// history does not settle this. Re-running it does.
//
// --keycensus stays useful for what it measures: the cost per box. Of lv22's
// 61, TWENTY-FIVE never divide the key at all and 22 carry 90% of the dividing.
// So boxes differ by orders of magnitude in what their bit costs, and a rule
// that keeps the free ones costs nothing. It is NOT evidence about why the cold
// run failed (audit AUD-20260921-12).
constexpr int kTouchBits = 32;
using TouchMask = std::conditional_t<(kTouchBits > 32), uint64_t, uint32_t>;
static_assert(kTouchBits <= 8 * (int)sizeof(TouchMask),
              "kTouchBits does not fit in TouchMask");
// `1 << bit` in the mask's own width. Writing `1u << bit` against a 64-bit mask
// is the silent truncation this constant exists to prevent.
constexpr TouchMask touchBit(int b) { return (TouchMask)1 << b; }
// ...and the way back: which bit a single-bit mask is. THE POINT IS THE TYPE.
// Three sites wrote `uint32_t mbit = tb.second; while (!(mbit & 1u) && b < 31)`,
// which at a width of 64 truncates every box above 31 to zero AND stops the
// walk at 31, so all of them reported bit 31 -- one shared fire tick for
// thirty-two different boxes, in markTouched, every tick. Taking a TouchMask
// and bounding by kTouchBits makes that shape impossible to write again.
constexpr int touchBitIndex(TouchMask m) {
    int b = 0;
    while (b < kTouchBits - 1 && !(m & (TouchMask)1)) { m >>= 1; ++b; }
    return b;
}
// ============================================================================

}  // namespace dp
