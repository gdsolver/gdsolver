// The mod's side of the solver core: dpbridge (dp_bridge.hpp), composed of the core's two copies.
//
// The core itself is compiled elsewhere, without Geode or cocos in scope: the first copy once for
// both the CLI and the mod (dp/src/dpcore.cpp, library gdsolver_dpcore), the second in
// dp_bridge2.cpp (gdsolver_dpcore2), both with the CLI's options (dp/CMakeLists.txt). This file
// includes no dp/ header -- only their plain-C++ faces -- so the whole of the mod's contact with
// the solver goes through dp_bridge.hpp, which mentions neither library.
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>

#include "mod/dp_bridge.hpp"

namespace dpbridge {

LevelStats statsFromCsv(const std::string& csv) { return dpcore::statsFromCsv(csv); }

void setFileSigHints(const std::unordered_map<std::string, std::string>& hints) {
    dpcore::setFileSigHints(hints);
    second::setFileSigHints(hints);
}

int solveInProcess(const std::string& csv, const std::vector<std::string>& args) {
    return dpcore::solveInProcess(csv, args);
}

std::vector<double> prepMarks() { return dpcore::prepMarks(); }
std::string callStages() { return dpcore::callStages(); }

SolveProgress progress() { return dpcore::progress(); }

SolveOutcome outcome() { return dpcore::outcome(); }

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

bool besideStart(const std::vector<std::string>& argv) {
    if (!g_beside.enabled) return false;
    // The checkpoint channel flies the plain search's checkpoints while it runs and waits for
    // their judgement; the second copy has no such channel, so with it on the ladder searches the
    // plain search itself.
    if (dpcore::checkEnabled()) return false;
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
    g_beside.th = std::thread([a, csv = dpcore::levelCsv()] {
        g_beside.rc = second::solve(csv, a);
        const SolveOutcome o = second::outcome();
        g_beside.solved.store(o.verdict == OutcomeSolved);
        g_beside.deepT.store(o.deepT);
        g_beside.done.store(true, std::memory_order_release);
        // the gamble: stop the ladder's attempt in progress if this is already what it will take
        if (g_beside.gamble && gambleHolds(dpcore::ladderBestT()))
            dpcore::ladderStop();
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
    dpcore::adoptOutcome(second::outcome());
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
    return dpcore::readFileBytes(a, x) && dpcore::readFileBytes(b, y) && x == y;
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
    f(a.resimKind == b.resimKind, "resimKind");
    f(a.dumpT == b.dumpT && a.dumpStart == b.dumpStart, "dump");
    f(a.startStateT == b.startStateT, "startStateT");
    f(a.resimUid == b.resimUid, "resimUid");
    f(a.resimObjX == b.resimObjX && a.resimObjY == b.resimObjY, "resimObj");
    f(a.resimTrig == b.resimTrig, "resimTrig");
    f(a.resimFrame == b.resimFrame, "resimFrame");
    f(a.replayDiedT == b.replayDiedT, "replayDiedT");
    f(a.rejoinT == b.rejoinT, "rejoinT");
    f(a.rejoinBadT == b.rejoinBadT, "rejoinBadT");
    f(sameText(a.rejoinBadWhy, b.rejoinBadWhy), "rejoinBadWhy");
    f(a.rejoinRefusedT == b.rejoinRefusedT && a.rejoinRefusedDieT == b.rejoinRefusedDieT && a.rejoinRefusedN == b.rejoinRefusedN, "rejoinRefused");
    f(a.rejoinShortT == b.rejoinShortT && a.rejoinShortEndT == b.rejoinShortEndT && a.rejoinShortN == b.rejoinShortN, "rejoinShort");
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
        dpcore::setPlainElsewhere(dpcore::PlainElsewhereHooks{besideStart, besideTake, besideDrop,
                                                              mode == 2 ? besideCheck : nullptr,
                                                              mode == 3 ? besideReady : nullptr});
    else
        dpcore::setPlainElsewhere(dpcore::PlainElsewhereHooks{});
}

bool releaseCaches() {
    // The plain search beside the ladder runs on a thread of its own and is joined only when the
    // ladder takes or drops it; while it may still be running, its copy's tables are in use. The
    // first copy's caller (the loop's worker) is the mod's to rule out (repair.hpp).
    if (g_beside.th.joinable()) return false;
    dpcore::releaseCaches();
    second::releaseCaches();
    return true;
}

std::size_t cacheBytes() {
    if (g_beside.th.joinable()) return 0;
    return dpcore::cacheBytes() + second::cacheBytes();
}
std::size_t groupCacheBytes() {
    if (g_beside.th.joinable()) return 0;
    return dpcore::groupCacheBytes() + second::groupCacheBytes();
}

std::string besideCheckTaken() {
    std::lock_guard<std::mutex> g(g_checkM);
    std::string s;
    s.swap(g_checkLast);
    return s;
}

void checkSubscribe(bool on) { dpcore::checkSubscribe(on); }

bool nextCheckpoint(SolveCheckpoint& out) { return dpcore::nextCheckpoint(out); }

unsigned long long checkCall() { return dpcore::checkCall(); }

bool passCheckpoint(unsigned long long call, std::size_t index) {
    return dpcore::passCheckpoint(call, index);
}

void cancelSearch(bool on) {
    dpcore::cancel(on);
    second::cancel(on);   // and the plain search beside the ladder, if one is running
}
long long envKillsTotal() {
    return dpcore::envKillsTotal() + g_besideKills.load(std::memory_order_relaxed);
}

bool runBegin(int level, unsigned long long seed, std::string& why) {
    // Both copies are asked before either opens: a refusal leaves neither half-begun.
    if (dpcore::runOpen()) return dpcore::runBegin(level, seed, why);
    std::string why2;
    if (!second::runBegin(level, seed, why2)) {
        why = "the second core: " + why2;
        return false;
    }
    return dpcore::runBegin(level, seed, why);
}
std::string runVerified(const std::vector<std::pair<long long, int>>& flown, long long deathT,
                        double x, bool cleared, long long divT) {
    second::runVerified(flown, deathT, x, cleared, divT);
    return dpcore::runVerified(flown, deathT, x, cleared, divT);
}
void runNoteSplice(const std::vector<std::pair<long long, int>>& edges, long long t0) {
    second::runNoteSplice(edges, t0);
    dpcore::runNoteSplice(edges, t0);
}
std::string runEnd() {
    const std::string s2 = second::runEnd();
    std::string s = dpcore::runEnd();
    if (s2.find("no run open") == std::string::npos) s += " | second core: " + s2;
    return s;
}
std::string coreVersion() { return dpcore::coreVersion(); }

std::string defaultsProfile() { return dpcore::defaultsProfile(); }

}  // namespace dpbridge
