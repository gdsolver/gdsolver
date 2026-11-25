// A hard bound that stopped a walk or a recursion before it finished.
//
// Several loaders cut a walk over the level's spawn chains / groups after a fixed number of steps (a group can contain the
// trigger that targets it, so a bound stands in for a visited set), and the trigger shadow stops a spawn chain at a depth.
// Hitting such a bound used to be silent: the loader returned what it had built so far and the run went on with a table
// that was missing rows. A bound that is hit is a failure of the loader, not a result, so it is printed -- once per site
// and process, with what it stopped. Printing is all it does; what the caller returns is unchanged.
#pragma once
#include <cstdio>
#include <mutex>
#include <set>
#include <string>

namespace dp {

inline void guardHit(const char* site, long long bound, const char* what) {
    static std::mutex mu;
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> lk(mu);
    if (!seen.insert(site).second) return;
    std::printf("guard: %s hit its bound of %lld; %s -- what it was building is INCOMPLETE (a failure of the loader, not a result)\n", site, bound, what);
    std::fflush(stdout);
}

}  // namespace dp
