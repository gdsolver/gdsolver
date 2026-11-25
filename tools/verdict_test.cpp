// verdict_test: src/mod/attempt_verdict.hpp (what ended an attempt, and the loop's decisions on it) walked path by path, without a game. Build and run:
//   cl /nologo /EHsc /std:c++17 /I src tools\verdict_test.cpp /Fe:verdict_test.exe && verdict_test.exe     (or clang++ -std=c++17 -I src tools/verdict_test.cpp)
// Exit 0 = every check held. The two paths a cold run rarely reaches -- an overlong attempt and a void attempt -- are the point.
#include "mod/attempt_verdict.hpp"
#include <cstdio>
#include <initializer_list>

static int g_bad = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++g_bad; } } while (0)

int main() {
    const AttemptEnd all[] = {AttemptEnd::Death, AttemptEnd::StallStill, AttemptEnd::EndzoneHung, AttemptEnd::Overlong, AttemptEnd::RefusedClear};

    // The classes, over every kind: wedge = stillness or a hung end; overlong = the tick bound; a guard put the attempt down = either.
    CHECK(!endIsStall(AttemptEnd::Death) && !endIsOverlong(AttemptEnd::Death) && !endPutDownByGuard(AttemptEnd::Death));
    CHECK(endIsStall(AttemptEnd::StallStill) && !endIsOverlong(AttemptEnd::StallStill) && endPutDownByGuard(AttemptEnd::StallStill));
    CHECK(endIsStall(AttemptEnd::EndzoneHung) && !endIsOverlong(AttemptEnd::EndzoneHung) && endPutDownByGuard(AttemptEnd::EndzoneHung));
    CHECK(!endIsStall(AttemptEnd::Overlong) && endIsOverlong(AttemptEnd::Overlong) && endPutDownByGuard(AttemptEnd::Overlong));
    // A refused clear is not a guard's put-down: it is the END of an attempt the loop may call void, not the cause of the next one's void.
    CHECK(!endIsStall(AttemptEnd::RefusedClear) && !endIsOverlong(AttemptEnd::RefusedClear) && !endPutDownByGuard(AttemptEnd::RefusedClear));

    // The attempt stamp: a verdict is this attempt's only when the numbers match; the plain death otherwise (the default, never stamped, matches nothing).
    {
        const AttemptVerdict v{AttemptEnd::Overlong, 7, 40001};
        CHECK(verdictFor(v, 7).kind == AttemptEnd::Overlong && verdictFor(v, 7).sinceTick == 40001);
        CHECK(verdictFor(v, 8).kind == AttemptEnd::Death && verdictFor(v, 6).kind == AttemptEnd::Death);
        const AttemptVerdict none;
        CHECK(verdictFor(none, 0).kind == AttemptEnd::Death && verdictFor(none, -1).kind == AttemptEnd::Death);
        CHECK(verdictFor(none, 0).sinceTick == -1);
    }

    // The void attempt: only a refused clear, after an attempt a guard put down, with a last death to repeat. Every other cell is false.
    for (AttemptEnd k : all)
        for (int prev = 0; prev < 2; ++prev)
            for (long long last : {-1LL, 0LL, 5000LL}) {
                const bool want = k == AttemptEnd::RefusedClear && prev == 1 && last >= 0;
                CHECK(isVoidAttempt(k, prev == 1, last) == want);
            }
    // The measured producer (lv8 / 11 / 17, 2026-08-25), as a sequence: a stall put the attempt down; the next one's t=0 end screen is refused; the one after is a normal death.
    {
        bool prevGuard = false;
        const AttemptEnd seq[] = {AttemptEnd::StallStill, AttemptEnd::RefusedClear, AttemptEnd::Death};
        const bool wantVoid[] = {false, true, false};
        for (int i = 0; i < 3; ++i) {
            CHECK(isVoidAttempt(seq[i], prevGuard, 12000) == wantVoid[i]);
            prevGuard = endPutDownByGuard(seq[i]);
        }
        // ...and an overlong put-down leaks the same way.
        CHECK(isVoidAttempt(AttemptEnd::RefusedClear, endPutDownByGuard(AttemptEnd::Overlong), 12000));
        // ...but a refusal after an ordinary death is not void, nor is the first attempt of a run (no last death).
        CHECK(!isVoidAttempt(AttemptEnd::RefusedClear, endPutDownByGuard(AttemptEnd::Death), 12000));
        CHECK(!isVoidAttempt(AttemptEnd::RefusedClear, true, -1));
    }

    std::printf(g_bad ? "verdict_test: %d FAILED\n" : "verdict_test: all checks held\n", g_bad);
    return g_bad ? 1 : 0;
}
