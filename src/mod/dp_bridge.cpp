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

#include <atomic>
#include <filesystem>
#include <thread>

#include "mod/dp_bridge.hpp"

namespace dpbridge {

// Only the solver worker owns this scope; the game's main thread uses its ordinary file reader.
void beginInputJob(unsigned long long session, const std::string& csv) {
    dp::g_inputFiles.begin(session, csv);
}

// Release pins before another replay can rewrite the recording files.
void endInputJob() {
    dp::g_loadPrinted = nullptr;   // a failed load may have unwound its output buffer
    dp::g_ladderLevel = dp::LadderLevelCache{};
    dp::g_inputFiles.end();
}

// Both the signature and the core's group parser read this same owned buffer.
InputFileInfo inputFileInfo(const std::string& path, bool immutable) {
    const auto data = dp::g_inputFiles.get(path, immutable);
    return {dp::inputSignature(data), data->revision};
}

// Level identities are also based on bytes and are scoped to this cold lineage.
unsigned long long inputLevelRevision() { return dp::g_inputFiles.levelRevision; }

// Counts make repeated preparation savings observable without a full profiler.
InputJobStats inputJobStats() {
    return {dp::g_inputFiles.reads, dp::g_inputFiles.hits, dp::g_inputFiles.levelHits};
}

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
    s.unsupported = L.unsupported;
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

std::vector<double> prepMarks() {
    return std::vector<double>(std::begin(dp::g_prepMark), std::end(dp::g_prepMark));
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
#include "mod/dp_bridge_outcome.inl"
}

// ---- the plain search beside the ladder (cli.hpp PlainElsewhere) ---------------------------
//
// The second copy of the core (dp_bridge2.cpp) runs it on a thread of its own, writing to side
// files next to the call's own; the ladder's take() moves them into place and copies the second
// copy's outcome into this one's g_outcome, so that everything after -- step 5 of the ladder, and
// the repair loop reading outcome() -- sees what the plain search in this copy would have left.
namespace {

struct Beside {
    bool enabled = false;
    bool gamble = false;               // plainBeside(3): taken as soon as ready() holds
    std::thread th;
    int rc = -1;
    std::string out;                   // the call's --out
    long long killsAt = 0;             // the second copy's envKillsTotal() when it started
    // Written by the search's own thread as it finishes, read by the ladder's (ready()).
    std::atomic<bool> done{false};
    std::atomic<bool> solved{false};
    std::atomic<long long> deepT{-1};
};

// The gamble's test (cli.hpp PlainElsewhere::ready): finished, and solved or no shallower than
// the ladder's deepest death. A ladder none of whose attempts has died yet may still solve at the
// first, so a plain search that did not solve is not taken before one has.
bool gambleHolds(long long bestT);
Beside g_beside;
std::atomic<long long> g_besideKills{0};   // env kills of the plain searches that were taken

std::string sidePath(const std::string& out) { return out + ".beside"; }

void removeSideFiles(const std::string& out) {
    std::error_code ec;
    std::filesystem::remove(sidePath(out), ec);
    std::filesystem::remove(sidePath(out) + ".trace.csv", ec);
}

void adoptOutcome(const SolveOutcome& s) {
    dp::SearchOutcome& o = dp::g_outcome;
    o.verdict = s.verdict;
    o.horizonCut = s.horizonCut;
    o.deepT = s.deepT;
    o.deepX = s.deepX;
    o.capHits = s.capHits;
    o.workStates = s.workStates;
    o.cancelT = s.cancelT;
    o.resimDead = s.resimDead;
    o.resimFirst = s.resimFirst;
    o.resimWhy = s.resimWhy;
    o.resimUid = s.resimUid;
    o.resimObjX = s.resimObjX;
    o.resimObjY = s.resimObjY;
    o.resimTrig = dp::TouchMask{};
    o.resimTrig.w[0] = s.resimTrig;
    o.resimFrame = s.resimFrame;
    o.replayDiedT = s.replayDiedT;
    o.rejoinT = s.rejoinT;
    o.rejoinBadT = s.rejoinBadT;
    o.rejoinBadWhy = s.rejoinBadWhy;
    o.needTrigMask = dp::TouchMask{};
    o.needTrigMask.w[0] = s.needTrigMask;
    o.needTrigPassed = dp::TouchMask{};
    o.needTrigPassed.w[0] = s.needTrigPassed;
    o.seedRotQ = s.seedRotQ;
    o.rotQOrder = s.rotQOrder;
    o.coinGates = s.coinGates;
    o.coinNoPrune = s.coinNoPrune;
    o.startRotHit = s.startRotHit;
    o.startRotGiven = s.startRotGiven;
    o.startRotMiss = s.startRotMiss;
    o.trigWinTouch = s.trigWinTouch;
    o.trigTotal = s.trigTotal;
    o.trigRelevantN = s.trigRelevantN;
    o.trigKept = s.trigKept;
    o.trigDroppedRelevant = s.trigDroppedRelevant;
    o.trigDroppedBehind = s.trigDroppedBehind;
    o.trigDroppedAhead = s.trigDroppedAhead;
    o.trigMaxKeptX = s.trigMaxKeptX;
    o.trigMapSig = s.trigMapSig;
    o.unsupported = s.unsupported;
}

bool besideStart(const std::vector<std::string>& argv) {
    if (!g_beside.enabled) return false;
    // The checkpoint channel flies the plain search's checkpoints while it runs and waits for
    // their judgement; the second copy has no such channel, so with it on the ladder searches the
    // plain search itself.
    if (dp::g_check.enabled.load(std::memory_order_acquire)) return false;
    std::vector<std::string> a = argv;
    std::string out;
    for (size_t i = 0; i + 1 < a.size(); ++i) {
        // outputs this does not move into place: leave such a call to the ladder
        if (a[i] == "--bands" || a[i] == "--snaplog" || a[i] == "--histstat") return false;
        if (a[i] == "--out") {
            out = a[i + 1];
            a[i + 1] = sidePath(out);
        }
    }
    if (out.empty()) return false;
    removeSideFiles(out);
    g_beside.out = out;
    g_beside.rc = -1;
    g_beside.done.store(false);
    g_beside.solved.store(false);
    g_beside.deepT.store(-1);
    second::takeOutput();   // nothing is left from a search before this one
    second::cancel(false);
    g_beside.killsAt = second::envKillsTotal();
    g_beside.th = std::thread([a, csv = dp::g_levelCsv] {
        g_beside.rc = second::solve(csv, a);
        const SolveOutcome o = second::outcome();
        g_beside.solved.store(o.verdict == OutcomeSolved);
        g_beside.deepT.store(o.deepT);
        g_beside.done.store(true, std::memory_order_release);
        // the gamble: stop the ladder's attempt in progress if this is already what it will take
        if (g_beside.gamble && gambleHolds(dp::g_ladderBestT.load()))
            dp::g_ladderStop.store(true);
    });
    return true;
}

bool gambleHolds(long long bestT) {
    if (!g_beside.done.load(std::memory_order_acquire)) return false;
    if (g_beside.solved.load()) return true;
    return bestT >= 0 && g_beside.deepT.load() >= bestT;
}

bool besideReady(long long bestT) { return gambleHolds(bestT); }

int besideTake() {
    if (g_beside.th.joinable()) g_beside.th.join();
    // what it printed, where the ladder's own plain search would have printed it
    const std::string said = second::takeOutput();
    std::fputs(said.c_str(), stdout);
    std::fflush(stdout);
    // Only the files it wrote: a plain search that returned before writing a plan leaves the
    // last attempt's in place, as it would have in this copy.
    std::error_code ec;
    const std::string side = sidePath(g_beside.out);
    if (std::filesystem::exists(side, ec)) std::filesystem::rename(side, g_beside.out, ec);
    if (std::filesystem::exists(side + ".trace.csv", ec))
        std::filesystem::rename(side + ".trace.csv", g_beside.out + ".trace.csv", ec);
    adoptOutcome(second::outcome());
    g_besideKills.fetch_add(second::envKillsTotal() - g_beside.killsAt,
                            std::memory_order_relaxed);
    return g_beside.rc;
}

void besideDrop() {
    second::cancel(true);
    if (g_beside.th.joinable()) g_beside.th.join();
    second::cancel(false);
    second::takeOutput();
    removeSideFiles(g_beside.out);
}

// ---- the instrument (plainBeside(2)): the plain search in both copies, compared --------------

bool sameFile(const std::string& a, const std::string& b) {
    std::error_code ec;
    const bool ea = std::filesystem::exists(a, ec), eb = std::filesystem::exists(b, ec);
    if (ea != eb) return false;
    if (!ea) return true;
    std::string x, y;
    return dp::readFileBytes(a, x) && dp::readFileBytes(b, y) && x == y;
}

bool sameText(const char* a, const char* b) {
    return (a == nullptr) == (b == nullptr) && (a == nullptr || std::strcmp(a, b) == 0);
}

// The fields of two outcomes that differ, by name; empty when they agree.
std::string outcomeDiff(const SolveOutcome& a, const SolveOutcome& b) {
    std::string d;
    auto f = [&](bool same, const char* name) {
        if (!same) d += std::string(d.empty() ? "" : ",") + name;
    };
    f(a.verdict == b.verdict, "verdict");
    f(a.horizonCut == b.horizonCut, "horizonCut");
    f(a.deepT == b.deepT, "deepT");
    f(a.deepX == b.deepX, "deepX");
    f(a.capHits == b.capHits, "capHits");
    f(a.workStates == b.workStates, "workStates");
    f(a.cancelT == b.cancelT, "cancelT");
    f(a.resimDead == b.resimDead, "resimDead");
    f(a.resimFirst == b.resimFirst, "resimFirst");
    f(sameText(a.resimWhy, b.resimWhy), "resimWhy");
    f(a.resimUid == b.resimUid, "resimUid");
    f(a.resimObjX == b.resimObjX && a.resimObjY == b.resimObjY, "resimObj");
    f(a.resimTrig == b.resimTrig, "resimTrig");
    f(a.resimFrame == b.resimFrame, "resimFrame");
    f(a.replayDiedT == b.replayDiedT, "replayDiedT");
    f(a.rejoinT == b.rejoinT, "rejoinT");
    f(a.rejoinBadT == b.rejoinBadT, "rejoinBadT");
    f(sameText(a.rejoinBadWhy, b.rejoinBadWhy), "rejoinBadWhy");
    f(a.needTrigMask == b.needTrigMask && a.needTrigPassed == b.needTrigPassed, "needTrig");
    f(a.seedRotQ == b.seedRotQ, "seedRotQ");
    f(a.rotQOrder == b.rotQOrder, "rotQOrder");
    f(a.coinGates == b.coinGates, "coinGates");
    f(a.coinNoPrune == b.coinNoPrune, "coinNoPrune");
    f(a.startRotHit == b.startRotHit && a.startRotGiven == b.startRotGiven
          && a.startRotMiss == b.startRotMiss, "startRot");
    f(a.trigWinTouch == b.trigWinTouch && a.trigTotal == b.trigTotal
          && a.trigRelevantN == b.trigRelevantN && a.trigKept == b.trigKept
          && a.trigDroppedRelevant == b.trigDroppedRelevant
          && a.trigDroppedBehind == b.trigDroppedBehind
          && a.trigDroppedAhead == b.trigDroppedAhead && a.trigMaxKeptX == b.trigMaxKeptX
          && a.trigMapSig == b.trigMapSig, "trig");
    f(a.unsupported == b.unsupported, "unsupported");
    return d;
}

std::mutex g_checkM;
std::string g_checkLast;   // the last comparison's line, until the caller takes it
long long g_checked = 0, g_differed = 0;

void besideCheck(int rc) {
    if (g_beside.th.joinable()) g_beside.th.join();
    second::takeOutput();   // this copy printed its own
    std::string d;
    auto add = [&](const std::string& s) { d += std::string(d.empty() ? "" : " ") + s; };
    if (rc != g_beside.rc) add("rc " + std::to_string(rc) + "/" + std::to_string(g_beside.rc));
    const std::string side = sidePath(g_beside.out);
    // A plain search that writes no plan leaves the last attempt's in this copy's files and none
    // in the side files: said as such rather than compared.
    std::error_code ec;
    if (!std::filesystem::exists(side, ec)) {
        add("noplan-beside");
    } else {
        if (!sameFile(g_beside.out, side)) add("plan");
        if (!sameFile(g_beside.out + ".trace.csv", side + ".trace.csv")) add("trace");
    }
    const std::string od = outcomeDiff(outcome(), second::outcome());
    if (!od.empty()) add("outcome(" + od + ")");
    removeSideFiles(g_beside.out);
    const std::string line = d.empty() ? std::string("capladder: beside check: same")
                                       : "capladder: beside check: DIFFERS " + d;
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
    std::lock_guard<std::mutex> g(g_checkM);
    ++g_checked;
    if (!d.empty()) ++g_differed;
    g_checkLast = line + " (" + std::to_string(g_differed) + " of " + std::to_string(g_checked)
                  + " differ so far)";
}

}  // namespace

void plainBeside(int mode) {
    g_beside.enabled = mode > 0;
    g_beside.gamble = mode == 3;
    if (mode > 0)
        dp::g_plainElsewhere = dp::PlainElsewhere{besideStart, besideTake, besideDrop,
                                                  mode == 2 ? besideCheck : nullptr,
                                                  mode == 3 ? besideReady : nullptr};
    else
        dp::g_plainElsewhere = dp::PlainElsewhere{};
}

std::string besideCheckTaken() {
    std::lock_guard<std::mutex> g(g_checkM);
    std::string s;
    s.swap(g_checkLast);
    return s;
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
    second::cancel(on);   // and the plain search beside the ladder, if one is running
}
long long envKillsTotal() {
    return dp::g_envKills.load(std::memory_order_relaxed)
           + g_besideKills.load(std::memory_order_relaxed);
}
std::string coreVersion() {
    // No version string exists in dp/ yet; the compile stamp of this TU is what identifies
    // the core that is linked in, and it moves whenever dp/ is rebuilt
    return std::string("dp core built ") + __DATE__ + " " + __TIME__;
}

std::string defaultsProfile() {
    dp::resetInvocationState();
    const std::string list = dp::defaultsProfile();
    uint64_t h = 1469598103934665603ULL;   // FNV-1a of the list, so two runs compare by one field
    for (char c : list) {
        h ^= (uint8_t)c;
        h *= 1099511628211ULL;
    }
    std::string core = coreVersion();
    for (char& c : core) if (c == ' ') c = '_';
    char fnv[24];
    std::snprintf(fnv, sizeof(fnv), "%016llx", (unsigned long long)h);
    return "core=" + core + " fnv=" + fnv + " " + list;
}

}  // namespace dpbridge
