// cfg rngearly: the parse and the off / on / seed settings (no game needed). Build: cl /nologo /EHsc /std:c++17 /I src tools/test_rng_early.cpp
#include <cstdio>
#include "mod/rng_early.hpp"
using p1::rngEarlyParse; using p1::kRngEarlySeed;
static int bad = 0;
#define EXPECT(cond) do { if (!(cond)) { std::printf("FAIL line %d: %s\n", __LINE__, #cond); ++bad; } } while (0)
int main() {
    long long g = kRngEarlySeed;                       // the default (config.hpp g_rngEarlySeed): on, with the built-in seed
    EXPECT(g == 0x48c67641LL && g != 0);
    g = rngEarlyParse("1");          EXPECT(g == kRngEarlySeed && g != 0);   // on with the built-in seed
    g = rngEarlyParse("12345");      EXPECT(g == 12345);                      // on with an arbitrary seed
    g = rngEarlyParse("0");          EXPECT(g == 0);
    g = rngEarlyParse("");           EXPECT(g == 0);
    g = rngEarlyParse("on");         EXPECT(g == 0);
    g = rngEarlyParse("-5");         EXPECT(g == 0);
    g = rngEarlyParse("12x");        EXPECT(g == 0);
    g = rngEarlyParse("99999999999999999999"); EXPECT(g == 0);                // too long: off, not an overflow
    g = rngEarlyParse("999999999999999999");   EXPECT(g == 999999999999999999LL);
    // the level-change sequence: default (on) -> a level that says rngearly=0 -> restore to the default -> a level that names a seed -> restore
    long long snap = kRngEarlySeed, cur = 0;           // snapshotGlobals() at the default
    cur = snap;                      EXPECT(cur == kRngEarlySeed);
    cur = rngEarlyParse("0");        EXPECT(cur == 0);                        // rngearly=0: the old behaviour
    cur = snap;                      EXPECT(cur == kRngEarlySeed);            // restoreGlobals(): on again, no leftover off
    cur = rngEarlyParse("777");      EXPECT(cur == 777);
    cur = snap;                      EXPECT(cur == kRngEarlySeed);
    std::printf(bad ? "FAILED %d\n" : "ok\n", bad);
    return bad ? 1 : 0;
}
