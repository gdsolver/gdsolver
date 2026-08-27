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
enum { OutcomeFailed = 0, OutcomePartial = 1, OutcomeSolved = 2 };

struct SolveOutcome {
    int verdict = OutcomeFailed;
    long long deepT = -1;
    double deepX = -1.0;
    long long capHits = -1;
    long long replayDiedT = -1;   // --replay only: where the model died, -1 = it survived
    // Touch boxes the call required, and those the anchor already sits past. A required box
    // behind the anchor can never be entered, so the frontier is empty before the first tick --
    // which looks exactly like an impassable level unless you can see this.
    unsigned needTrigMask = 0, needTrigPassed = 0;
};
SolveOutcome outcome();

}  // namespace dpbridge
