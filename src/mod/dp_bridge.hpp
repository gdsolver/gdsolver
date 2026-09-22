#pragma once
// The mod's view of the solver core (dp/).
//
// Narrow on purpose, and free of BOTH Geode and dp types. dp/ is compiled in its own
// translation unit with no cocos or Windows headers in scope -- their macros (min, max, near,
// far, small) do not survive contact with it -- and in return the mod never sees a dp:: type.
// Everything crosses this boundary as plain C++.
//
// This is the seam Stage B is built on: the mod hands the solver the very same objrects CSV it
// writes to disk for the CLI, so the two share one parser and one model.
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace dpbridge {

// What the parser made of an objrects CSV. Counts and extents only -- enough to compare "the
// level the mod built in memory" with "the level the CLI reads from the dump" without either
// side knowing the other's types.
// The fields are the ones the CLI prints in its own `level:` line, so the two can be compared
// term by term (that comparison is the Stage B acceptance).
struct LevelStats {
    bool ok = false;
    std::size_t objs = 0, portals = 0, pads = 0, orbs = 0, moving = 0;
    double maxX = 0.0;
};

// Parse a CSV held in memory (the same bytes that go into objrects.txt).
LevelStats statsFromCsv(const std::string& csv);

// Run one solve in this process, on the level held in `csv`, with the CLI's own arguments
// (argv[0] and the level path are supplied here; pass the rest, e.g. {"--out", path,
// "--horizon", "3000"}). Returns the CLI's exit code.
//
// It is the CLI's entry point that runs, not a copy of it -- see dp/cli.hpp. Blocking and
// long: call it from a worker thread, never from GD's main thread.
int solveInProcess(const std::string& csv, const std::vector<std::string>& args);

// One line naming the build of the solver core that is linked in. Written to result.txt at
// session start so a run can always say which solver produced it.
std::string coreVersion();

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
SolveProgress progress();

// What the last solveInProcess call concluded. The CLI prints this and the Python driver parses
// it back out of stdout; in the mod there is no pipe, so the core publishes it directly (see
// dp/progress.hpp). Read after solveInProcess returns.
//
// The repair loop needs all four: `verdict` decides whether the tail can be spliced, `deepT`
// is how far the model thinks it gets (a doomed tail that reaches past the current wall is
// still progress), and `capHits` says whether a bigger capacity could change the answer.
// OutcomeCancelled is NOT "this anchor has nothing": the caller stopped the search itself
// (cancelSearch below) because the game refuted one of its checkpoints. No plan file is
// written for it, and a caller that reads it as a failure would escalate or give up on an anchor
// that was never answered.
enum { OutcomeFailed = 0, OutcomePartial = 1, OutcomeSolved = 2, OutcomeCancelled = 3 };

struct SolveOutcome {
    int verdict = OutcomeFailed;
    long long deepT = -1;
    double deepX = -1.0;
    long long capHits = -1;
    // OutcomeCancelled only: the layer the search was on when it saw the cancel. -1 otherwise.
    long long cancelT = -1;
    // Ticks of the emitted plan on which the model itself fired a kill, counted
    // on the plan's own walk. -1 = the walk never ran, which is NOT zero.
    long long resimDead = -1, resimFirst = -1;
    const char* resimWhy = nullptr;   // string literal; dp is linked in here
    // ...and the killer at that tick. A cause is a category; this is an object,
    // so a row can be joined to the dump and to another walk's row. uid is the
    // LEVEL's, not this build's ordinal. -1 = the walk found no death.
    int resimUid = -1;
    float resimObjX = 0.f, resimObjY = 0.f;
    // WIDE ON PURPOSE. This mirrors dp's touch mask, whose width is a single
    // constant over there (kTouchBits). This header deliberately includes no
    // dp/ header, so it cannot name that type -- and a 32-bit field here would
    // truncate the moment the constant moves, on the far side of a boundary
    // where nothing would say so.
    unsigned long long resimTrig = 0;
    int resimFrame = -1;              // the frame resimObjX/Y are read in
    long long replayDiedT = -1;   // --replay only: where the model died, -1 = it survived
    long long rejoinT = -1;       // --rejoinuse: where the search joined, -1 = no
    long long rejoinBadT = -1;    // ...and where the joined walk left the old plan's, -1 = never
    const char* rejoinBadWhy = nullptr;
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
};
SolveOutcome outcome();

// ---- checkpoints: lineages to fly while the search is still running ------------------------
//
// At fixed layers past its anchor the search publishes the lineage of its frontier's first state,
// cut at that layer (dp/progress.hpp has the schedule and the argument). The caller flies them in
// order and judges each: passCheckpoint when the flight reached its last tick alive, cancelSearch
// when the game killed it strictly inside. The search does not return an answer until every
// checkpoint it published has been judged, so nothing about the outcome depends on which thread
// got there first. No arena index crosses this seam; a checkpoint is ticks and input edges.
struct SolveCheckpoint {
    unsigned long long call = 0;   // the solve call that published it
    std::size_t index = 0;         // its position in that call's order
    long long t0 = 0;              // the anchor the call searches from
    long long tick = 0;            // the last tick its inputs cover
    // `input=press,level`, through the plan writer's own per-mode latency conversion.
    std::vector<std::pair<long long, int>> edges;
};

// Turn the channel on for this process. With it off the search does no extra work, waits for
// nothing and prints nothing, so the CLI's output is what it was without any of this.
void checkSubscribe(bool on);
// The next checkpoint to judge in the call that owns the channel -- read with the call id and the
// judged count under one lock, so the three always belong together. False when there is none yet.
bool nextCheckpoint(SolveCheckpoint& out);
// Just the owning call's id, with no lock: cheap enough to ask on every physics tick.
unsigned long long checkCall();
// The verdict that checkpoint `index` of `call` survived. Refused (false) for a call that no
// longer owns the channel or for an index out of order.
bool passCheckpoint(unsigned long long call, std::size_t index);

// Ask the search in flight to stop. It leaves the layer loop at the next layer (or its wait for
// judgements), publishes OutcomeCancelled, writes no plan and returns a distinct rc. Cleared by
// the caller before the next search call.
void cancelSearch(bool on);
// Kills by the Area Move boxes (hazard twins) over the life of the process (dp::g_envKills);
// take the difference across the span to be counted.
long long envKillsTotal();
}  // namespace dpbridge
