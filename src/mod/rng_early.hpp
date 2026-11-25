// cfg `rngearly` -> the setting, with no dependency on the game (config.hpp holds the variable; tools/test_rng_early.cpp and py/test_rng_early_cfg.py hold the checks).
// One number carries the whole setting: 0 = off, otherwise the seed the early pin writes. (A separate bool was not part of the effective cfg line nor of snapshotGlobals, AUD-866.)
#pragma once
#include <cstdlib>
#include <string>
namespace p1 {
// 0x48c67641 is the seed whose FIRST draw is r = 0.49998 (x' = x * 0x343fd + 0x269ec3, r = ((x' >> 16) & 0x7fff) / 32767: x' = 0x3fff8000), i.e. the middle of the weight line: an in-reset Advanced Random takes the branch holding the middle
// (a custom level's majority branch) instead of the first entry that seed 0's first draw (r = 0.00116) picks.
constexpr long long kRngEarlySeed = 0x48c67641LL;
// "1" = on with the built-in seed, a decimal N > 1 = on with N, anything else (absent, "0", text, a sign, 19+ digits) = off
inline long long rngEarlyParse(const std::string& val) {
    if (val == "1") return kRngEarlySeed;
    if (!val.empty() && val.size() < 19 && val.find_first_not_of("0123456789") == std::string::npos) { const long long n = std::strtoll(val.c_str(), nullptr, 10); if (n > 1) return n; }
    return 0;
}
}  // namespace p1
