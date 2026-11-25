#pragma once
// The solver core's entry points: the narrow face of dp/ that is compiled once (dpcore.cpp) and
// linked by both the leveldp CLI and the Geode mod.
//
// Plain C++ only, and no other dp/ header: everything crosses this boundary as standard types, so
// a caller compiles without the solver's headers -- the mod with its Windows/cocos macro soup in
// scope, the CLI's main with nothing. The hot loop (search and step) stays inside dpcore.cpp's one
// translation unit, where it was compiled before, and keeps its inlining.
//
// The types are the ones the mod sees as dpbridge::* (src/mod/dp_bridge.hpp has the contract of
// each call); the mod's second copy of the core fills the same types (src/mod/dp_bridge2.cpp).
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dpcore {

// What the parser made of an objrects CSV. Counts and extents only -- enough to compare "the
// level the mod built in memory" with "the level the CLI reads from the dump" without either
// side knowing the other's types.
// The fields are the ones the CLI prints in its own `level:` line, so the two can be compared
// term by term (that comparison is the Stage B acceptance).
struct LevelStats {
    bool ok = false;
    std::size_t objs = 0, portals = 0, pads = 0, orbs = 0, moving = 0;
    double maxX = 0.0;
    // Non-empty when the parser refused the level (dp Level::unsupported, e.g. more
    // gravity portals than the latch holds). Nothing may be solved on it then.
    std::string unsupported;
};

// Where the search has got to, sampled from another thread while solveInProcess runs. The CLI
// shows the same three numbers by printing a line every 500 ticks; on screen they have to be
// readable at any moment, so they are polled instead.
struct SolveProgress {
    bool running = false;
    // `from` is where this search resumed, so the span it has to cover is horizon - from. A
    // tail solve does not start at tick 0 and a bar that assumes it does opens a third full.
    long long from = 0, tick = 0, horizon = 0;
    double x = 0.0;
    std::size_t alive = 0;
};

// What the last solveInProcess call concluded. The CLI prints this and the Python driver parses
// it back out of stdout; in the mod there is no pipe, so the core publishes it directly (see
// dp/progress.hpp). Read after solveInProcess returns.
//
// The repair loop needs all four: `verdict` decides whether the tail can be spliced, `deepT`
// is how far the model thinks it gets (a doomed tail that reaches past the current wall is
// still progress), and `capHits` says whether a bigger capacity could change the answer.
// OutcomeCancelled is NOT "this anchor has nothing": the caller stopped the search itself
// (cancelSearch) because the game refuted one of its checkpoints. No plan file is written for
// it, and a caller that reads it as a failure would escalate or give up on an anchor that was
// never answered.
enum { OutcomeFailed = 0, OutcomePartial = 1, OutcomeSolved = 2, OutcomeCancelled = 3 };

struct SolveOutcome {
    int verdict = OutcomeFailed;
    // OutcomeSolved by outliving --horizon rather than by reaching the end (dp progress.hpp).
    bool horizonCut = false;
    long long deepT = -1;
    double deepX = -1.0;
    long long capHits = -1;
    long long workStates = 0;   // dp's SearchOutcome::workStates: the search's own work
    // OutcomeCancelled only: the layer the search was on when it saw the cancel. -1 otherwise.
    long long cancelT = -1;
    // Ticks of the emitted plan on which the model itself fired a kill, counted
    // on the plan's own walk. -1 = the walk never ran, which is NOT zero.
    long long resimDead = -1, resimFirst = -1;
    const char* resimWhy = nullptr;   // string literal; dp is linked in here
    int resimKind = -1;               // its DIE kind (0 physics, 1 rule, 2 prune), -1 = no death
    // ...and the killer at that tick. A cause is a category; this is an object,
    // so a row can be joined to the dump and to another walk's row. uid is the
    // LEVEL's, not this build's ordinal. -1 = the walk found no death.
    int resimUid = -1;
    float resimObjX = 0.f, resimObjY = 0.f;
    // WIDE ON PURPOSE. This mirrors dp's touch mask, whose width is a single
    // constant over there (kTouchBits). This header deliberately includes no
    // other dp/ header, so it cannot name that type -- and a 32-bit field here would
    // truncate the moment the constant moves, on the far side of a boundary
    // where nothing would say so.
    unsigned long long resimTrig = 0;
    int resimFrame = -1;              // the frame resimObjX/Y are read in
    long long replayDiedT = -1;   // --replay only: where the model died, -1 = it survived
    long long rejoinT = -1;       // --rejoinuse: where the search joined, -1 = no
    long long rejoinBadT = -1;    // ...and where the joined walk left the old plan's, -1 = never
    const char* rejoinBadWhy = nullptr;
    long long rejoinRefusedT = -1;      // --rejoinprobe: the join this call refused (its walk died at or past it) before the search whose answer this is, -1 = none
    long long rejoinRefusedDieT = -1;   // ...and the tick that walk died on
    long long rejoinRefusedN = 0;       // ...and the joins the call refused in all
    long long rejoinShortT = -1;        // --rejoinuse: the first join not taken because the old plan's inputs end short of the goal and of the call's end, -1 = none
    long long rejoinShortEndT = -1;     // ...the tick those inputs end at
    long long rejoinShortN = 0;         // ...and the layers that declined one
    // Touch boxes the call required, and those the anchor already sits past. A required box
    // behind the anchor can never be entered, so the frontier is empty before the first tick --
    // which looks exactly like an impassable level unless you can see this.
    unsigned long long needTrigMask = 0, needTrigPassed = 0;   // wide: see resimTrig
    // --seeddump only: the ready-made `--startrotq` argument for the dumped
    // tick. Empty unless the call asked for a dump and the level has a queue.
    // It crosses here rather than being re-derived on this side because the
    // bit->uid inversion depends on buildRotQueue's ordering, which only dp
    // holds -- a caller reproducing it would be keeping a copy of a proxy.
    std::string seedRotQ;
    // The queue in dp's own order and --startrotq's read-back (dp progress.hpp
    // has what each field means). Same reason as seedRotQ: the ordering lives in
    // dp, and the mod has no pipe to read dp's `startrotq:` line.
    std::string rotQOrder;
    // Where a tap-gated coin is lost for good (dp progress.hpp, coinGates).
    std::string coinGates;
    // The coins dp does not call missed when passed (dp progress.hpp, coinNoPrune).
    std::string coinNoPrune;
    int startRotHit = -1, startRotGiven = -1;
    std::string startRotMiss;
    // What the touch window did on this call (dp progress.hpp has what each
    // field means). Here because dp's stdout does not reach this side: the
    // coverage line and the auto-window's own printf are invisible in-process,
    // and a grep of result.txt for either returns 0 whether it fired or not.
    // trigWinTouch is -1 when the call had no touch boxes -- not 0.
    int trigWinTouch = -1;
    long long trigTotal = 0, trigRelevantN = 0, trigKept = 0,
              trigDroppedRelevant = 0;
    // ...and which side of the window they fell off. Only Ahead is a world the
    // call cannot see; Behind is the anchor window working as designed.
    long long trigDroppedBehind = 0, trigDroppedAhead = 0;
    double trigMaxKeptX = 0.0;
    unsigned long long trigMapSig = 0;   // over the kept uids IN BIT ORDER
    // The load refused the level (dp Level::unsupported); the call returned 2 and
    // searched nothing. Empty otherwise.
    std::string unsupported;
    // The cap ladder's account of the call (dp progress.hpp SearchOutcome::ladder): its
    // attempts, how many never bound their cap, the plain search's plan hash and the chosen
    // plan's. Empty for a call that was not a ladder.
    std::string ladder;
    std::string camy;   // --camy: the call's camera counters (dp/cam_y_obs.hpp camySummary)
    std::string shstep;   // --shadowstep: the call's tick counts, one line (dp/shadow_step.hpp shadowStepSummary)
    // --dumpstate / --start-state (dp statefile.hpp, progress.hpp has the fields' meaning).
    long long dumpT = -1;
    std::string dumpStart;
    long long startStateT = -1;
};

// A lineage the search published while still running, to be flown and judged (dp/progress.hpp
// has the schedule and the argument). No arena index crosses this seam; a checkpoint is ticks
// and input edges.
struct SolveCheckpoint {
    unsigned long long call = 0;   // the solve call that published it
    std::size_t index = 0;         // its position in that call's order
    long long t0 = 0;              // the anchor the call searches from
    long long tick = 0;            // the last tick its inputs cover
    // `input=press,level`, through the plan writer's own per-mode latency conversion.
    std::vector<std::pair<long long, int>> edges;
};

// ---- the CLI ---------------------------------------------------------------------------------

// leveldp's whole program: dp::cliMain, the cap ladder around the search.
int cliMain(int argc, char** argv);

// ---- the mod's first copy of the core --------------------------------------------------------
//
// Each is the dp side of the dpbridge call of the same name (src/mod/dp_bridge.hpp has the
// contract); the mod composes them with its second copy there.

LevelStats statsFromCsv(const std::string& csv);
void setFileSigHints(const std::unordered_map<std::string, std::string>& hints);
// argv[0] and the level path are supplied here, as in dpbridge::solveInProcess.
int solveInProcess(const std::string& csv, const std::vector<std::string>& args);
std::vector<double> prepMarks();
std::string callStages();
SolveProgress progress();
SolveOutcome outcome();
// The reverse of outcome(): an outcome from the second copy written into this copy's, so that
// what follows in this copy sees what its own plain search would have left.
void adoptOutcome(const SolveOutcome& s);
// The level held for the call in flight (a copy).
std::string levelCsv();
// Let go of the tables kept from one call to the next (dp cli.hpp releaseCrossCallCaches): the
// parsed recordings, the level text's tables, the last --replay's level. Only between calls.
void releaseCaches();
// ...and roughly how many bytes they hold allocated (dp cli.hpp crossCallCacheBytes). Only between calls.
std::size_t cacheBytes();
// ...the recordings' part of them (dp cli.hpp crossCallGroupBytes). Only between calls.
std::size_t groupCacheBytes();
// The checkpoint channel has a subscriber.
bool checkEnabled();
// The cap ladder's deepest death so far, and the stop for its attempt in progress.
long long ladderBestT();
void ladderStop();
// dp cli.hpp PlainElsewhere, by its five hooks; all null = none.
struct PlainElsewhereHooks {
    bool (*start)(const std::vector<std::string>& argv) = nullptr;
    int (*take)() = nullptr;
    void (*drop)() = nullptr;
    void (*check)(int rc) = nullptr;
    bool (*ready)(long long bestT) = nullptr;
};
void setPlainElsewhere(const PlainElsewhereHooks& h);
bool readFileBytes(const std::string& path, std::string& out);
void checkSubscribe(bool on);
bool nextCheckpoint(SolveCheckpoint& out);
unsigned long long checkCall();
bool passCheckpoint(unsigned long long call, std::size_t index);
// Raise or clear the cancel of the search in flight.
void cancel(bool on);
long long envKillsTotal();
// A run is open in this copy.
bool runOpen();
bool runBegin(int level, unsigned long long seed, std::string& why);
std::string runVerified(const std::vector<std::pair<long long, int>>& flown, long long deathT,
                        double x, bool cleared, long long divT);
std::string runEnd();
void runNoteSplice(const std::vector<std::pair<long long, int>>& edges, long long t0);
std::string coreVersion();
std::string defaultsProfile();

}  // namespace dpcore
