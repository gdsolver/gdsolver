#pragma once
// The mod's view of the solver core (dp/).
//
// Narrow on purpose, and free of BOTH Geode and dp types. dp/ is compiled in its own
// translation unit with no cocos or Windows headers in scope -- their macros (min, max, near,
// far, small) do not survive contact with it -- and in return the mod never sees a dp:: type.
// Everything crosses this boundary as plain C++: the types below are dp/src/dpcore.hpp's, the
// core's own face, which names no dp:: type either.
//
// This is the seam Stage B is built on: the mod hands the solver the very same objrects CSV it
// writes to disk for the CLI, so the two share one parser and one model.
#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "dpcore.hpp"

namespace dpbridge {

// What the parser made of an objrects CSV (dpcore.hpp has the fields).
using dpcore::LevelStats;
// Where a running search has got to (dpcore.hpp).
using dpcore::SolveProgress;
// What the last solveInProcess call concluded, and its verdicts (dpcore.hpp).
using dpcore::SolveOutcome;
using dpcore::OutcomeFailed;
using dpcore::OutcomePartial;
using dpcore::OutcomeSolved;
using dpcore::OutcomeCancelled;
// A lineage to fly while the search is still running (dpcore.hpp).
using dpcore::SolveCheckpoint;

// Parse a CSV held in memory (the same bytes that go into objrects.txt).
LevelStats statsFromCsv(const std::string& csv);

// Run one solve in this process, on the level held in `csv`, with the CLI's own arguments
// (argv[0] and the level path are supplied here; pass the rest, e.g. {"--out", path,
// "--horizon", "3000"}). Returns the CLI's exit code.
//
// It is the CLI's entry point that runs, not a copy of it -- see dp/cli.hpp. Blocking and
// long: call it from a worker thread, never from GD's main thread.
int solveInProcess(const std::string& csv, const std::vector<std::string>& args);
// The caller's content signature per file argument (repair.hpp fileSig of a recording the mod
// wrote, taken while writing it): dp's groups cache skips reading a file whose signature matches
// the one it parsed. Set before a call, for both core copies; an empty map is the CLI's behaviour.
void setFileSigHints(const std::unordered_map<std::string, std::string>& hints);

// Let go of what both core copies keep from one call to the next (the parsed recordings, the level
// text's tables, the last --replay's level): for when the solving is over, so a solved level does
// not hold them for the rest of the game. Refused (false) while a search is in flight in either
// copy -- the caller asks again later; true when both were emptied.
bool releaseCaches();
// Roughly how many bytes those caches hold allocated, both copies together (dp cli.hpp
// crossCallCacheBytes). The same rule: not while a search beside the ladder may be running -- then 0.
std::size_t cacheBytes();
// ...the moving-geometry recordings' part of them (dp cli.hpp crossCallGroupBytes); the same rule.
std::size_t groupCacheBytes();

// One line naming the build of the solver core that is linked in. Written to result.txt at
// session start so a run can always say which solver produced it.
std::string coreVersion();
// The core's built-in default profile: `core=<build> fnv=<hash of the list> name=value ...`, every
// global a call's reset sets to a constant, read right after that reset (tools/gen_effective_cfg.py
// generates the list). Resets the core's per-call globals, so only while no solve is running.
std::string defaultsProfile();

// Where the search has got to, sampled from another thread while solveInProcess runs. The CLI
// shows the same three numbers by printing a line every 500 ticks; on screen they have to be
// readable at any moment, so they are polled instead.
SolveProgress progress();

// What the last solveInProcess call concluded. Read after solveInProcess returns.
SolveOutcome outcome();
// Seconds into the last call at the end of each part of its preparation (dp::g_prepMark: arguments,
// group recordings, touch/auto triggers, the level, the rest of the tables; -1 = not reached).
std::vector<double> prepMarks();
// The stages of the last call (dp::g_callStages): one entry per search or replay the call ran, each with its seconds and its own cap ladder's account.
std::string callStages();

// ---- checkpoints: lineages to fly while the search is still running ------------------------
//
// At fixed layers past its anchor the search publishes the lineage of its frontier's first state,
// cut at that layer (dp/progress.hpp has the schedule and the argument). The caller flies them in
// order and judges each: passCheckpoint when the flight reached its last tick alive, cancelSearch
// when the game killed it strictly inside. The search does not return an answer until every
// checkpoint it published has been judged, so nothing about the outcome depends on which thread
// got there first. No arena index crosses this seam; a checkpoint is ticks and input edges.

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
// judgements), publishes OutcomeCancelled, writes no plan and returns a distinct rc -- whether or
// not the checkpoint channel is subscribed. Cleared by the caller before the next search call.
void cancelSearch(bool on);
// Kills by the Area Move boxes (hazard twins) over the life of the process (dp::g_envKills);
// take the difference across the span to be counted.
long long envKillsTotal();

// The cap ladder's plain search beside the ladder (dp cli.hpp PlainElsewhere), from a second copy
// of the core: 1, a ladder starts its plain search at once on a thread of its own and takes its
// result where it would have searched it; what the call returns is the same, only sooner. 0, the
// ladder searches it itself after the attempts. 2, the instrument: both, the ladder's own result
// kept and the other compared with it (plan, trace, exit code, outcome) -- besideCheckTaken.
// 3, the gamble: 1, and the plain search taken as soon as it has finished and solved or died no
// earlier than the ladder's deepest death so far -- not reproducible (dp cli.hpp
// PlainElsewhere::ready). Only between solves.
void plainBeside(int mode);
// The last comparison's line under plainBeside(2), taken (empty when this call made none).
std::string besideCheckTaken();

// The run: one level's solve, across its calls (dp search_dp.hpp SearchRun). Both copies of the
// core are told, so neither can carry a run into the next level. runBegin refuses (false, `why`)
// while a run is open in either copy. runVerified reports a flight of `flown` (its input edges):
// died at `deathT` / `x`, or cleared; `divT` the first tick the recorder saw the model and GD part,
// -1 for none. runEnd closes it. Each returns or fills the line the caller logs.
bool runBegin(int level, unsigned long long seed, std::string& why);
std::string runVerified(const std::vector<std::pair<long long, int>>& flown, long long deathT,
                        double x, bool cleared, long long divT);
std::string runEnd();
// The section search spliced a plan in (all of its edges, press / level) from tick `t0` on: a flight
// of it is reported as the section search's (`via=secsolve`), not as a plan the core emitted.
void runNoteSplice(const std::vector<std::pair<long long, int>>& edges, long long t0);

// The second copy of the core (dp_bridge2.cpp). Nothing but plainBeside's hooks calls it.
namespace second {
int solve(const std::string& csv, const std::vector<std::string>& argv);   // argv[0] included
void setFileSigHints(const std::unordered_map<std::string, std::string>& hints);
SolveOutcome outcome();
// What it printed since the last call, taken: its stdout is held rather than interleaved with the
// first copy's (dp_bridge2.cpp).
std::string takeOutput();
void cancel(bool on);
long long envKillsTotal();
void releaseCaches();
std::size_t cacheBytes();
std::size_t groupCacheBytes();
bool runBegin(int level, unsigned long long seed, std::string& why);
std::string runVerified(const std::vector<std::pair<long long, int>>& flown, long long deathT,
                        double x, bool cleared, long long divT);
std::string runEnd();
void runNoteSplice(const std::vector<std::pair<long long, int>>& edges, long long t0);
}  // namespace second
}  // namespace dpbridge
