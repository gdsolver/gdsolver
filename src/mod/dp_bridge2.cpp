// A second copy of the solver core, for a second search running at the same time as the first.
//
// dp/ keeps a search's state in namespace-scope globals (the arguments it parsed, the level, the
// caches, the outcome), so two searches cannot run in one copy of it. This translation unit
// compiles the same headers once more with the namespace renamed, which gives every one of those
// globals a second, independent instance -- dp2::g_outcome beside dp::g_outcome, and so on -- with
// no change to dp/ itself. Everything dp/ declares is inside namespace dp (the rename reaches all
// of it); the gdapprox types it includes carry no state.
//
// Used for one thing: the plain search that ends a cap ladder, started beside the ladder instead
// of after it (cli.hpp PlainElsewhere; the hooks are in dp_bridge.cpp). Compiled outside the mod
// target, as library gdsolver_dpcore2, with the same options as the first copy (dp/src/dpcore.cpp,
// dp/CMakeLists.txt gdsolver_dp_options), so the second copy steps the model exactly as the first
// does. Like dpcore.cpp it sees no Geode, cocos or Windows header.
#undef NEAR
#undef FAR
#undef near
#undef far
#undef small
#undef min
#undef max

// Every standard header the core includes, first: the rename of `std` below must not reach them.
// One the list misses fails to compile rather than compiling wrong.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <xmmintrin.h>   // as cli.hpp and repair.hpp: x86 only, so the Android build compiles
#endif

// THE SECOND COPY'S STDOUT IS HELD, and printed or thrown away with the search it came from
// (dp_bridge.cpp), so that a log reads as it would if the plain search had run in the first copy:
// after the ladder's line that hands over to it, not through the ladder's attempts. The core
// writes stdout through std::printf, std::fputs, std::fwrite and std::fflush only; inside this
// file `std` names dp2std, which has those four and everything else from std. stderr and files
// go straight through.
namespace {
namespace dp2out {
std::mutex g_m;
std::string g_held;
void hold(const char* s, std::size_t n) {
    std::lock_guard<std::mutex> g(g_m);
    g_held.append(s, n);
}
int holdf(const char* fmt, va_list ap) {
    char buf[1024];
    va_list again;
    va_copy(again, ap);
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    if (n < 0) {
        va_end(again);
        return n;
    }
    if ((std::size_t)n < sizeof buf) {
        hold(buf, (std::size_t)n);
    } else {
        std::string big((std::size_t)n + 1, '\0');
        std::vsnprintf(big.data(), big.size(), fmt, again);
        hold(big.data(), (std::size_t)n);
    }
    va_end(again);
    return n;
}
}  // namespace dp2out
}  // namespace

namespace dp2std {
using namespace ::std;
inline int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int n = dp2out::holdf(fmt, ap);
    va_end(ap);
    return n;
}
inline int fprintf(::FILE* f, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int n = f == stdout ? dp2out::holdf(fmt, ap) : ::std::vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}
inline int fputs(const char* s, ::FILE* f) {
    if (f != stdout) return ::std::fputs(s, f);
    dp2out::hold(s, ::std::strlen(s));
    return 0;
}
inline ::std::size_t fwrite(const void* p, ::std::size_t size, ::std::size_t n, ::FILE* f) {
    if (f != stdout) return ::std::fwrite(p, size, n, f);
    dp2out::hold((const char*)p, size * n);
    return n;
}
inline int fflush(::FILE* f) { return f == stdout ? 0 : ::std::fflush(f); }
}  // namespace dp2std

#define std dp2std
#define dp dp2
#include "dp/cli.hpp"
#undef std

#include "mod/dp_bridge.hpp"

namespace dpbridge::second {

void setFileSigHints(const std::unordered_map<std::string, std::string>& hints) {
    dp::g_fileSigHint.clear();
    for (const auto& kv : hints) dp::g_fileSigHint[kv.first] = kv.second;
}

int solve(const std::string& csv, const std::vector<std::string>& argv) {
    std::vector<std::string> a = argv;
    std::vector<char*> ptr;
    ptr.reserve(a.size());
    for (std::string& s : a) ptr.push_back(s.data());
    dp::g_levelCsv = csv;
    int rc = -1;
    try {
        rc = dp::cliMainOnce((int)ptr.size(), ptr.data());
    } catch (...) {
        rc = -2;
    }
    dp::g_levelCsv.clear();
    return rc;
}

SolveOutcome outcome() {
#include "dpcore_outcome.inl"
}

std::string takeOutput() {
    std::lock_guard<std::mutex> g(dp2out::g_m);
    std::string s;
    s.swap(dp2out::g_held);
    return s;
}

void cancel(bool on) {
    dp::g_check.cancel.store(on, std::memory_order_release);
}

long long envKillsTotal() {
    return dp::g_envKills.load(std::memory_order_relaxed);
}

void releaseCaches() { dp::releaseCrossCallCaches(); }
std::size_t cacheBytes() { return dp::crossCallCacheBytes(); }
std::size_t groupCacheBytes() { return dp::crossCallGroupBytes(); }

bool runBegin(int level, unsigned long long seed, std::string& why) {
    return dp::runBegin(level, seed, why);
}
std::string runVerified(const std::vector<std::pair<long long, int>>& flown, long long deathT,
                        double x, bool cleared, long long divT) {
    return dp::runVerified(flown, deathT, x, cleared, divT);
}
std::string runEnd() { return dp::runEnd(); }
void runNoteSplice(const std::vector<std::pair<long long, int>>& edges, long long t0) {
    dp::runNoteSplice(edges, t0);
}

}  // namespace dpbridge::second

#undef dp
