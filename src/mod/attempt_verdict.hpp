#pragma once
// WHAT ENDED AN ATTEMPT, as the game-side guards know it, and the loop's decisions that read it (design: notes/design_wall_key_and_fixed_point_gd01_1119.md,
// stage S1). No game dependency on purpose: tools/verdict_test.cpp compiles this alone and walks every path -- including the two a cold run rarely reaches
// (an overlong attempt, a void attempt).
//
// The stall guard (hooks_gamelayer.cpp) and the false-clear gate (hooks_playlayer.cpp) end an attempt for a reason they KNOW; the loop used to re-derive it
// from tick counts (900 still rows, a 30,000-tick wedgeT, "fewer than 50 rows and a death before 600"). They set the verdict BEFORE the end is reported;
// dpsolve::onDeath of the same attempt reads and clears it (a verdict whose attempt is not the current one is ignored).
//   StallStill   the player's x and y did not move for the guard's stillness bound; `sinceTick` is the first tick of the stillness
//   EndzoneHung  the end sequence started (the player is locked) and never completed; `sinceTick` as above
//   Overlong     the attempt outran the guard's tick bound (the moving zombie); `sinceTick` = the tick it was put down at
//   RefusedClear a clear the false-clear gate refused (short of the goal, under 95%); `sinceTick` = the tick the clear was reported at
enum class AttemptEnd : int { Death = 0, StallStill, EndzoneHung, Overlong, RefusedClear };
struct AttemptVerdict { AttemptEnd kind = AttemptEnd::Death; int attempt = -1; long long sinceTick = -1; };

// The verdict that belongs to `curAttempt`, or the plain death when it is a stale one.
inline AttemptVerdict verdictFor(const AttemptVerdict& v, int curAttempt) {
    return (v.attempt >= 0 && v.attempt == curAttempt) ? v : AttemptVerdict{};
}
// The attempt stood still (or its end sequence hung): the loop credits the death where the stillness began.
inline bool endIsStall(AttemptEnd k) { return k == AttemptEnd::StallStill || k == AttemptEnd::EndzoneHung; }
// The attempt had to be forced over by the tick bound: wedge-class for every purpose.
inline bool endIsOverlong(AttemptEnd k) { return k == AttemptEnd::Overlong; }
// An attempt put down by a guard: its completion state can leak across the reset into the next attempt (g_prevAttemptWasZombie).
inline bool endPutDownByGuard(AttemptEnd k) { return endIsStall(k) || endIsOverlong(k); }
// A VOID attempt: the attempt before was put down by a guard, and this one ends in the false-clear gate's refusal, with a last death to repeat.
inline bool isVoidAttempt(AttemptEnd k, bool previousWasPutDownByGuard, long long lastDeath) {
    return k == AttemptEnd::RefusedClear && previousWasPutDownByGuard && lastDeath >= 0;
}
