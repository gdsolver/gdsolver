// The one translation unit that compiles the solver core (dp/) inside the mod.
//
// NOTHING from Geode or cocos may be included here, and nothing here may include a mod header
// that does: the dp headers are written for a plain C++ toolchain, and the Windows/cocos
// macro soup breaks them. The whole of the mod's contact with the solver goes through
// dp_bridge.hpp, which mentions neither library.
//
// Geode force-includes its prelude into every source of the mod target, and that drags in
// <windows.h>, so "include no Windows headers" is not something this file can decide for
// itself -- windef.h has already defined NEAR and FAR as empty macros by the time the first
// line here is compiled, and dp's `enum Bucket { NEAR, PORT, ... }` becomes a syntax error.
// Undefining them is the whole fix: nothing in dp wants the segment-model keywords, and this
// TU calls no Windows API.
#undef NEAR
#undef FAR
#undef near
#undef far
#undef small
#undef min
#undef max

// cli.hpp is the whole of leveldp -- the dp chain plus the front end as dp::cliMain. The CLI
// executable is a three-line main() around the same header, so the mod runs the solver's code
// path rather than an imitation of it.
#include "dp/cli.hpp"

#include "mod/dp_bridge.hpp"

namespace dpbridge {

LevelStats statsFromCsv(const std::string& csv) {
    std::istringstream in(csv);
    dp::Level L = dp::loadLevelFrom(in);
    LevelStats s;
    s.ok = true;
    s.objs = L.objs.size();
    s.portals = L.portals.size();
    s.pads = L.pads.size();
    s.orbs = L.orbs.size();
    s.moving = L.dyn.size();
    s.maxX = L.maxX;
    return s;
}

int solveInProcess(const std::string& csv, const std::vector<std::string>& args) {
    // The level comes from memory; the path argument is still required positionally, so it is
    // given a name that says where the bytes really came from if it ever shows up in a message
    std::vector<std::string> argv{"leveldp", "(in-process level)"};
    argv.insert(argv.end(), args.begin(), args.end());
    std::vector<char*> ptr;
    ptr.reserve(argv.size());
    for (std::string& a : argv) ptr.push_back(a.data());

    dp::g_levelCsv = csv;
    int rc = -1;
    try {
        rc = dp::cliMain((int)ptr.size(), ptr.data());
    } catch (...) {
        rc = -2;   // never let an exception cross back into the game's frame
    }
    dp::g_levelCsv.clear();
    return rc;
}

SolveProgress progress() {
    SolveProgress p;
    p.running = dp::g_progress.running.load(std::memory_order_acquire);
    p.from = dp::g_progress.from.load(std::memory_order_relaxed);
    p.tick = dp::g_progress.tick.load(std::memory_order_relaxed);
    p.horizon = dp::g_progress.horizon.load(std::memory_order_relaxed);
    p.x = dp::g_progress.x.load(std::memory_order_relaxed);
    p.alive = dp::g_progress.alive.load(std::memory_order_relaxed);
    return p;
}

SolveOutcome outcome() {
    SolveOutcome o;
    o.verdict = dp::g_outcome.verdict;
    o.deepT = dp::g_outcome.deepT;
    o.deepX = dp::g_outcome.deepX;
    o.capHits = dp::g_outcome.capHits;
    o.cancelT = dp::g_outcome.cancelT;
    o.resimDead = dp::g_outcome.resimDead;
    o.resimFirst = dp::g_outcome.resimFirst;
    o.resimWhy = dp::g_outcome.resimWhy;
    o.resimUid = dp::g_outcome.resimUid;
    o.resimObjX = dp::g_outcome.resimObjX;
    o.resimObjY = dp::g_outcome.resimObjY;
    o.resimTrig = dp::g_outcome.resimTrig;
    o.resimFrame = dp::g_outcome.resimFrame;
    o.replayDiedT = dp::g_outcome.replayDiedT;
    o.rejoinT = dp::g_outcome.rejoinT;
    o.rejoinBadT = dp::g_outcome.rejoinBadT;
    o.rejoinBadWhy = dp::g_outcome.rejoinBadWhy;
    o.needTrigMask = dp::g_outcome.needTrigMask;
    o.needTrigPassed = dp::g_outcome.needTrigPassed;
    o.seedRotQ = dp::g_outcome.seedRotQ;
    o.rotQOrder = dp::g_outcome.rotQOrder;
    o.coinGates = dp::g_outcome.coinGates;
    o.startRotHit = dp::g_outcome.startRotHit;
    o.startRotGiven = dp::g_outcome.startRotGiven;
    o.startRotMiss = dp::g_outcome.startRotMiss;
    o.trigWinTouch = dp::g_outcome.trigWinTouch;
    o.trigTotal = dp::g_outcome.trigTotal;
    o.trigRelevantN = dp::g_outcome.trigRelevantN;
    o.trigKept = dp::g_outcome.trigKept;
    o.trigDroppedRelevant = dp::g_outcome.trigDroppedRelevant;
    o.trigDroppedBehind = dp::g_outcome.trigDroppedBehind;
    o.trigDroppedAhead = dp::g_outcome.trigDroppedAhead;
    o.trigMaxKeptX = dp::g_outcome.trigMaxKeptX;
    o.trigMapSig = dp::g_outcome.trigMapSig;
    return o;
}

void checkSubscribe(bool on) {
    dp::g_check.enabled.store(on, std::memory_order_release);
    if (!on) dp::g_check.reset();
}

bool nextCheckpoint(SolveCheckpoint& out) {
    // One lock for the call id, the judged count and the point: reset() changes all three under
    // it, so reading them apart could pair an index from one call with a point from the next.
    std::lock_guard<std::mutex> g(dp::g_check.m);
    const size_t idx = dp::g_check.judged.load(std::memory_order_acquire);
    if (idx >= dp::g_check.points.size()) return false;
    const dp::SearchCheckpoints::Point& p = dp::g_check.points[idx];
    out.call = (unsigned long long)dp::g_check.call.load(std::memory_order_acquire);
    out.index = idx;
    out.t0 = p.t0;
    out.tick = p.tick;
    out.edges = p.edges;
    return true;
}

unsigned long long checkCall() {
    return (unsigned long long)dp::g_check.call.load(std::memory_order_acquire);
}

bool passCheckpoint(unsigned long long call, std::size_t index) {
    return dp::g_check.pass((uint64_t)call, index);
}

void cancelSearch(bool on) {
    dp::g_check.cancel.store(on, std::memory_order_release);
}
long long envKillsTotal() {
    return dp::g_envKills.load(std::memory_order_relaxed);
}
std::string coreVersion() {
    // No version string exists in dp/ yet; the compile stamp of this TU is what identifies
    // the core that is linked in, and it moves whenever dp/ is rebuilt
    return std::string("dp core built ") + __DATE__ + " " + __TIME__;
}

}  // namespace dpbridge
