#pragma once
// Session configuration (autorun.cfg keys), data root, shared session state.
#include "mod/prelude.hpp"
#include "mod/plan_edges.hpp"   // InputCmd, pushInputMask, writeInputLines
#include <memory>

namespace p1 {

// Root for all input/output. If the launch argument --geode:gdsolver.solver.data-root=<absolute path>
// is given, that is used. Without it, other GD instances on the same machine fight over
// autorun.cfg / result.txt, so workers must always pass it.
// Only absolute paths are accepted; immutable for the lifetime of the process (resolved before
// the first file access)
inline std::string g_dataDirStorage;
inline const char* DATA_DIR = "";
inline bool g_dataDirResolved = false;

inline void resolveDataDir() {
    if (g_dataDirResolved) return;
    g_dataDirResolved = true;
    // The default is the mod's own save directory (geode/config/<mod-id>/). A plain install
    // has no launch argument and no development tree, so this is the only place that is
    // certain to exist and to be writable -- the default used to be a hard-coded path from
    // the developer's machine, and on any other install every write silently went nowhere.
    g_dataDirStorage = Mod::get()->getSaveDir().lexically_normal().generic_string();
    if (auto arg = Mod::get()->getLaunchArgument("data-root"); arg && !arg->empty()) {
        std::filesystem::path requested(*arg);
        // Relative paths depend on the working directory and are dangerous, so reject them
        if (requested.is_absolute()) {
            g_dataDirStorage = requested.lexically_normal().generic_string();
        } else {
            log::error("gdsolver: ignoring non-absolute data-root '{}'", *arg);
        }
    }
    DATA_DIR = g_dataDirStorage.c_str();
    std::error_code ec;
    std::filesystem::create_directories(g_dataDirStorage, ec);
    log::info("gdsolver: data root = {}", DATA_DIR);
}

// Label for the window title (keeps a human's GD and a worker from being mixed up). Built from
// the worker ID in the data root (.../worker-90/...); anything else is "local"
inline std::string workerTag() {
    std::string d = g_dataDirStorage;
    auto p = d.find("worker-");
    if (p == std::string::npos) return "local";
    auto e = d.find_first_of("/\\", p);
    return d.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

inline void updateWindowTitle();   // the definition comes after the g_cfg / g_started declarations

// Set once a snapshot has written a player's bytes back (solver/psnap.hpp restorePlayer): the
// mode flags (m_isShip, m_isRobot, ...) then hold the snapshot's while the mode sprites stay as
// they were -- the mode toggles that show and hide them are never called -- and GD's own toggles
// act only on a flag that changes, so a later reset leaves several modes drawn at once. Here, ahead
// of both, because psnap.hpp and repair.hpp (tidyPlayerModes) read it.
inline bool g_psnapPlayerWritten = false;

// One edge of a button from `step` on: player 1's jump, or (p2) player 2's -- the second button of a two-player level.
// A plan file's `input=<step>,<mask>` is the buttons held from that tick, bit 0 player 1's jump and bit 1 player 2's (plan_io.hpp).
struct ToggleCmd { int step; std::string mode; };

// The glitch-avoid mode's margins when it is simply switched on (the play menu, `glitchavoid=1`),
// each from an official solution the user named as a glitch: lv22's early dash release passes a
// 1x portal 12.3 px clear, and lv17's wave threads 0-4 px over a row of spikes.
// The ship's is NOT from a glitch but from what the level asks for: lv1's third coin lies behind a
// slot exactly as tall as the ship (a step's top at 480, the ceiling at 510), and the coin route
// flies it 11.4 px into the step; over the whole coin solution the ship's box sinks 5.6-12.1 px
// into a block at eleven places, every one of them ordinary flying. At 3 px the mode forbade the
// coin (lv1 coins on, 2/3 on every clear). 14 is the user's choice: above 12.1, under the half
// (15) that the half-sunk ride reaches.
constexpr double kGlitchPortalDefault = 15.0;
constexpr double kGlitchWaveDefault = 5.0;
constexpr double kGlitchEmbedDefault = 14.0;
// The wave-behaviour choice of Avoid glitches (the play menu's third value, `avoidwave=1`): the shortest
// press or release the wave is planned with, in ticks (240 a second), and the clearance it keeps from
// the floor, the ceiling and solids, in px. PROVISIONAL until measured on the official wave sections.
constexpr int kWavePulseDefault = 12;
constexpr double kWaveCentreDefault = 10.0;
// The wave's PREFERENCES (dp --wavetidy / --wavetoggle, thread_pool.hpp): a cost per wave tick that grows with the
// square of how far the wave sits from the middle of its corridor (at most this much, at a wall), and a cost per
// change of the button. Both only order the routes, they drop none. Measured offline on three official wave
// levels, with the floor and the clearance above on: the median button run 12..18 -> 26..30 ticks, the 90th
// percentile of the distance from the middle 0.55..0.62 -> 0.29..0.45 of the half-corridor.
constexpr double kWaveTidyDefault = 8.0;
constexpr int kWaveToggleDefault = 4;

struct Config {
    bool enabled = false;
    int levelId = 1;
    // levelfile=<path>: raw (uncompressed) level string of a custom calibration map.
    // The 22 official levels yield no samples of the physics constants — across the 22
    // verified solutions there are only 5 slope launches in total, and zero launches for
    // ball/robot/spider/UFO and zero |m|=2 for the forward-moving cube (measured 2026-08-17).
    // So when a "constant measured at one point" disagreed there was nothing to decide it
    // with. We build the calibration rig ourselves and have GD load it.
    // Compression is left to GD's own ZipUtils (so we never guess the encoding).
    // The generator is py/mklevel.py; the measured table of placeable objects is
    // py/objpalette.py.
    std::string levelFile;
    // cfg `uishow=<kind>` (diagnostic, default none): show one of the play menu's popups over the main menu 3 s after it appears -- info0 / info1 / info2 (Options' (i)
    // texts), options, menu (the Solve menu for cfg level / levelfile, with Options set to something other than the defaults) -- so a layout can be looked at without playing to it.
    // `press` is the other use: the menu for that level with Start pressed in the mode cfg `uimode` names (1 Replay, 2 Solve), the player's route minus the clicks.
    std::string uiShow;
    // cfg `replaypanel` (0 off, 1 the whole path -- the default --, 2 its last stretch; 3 is read as 1; the replay-panel key goes
    // round full -> recent -> off whatever this says): GD's own record of the deepest verified attempt, with the plan the solver's
    // latest search found joined on from its anchor (the walk the model made of it, in another colour), played in the free
    // area from the middle of the screen to the top-right, with the level, the run's recording of the moving parts and the
    // points where the model was wrong (replay_panel.hpp). Drawn only while a solve runs AND the screen is dark (F5 off), never
    // in the showing of the solution; with the screen on it is hidden, its state kept, and the keys are the game's. It reads
    // what the solver has already kept and writes nothing back; the research harness turns it off (gdtas/worker.py), since
    // nobody watches a worker. `replaypanelspan=<ticks>` is the length of the recent stretch (default 4800 = 20 s),
    // `replaypanelspeed=<x>` the pace it starts at (a notch of the game's spectating speeds; 1 = real time).
    int replayPanel = 1;
    long long replayPanelSpan = 4800;
    // cfg `replaypanelplans=<n>` (default 8): how many of the model's latest plans the panel draws together (the deepest is the path's
    // end and what the camera follows, the rest are ghosts beside it, each a figure of its player's mode; 1 = only the deepest).
    long long replayPanelPlans = 8;
    float replayPanelSpeed = 1.f;
    // levelfiletype=main: build that level as a MAIN (official) level rather than an editor
    // one. Measured 2026-09-22: in an editor-type level GD loads none of the secret coins
    // (id 142) -- the coin rig in py/mklevel.py placed 22 and kept none, and six official
    // spin-off levels with three each came up `pois: ... 0 coins` -- while the same kind of
    // level built as a main level loads all three and credits them (coingd 3/3). User coins
    // (id 1329) load either way: calib_coincal.lvl gives `3 coins` and the same pickup ticks
    // as editor and as main. Off by default: a rig is an editor level, and nothing that
    // already runs changes.
    bool levelFileMain = false;
    int maxAttempts = 1;
    float delaySec = 2.f;
    bool quitWhenDone = true;
    int fps = 0;        // 0=leave unchanged. For verification C: force the frame rate
    int cbs = -1;       // -1=leave unchanged. 0/1= force m_clickBetweenSteps
    int cos = -1;       // -1=leave unchanged. 0/1= force m_clickOnSteps
    float framedt = 0;  // >0: pin the dt passed to BGL_update to this value (fake frame rate)
    bool blockInput = false; // block real input (other than injection) during replay
    // cfg `progblock`: block the level-progress recording (percentage, attempts, rewards).
    // ON by default and meant to stay on -- it exists as a switch ONLY so the session-end
    // audit can be demonstrated to be non-vacuous: run once with progblock=0 and the
    // "level record changed:" line names the fields GD wrote. Achievements, statistics and
    // coins have no such switch and are blocked unconditionally.
    bool progressBlock = true;
    // cfg `netblock` / `saveblock` / `memaudit`: the three layers under the function-level gate
    // (see record_gate.hpp). All ON by default and meant to stay on; each is a switch only so a
    // test can show the layer is not vacuous (a request let through, a save let through, an
    // audit that fires). `nettest=1` makes the session send one harmless request to a closed
    // local port, to show the network layer catches a real call.
    bool netBlock = true;
    bool saveBlock = true;
    bool memAudit = true;
    bool netTest = false;
    // cfg `savetest=1`: the session asks the game's own atomic file writer (2.209) to write one small file named
    // gdsolver-savetest.dat in the save folder, to show the save layer refuses a real call (savetest=1 with
    // saveblock=0 is the positive control: the file appears).
    bool saveTest = false;
    float fastdt = 0;   // >0: fast mode. dt passed per update call (e.g. 1.0 = 240 ticks)
    int fastloops = 1;  // in fast mode, number of update calls per rendered frame
    // cfg `framebudgetms`: in fast mode, also end the frame's batch once this much wall time has
    // gone (0 = off: the batch is always `fastloops` calls, as it was). A batch is 7,200 ticks,
    // which is 0.4 s on lv1 and 8-18 s on a custom level (400-900 ticks/s), long enough for
    // Windows to call the game "not responding". Only the panel's solve turns it on
    // (uiConfigureSession); an autorun session keeps the fixed batch unless asked.
    double frameBudgetMs = 0.0;
    bool skipRender = false; // skip rendering of the game layer in fast mode
    bool noTrace = false;    // stop trace/dump writes (for speed-first runs)
    // cfg `dpselftest=1`: build the objrects table in memory, hand it to the solver core that
    // is linked into the mod, and report what it made of it. The acceptance instrument for
    // "the mod's level and the CLI's level are the same level" (Stage B) -- it writes the
    // `dpselftest:` lines to result.txt and changes nothing else
    bool dpSelfTest = false;
    // cfg `dpsolve=1`: solve this level with the solver core inside the mod and replay the
    // result -- the whole loop in one process (Stage B). `dparg=<...>` appends one more
    // argument to the solver's own command line (repeatable), so anything the CLI takes can be
    // tried without a rebuild.
    bool dpSolve = false;
    // `dphorizon` is THE LENGTH OF THE PLAN in ticks, not the depth of the search. 0 = derive
    // it from the level, which is the only sane default for a single solve: with a fixed 3000
    // the plan simply stops there and the player dies where it ran out -- measured on lv1,
    // x=4,092 of 26,724, i.e. exactly the 15% that looked like a solver failure.
    int dpHorizon = 0;
    // THE PLAN LENGTH AND THE REJOIN (default on since 2026-09-19). On the 22-level cold run in
    // one session, measured with nothing else running on the machine: step + adaptive length +
    // fast veto 1,437 s; plus the rejoin 1,120 s (and 1,123 s on a second run, every round of
    // every level identical). The previous habit (plan the whole level, shorten after stalls)
    // was published at 30 minutes for v0.1.4; measured next to the step length on a machine that
    // was also running other work it was 2,199 s against 1,629 s. Each is a cfg so a diagnostic
    // run can turn it off (value 0).
    //
    // cfg `dpstephorizon`: plan this many ticks at a time and keep the whole-level plan
    // (dphorizon) for when the run is stuck -- the inverse of the old habit. 0 = the old habit.
    // The no-death recording pass and the restart from the start still use the whole level.
    int dpStepHorizon = 3000;
    // cfg `dphorizongrow` (0 = off; a percent >= 100, needs dpstephorizon): under the adaptive length, a plan the
    // game flew past its step is answered with that percent of the ticks it flew (at least the step, at most the
    // level, at most dphorizonmax) instead of the whole level. A long ship/wave level: the whole-level answer made
    // four jobs (rounds 3, 4, 8, 10) read 24,000 ticks ahead to the model's own wall, 916 s of a 1,715 s run,
    // where the game flew 3,000 to 7,300 ticks before it parted from the model.
    int dpHorizonGrow = 200;   // on at 200 % by default (release flags, 2026-10-17); cfg dphorizongrow=0 is the off arm
    // cfg `dphorizonmax` (0 = none): the longest plan dphorizongrow asks for. At 200 % a 7,000-tick flight asked
    // 14,000 ticks, and one search that long near the wall took 100 to 230 s.
    int dpHorizonMax = 0;
    // cfg `dphorizongrowmin` (ticks, 60000): dphorizongrow acts only on a level whose whole length in ticks is at least this; a shorter one keeps the whole-level answer.
    // Measured on the official 22, nocoin, one session each: with the growth at 200 % lv20 took 8 rounds (3 without), lv21 7 (2), lv22 25 rounds in 1,467 s (16 in 503 s);
    // on the long ship/wave level it was made for (84,333 ticks) it took 1,715 s to 1,238 s. The levels this threshold keeps out are all under 40,000 ticks.
    int dpHorizonGrowMin = 60000;
    // cfg `dpretirestall` (3): rounds without 100 px more that a speed-switched plan gets before the plan it replaced
    // is flown again (repair.hpp RetiredBest). Under dpneedspeed only.
    int dpRetireStall = 3;
    // cfg `dpseed`: the seed handed to the search at the start of a level's run (dp_bridge
    // runBegin). Part of the host's promise to a search: a method that draws random numbers draws
    // them from this, so the same seed gives the same plan (dpsearch=montecarlo does; dp does not).
    long long dpSeed = 0;
    // cfg `dpsearch`: the search a call's answer comes from -- `dp` (the layer DP, the default),
    // `beam` (the best states by the rules' score each tick, deterministic) or `montecarlo`
    // (random rollouts; both in dp search_dp.hpp). The play menu's Options offers the same three.
    // baseArgs passes it as --search. The cap ladder is the DP's own (it re-runs the search at
    // other caps), so it is not passed for another method: montecarlo ignores the cap, and each
    // re-run would draw another random sequence.
    std::string dpSearch = "dp";
    // cfg `dpfastveto`: a round that flew EXACTLY the plan the round before flew, and died on the
    // same tick, drops the veto box at once instead of waiting for the fourth hit -- the search is
    // deterministic, so the repeat is not new evidence. Always on since the flag clean-up.
    // cfg `dpfastvetoall` (off, measured inert under the adaptive length): the same, against every
    // plan this level has flown rather than only the last round's.
    bool dpFastVetoAll = false;
    // cfg `dprejoinwatch`: hand dp the model's trace of the plan that last died (--rejoinwatch).
    // On its own it only prints how soon the next search comes back onto that plan. Always on since the flag clean-up.
    // cfg `dprejoinuse` (needs dprejoinwatch): stop the search where a state that did not go
    // through the old death comes back onto the old plan's trajectory, and keep the old plan's
    // inputs from there on (dp's --rejoinuse). The layers past the join were 31-45% of the search
    // on lv16/20/22; lv16 258 -> 163 s, lv22 385 -> 222 s. Its one measured loss is lv20
    // (212 -> 247 s): the old plan's continuation past the join died 44 ticks on, in ground the
    // game had never flown. Always on since the flag clean-up.
    // cfg `dprejoinchain`: also join a plan that was itself a join. Off was measured and is worse:
    // it did not repair lv20, and on lv22 the changed route fell into the off-board hole
    // dpoffboardkill describes. Always on since the flag clean-up.
    // cfg `dprejoinfull` (off, measured worse): an exact rejoin also needs the search's dedupe key
    // to match. It removed most of lv22's joins -- equal on y/vy/mode but not on the key, and still
    // good in the game -- and lv22 went 222 -> 507 s.
    bool dpRejoinFull = false;
    // cfg `dpoffboardkill` (off): pass the loop's playfield bound to the search (--offboard). It
    // closes an off-board stall the default route does not reach, and costs lv22 34 s on it.
    bool dpOffBoardKill = false;
    // cfg `offboardfree` (on by default since 2026-10-06; 0 = off): the off-board credit skips the
    // rows recorded in Free Mode, where GD's band bounds no mode and says nothing about where the
    // player is (see offBoardTick).
    bool offBoardFree = true;
    // cfg `offboardband` (on by default since 2026-10; 0 = off): the off-board credit also disarms on a
    // row whose band moved by more than kOffBoardTpDy at either edge while the player did not -- a
    // cutscene's zoom rewrites GD's band around the camera (see offBoardTick).
    bool offBoardBand = true;
    // cfg `dpadaptivehorizon` (needs dpstephorizon): choose the next plan length from where the
    // game ended the last one. Killed within the step of its anchor = the model is wrong here, plan
    // the step; alive past it = the model is right here, plan the level. Always on since the flag clean-up.
    // How many repair iterations the loop may spend before it reports the wall (Stage C).
    // A report, not a failure: "stopped at iteration N, deepest t=..." is the diagnostic the
    // level is asking for. 40 undercounted a level the loop can actually clear: with the phantom
    // veto (2026-08-24), lv16 needs 53 rounds and clears cold at the default cap raised here --
    // at 40 it reported "could not solve" one wall short of the real answer. 200 is comfortably
    // past that with margin for a harder level (lv22 stands at 84% after 184 of a 400-round
    // budget), while an actually-unsolvable wall still gives up in well under a minute of extra
    // wall clock rather than running unbounded.
    int dpMaxIters = 200;
    // cfg `dptopstop=K` (0 = off): the same report, reached when the search has spent K rounds at
    // its largest capacity (the last of kCapTiers) without getting deeper. Without it such a run
    // is stopped by the harness' wall clock, and a round at that size is the slowest there is
    // (SubZero 4002 spent 4,410 s on 20 of them and got nowhere). K is derived rather than
    // chosen: over 129 SubZero cold logs, all 18 visits to the top tier that got deeper did so
    // within 8 rounds -- nine of them on the way to clearing the level -- so K <= 8 would have
    // cut one of those. Of the 28 visits that never got deeper, four stayed 12 to 60 rounds.
    // A round count since the last new depth was measured too and rejected: 4003 has cleared 76
    // rounds after its last new depth, which leaves no useful N.
    int dpTopStop = 0;
    // cfg `dpcapenvrepeat=K` (default 3, 0 = off): from the K-th death of this run at the same wall (the same 4-tick bucket,
    // counted by g_deathRuns) the searches are asked for --capenvelope as well, until the run gets deeper. The cap
    // keeps a layer's states by a stride in layer order; the envelope keeps the extremes of each class first
    // (dp search_dp.hpp). A custom level dies 8 times in a row at one wall that every cap of the
    // ladder (2000) fails and that cap 2000 with the envelope passes; the flag is not worth its cost as a default
    // (the custom 22 at 1200 s: 21 cleared without it, 19 with it), only once a wall has repeated.
    int dpCapEnvRepeat = 3;
    // cfg `dpmissedportalrepeat=K` (default 3, 0 = off): from the K-th death of this run at the same wall, the loop asks the
    // missed-portal question (repair.hpp missedPortalTick) without waiting for the ladder to run out of anchors: a
    // mode portal the run crossed without coming out in its mode is where the next rung goes back to, with the
    // crossing forced. Cube portals count (they are left out of the hint's list otherwise: a level may hold ones
    // nothing needs to take). A custom level dies 8+ times in a row at a wall whose cause is the
    // spider branch taken at a cube portal 2,000 ticks earlier, while every rung of the ladder finds some tail.
    int dpMissedPortalRepeat = 3;
    // cfg `dpcaptiers=0` (1 = on, the default): escalate() skips the capacity tiers (kCapTiers in
    // repair.hpp), so the restart from the start comes next and runs at the base setting instead
    // of inheriting the top tier's 40,000 states for the whole level. Asked because, over the
    // spinoff cold logs (2026-09-26), a restart at the top tier cost 16,263 s in 125 runs for one
    // real step forward, the escalation job itself returned no plan 159 times in 170, and a
    // model-only first plan from t=0 never needed more than 2,000 on official lv1-22 (lv20-22 stop
    // at the same tick at 40,000). The tiers did carry SubZero 4003 and two custom levels to the
    // end in runs without a section solve -- which is what an A/B with this off has to answer.
    int dpCapTiers = 1;
    // cfg `dpsectierfirst` (on by default since 2026-10; 0 = off): when the ladder finds no anchor,
    // the wall goes to a section solve before the capacity is raised (escalate() in repair.hpp) --
    // one window per escalation, each further back, as autoFire draws them -- and only a wall none
    // of those windows crossed goes on to the next tier. The user's order (2026-09-26): a full
    // frontier is usually a divergence, so the game's own search goes first, and more states only
    // after it.
    int dpSecTierFirst = 1;
    // cfg `dpsecmaxback=N` (0 = no limit, the default): no section-solve window begins more than N
    // ticks before its wall (repair.hpp autoWindow, where the measurement is); a wall whose next
    // window would is done, and with dpsectierfirst the capacity tier comes next.
    int dpSecMaxBack = 0;
    // cfg `coinmissrev=1` (off): the coinroute attempt cut (hooks_gamelayer.cpp) skips the coins
    // dp's search does not call missed when passed (Outcome::coinNoPrune -- something past them
    // turns the player round). Without it the cut ends the attempt on the way out past a coin the
    // plan collects on the way back.
    bool coinMissRev = false;
    // cfg `coinmissmove` (on; 0 = off): the same cut leaves a coin alone while a Move that carries it
    // (solver::g_coinMoveX, not spawn-fired) still lies ahead of the player, and for good once past
    // it when the search also does not call the coin missed (a reverser beyond it). Narrower than
    // coinmissrev, which spares every coin with any reverser past it and so loses the signal for
    // coins that are never revisited (SubZero 4002's first two).
    bool coinMissMove = true;    // ON BY DEFAULT with the SubZero coin set (0 = off)
    // cfg `groupsretime` (on; 0 = off): the live moving-geometry recording (harvestGroups) is replaced by
    // a SHALLOWER replay's when the two replays' player x parted before that replay died. The
    // recording is indexed by tick, and a route that skips a speed portal reaches the same x
    // hundreds of ticks later, so the deepest record is out of phase for it from where they part.
    // Measured on SubZero 4002: the two-coin route missed the 3x portal at x=11,655 and arrived
    // 1,901 ticks late; the kept record was the coin-less route's, and the model killed a line GD
    // flies on a spike that the record had already moved 107 px up.
    bool groupsRetime = true;    // ON BY DEFAULT with the SubZero coin set (0 = off)
    // cfg `coinmisspost` (on; 0 = off): a clear refused for a missing coin (hooks_playlayer.cpp) is filed
    // as a coin miss at the attempt's CLOSEST APPROACH to the first coin it missed, instead of as a
    // death at the finish line. On a level that turns or reverses, the attempt cut never fires
    // (passing a coin says nothing there), so the refused clear is the only miss the loop hears --
    // and filed at the goal it sends the ladder thousands of ticks away from the coin.
    bool coinMissPost = true;    // ON BY DEFAULT with the SubZero coin set (0 = off)
    // cfg `coinmissearly` (on by default since 2026-10; 0 = off; with coinmisspost and
    // routeprereq): a coin whose box the attempt went past unopened is filed as missed at that
    // attempt's death, without waiting for a refused clear (repair.hpp fileCoinMissEarly).
    bool coinMissEarly = true;
    // cfg `coinoverdepth` (on; 0 = off; with coinmisspost): an attempt that has every coin the deepest
    // plan had, and has taken in the game a coin the deepest plan missed, is progress whatever tick
    // it died on. coinmisspost ranks the deepest plan at its closest approach to that coin, which is
    // a tick on ITS route; the new attempt's death is a tick on another, so the two ticks do not
    // compare. Measured on SubZero 4002 (4619661, 2026-09-26): a section solve took coin 3 (GD
    // credited it at t=21,334), the flight died at t=21,477, and the loop rewound onto the coin-less
    // plan ranked at t=21,552 -- the only three-coin plan of the run, thrown away for 75 ticks.
    bool coinOverDepth = true;   // ON BY DEFAULT with the SubZero coin set (0 = off)
    // cfg `coinapproachoff` (on; 0 = off): a coin's closest approach (coinmisspost's rank) is also taken
    // over the ticks its group was switched off, and the later of that and the on-only approach is
    // the one used. SubZero 4003 (2026-09-26): the third coin's group is off from t~10,274 on every
    // attempt; the failing route passed it at 102 px at t=13,557 but was ranked at t~9,650, 1,336 px
    // away, where no repair reaches it -- and the route GD credited it to took it at t=13,707.
    bool coinApproachOff = true; // ON BY DEFAULT with the SubZero coin set (0 = off)
    // cfg `coinapproachreach` (on by default since 2026-10; 0 = off; with coinapproachoff): among
    // the passes made while the coin was off, the last one within pickup reach is ranked rather
    // than the closest. lv22's third
    // coin on the 1x route: passed at 1.9 px while off at t=12,055, and at 23.6 px at t=14,960 where
    // the six presses that switch it on can already have happened (repair.hpp coinApproach).
    bool coinApproachReach = true;
    // cfg `routeprereq` (on by default since 2026-10; 0 = off; only a coin session builds the
    // census): a coin that needs something entered first (solver/route.hpp: the
    // touch box, key or toggle block whose chain switches the coin, or a platform/orb near it, on)
    // is ranked, in an attempt that has not switched that on, at the attempt's closest approach to
    // the BOX rather than to the coin (coinmisspost's rank), and the search anchored before the box
    // is told to enter it (--needtrig-uid). SubZero 4002's second coin: its platforms come on only
    // from the key 4,314 px before it, and every repair went to the coin. Until a coin is engaged
    // (the fallback below) nothing reads the census, so the loop and the DP argv are the old ones.
    // It was off by default until 2026-10: with the SubZero coin set on, SubZero 4001's coin-on
    // cold took 33 rounds with it on and with it off (four runs, one last [fp]), and over the
    // official 22 it never engaged either way. It stays for what the chain rig route2 shows: on, 9
    // rounds; off, no anchor past the second key.
    bool routePrereq = true;
    // cfg `routeprereqafter=N` (10): ...and only for a coin the loop has not got by itself -- N
    // rounds after its miss was filed without GD crediting it, or when the ladder has run out of
    // anchors (escalate), whichever comes first. 0 = from the filing on. The model's own repairs at
    // the coin come first: on trunk 2a20c92 SubZero 4002's second coin fell to them 8 rounds after
    // the filing, and going to the key at once cost the run its clock (2 of 2 runs).
    int routePrereqAfter = 10;
    // cfg `dpsecauto` (on; 0 = off): the loop fires a section solve as a rung (see secrung in session.hpp)
    // by itself, at the deepest wall, when one of the signals below says point fixes are not
    // getting it across. As many rungs per wall as it takes, each from further back -- 200, 400,
    // 800 ticks, then 800 more each time -- until one from the level's first tick has failed too
    // (repair.hpp autoWindow). The first window: from the head of the fixups recorded
    // at the wall (or 200 ticks before the death if there are none) to dpsecmargin ticks past
    // the death -- measured on four walls of lv4003 under an older model: a window that ends
    // before the death does not cross a death
    // the model cannot see, and one that starts 100 ticks before it is already too late.
    // Unless the session sets secsnap/secverifyevery itself, the rung searches with the player
    // snapshot and a cross-check every 20 layers (see autoFire).
    // The thresholds have no settled value yet (the search is still being made faster), so
    // both are 0 = off unless the session names them; the pinned-out signal needs none.
    // On by default since 2026-09-25, with dpsecrent, dpsecreuse and dpsecchain: the rung is the
    // loop's last line -- it searches the game itself -- and since 1c2de49 it also works in dual
    // sections (official lv16 with coins, cap ladder and grid off: timeout -> 43 rounds; a custom
    // level with the ladder: 37 rounds, where every laddered run had timed out).
    bool dpSecAuto = true;
    int dpSecStall = 0;     // cfg `dpsecstall`: rounds spent at the deepest wall (the same wall
                            // until the deepest death moves kAutoCreep ticks, repair.hpp)
    int dpSecNoRec = 0;     // cfg `dpsecnorec`: deaths there in a row the recorder wrote nothing for
    int dpSecMargin = 54;   // cfg `dpsecmargin`: how long past the death (ticks) the rung's
                            // answer has to stay alive -- about the 70 px the measured
                            // windows ended past it, at normal speed
    int dpSecCap = 100;     // cfg `dpseccap`: the search's frontier cap
    int dpSecCapTiers = 2;  // cfg `dpseccaptiers`: a window that found nothing, again at twice
                            // the cap, up to this many times (repair.hpp autoCapRetry). 2 by
                            // default since 2026-09-28: only a window whose search came back
                            // empty with its cap binding is searched again, so a wall the base
                            // cap crosses costs nothing more (0 = off).
    // cfg `dpsecstate` (on by default since 2026-10; 0 = off; needs dpsecauto): a wall the first
    // window and its cap tiers could not cross is asked whether player 1's size is what stops it --
    // the same window, searched with the size the plan took at its last size portal undone -- and
    // if so, the next windows begin before that portal and keep the other size (repair.hpp
    // stateFire).
    bool dpSecState = true;
    // cfg `dpneedspeed` (off; 1 = on; needs dpsecauto): the same question for player 1's SPEED. A wall the loop keeps
    // dying at -- the stall rule that would hand it to a section solve -- is asked first whether the speed the plan
    // has there is what stops it: the plan's speed changes before the wall are tried, newest first, each by a
    // search from just before the change that must not cross the wall's x at the speed the plan has there (dp
    // --needspeed, repair.hpp speedArm). A speed cannot be swapped into the anchor the way a size can (a faster
    // player is at another place at the same tick), so there is no probe; the search is the question.
    // The size is asked first: this waits until stateFire has settled it at the wall (a size wall costs a speed
    // question per change otherwise). Flights that were at different speeds at the same x are ranked by x, not by
    // tick, when they died 1,000 px or more apart on upright ground (kLaneMargin), and once the run has crossed a
    // wall in another lane the changes up to that wall are not asked again.
    // With cfg `stallbound` the loop's wedge bound is the guard's (repair.hpp onDeath), which a long level needs.
    bool dpNeedSpeed = true;   // on by default (release flags, 2026-10-17); cfg dpneedspeed=0 is the off arm
    // cfg `dpspeedfirst` (on by default; 0 = off; needs dpneedspeed and dpsecauto): the speed question goes AHEAD of the size probe at a wall
    // where a flight in another speed lane has died clearly short of the deepest plan (the "ranked by x, not deeper" case,
    // kLaneMargin). Such a flight says the wall is not one the size probe could settle: the ladder's claim for the other
    // lane is being tried the expensive way, round after round. A long custom level's 2026-10-17 profile run: the
    // wall at t=30794 took three ladder jobs (56, 286 and 112 s at the same anchor), a section solve and a size probe
    // before the speed question was asked, and it was answered in 15 s -- 6 iterations, 56% of the 1265 s wall.
    bool dpSpeedFirst = true;
    // cfg `dpsecchain` (on; 0 = off; needs dpsecauto): a wall right behind the pin -- the splice died
    // within kAutoCreep ticks of its end, which leaves the ladder nothing to plan with -- doubles
    // the next window's margin, and doubles it again for each such wall in a row (up to 8x).
    // Coin-off SubZero lv4001 with dpsecauto: 43 rungs, 30 of them 2-9 ticks past the last pin,
    // each window 254 deep and each advancing ~57 ticks -- 10,922 layers searched for 3,732 ticks.
    bool dpSecChain = true;
    // cfg `secredoreach` (on by default since 2026-10; 0 = off): a snapshot search caught lying by
    // its spine is not asked again on checkpoints when it reached the plan's verified death anyway
    // (hooks_gamelayer, the rung's end).
    bool secRedoReach = true;
    // cfg `seccpfallback` (0 by default): a section search of the repair loop never runs on
    // checkpoints -- not as the re-ask after a snapshot caught lying, nor when the snapshot does not
    // qualify for the window; such a window is given back to the loop unsearched (hooks_gamelayer).
    // `seccpfallback=1` brings both back (a diagnostic). Off by default since the release decision
    // of 09-30: no section search at prim=cp over the official 22, coins off and on.
    bool secCpFallback = false;
    // cfg `secthrow=<length|alloc|both>@<depth>` (off; a measurement, never a default): the section
    // search throws on purpose on the first step at that depth once the layer holds a child --
    // std::length_error, std::bad_alloc, or (both) the first in the first search that gets there
    // and the second in the next -- once per kind in the process. It is what shows the containment
    // around the search's layers working: the game goes on, the search ends EXHAUSTED with
    // stop=exception, and the rung gives the window back as a search that found nothing.
    int secThrowKind = 0;    // 0 off, 1 length_error, 2 bad_alloc, 3 both
    int secThrowDepth = 0;
    // cfg `dpsecchainspan`: how far past the pin (ticks) a wall still counts as right behind it.
    // 30 is the wall's own identity (kAutoCreep); on lv4003's wave, with the fitted prices and
    // the leaf from the middle, the walls came 10-90 ticks past each pin, so most links of a
    // 13-rung chain reset the margin instead of doubling it.
    int dpSecChainSpan = 30;
    // cfg `dpsecpinback` (-1 = off): with dpsecauto, a wall less than 100 ticks past the pin starts its window this
    // many ticks before the pin rather than 200 before the death. On SubZero 4003 (81179e8) 28 of
    // 52 rungs were such walls, each re-searching 100-190 ticks the previous splice had already
    // crossed in the game, at depths 254-416: 183 of the 277 s the rungs searched. Walls 100+ past
    // the pin start at the pin already and ran 159-238 deep. Not at the pin itself: many of those
    // walls are 10-30 ticks past it, where the state the splice left may already be lost.
    // 60 by default: 4003 762 -> 653 s (82 -> 77 rounds, rungs 53 -> 48, search 296 -> 213 s,
    // every rung still solved on its first try); 4001 and 4002 never have such a wall, [fp] identical.
    int dpSecPinBack = 60;
    // cfg `dplearnresets` (1 by default since 2026-10; -1 = no limit, the old behaviour): how many
    // times one wall (one value of the best death) may send the ladder back to its shallowest rung
    // because a round recorded fixups. On a custom level, x=29,779: a ship ring GD fires a tick
    // before the model re-fired in the model after every re-anchor, so each round wrote one record
    // one tick later and reset the ladder -- 13 rounds in a row, the backoff never past 96, while
    // the route needed the anchor 105-119 ticks back (repair.hpp, the reset). 1 = the first lesson
    // at a wall reopens it. It was off (-1) until 2026-10: the stall it was written for was not
    // reproduced in a cold run, and on official lv16 a cap of 1 took 21 rounds to 19.
    int dpLearnResets = 1;
    // cfg `secdriftwhere=1` (diagnostic): at each psnap cross-check, print where the node that
    // drifted most first left its own ancestors (`secdrift:` line).
    bool secDriftWhere = false;
    // cfg `dpsecstateprice` / `dpseccallprice`: what dpsecrent charges a DP search, per carried
    // state and per call, in microseconds (repair.hpp workDpState).
    double dpSecStatePrice = 1.75;
    double dpSecCallPrice = 950000.0;
    // cfg `dpsecsolved` (off, needs dpsecauto): a plan that claimed the goal dying at the deepest
    // wall is a signal of its own -- the model got to the end and one stretch of it is wrong, so
    // that stretch goes to a section solve at once instead of after the rounds a stall needs.
    // The death's fixup recorder still runs first (the window's entry is where its records start).
    // Expected to lose where one repair round crosses the wall, which is most walls (coin-off
    // SubZero, dpsecauto off: 16 of 20 walls on 4001 and 13 of 24 on 4002 took one round, at
    // 6.7 and 15.8 s a round, against a rung of about 12.5 s search plus its replays).
    bool dpSecSolved = false;
    // cfg `dpsecrecaway` (on by default since 2026-10, needs dpsecauto; 0 = off): when the deepest wall's section solve
    // is due on a round whose own death is away from that wall, the death's fixup recorder runs
    // first and the section solve after it (the same record-then-rung path as dpsecsolved), instead
    // of the section solve returning before the job that records. Off, a solved tail that dies
    // short of the wall is never recorded while the wall keeps firing rungs: a custom level went
    // four times round the same anchor, the same SOLVED tail and the same death before the veto.
    bool dpSecRecAway = true;
    // cfg `dpsecreuse` (on; 0 = off): the rest of the plan a rung was fired from stays the rejoin target
    // of the search after the splice (g_rjKeepTarget in repair.hpp), so the next plan can come
    // back onto it and keep its inputs. Not pasted after the window: see the rung in
    // hooks_gamelayer for what that did.
    bool dpSecReuse = true;
    // cfg `dpsecrent` (on; 0 = off; needs dpsecauto): fire when the work the loop has spent at the
    // deepest wall -- the rounds that failed to pass it: their searches' states, the recorder's
    // replayed ticks, the ticks flown -- reaches what a rung is expected to cost (the mean of this
    // run's rungs so far, or `dpsecrungprior` seconds' worth before the first). Counted, never
    // timed: the same run must fire at the same rounds on a busy machine (repair.hpp kWork*).
    // The rent-or-buy rule: it never pays more than twice what the better of "keep repairing" and
    // "solve the section" would have, and the boundary comes from the run's own costs instead of
    // a count. Why a count is wrong: a round at the end of 4001 costs a median 3.5 s (the model
    // plans only what is left), one on 4002 16 s, and a rung about 17 s on either.
    bool dpSecRent = true;
    // cfg `dpsecboxwait` (on; 0 = off; needs dpsecauto and routeprereq): no section solve while the
    // deepest death was ranked at a box a coin waits on (repair.hpp deathNeed). The ordinary search is
    // the one told to enter that box (--needtrig-uid); a section solve is not, and it cannot take the
    // coin in a window at the box either. SubZero 4002 with coins (trunk 9d7b4768): the rent fired a
    // rung on the second round at the third coin's box uid 18126, and thirteen rounds of rungs widening
    // to 1,654 ticks never opened it (TIMEOUT at 1,200 s); with dpsecauto=0 the same route opened it on
    // the second round at the box and cleared in 6 rounds.
    bool dpSecBoxWait = true;
    int dpSecRungPrior = 15;
    // cfg `dpsecwallbudget=<s-equiv>` (0 = none): the section-solve work one wall episode may spend,
    // in the rent's currency (1 = 1e6), counted from the rungs' replay ticks and search steps -- never
    // a clock. Spent, the loop's further rungs there are refused and the fixup loop carries on
    // (repair.hpp g_secEpisodeWork).
    double dpSecWallBudget = 0.0;
    // cfg `dpunseenfirst` (off): when the rent first asks for a section solve and the search has
    // not been told to enter the doors whose effect it has not seen (repair.hpp g_needUnseen), the
    // loop turns that on and plays one more round instead; a wall that still holds then gets its
    // rung. The two answers to a stalled wall were ordered by their triggers, not their cost.
    bool dpUnseenFirst = false;
    // cfg `dpseedpickups` (on; dpseedpickups=0 is the off arm): dp's --touchseed names the pickups
    // (objrects type 30) the attempt had overlapped by t0 as well as the touch Toggles, so an anchor
    // past a key that switches a group on opens that box (repair.hpp touchSeedArg).
    bool dpSeedPickups = true;
    // cfg `obbnolatch` (on; obbnolatch=0 is the off arm): the level export's obb.txt pass leaves every
    // rotated object as it found it -- rect latch (m_shouldUseOuterOb), oriented box, both dirty bits
    // and the cached rect -- and its oob column is the latch from before the pass (solver.hpp,
    // buildPois). Off, a session that ran the export played every rotated object latched, which GD
    // itself does only for what its load or a transform action latches: a rotated saw on SubZero
    // 4002 killed seven ticks later than in a plain replay of the same plan.
    bool obbNoLatch = true;
    // cfg `dpseccoinrung` (on; 0 = off; with dpsecauto, under coinroute): the rung's wall is the COIN WALL --
    // the last cut for a coin, on the plan that was cut there -- until GD credits that coin, and the
    // search there must take it (repair.hpp g_coinWall). Off, the rung goes to
    // the deepest death as before.
    bool dpSecCoinRung = true;   // ON BY DEFAULT with the SubZero coin set (0 = off)
    // cfg `dpendtrigclear` (on; 0 = off): a clear GD raises short of the end portal is accepted when
    // an End trigger (id 3600) fired in that attempt -- such a level finishes wherever the trigger
    // fires. On by the user's ruling (2026-09-24, "an End trigger is a clear").
    bool dpEndTrigClear = true;
    // cfg `slice` (on; 0 = off): a solve whose level has at least `slicemin` objects the run
    // cannot depend on is solved on a copy without them, and the plan that clears the copy is
    // verified on the level itself before it is filed (mod/level_slice.hpp, solver/slice.hpp).
    bool slice = true;
    // cfg `slicemin=<n>`: how many objects the cut has to remove for the copy to be used. What
    // the default (10,000) rests on is at kSliceMinDefault (level_slice.hpp).
    int sliceMin = -1;   // -1 = kSliceMinDefault
    // cfg `sliceaddbacks=<n>`: how many times a plan that did not hold on the level itself may
    // put objects back into the copy around where the two parted before the solve moves to the
    // level itself.
    int sliceAddBacks = 2;
    // cfg `slicegiveupcopy` (on by default since 2026-10; 0 = off): when the loop gives up on the
    // copy at a wall the level was flown to and shares (onVerifyDeath's "the level's own"), the
    // solve goes on on a fresh copy with a full carry instead of on the level itself -- once per
    // wall (levelslice::onGiveUp).
    bool sliceGiveUpCopy = true;
    // cfg `stagecuts=<x1>,<x2>,...` (off when empty): the level is solved in pieces, each on the level cut at
    // an x, and a piece's plan is flown first on the next, the last being the level itself (level_slice.hpp,
    // "STAGES"). Used instead of the slice's own cut.
    std::string stageCuts;
    // cfg `stagecutcheck=<0|1|2>` (default 2): what is done about a cut that splits a group a kept trigger reads
    // as a position (level_slice.hpp, "STAGES"). 0 = nothing; 1 = a log line; 2 = the cut moves to the nearest x
    // that does not, within `stagecutwindow` px (default 3000), or is dropped when there is none.
    int stageCutCheck = 2;
    int stageCutWindow = 3000;
    // cfg `stagebreak=<n>` (test only, 0 = off): the plan one piece hands the next keeps its first n inputs -- a
    // hand-off that parts from the clearing flight on purpose, to see that the check says so.
    int stageBreak = 0;
    // cfg `slicecount=1`: print what the cut would remove and end the session without solving
    // (a census of levels against the threshold).
    bool sliceCount = false;
    // cfg `slicenoposition=1`: the cut drops the decorations kept for being read as a position
    // too -- wrong on purpose, to exercise the verification on the level and the add-back.
    bool sliceNoPosition = false;
    // cfg `slicenotouch` (on by default since 2026-10; 0 = off): an id the game built both as a
    // decoration and as one other type is decided per object by No Touch (key 121), so its
    // decorations can be cut (slice.hpp).
    bool sliceNoTouch = true;
    // cfg `slicetriggers` (on by default since 2026-10; 0 = off): the cut drops the triggers that
    // can only act on what the run cannot depend on -- movers outside the relevant groups and
    // cosmetic triggers (slice.hpp). A run that takes coins keeps them all (level_slice.hpp maybeSlice).
    bool sliceTriggers = true;
    // cfg `sliceuids` (on; 0 = off): an object the cut leaves out is replaced in its place by a
    // placeholder that takes the same uids, so the copy numbers its objects as the level does
    // (slice.hpp "HOLDING THE UIDS"). Off removes it, which renumbers every object after it.
    bool sliceUids = true;
    // cfg `slicekeepgroups` (on by default; `=0` restores the old copy): the placeholders keep their
    // objects' groups and parent groups, so the copy's groups have the level's members, order and
    // parents (slice.hpp g_placeholderGroups). Without it the copy runs the game's group logic on
    // other groups than the level's: a plan lived past a spike in the copy and died on it in the
    // level. Placeholders then move with their groups, which costs time on group-heavy levels.
    bool sliceKeepGroups = true;
    // cfg `sliceverifyrec` (0 = off, the default; 1 = the old behaviour): the check flights on the level itself --
    // the plan that cleared the copy, a wall check -- do not record the moving geometry. Nothing reads that
    // recording once the plan has cleared (a clear is filed and the showing follows), and writing it was 13 s of a
    // 21 s flight on a 167,383-object level (21 s against the 5 s of a replay with no recording). A clear check that
    // dies is flown again with the recording, from a fresh swap, before its death is read (level_slice.hpp
    // onVerifyDeath): the loop that takes the death on the level itself starts from that recording. A wall check's
    // death is always taken over by the slice, which reads the player's rows, never the recording.
    bool sliceVerifyRec = false;
    // cfg `sliceverifybreak=<n>` (test only, 0 = off): the plan handed to the level after the copy cleared loses every
    // input past the n-th, so that the check flight dies where the copy's plan did not -- a verification that fails,
    // wrong on purpose like slicenoposition, to run the take-overs and the second flight of sliceverifyrec.
    int sliceVerifyBreak = 0;
    // cfg `slicefirstref` (on by default since 2026-10; 0 = off): before a wall check's death is
    // read as the cut's, the plan is flown on a fresh copy and the level's flight is compared with
    // that first attempt instead of the copy's deepest attempt (level_slice.hpp onRefFlightEnd).
    bool sliceFirstRef = true;
    // `dpseedplan=<path>`: skip the FIRST leveldp call and install this plan file instead, then
    // let the game verify it for real and search onward from wherever it actually lands. Empty
    // (the default) is a normal cold solve. See JobSeedPlan's note in repair.hpp for why this
    // exists and why it does not weaken "solving is always cold" -- it is a development-time
    // shortcut past ground already re-derived on every prior run, not a substitute for GD's own
    // verification, and it is never on unless this is set explicitly.
    std::string dpSeedPlan;
    // Whether a solve ends by replaying its solution at 1x, with the artwork and the song --
    // the one run of the whole session worth watching. -1 = decide from the session: a panel
    // session shows it, a headless worker does not (there is no screen to come back to, and the
    // extra replay would be 90 seconds of nothing before the session could close).
    // cfg `dpshow=<0|1>` forces it either way, which is also how the path gets tested off-screen.
    int dpShow = -1;
    // cfg `dpcoinwin=<yBin>,<vyBin>,<len>` (60,2,700 by default since 2026-10; 0 = off): dp
    // --coinwin with coinroute, the alive cap's classes split by (y, vy) cell in the last <len> px
    // before each coin a state still lacks, so a minority route to the coin is not thinned away (dp
    // thread_pool.hpp g_coinWinY).
    std::string dpCoinWin = "60,2,700";
    // THE GLITCH-AVOID MODE, the user's choice on the play menu ("Avoid glitches", Solve only) or
    // cfg `glitchavoid=1`; `glitchportal=<px>` / `glitchwave=<px>` set the two margins on their
    // own. 0 = off, which is the default and what every cold runs with. Handed to the searches
    // only (baseArgs), never to the fixup recorder's replay: the margins drop branches, they are
    // not the game's physics, and a replay that "died" on one would teach the model a revive.
    // What each margin drops is dp's thread_pool.hpp g_glitchPortal / g_glitchWave.
    double glitchPortal = 0.0;
    double glitchWave = 0.0;
    double glitchEmbed = 0.0;   // cfg `glitchembed=<px>`: dp's g_glitchEmbed
    bool glitchDeco = false;    // cfg `glitchdeco=1`: dp's --glitchdeco (hazard look-alikes)
    // THE WAVE'S BEHAVIOUR (Avoid glitches, third choice): cfg `avoidwave=1` (with `glitchavoid=1`), or
    // `wavepulse=<ticks>` / `wavecentre=<px>` on their own. 0 = off, the default. Searches only, like the
    // margins above (baseArgs): dp's --wavepulse (the wave's button keeps a level at least this long) and
    // --wavecentre (the wave stays this far from the floor, the ceiling and solids). A search that finds no
    // route with them is retried without them (repair.hpp escalate, g_waveRelaxed) and the run says so.
    int wavePulse = 0;
    double waveCentre = 0.0;
    // ...and its two preferences, cfg `wavetidy=<cost>` / `wavetoggle=<cost>` (avoidwave=1 sets both defaults): dp's
    // --wavetidy / --wavetoggle. Unlike the two above they drop nothing, so a search that relaxes the wave
    // behaviour keeps them.
    double waveTidy = 0.0;
    int waveToggle = 0;
    // cfg `dpfixp2`: let the SECOND BODY's transition error decide, on its own, that a dual
    // transition is worth a record -- see writeFixup's no-op test, which is where it acts.
    //
    // ON since 2026-08-29, and it was OFF for most of that day. Both settings were measurements,
    // not policy, and what moved between them was the PHYSICS. When it went in, the second body
    // had 27 unrecorded transition errors on lv16 in 12 clusters, and recording them cost the
    // level: 66 -> 150 iterations. A fixup is a patch applied at matched states, so a model made
    // faithful at 27 points and left wrong between them steers the search into routes it then
    // abandons -- the records were a to-fix list for the physics, not a fix.
    //
    // Three of those clusters have since been closed in the physics (the ramp ride's counter
    // across a tap, the dual ball's flip on the tick its partner lands, and "already inside" a
    // rotated portal). lv16 is down to SEVEN records in two clusters, and with so few the flag
    // is cheaper to run with than without. Measured back to back, same build, same machine:
    //
    //            lv16              lv17      lv20
    //   off   95 iters / 847 s    1 / -    34 / 1358 s
    //   on    55 iters / 527 s    1 / -    40 / 1448 s
    //
    // lv20 pays 6 iterations and 7% for what lv16 gains 40 and 38% on; across the three dual
    // levels -- the only ones this can reach -- it is 2,205 s -> 1,975 s. lv20's heavy solves
    // (>= 10 s) move 806 -> 817 s, i.e. the extra iterations there are the cheap kind.
    // Always on since the flag clean-up. (the off arm, which A/B-ed the records back out, is gone.)
    // cfg `dpbandtrack`: hand the search the CAMERA's recorded flight band
    // (--bandtrack). On since 81f2a09; off is how that commit's remaining half is
    // A/B'd, since lv22's second cold regression bisects to it.
    bool dpBandTrack = true;
    // cfg `dppin` (OFF): hand the model the ticks GD holds player 1 on a spot the physics does not give it
    // (--pinrows, built from the anchor source's x column in addWorldArgs): a Teleport trigger a Spawn loop fires
    // again every tick, onto a target something is moving. The model fires a teleport trigger once, at its crossing.
    bool dpPin = false;
    // cfg `dptap` (OFF): on a two-player level, press the second player's button a few ticks after the first player's plan passes a Tap trigger
    // (dp --p2tappress): what the Tap opens is not in the level data, so the press is made blind and the game's own recording of the attempt
    // shows the door to every later call.
    bool dpTap = false;
    // cfg `dpswitch` (OFF): on a two-player level, run the level twice more before the first solve with one body pressing on a schedule (nodeath) to see what
    // each button does to the world, and give the search the table (dp --p2switch). See repair.hpp "cfg dpswitch".
    bool dpSwitch = false;
    // cfg `dpincanchor` (ON since 2026-10-21; 0 is the off arm): place the objects an autonomous trigger moves by a closed form only on the ticks something about them can change
    // (dp --incanchor). Exact: the same plan, a quarter of the placement time on a level with 25,000 moving objects.
    bool dpIncAnchor = true;
    // cfg `dprlbound` (1500 since 2026-10-21; 0 = off): probe each rung of the resim ladder with a search to the walk's death plus this many ticks before the rung's full
    // search (dp --rlbound).
    long long dpRlBound = 1500;
    // cfg `dprlspread0` (OFF): a rung of the resim ladder searches from one state, so the crossings it already made are shared by every group of that
    // search (dp --rlspread0).
    bool dpRlSpread0 = false;
    // cfg `dpstatecross` (ON since 2026-10-21; 0 is the off arm): each state dates the autonomous triggers it crossed later than the frontier's leader by its own crossing, instead of a group
    // working its present speed back from its x (dp --statecross; the model-walk dates by its own crossing too, as --witnessauto).
    bool dpStateCross = true;
    // cfg `dpareaadd` (ON since 2026-10-22; 0 is the off arm): an Area Move acts on an object a closed form also moves, on top of the closed form's place (dp --areaadd).
    bool dpAreaAdd = true;
    // cfg `dpcrosscx` (OFF): --statecross keeps one record per x of the triggers it dates late, not one per trigger (dp --crosscx).
    bool dpCrossCx = false;
    // cfg `dpcrossbehind` (ON since 2026-10-22; 0 is the off arm): a group that has not reached an autonomous trigger sees it as not fired, though the frontier's leader fired it (dp --crossbehind).
    bool dpCrossBehind = true;
    // cfg `dpwaveflipcap` (ON since 2026-10-22; 0 is the off arm): a flipped wave under a plain ceiling ramp is the upright wave's outline (dp --waveflipcap).
    bool dpWaveFlipCap = true;
    // cfg `dprampseatall` (ON since 2026-10-22; 0 is the off arm): a growing portal's same-tick ramp seat goes through every ramp, at the grown box's lowest point (dp --rampseatall).
    bool dpRampSeatAll = true;
    // cfg `dpportalobbmode` (ON since 2026-10-22; 0 is the off arm): GD's own box (obb.txt) also for a turned portal that changes the mode, not the gravity/size ones alone (dp --portalobbmode).
    bool dpPortalObbMode = true;
    // cfg `dpslopelagplain` (ON since 2026-10-23; 0 is the off arm): a plain moving ramp's kill reads the line as the previous tick left it (dp --slopelagplain).
    bool dpSlopeLagPlain = true;
    // cfg `dpwaveflipcapfloor` (ON since 2026-10-23; 0 is the off arm): a flipped wave on a plain floor ramp has the capped outline (dp --waveflipcapfloor).
    bool dpWaveFlipCapFloor = true;
    // cfg `dpwaveflipceilup` (ON since 2026-10-23; 0 is the off arm): a flipped wave under a plain ascending ceiling ramp is killed 1.0 px further out (dp --waveflipceilup).
    bool dpWaveFlipCeilUp = true;
    // cfg `dpwaveportalrotmini` (ON since 2026-10-23; 0 is the off arm): the wave-portal step back is for a mini wave only (dp --waveportalrotmini).
    bool dpWavePortalRotMini = true;
    // cfg `dpballseatground` (ON since 2026-10-23; 0 is the off arm): an upright ball a slope-law seat landed last tick counts as grounded, so its press taps (dp --ballseatground).
    bool dpBallSeatGround = true;
    // cfg `dprobotslopenohover` (ON since 2026-10-23; 0 is the off arm): a robot's jump taken from a live ramp contact arms no hover budget (dp --robotslopenohover).
    bool dpRobotSlopeNoHover = true;
    // cfg `dpridesupgrace` (ON since 2026-10-23; 0 is the off arm): the ramp support window of the next tick's press runs one tick's movement past the ramp's far end (dp --ridesupgrace).
    bool dpRideSupGrace = true;
    // cfg `dpcrossminlag` (1 since 2026-10-22; was 3, which stuck the official lv22): ticks late below which a crossing under --statecross reads the leader's date (dp --crossminlag). Measured on a long ship/wave level: 1 clears in 349-381 s (16 rounds, no model re-anchor), 2 in 855 s, 3 in 667 s; on the official lv20 1 takes 19 rounds against 10, and on a custom level it stops at two same-tick deaths.
    long long dpCrossMinLag = 1;
    // cfg `dpdualbornlinked` (default on since 2026-10-23; --no-dualbornlinked is the off arm): a dual portal of a two-player level gives the new body the inverse gravity and negated vy of the first, as the game does without an Unlink Dual Gravity (dp --dualbornlinked).
    bool dpDualBornLinked = true;
    // cfg `dpbirthrung` (OFF): on a two-player level, a death after a dual portal made the second body is first tried again from an anchor 3 ticks after that birth, where the anchor has the
    // second body and the search is the two-body one (the first body's pass, then the second's against it); a rung of its own, tried once per wall, before the usual back-off.
    bool dpBirthRung = false;
    // cfg `dpctrlwin`: hand the model GD's recorded control-disabled windows
    // (--ctrlwin, built from the anchor source's ctrlOff rows in addWorldArgs).
    // It landed OFF (43d73db) so that commit could be proven behaviour-preserving;
    // it is ON now that an lv22 cold census has been read with it on and both
    // window edges were checked against GD (a press inserted on either side of
    // each edge: GD and the model agree to the tick). Only lv22 has these windows
    // among the official levels, so elsewhere this passes nothing.
    // Known gap, not closed here: a press held into the window and released on
    // its last tick makes GD jump as the controls come back. The replay path
    // (the fixup resim) reproduces that; the search does not, so it cannot plan
    // that one-tick-earlier jump and presses a tick later instead. Always on since the flag clean-up.
    // cfg `dprotseed`: seed the 2.2 rotation queue at each re-anchor from GD's own
    // recording, and hand the queue (--rotqueue --startrotq) ONLY to the calls whose
    // seed that recording fixes exactly; every other call gets no queue at all, so an
    // empty seed can never reach a running queue. 0 = off (nothing is passed and the
    // argv is as before), 1/2/3 = A/E/F -- how much counts as having seen a queue
    // entry, strictest first (rotSeedFor in repair.hpp) -- and 4 = S, the game's own
    // consumption loop replayed over the recording (rotseed::seedSim). Refused, and named, when a
    // cfg `dparg=--rotqueue` already turns the queue on for every call.
    // On by default (at A), always with the toggle rule (--rotqtoggle on the anchored calls): the
    // queue without it fires rotations the game only consumes (lv22 uid5809). On by default (at S) too, with dpTouchSeedNow:
    // on lv22 every anchored call's seed is derivable (122 of 122 against 46 of 157 at
    // A) -- derivable from S's rule, not independently checked in the game.
    // `dprotseed=A dptouchseednow=0` is the previous default.
    int dpRotSeed = 4;
    // cfg `dprotseedanchor`: whether an exact seed also hands the queue to the ANCHORED
    // SEARCH (site=anchor), or only to the fixup resim. 1 = both (dprotseed's own
    // behaviour); 0 = resim only -- the anchored call still logs its class on the
    // rotseed: line, marked queue=withheld, and gets no --rotqueue. The A/B arm for
    // whether the queue in the search is what an lv22 run piles up on.
    bool dpRotSeedAnchor = true;
    // cfg `dpswingpending`: an anchor taken on the tick a SWING's press takes effect carries
    // the pending flip in --start's 16th field (see swingPendingAt in repair.hpp). On since
    // v0.1.4; always on since the flag clean-up.
    // cfg `dphoverstrict`: an anchored robot whose vy is not flat into t0 gets no hover budget
    // unless the hover starts on t0 itself (see robotHoverLeft in repair.hpp). On since
    // v0.1.4; always on since the flag clean-up.
    // cfg `dpbandend`: pass --bandtrackend with the last tick the recording behind
    // dp_band.txt reached, so a portal past it sets the band instead of the held last row
    // (dp/src/dp/bands.hpp g_bandTrackEnd). On since v0.1.4; always on since the flag clean-up.
    // cfg `dpsnapshot`: keep a copy of the file inputs every solver call read (the moving-
    // geometry groups, the camera band, the fixups, the replayed plan) and of every anchored
    // search's emitted plan, each named by its own size/fnv so it matches the call's
    // `input sig` line. The loop rewrites those files as it goes, so without this a call
    // cannot be rebuilt offline with the inputs it actually had. Off by default: it only
    // writes files and log lines, and costs disk (lv22 keeps ~20 group versions).
    bool dpSnapshot = false;
    // cfg `capture=1` (off by default): keep the per-attempt research copies beside each died
    // plan -- dp_band_itN/dp_groups_itN, the recordings that plan was solved against, and the
    // recorder's own dp_fixin_*/dp_attempt_itN files (keepRecorderInputs in repair.hpp). Off by
    // default: a player's own Solve accumulates one such set per died attempt, and over a long
    // run that is hundreds of MB to a GB of files nobody outside a harness reads. The cold/
    // research harness (py/cold_regress.py, py/gdtas/worker.py and the other drivers that launch
    // a solve session) turns it on explicitly with cfg `capture=1`. dp_died_itN -- the plan
    // itself, ~15 KB -- is kept either way. An attempt from a run with this off has no recorder
    // inputs to rebuild from: only the plan and the `[inputs]`/`[fixin]` summary lines survive it.
    bool researchCapture = false;
    // cfg `dpcheck`: while a search runs, the game flies the search's checkpoints -- at fixed
    // layers past the anchor, the lineage of the frontier's first state -- and a checkpoint the
    // game kills cancels the search, runs the fixup recorder on that flight and solves the same
    // question again (dpsolve::ckConsider in repair.hpp, dp/progress.hpp). The outcome does not
    // depend on timing: checkpoints are layers, flights are judged in order, and the search holds
    // its answer until all of them are judged. It is NOT the same run as with this off -- the
    // model learns from flights the loop would never have made -- so two runs with it on are the
    // comparison, not one with and one without.
    bool dpCheck = false;
    // cfg `dpcheckobs` (with dpcheck): watch only. A flight the game kills neither cancels the
    // search nor teaches the model; the job's first such death is kept (later checkpoints are
    // passed unflown), and once the job's plan is installed and flown the loop prints whether
    // that plan shared the flight's inputs up to the death and died on the same tick
    // (`[checkobs]` lines) -- how often work begun on a flight's death while the search still
    // ran would have been the very work the loop does after it. Changes no decision.
    bool dpCheckObs = false;
    // cfg `dpcheckfirst` (with dpcheck): fly checkpoints during the FIRST solve only, and when one
    // dies, stop that solve and hand the flight's own plan over as the first plan -- the loop flies
    // it, it dies on the same tick, and the rounds go on as usual. The whole-level first solve is
    // the loop's single largest waste on a level the model gets wrong early (coin-off SubZero,
    // dpcheckobs: 4001's died at t=2,450, known 2.6 s into a 59 s search; 4002's at t=1,101, 4.8 s
    // into 135 s), while flying the short step searches cost more in waiting than it saved.
    bool dpCheckFirst = false;
    // cfg `dpcontenthorizon` (0 = off): where the loop would plan the whole level, plan only to
    // the first object newer than 1.7 past the anchor, plus one step (repair.hpp horizonFor).
    // 1 = every such plan, 2 = the first solve only (the default: official lv16-22 1,841 ->
    // 1,557 s with lv19/21 slower and lv17-19 in more rounds, SubZero 4001 239 -> 180 s and 4002
    // 501 -> 398 s; one run each).
    int dpContentHorizon = 2;
    // cfg `dpcapladder` (0 = off): pass --capladder <n> to the base searches (not the capacity
    // tiers), so each one starts at cap n and raises it only around where it runs out of states
    // (dp/cli.hpp cliMain). 125 by default together with dpinputgrid=2: official 22/22 in 116
    // rounds against 181 without either (one Windows cold run each), and every added death at a
    // place the plain run never died at. Of eight cleared custom levels it loses one, whose
    // route then crosses the dual section (x~11,500-12,500) where the model and the game disagree:
    // 61-148 fixups recorded there per run against 9, and game deaths with p1 exact and p2 160 px
    // away.
    int dpCapLadder = 125;
    // cfg `dpplainbeside`: 1, the ladder's plain search starts with the ladder, in a second copy of
    // the solver core on a thread of its own, and the ladder takes its result where it would have
    // searched it (dp_bridge.hpp plainBeside). The call's answer is the same; only its time moves.
    // 2, the instrument for that claim: both run, the ladder's own result is kept, and each call
    // writes a `[beside]` line saying whether the other agreed. 3, THE GAMBLE, which gives the
    // claim up for time: the plain search is taken as soon as it has finished and solved or died no
    // earlier than the ladder's deepest death so far, stopping the ladder where it is (dp cli.hpp
    // PlainElsewhere::ready). Which one finishes first depends on the machine, so a run is no
    // longer reproducible; every plan is still flown and judged by the game. 0, off. Not given
    // (-1): the settings menu's "Solve faster" decides between 3 and 0, and it is off by default
    // (repair.hpp start()).
    int dpPlainBeside = -1;
    // cfg `dpsigcheck` (debug, 0 = off): every signature fileSig takes from g_sigKnown is checked
    // against a scan of the file, and dp checks its groups-cache signature hits against the bytes
    // (--groupsigcheck); a disagreement is printed. Measures nothing, slows every call.
    int dpSigCheck = 0;
    // cfg `gtrollcheck` (debug, 0 = off): at each grouptrace roll, the bytes the old copy would have
    // written (hashed before the rename) against grouptrace_last.txt after it, one log line each.
    int gtRollCheck = 0;
    // cfg `gtrestrow` (1 = on, since 2026-10-14): grouptrace writes a row when an object comes to rest where its last row does not
    // have it, whatever the difference (grouptrace::tick). The kEps gate leaves an ease's settling stretch unwritten.
    int gtRestRow = 1;
    // cfg `recenteredfire` (1 = on; off by default): the boxes a kept recording's own attempt entered (dp's
    // --rectouchentered, repair.hpp touchEnteredRows) also take a box GD fired in that attempt
    // (touchfire, playerTouchedTrigger) where the rows never meet its load-time box: a box that rides a
    // moving object. Without it dp read "not entered", so --touchonrec kept the recording's switch and
    // removed what the box had switched off in the recording's attempt only.
    // Off because dp's own entry test for the state misses the same boxes: on SubZero 4002 a moving box
    // that x-crossing Toggles also switch on and off read "not entered" for an attempt GD saw enter it,
    // and the split this cfg makes possible then turned a platform off under it (6 new divergences).
    // The recording side alone is right; it waits for the state side.
    int recEnteredFire = 0;
    // cfg `gtdeathrow` (1 = on, since 2026-10-14): at a death, write the grouptrace row of the dying tick (g_tick + 1, the
    // dump's dead row) before the roll, and end the recording there. The per-tick row is written after
    // ++g_tick, by which time roll() has already committed the attempt, so a dying attempt's recording
    // stopped one row short of the kill: an instant Move that killed on that tick was in no recording the
    // model got. SubZero 4001 coin run t=20,674: spikes following group 130 (Follow) jump 744 px down
    // onto the ship on the dead row, and the model died past them five times; with the rows, none. The rows
    // are taken before GD's destroyPlayer runs, so what the death itself switches (an On Death trigger's
    // group) stays out. Ten official and spin-off colds, both arms on one package: equal iterations on
    // eight, 4001 11 -> 7, and 4001's coin run 20 -> 33 (a later route into a death the model predicts,
    // t=18,913); no divergence more on any.
    int gtDeathRow = 1;
    // cfg `windowguard` (0 = off): hold the game window at its launch size before each level and each
    // head reset of a driven session (window_guard.hpp). Off in the mod as shipped -- a player's window is
    // theirs; the verification workers' launcher (py/gdtas/worker.py) turns it on.
    int windowGuard = 0;
    // cfg `dpinputgrid` (1 = off): pass --inputgrid <n> to the searches, so the button changes only
    // on every n-th tick; repair.hpp baseArgs lifts it again once the loop escalates to a
    // capacity tier or restarts. 2 by default, measured with dpcapladder above.
    int dpInputGrid = 2;
    // cfg `dpphaseprof` (print only): time every call into dp (`dpcall:`) and pass --phaseprof to
    // the searches (`phaseprof: ... prep=`) -- repair.hpp timedSolve.
    bool dpPhaseProf = false;
    // cfg `memcensus` (print only): a `memcensus:` line -- the process's private bytes and working set, and
    // what the mod's main containers and the solver's caches hold allocated (MB), with the wall clock --
    // after every round's [fp] line, at every session's start and end, when a section search is dropped,
    // and when the level is left (repair.hpp memCensus). To result.txt and the Geode log: result.txt
    // starts afresh at every session, the log keeps the whole game, so a growth can be traced to what
    // the player was doing.
    bool memCensus = false;
    // cfg `dpwatchfired=<uid>[,<uid>...]`: the dump's `firedw` column carries each listed
    // object's +0x28e byte (the flag checkSpawnObjects tests before it calls triggerObject)
    // on every tick. Empty by default, and then the column reads "-". Print only.
    std::vector<int> dpWatchFired;
    // cfg `dpmodelreanchor` (default 3; 0 = off; N = at most N re-anchors per ladder): before a plan goes to
    // the game, its own walk (the search's witness walk) is read, and when the MODEL already dies on it the
    // loop re-anchors on the model's state a few ticks before that death and searches again, without a game
    // round (repair.hpp modelReanchor). The state crosses whole, through dp --dumpstate / --start-state
    // (dp statefile.hpp): a replay of the plan from the call's own anchor writes it, a search takes it up.
    // Default on since 2026-10-14: one-session colds, off -> 3, moved iterations only down (one coin run
    // 53 -> 20, the official last level 18 -> 16, two more by 2-4) and left every other level as it was.
    int dpModelReanchor = 3;
    // (cfg `dpspentorb` is gone: dp --spentorb, the rings this attempt fired before t0, is passed
    // to the anchored solves and the fixup resims always since 2026-09-26. Without it an anchored
    // model re-fires a ring GD spent just before t0 -- an old custom level, six kitref
    // episodes anchored at 2,561 in a field of stacked rings. See spentOrbArg in repair.hpp.)
    // cfg `dptouchseednow`: --touchseed tests the recorded row's own y only, which is
    // dp's markTouched under the default --touchprey=button. Off (`dptouchseednow=0`), it
    // also accepts the previous row's y, the older --touchprey=parent reading
    // (touchSeedArg in repair.hpp). On by default, with dpRotSeed = S.
    bool dpTouchSeedNow = true;
    // Always on since the 0.2.0 flag clean-up, and no longer cfg keys:
    //   * anchored calls pass --touchentered, the touch boxes the attempt's recorded positions
    //     overlapped by t0 (touchEnteredArg in repair.hpp), each as `uid:tick` with the first
    //     recorded row that overlapped it; dp's anchor scan opens only those, dated from that
    //     tick (it was `dptouchentered` / `dptouchenteredtick`). One unit with dp's autonomous
    //     lag: alone it broke lv22, because the old opening's wrong ceiling height was what let
    //     GD's recorded deaths through the fixup gate; and lv22's trap 18092, entered at 2,614,
    //     had been dated 2,255 from the ceiling's autonomous descent.
    //   * after the live recording (dp_groups.txt, written only once a replay has died) the
    //     calls pass --groupholddeath, so dp holds its last row one tick before the bootstrap
    //     takes over (it was `dpgroupholddeath`; see g_groupHoldDeath in groups.hpp). The kill
    //     on that recording's last tick is decided on dp's next row, where lv20's spike
    //     uid13068 was 3,000 px away and switched off in the bootstrap.
    // cfg `dpfingerprint`: one `[fp]` line per iteration pinning the loop's whole state
    // (see logFingerprint). This is the acceptance instrument for a change to the loop,
    // so it is ON by default -- a run that cannot be compared to a previous one cannot be
    // used to prove a refactor. Cheap: two file sums per iteration, and an iteration is
    // seconds of solving.
    bool dpFingerprint = true;
    std::vector<std::string> dpArgs;
    bool dumpEarly = false;  // diagnostic: write tick<=600 to dump
    // Record the state-flag transitions in the end zone (locked/ctrlOff/completed/dead).
    // During the suck-in visual effect m_hasCompletedLevel is still false = the completion
    // flag is not the end of controllability
    bool endTrace = false;
    // Record orb firing points (press tick / fire tick / offset from the orb centre)
    bool orbTrace = false;
    float orbTraceX = 0; // >0: record only orbs near this x (±300)
    // SUBSTITUTION PROBE (cfg `subringspent=1`, windowed by hbfrom/hbto, default off).
    // Forces the player's +0x98a ("this press has not been consumed", the per-tick mirror
    // of 0x986 written at 0x389f18) to 1 for the duration of one ringJump call, so GD runs
    // its own ring path under the MODEL's rule ("a held press still fires") instead of its
    // own. It answers a question observation cannot: not "what differs" but "what does
    // changing this rule DO", in GD's arithmetic.
    // THIS MAKES THE RUN NOT-GD. A dump produced with this set must never reach gdref or a
    // baseline; the session prints `subst:` lines so a stray dump names itself.
    bool subRingSpent = false;
    // Record pad firing (cfg `padtrace=1`). Makes activatedByPlayer (pad types only, with
    // used=m_activatedByPlayer1) and propellPlayer (the launch itself) name themselves.
    // Injection breaks the contact state and changes the result even with the same values, so
    // boundaries are pinned down with this + plan changes
    bool padTrace = false;
    // Seed an anchored solve's touch bits from what GD observed rather than
    // from the moving-geometry recording (cfg `touchpayload=1`, dp's
    // --anchor-state). OFF, because measured on lv22 on 2026-09-04 the two
    // sides name almost disjoint sets of objects: GD reported 24 activations,
    // of which 22 are not in triggers_lv22.txt at all and the other 2 sit at
    // cx 13,335 and 15,549, outside the 32-box window an early anchor holds
    // (cx 511..2,283). Zero of 24 mapped. The payload still declares
    // `owns=touch`, so leaving it on would SUPPRESS the recording-derived
    // seeding on every level and put nothing in its place.
    //
    // The cause is the population, not the wiring: the hook fires on
    // EnhancedGameObject::activatedByPlayer, which catches every
    // touch-triggered EffectGameObject, while dp's window models the
    // move-style triggers only. Turning this on before that is reconciled
    // measures the suppression, not the payload.
    bool touchPayload = false;
    // Seed an anchored solve's SPENT-GRAVITY-PORTAL mask from what GD observed
    // (cfg `portalpayload=1`, dp's --anchor-state owns=portal). Separate from
    // touchpayload because the two are in different states: that one is held off
    // by a population mismatch, while this map's uids are dp's own portal uids
    // and land exactly (checked on lv22: dp numbers 7 portals and all 7 latch on
    // the tick GD activates them).
    // ON BY DEFAULT (user's decision, 2026-09-29). It was held off because no anchored instrument
    // can see it (lv22's quick_regress sections are byte-identical either way) and on the argument
    // that an empty latch "can only fail to inherit a refusal, never invent one". The refusal it
    // fails to inherit is itself a false death: on lv22's glitch-avoid route with coins an anchor
    // at t=6,862 overlaps a blue gravity portal (uid 13833) GD had already used -- the ball stays
    // upside down through it -- and with an empty latch the model fired it again on the first tick,
    // put gravity back and killed the ball on the spikes 51 ticks later, so every search from that
    // anchor died there and the ladder could not back off through it. With the portal handed over
    // the search went on to t=8,113, and GD's own plan replayed from the anchor matched GD to its
    // death at 8,362.
    bool portalPayload = true;
    // cfg `anchorframe2dir` (on by default; 0 = off): an anchor in GD's frame 2 takes --start's rev
    // (22nd field) from the way the player is going, as the other frames already do, instead of 1
    // outright (repair.hpp startArg). GD frame 2 is the model's frame 0 reversed only while the player
    // runs back along x; a 2900 that also reverses the player (a custom level, gframe 2 with x
    // climbing 1.95 a tick) is the model's frame 0 going forward.
    bool anchorFrame2Dir = true;
    // Carry GD's slope-ride state into an anchored solve (cfg `histride=1`). The hist
    // payload becomes version 4 -- version 3's values plus the ride's raw facts:
    // slopeOn (+0x9b0), slopeUnder (+0x9b8), slopeUid (the ramp at +0x678), slopeAge
    // (the ride's clock +0x598 against +0xaa0, in ticks) and slopeLanded (a grounded
    // row since the ride's clock was stamped) -- and the solver is passed
    // --anchorride, which maps them onto the model's ride fields (onSlope / slopeT /
    // rideLanded, the not-yet-landed seat's seatT, or the underside's ceilT / ceilM4 /
    // ceilMode). Without it an anchor taken mid-ride guesses a saturated, landed ride.
    // What the recording point sees, measured with cfg `slopetrace`: +0x9b0 reads 1
    // on every tick of a ride, with the ramp's uid, and the clock reads 1, 2, 3...
    // ticks from the contact tick; a floor ride handed straight to an underside
    // contact keeps the same clock; +0x9b8 is not cleared when the ride ends, so it
    // means something only while +0x9b0 is 1.
    // On by default since 2026-09-30, as one unit with dp's consumer (g_anchorRide, on by
    // default in the same change): this key alone turns both, since the solver is passed
    // --anchorride with it and --no-anchorride without it, so a recording and a consumer
    // that disagree cannot arise from the cfg. cold_manifest.py counts, per anchored call,
    // the payload's version against the consumer the call ran with, and `check` refuses a
    // run where they disagree. The evidence: on the official levels a ride anchored after
    // 18 grounded rows launched at vy 10.342 in the game; the geometric guess (a full
    // ride) gave 13.065 and the recorded age gives 10.343. With the key on, both cold
    // suites (coins on and off) came out identical to the key off -- the paths they take
    // do not reach such an anchor, so that is safety, not a measured gain.
    // With it off the payload is version 3, byte for byte, and the consumer is off.
    // Version 5 (repair.hpp histPayload) adds the second body's dash to version 4, under
    // this key too: a pair taking one dash ring together dashes both.
    bool histRide = true;
    // Seed an anchored solve's history values that --start does not carry
    // (dp's --anchor-state owns=hist; version 1 = the press latch ->
    // State::pressSpent, version 2 adds the Free Mode byte -> State::bandBranch).
    // On by default since 2026-09-19, together with the two
    // rules that read the latch (--shipheldflap / --wavespentgate): with the
    // payload off they would read the latch's default after an anchor. A cold
    // run carried it on every call that has --start (421 of 421). Always on since the flag clean-up.
    // Observe the stair snap (checkSnapJumpToObject). For measuring the phenomenon where x
    // advances extra on the landing tick
    bool snapTrace = false;
    // cfg snapnodesync (on by default; snapnodesync=0 is the off arm): with the visibility pass
    // skipped (screen off), put the object a stair snap reads at its real position first
    // (hooks_player, checkSnapJumpToObject). The convention is the real position, not the node a
    // 1x run would show (that one lags by up to a render frame and so depends on the refresh rate).
    // Scope: it acts only on a screen-off snap (visibilityPassOff()). With the screen on, nothing is
    // synced, and an object caught mid-move is still read at the node the last render left, so a
    // screen-on snap keeps depending on the render cadence.
    bool snapNodeSync = true;
    bool snapNodeLog = false;   // cfg snapnodelog=1: print a snap whose object's node is off its real position
    // cfg resetlog (off; 1 = on): one result.txt line per PlayLayer::resetLevel, before and after the engine's
    // reset: the attempt, what kind of reset it was, the persistent item table and each of its items' live
    // value, the spawn bookkeeping (m_spawnTuples, m_spawnTriggerActions), and how many objects have their
    // group disabled. Read-only (first 2000 resets). The instrument for "what does an attempt start from",
    // to set a fast-mode attempt beside a person's (resets, persistent items, attempt sequences).
    bool resetLog = false;
    // cfg resetdump (off; print only): after each head reset, player 1's whole PlayerObject (`rdump:`), the layer's
    // GJBaseGameLayer part (`rdumpL:`) and the GameObject part of the objects cfg resetdumpuids=a,b,c names
    // (`rdumpO:`), as raw bytes, with every member's name, offset and size once (`rdmem:`) -- the state an attempt
    // inherits before its first tick, for diffing two histories by name.
    bool resetDump = false;
    std::vector<int> resetDumpUids;
    // cfg persistworld=k (off = -1): every head reset starts from one fixed table of kept items. An
    // Item Persistence trigger (3641) writes an item's value into GJEffectManager's kept table
    // (m_persistentItemCountMap, +0x370; timers in m_persistentTimerItemSet, +0x3b0) when it fires
    // (activatePersistentItemTrigger, 0x234a40), and every reset copies that table into the live counts
    // (resetEffects -> transferPersistentItems, 0x262ed0). So an attempt starts with whatever the
    // attempts before it left, the bootstrap record and every flight included, and a level that reads
    // its kept items is a different level on each of them. With this cfg the table is replaced before
    // each head reset by a fresh game's after k empty attempts: empty for k = 0, `persistitems` for
    // k > 0 (that table as k empty attempts leave it -- cfg resetlog's `persistent{...}` on attempt
    // k + 1 of a fresh game). A clear made this way is judged from a fresh game with the same k empty
    // attempts first. The attempt count (item type 5) is not a kept item and is not fixed here (cfg
    // attemptone fixes both, for k = 0, by not copying the table back at all); checkpoint restores are
    // left as GD does them.
    int persistWorld = -1;
    // cfg persistitems=id:v,id:v (with persistworld=k > 0): the kept items of that world.
    std::string persistItems;
    // cfg voidband (on by default since 2026-10; 0 = off): a death at the end of a long run over EMPTY
    // floor closes that run to the search (repair.hpp checkVoidRun). The ground plane under a level is
    // alive in GD, so a body that falls out of the level's lane can run on it for thousands of ticks,
    // and the loop's deepest-first takes that as progress until a catch at the end of the stretch
    // kills it.
    bool voidBand = true;
    // cfg voidcredit (default off): a closed void run that also wedges credits the tick it began as the depth to beat (repair.hpp,
    // voidCredit): GD flew the plan alive to there, and a wedge never ranks, so without it the loop rewinds past that ground.
    bool voidCredit = true;   // on by default (release flags, 2026-10-17); cfg voidcredit=0 is the off arm
    // cfg stallbound (default off): the 40,000-tick "an attempt that never ends" bound grows to the level's length at the slowest speed
    // (hooks_gamelayer.cpp). One custom level runs ~52,000 ticks, and every plan alive at 40,000 was put down as a death.
    bool stallBound = true;   // on by default (release flags, 2026-10-17); cfg stallbound=0 is the off arm
    // cfg recprovchain (default off): --recprov names one attempt for the recording and the anchor's
    // rows also when they are two attempts whose worlds agree up to a tick -- a checkpoint flight
    // carries the world of the attempt that took the checkpoint up to its tick (repair.hpp,
    // recProvArg) -- with the reach cut at that tick.
    bool recProvChain = false;
    // cfg cpfollowsection (on by default since 2026-10; 0 = off): a checkpoint restore puts each Follow Player Y object back in
    // the collision section cell the flight had it in when the checkpoint was taken
    // (hooks_gamelayer cpRestoreState). processPlayerFollowActions moves such an object without
    // re-bucketing it, so a run from the head meets it only through the cell it was last bucketed in;
    // the checkpoint load re-buckets it where it stands, and a flight from a checkpoint then met
    // objects a run from the head does not.
    bool cpFollowSection = true;
    bool hitboxTrace = false;   // record the hitboxes GD actually uses (cfg hitboxtrace=1)
    // cfg killersite=1: with every `killer:` line, where in GD the call came
    // from. `killer:` names the OBJECT, and an object-free death names nothing
    // at all -- which is most of what the lv22 c1 corridor produces. The call
    // site is the only thing that separates one object-free rule from another.
    bool killerSite = false;
    // Print the raw fields that decide collidedWithObjectInternal's return value, at the call
    // itself (cfg `fieldprobe=1`, `fprobe:` lines). Reading them off the object/player by literal
    // offset is deliberate: the bindings carry no offset annotations here, and inferring a
    // position from an `m_unkNNN` name has already produced a wrong answer once. The line
    // therefore carries a LAYOUT CANARY -- offsetof(PlayerObject, m_isUpsideDown), which must
    // read 0x9bf -- so a disagreeing compiler layout invalidates the row instead of silently
    // renaming the bytes.
    bool fieldProbe = false;
    // Watch the candidate list of collisionCheckObjects (cfg `watchuid=N`, combined with the
    // hbfrom/hbto window). Speed portal lv19 uid13689 "overlapping by 4px for 3 ticks yet not
    // firing" cannot be explained by the test formula (plain AABB, confirmed by disassembly),
    // leaving only one suspect: "not in that tick's objects list" (section membership).
    // On every call, look for the uid in the list and emit presence + the player rect as a
    // `ccl:` line
    int watchUid = 0;
    // Tick window for emitting hbox/hbin (cfg hbfrom / hbto, default is the whole run).
    // The 40,000-line cap runs out after a few thousand ticks, so to look at the late part of
    // a long level you must cut a window or not a single line survives at the tick you want
    // (while investigating x=24,843 of lv20 it was cut off at t=14,009).
    long long hbFrom = 0, hbTo = 0;   // hbTo=0 means no upper bound
    // Name the object the player is standing on (m_objectSnappedTo / m_currentSlope).
    // Needed to track surfaces that cannot be looked up by position (objects that do not
    // exist at load time)
    bool standTrace = false;    // cfg standtrace=1
    bool noDeath = false;       // swallow deaths (cfg nodeath=1, observation only)
    // cfg nodeath as given: 0 none, 1 both bodies (noDeath), 2 only player 1's deaths are swallowed, 3 only player 2's (observation: the other body
    // dies as usual, so a two-player level says which body can walk its side of the level with no input). With 1, 2 or 3 in a replay session each
    // swallowed call writes its `killer:` line (who=p1/p2, where), which is the detection of where an idle body would die.
    int noDeathCode = 0;
    // Rollback verification (method B: practice-mode checkpoints)
    int practiceAt = -1;     // turn practice mode ON at this tick
    int checkpointAt = -1;   // create a checkpoint at this tick
    int restoreAt = -1;      // restore at this tick (once only)
    // cfg `snapat=t1,t2,...`: take a checkpoint at EVERY tick
    // in the list during one replay of a verified solution, so that a section
    // run can start from any window's entry without replaying the level again.
    // The trajectory the veto boxes are translated through is not a new output:
    // dump.csv already carries tick, x and y for the whole pass.
    std::vector<int> snapAt;
    // cfg `snapverify=N`: after the last window has gone by, restore each
    // snapshot and run N ticks, comparing against what this same pass did from
    // the head. This is wanted on every snapshot -- it is the
    // acceptance that a section's entry is faithful, and it doubles as the
    // regression detector for five known holes.
    int snapVerify = 0;
    // cfg `cpflight=S` (600 by default since 2026-10; 0 = off): the loop's flights take a checkpoint
    // every S ticks, and a plan whose inputs agree with an earlier attempt's up to one of them flies
    // from it instead of from tick 0 (mod/cp_flight.hpp).
    int cpFlight = 600;
    // cfg `cpflightprobe=1` (with cpflight=S): the loop keeps flying from the head, and each death
    // is flown again from its own latest checkpoint while the solve runs and compared with the head
    // flight (`cpflight: probe` lines). Print only.
    bool cpFlightProbe = false;
    // cfg `cpflightprobecarry=1` (with cpflightprobe=1; print only): a probe is flown and compared with
    // its control also when what the control inherits is not what the checkpoint's attempt inherited --
    // the case the carry gate flies from the head. The verdict line names the parts that differ and
    // whether the first difference comes before the checkpoint (the inheritance changed the head
    // flight's own way to it) or after it (something the restore leaves as the inheritance has it);
    // the session line counts the verdicts by part.
    bool cpFlightProbeCarry = false;
    // cfg `cpflightgaps=1` (with cpflight=S; print only): the state a checkpoint restore is not known to
    // bring back -- the disabled (broken) objects, the persistent item counts, the players' rotate-object,
    // follow-float and touching-ring containers, the layer's non-effect list -- fingerprinted at each
    // take and again right after each restore (`cpflight: gaps` lines; cp_flight.hpp GapSig).
    bool cpFlightGaps = false;
    // cfg `cpdisabled` (on by default since 2026-10-14; 0 = off): after a checkpoint restore of the loop,
    // every object's two disabled flags (m_isDisabled / m_isDisabled2 -- a broken breakable block) are put
    // back as they were when the checkpoint was taken; the checkpoint does not save them.
    bool cpDisabled = true;
    // cfg `cpfollowfloats` (on by default since 2026-10-14; 0 = off): each player's m_playerFollowFloats
    // is saved with a checkpoint of the loop and put back after its restore; the checkpoint does not
    // save it.
    bool cpFollowFloats = true;
    // cfg `cprotnode` (on by default since 2026-10-14; 0 = off): the node rotation of every object a
    // trigger has rotated is saved with a checkpoint of the loop and put back after its restore, which
    // otherwise sets it from the object's start rotation plus its offset (cp_flight.hpp captureRotNodes).
    bool cpRotNode = true;
    // cfg `cppersistguard` (on by default since 2026-10-14; 0 = off): while the level's Item Persistence
    // table holds anything (and cfg attemptone is off), the loop's plans fly from the head, not from a
    // checkpoint: GD keeps that table across attempts and a checkpoint does not rewind it (cp_flight.hpp
    // requestStart).
    bool cpPersistGuard = true;
    // cfg `cpcarryparts=<mask>` (2 by default since 2026-10-18, 3 before it, 31 = every part, as before): the parts of
    // what an attempt inherits that turn a checkpoint flight down when they differ from the checkpoint
    // attempt's -- 1 band, 2 rect latch, 4 area list, 8 players' snap, 16 stale nodes. 2 keeps the latch only: a band
    // that differs does not turn a checkpoint flight down (a long custom level: all 20 refusals of a run were the band, and
    // letting them fly cut the flights from 159 s to 38 s with the same solution). cpcarryparts=3 is the arm with the band.
    // The nodes part stays in force whatever the mask unless cfg snapnodesync is on and the
    // screen is off (cp_flight.hpp gateMask).
    int cpCarryParts = 2;
    // cfg `cpactivation` (off by default): a checkpoint restore of the loop, the takeover and the seek
    // bar puts every object's activation and the layer's active list back as the flight had them
    // (cp_flight.hpp, AreaState).
    // cfg `cpvisafter` (off by default): a checkpoint restore skips the visibility pass inside its reset
    // and runs one after the camera is put back (hooks_playlayer updateVisibility, cpRestoreState).
    bool cpVisAfter = false;
    int cpActivation = 0;   // 1 = the flag only, 2 = through GD's transitions + the list (crashes the draw)
    // cfg `cpflightobj=<uid>` (print only): that object's area offsets, stamp and rect around each
    // checkpoint and restore of cfg cpflight (`cpflight: obj` lines).
    int cpFlightObj = -1;
    // cfg `cpflightobjfrom=<t0>` / `cpflightobjto=<t1>` (with cpflightobj; print only): that object's
    // line after every update call whose tick is in [t0, t1], in every flight (`cpflight: obj tick att=N`).
    int cpFlightObjFrom = -1, cpFlightObjTo = -1;
    // cfg `cpflightprobeat=<tick>` (with cpflightprobe): probe from the latest checkpoint at or before
    // that tick, so every death past it flies the same stretch again. -1 = the latest before the death.
    int cpFlightProbeAt = -1;
    // cfg `cpflightlayerdiff=1` (print only): the PlayLayer's raw bytes at each checkpoint, and after a
    // restore the ranges that differ from them (`cpflight: layerdiff` lines).
    bool cpFlightLayerDiff = false;
    // cfg `cpflightcarrylog=1` (print only): each attempt's carry signature (`cpflight: carry` lines).
    bool cpFlightCarryLog = false;
    // cfg `cpflightnodewatch=<uid>[,<uid>...]` (print only): those objects' nodes at each reset and
    // restore and whenever they move (`cpflight: node` lines).
    std::string cpFlightNodeWatch = "-";
    // cfg `robodbg=t0,t1`: per-substep state over a tick range, from both the
    // plain replay and the section search (they share processCommands).
    long long roboDbg0 = -1, roboDbg1 = -1;
    // Feasibility measurement for the section solver (cfg `restoreloop=N`).
    // Restore N times in a row from the checkpoint created by checkpointat and report the cost
    // per restore. This number decides the design: 300 states per layer x 200 layers = 60,000
    // restores, so 1ms each is 60 seconds, 50ms each is 50 minutes and unusable.
    int restoreLoop = 0;
    // cfg `restoreloopkeep=1`: do NOT clear m_checkpointArray between the loop's
    // restores, so it grows one checkpoint per iteration the way the section
    // search grows it. Without this the bench holds the array at length 1 and is
    // blind to any cost that scales with it -- which is exactly the quantity the
    // 2026-09-02 sweep had left unmeasured (restores flat, depth +9%).
    bool restoreLoopKeep = false;
    // cfg `restoreloophold=N`: keep N extra CheckpointObjects ALIVE (retained,
    // never restored from) while the loop is timed, so the only thing that
    // varies between runs is how many live copies of the game state exist. The
    // search holds one per retained node; this bench held exactly one, which is
    // why it is flat where the search ramps.
    int restoreLoopHold = 0;
    // cfg `restoreloopcycle=1`: restore from the HELD checkpoints in turn rather
    // than from the same one every time, so each restore reads an object that has
    // not been touched for N iterations -- which is the one thing the search does
    // that this bench did not.
    bool restoreLoopCycle = false;
    // The two control arms for what the bench does that the search does not.
    // `restoreloopclearq=1`: drop m_queuedButtons every restore, as secRestoreFrom
    // does -- resetLevel pushes one synthetic command per restore and
    // removeReleasedButtons walks the whole queue.
    // `restoreloopdrain=1`: pop cocos's autorelease pool every restore, standing in
    // for the end of a frame this loop never reaches.
    bool restoreLoopClearQ = false;
    bool restoreLoopDrain = false;
    // cfg `restoreloopheap=<MB>`: allocate and TOUCH that many MB before the
    // loop starts and hold it for the whole bench, so the process runs with a
    // heap the size the search ends up with (325 -> 545 MB over 584 layers)
    // while the number of cumulative restores stays at zero.
    //
    // This is the arm that separates the two readings of the ramp. The section
    // vectors are out (2026-09-03: the sort walks exactly 2,811 objects on every
    // one of 584 layers while the per-restore cost goes 2,733 -> 6,841 us), and
    // what is left is a fixed amount of work getting slower as the heap grows.
    // If per-restore starts high here, the cause is the heap's SIZE and
    // "cumulative restores" was only a correlate; if it starts low and still
    // ramps, the restore itself is leaving something behind.
    int restoreLoopHeapMB = 0;
    // cfg `restoreloopdrainat=N`: pop the autorelease pool ONCE, after the Nth
    // restore, instead of on every one. The buckets on either side of N then
    // answer the follow-up: if the cost falls back after the drain, it tracks
    // the LIVE heap (which is the locality reading); if it stays high, something
    // persistent is left over that a drain does not reach.
    int restoreLoopDrainAt = 0;
    // cfg `oobtest=1`: force the out-of-bounds latch (player+0x187) to 1 just
    // before the checkpoint is taken, so that the save/restore of it can be
    // tested at all. The latch is only set by a substep that is genuinely out
    // of bounds, and a plain replay cannot reach that -- the band clamps.
    bool oobTest = false;
    // cfg `vytest=<double>`: write this y velocity just before the checkpoint
    // is taken, to test whether the restore re-rounds it onto the 0.001 grid.
    // Injected rather than found, because the checkpoint does not land on the
    // tick it is asked for (markCheckpoint has its own spacing), and the
    // half-grid values are single ticks -- six checkpoints aimed at three of
    // them all landed 1 to 17 ticks late, on grid values.
    double vyTest = 0.0;
    bool vyTestOn = false;
    // A panel session started with Coins on (g_uiCoins): its solution and iteration map are the
    // COIN files (solution_lv<N>_coins.txt, itermap_lv<N>_coins.txt), so a coin solve does not
    // replace the plain solution Replay plays, and a coin replay finds the coin one. Set by the
    // panel only; a cfg-driven run keeps its file names whatever it routes for.
    bool coinFiles = false;
    bool coinMode = false;   // enable our own coin-pickup detection (for coin verification
                             // during replay)
    // cfg `coinroute=1`: the repair loop solves for every coin as well as the end. The search
    // gets --coins (and --coinmask at an anchor), and an attempt that passes a coin GD has not
    // credited is ended there (hooks_gamelayer.cpp), the same verdict the search's miss prune
    // gives. Implies coinMode: GD's own pickup (the pickupItem hook) is what it reads.
    bool coinRoute = false;
    // cfg `worldgates` (default on, worldgates=0 is the off arm): the level's item gates (Count, Tap, Item
    // Compare and the pickups that feed them) are part of the model's world without coinroute too
    // (dp --worldgates): the searches and the fixup replay get --worldgates and items.txt, and the
    // anchors carry GD's item counters (--itembase). The coins themselves stay coinroute's.
    // On since a custom level's spider pad is switched on by a Count fed by two pickups (dp --countactgrp): off,
    // the model only had the recording's pad, and no cold run took both pickups. Six gated levels A/B
    // (900 s, secsolve off) cleared the same two either way.
    bool worldGates = true;
    // cfg `coinwatch=1`: repeat the `coinlive:` line every 1,200 ticks. For the
    // question "when does this coin's collision rect move", which the once-per
    // -coin default cannot answer.
    bool coinWatch = false;
    // cfg `areaenv=1` turns on the recording of where GD's own random numbers can put an Area
    // Move's objects (solver/areaenv.hpp), which the model then treats as deadly. Off by default since
    // 2026-10: the seeds are fixed while the mod drives (cfg rngfix), so the recording holds this
    // game's rects and every attempt and replay meets the same ones. A plan played without the mod
    // (another bot, no fixed seeds) can meet other positions.
    bool areaEnv = false;
    // Music handling. continuous=keep the song playing / mute=silent / normal=untouched
    std::string music = "continuous";
    // The human takeover (mod/takeover.hpp): fly the plan to a point, put the strict checkpoint
    // there, and give the input to a person. cfg `takeover=<tick>` hands over at the first update
    // boundary at or past that tick, cfg `takeoverx=<x>` once player 1's x has passed it (either
    // arms it; neither = off). cfg `takeoverwait=<s>` is the stop before the person's run starts,
    // after the checkpoint is taken and after every restore. A debugging instrument: what the
    // person plays is measured, never solved from.
    long long takeoverAt = -1;
    float takeoverX = 0.f;
    double takeoverWait = 1.0;
    // cfg `takeoverkey=1` (off by default): no point is fixed ahead. The plan plays as an ordinary
    // replay, and the takeover end key (setting key-takeover-end, F8) hands the input over where the
    // replay stands -- the strict checkpoint is put there and the mode begins as it does at a fixed
    // point. The same key then ends it, as before.
    bool takeoverKey = false;
    std::vector<InputCmd> inputs;
    std::vector<ToggleCmd> toggles; // for verification D: switch mode at the given tick (calls
                                    // the same function a portal does)
};

inline Config g_cfg;
inline bool g_started = false;
// This session was started from the play menu (hooks_playmenu.cpp). Comments across the mod
// call it a "panel" session, after the corner panel the menu replaced.
inline bool g_uiSession = false;

// ---- the level suite (autorun.cfg `levels=1,2,...`) ------------------------
//
// Several levels solved one after another WITHOUT RESTARTING THE GAME. One process per level
// is what everything here has always done, and under that arrangement nothing about a second
// level is ever exercised: every global starts at its declared value, every file in the data
// dir was deleted by the launcher, and the mod's own duty to clean up after a level is an
// assumption no test can fail. It went wrong exactly there -- a level's moving-geometry
// recording was still being offered to the next level (src/solver/grouptrace.hpp) -- and the
// whole 22-level cold regression was green throughout, because it never once solved two
// levels in the same game.
//
// So the suite is not a convenience for running the sweep with fewer launches. It is the only
// arrangement in which "starting a level leaves nothing of the last one behind" is a claim
// with evidence, and the iteration counts it produces have to equal the ones a fresh process
// gives, level for level. That equality is the test.
//
// DELIBERATELY NOT A Config MEMBER: leaving a level resets g_cfg to its defaults
// (PlayLayer::onQuit), and this list has to outlive that.
namespace suite {

inline std::vector<int> g_levels;   // in the order they will be solved
inline size_t g_at = 0;             // index of the one running now
// The session has ended and the next level is waiting for the scene to be clear enough to
// enter (see SuiteKeeper in level_entry.hpp -- a level cannot be entered mid-transition).
inline bool g_advance = false;
inline int g_waited = 0;            // resident-poll ticks since the session ended
inline int g_settle = 0;            // ...and ticks since the scene became enterable

inline bool active() { return !g_levels.empty(); }
inline bool hasNext() { return active() && g_at + 1 < g_levels.size(); }
inline int current() { return g_at < g_levels.size() ? g_levels[g_at] : 0; }

// cfg `leveldir=<dir>` (default empty = off): a suite level whose ID is not one of the 22 main
// levels is read from `<dir>/<id>.lvl` -- a raw level string, as `levelfile=` takes -- so levels
// that exist only as files can be solved in one game too. Only that directory is read: an ID with
// no file there refuses the suite rather than falling back to a saved or online level of that ID.
// Main levels 1-22 are still entered by ID. With a directory set the IDs must be strictly
// ascending, which fixes the order and refuses a duplicate. Checked once, at the first level
// (level_entry.hpp suiteLevelSource). Kept here, not in Config, for the same reason as g_levels.
inline std::string g_levelDir;
inline bool g_levelDirChecked = false;

// cfg `suiteplans=<dir>` (default empty = off): each suite level plays the plan `<dir>/solution_lv<ID>_dp.txt` (a plan file, as `planfile=` takes it), put into
// g_cfg.inputs after the cfg is read for that level. The cfg is read again for every level of a suite and holds one plan at most, so a suite that replays (solve=0) needs
// the plan of each level named by its ID; this is how cold_regress --fresh-verify-one-session plays the saved solutions of all its levels in one game. A level with no such
// file plays no input and says so in result.txt. Kept here, not in Config, for the same reason as g_levels.
inline std::string g_planDir;

}  // namespace suite
// On the session's first resetLevel, turn practice mode OFF and wipe all checkpoints. If the
// leftovers of the previous session (practice ON + a checkpoint near the end) carry over, the
// checkpoint restore gives "instant clear -> left behind"
inline bool g_forceCleanStart = false;
// ---- When recording is suppressed ----
// [2026-08-23, user decision] Only while the mod is DRIVING the game -- solving and replaying.
// The mod merely being loaded no longer suppresses anything: a human playing normally records
// progress, achievements and statistics as usual.
//
// The predicate is `g_started`, i.e. "an automated session is open", and NOT a list of the
// modes that count. That distinction is the whole point: the earlier attempt gated on
// `solve=1`, and the verification harness (which runs with solve=0) sailed straight through --
// 209 clears out of 252 runs were recorded before anyone noticed. Every automated path
// (autorun.cfg, serve mode, a session started from the panel, the section solver, plan replay,
// every verification harness) opens a session, so gating on the session covers them all,
// including ones not written yet.
//
// (Until 2026-08-23 there was a second term for the one-key warp, which drove the player
// without a session. The warp is gone, so a session is the whole story again.)
inline bool botDriving() { return g_started; }

// Whether the level's own record is being held back right now. Lives here, next to the state it
// reads, because both the hooks and the guard around GD's originals must ask the SAME question:
// the first version of this had the guard testing only the cfg switch, and a human's clear was
// still not recorded even though every hook correctly passed it through.
inline bool progressBlockActive() { return botDriving() && g_cfg.progressBlock; }

// Whether this build carries the test instruments below (CMake GDSOLVER_TEST_INSTRUMENTS, OFF unless a measurement asks): in any other build their
// cfg keys are read and ignored, with a line in the log (testInstrument in session.hpp), so a release cannot be told to drop a player's requests.
#ifdef GDSOLVER_TEST_INSTRUMENTS
inline constexpr bool kTestInstruments = true;
#else
inline constexpr bool kTestInstruments = false;
#endif
// TEST ONLY. cfg `netcatch_testonly` (autorun.cfg, no session needed), for the network layer's positive
// control: a person plays, the game builds its death-data upload, and the request is seen at
// CCHttpClient::send and dropped there, so nothing reaches a server.
//   1          refuse and name EVERY request the process makes, whoever asked (a player's too)
//   deathdata  refuse and name only the death-data upload (uploadGJDeathData); everything else passes,
//              so a test account can still log in and the game otherwise behaves as it does
// Never on outside a measurement: process-wide on purpose (not in g_cfg, which a session resets), off
// unless that file says so (no UI sets it, no default turns it on), and every request it refuses is
// named in the log.
inline int g_netCatchTestOnly = 0;   // 0 off, 1 everything, 2 the death-data upload only
// TEST ONLY. cfg `nettrace_testonly=1`: name every request that goes out and what comes back (method, host, path, sizes, the HTTP code, a bare
// numeric answer; never a query, a body or an answer's text). Process-wide, off unless autorun.cfg says so. See nettrace in record_gate.hpp.
inline bool g_netTraceTestOnly = false;
// TEST ONLY. cfg `deathtest_testonly=1`: flag the level being entered for 2.209's death-data upload (see deathtest in record_gate.hpp). Only ever with
// netcatch_testonly=1, on a throwaway profile.
inline bool g_deathTestTestOnly = false;
// TEST ONLY. cfg `heapphase_testonly=<n>` (n >= 1): validate every heap at each change of the section search's step, at most once every n-1 ms (postmortem.hpp
// heapPhaseCheck). 0 = off.
inline int g_heapPhaseTestOnly = 0;
// TEST ONLY. cfg `phasefile_testonly=1`: the section search's step written over one line of <data>/secphase.txt at every change (postmortem.hpp phaseFileWrite).
inline bool g_phaseFileTestOnly = false;
// The two gates below the function-level one (record_gate.hpp, hooks_record_gate.cpp): what leaves
// the process and what reaches the disk. Not the same question as progressBlockActive: that one
// asks whether the level's record is held back, these ask whether GD may talk to a server or
// write a save file at all while the mod drives.
inline bool netBlockActive() { return g_netCatchTestOnly == 1 || (botDriving() && g_cfg.netBlock); }
// True while a driven session's level is being left (PlayLayer::onQuit). The session has already
// been torn down by then -- g_started is false and g_cfg is back to its defaults -- but GD's own
// leave-the-level code still runs, and it is the one that writes the platformer save.
inline bool g_quitDriving = false;
// ...and the same for the level's record: set across the leave of a driven level, so that what GD commits
// to the record while leaving (the play time) is undone too (hooks_playlayer.cpp, commitPlayTime209).
inline bool g_quitProgress = false;
inline bool timeBlockActive() { return progressBlockActive() || g_quitProgress; }
// saveBlockActive() is below, after g_sessionOver.

// Number of blocked achievements / statistics / coins (logged at every session end to make the
// feature visible)
inline long long g_blockedAch = 0, g_blockedStat = 0, g_blockedCoin = 0;
// Number of blocked level-progress records: the percent / attempt / reward writes that GD does
// from destroyPlayer and levelComplete (GJGameLevel::savePercentage and friends). Kept separate
// from the statistics counter because these are the paths that write the LEVEL's own record --
// the progress bar percentage, the attempt count, orbs, diamonds, the completion flag.
inline long long g_blockedProgress = 0;
// Requests GD handed to its HTTP client while the mod drove, and save-file writes it asked for.
// Both are refused (see hooks_record_gate.cpp) and both are counted: the session-end line says
// how many, so "nothing left the process" is a number and not an argument.
inline long long g_blockedSend = 0, g_blockedSave = 0;
// 2.209's per-reset death-data rollover that was not let through while driving (hooks_system.cpp). Not a blocked
// recording path -- the player's data being filed away, at the bot's time -- so it is counted on its own.
inline long long g_rolloverSkipped = 0;

// The level's own counters, sampled when the session opens and again when it ends. GD keeps
// them on GJGameLevel and they persist into the save, so "did this session record anything"
// has to be answered with the numbers, not with an argument about which hooks exist.
struct LevelProgress {
    int attempts = -1, jumps = -1, clicks = -1;
    int normalPercent = -1, practicePercent = -1;
    int newNormalPercent2 = -1, orbCompletion = -1;
    bool valid = false;
};

inline LevelProgress sampleProgress(GJGameLevel* lv) {
    LevelProgress p;
    if (!lv) return p;
    p.attempts = lv->m_attempts.value();
    p.jumps = lv->m_jumps.value();
    p.clicks = lv->m_clicks.value();
    p.normalPercent = lv->m_normalPercent.value();
    p.practicePercent = lv->m_practicePercent;
    p.newNormalPercent2 = lv->m_newNormalPercent2.value();
    p.orbCompletion = lv->m_orbCompletion.value();
    p.valid = true;
    return p;
}

inline std::string progressDiff(const LevelProgress& a, const LevelProgress& b) {
    if (!a.valid || !b.valid) return "unavailable";
    std::string s;
    auto one = [&](const char* name, int x, int y) {
        if (x != y) s += std::string(s.empty() ? "" : " ") + name + "="
                       + std::to_string(x) + "->" + std::to_string(y);
    };
    one("attempts", a.attempts, b.attempts);
    one("jumps", a.jumps, b.jumps);
    one("clicks", a.clicks, b.clicks);
    one("normal%", a.normalPercent, b.normalPercent);
    one("practice%", a.practicePercent, b.practicePercent);
    one("newNormal%2", a.newNormalPercent2, b.newNormalPercent2);
    one("orbs", a.orbCompletion, b.orbCompletion);
    return s.empty() ? "none" : s;
}

// The record to restore to. Sampled when the level is entered AND again when a session opens:
// if a human played first and the mod takes over afterwards, what has to be preserved is the
// record as of the moment the bot started, not as of level entry.
inline LevelProgress g_progressAtStart;
inline GJGameLevel* g_progressLevel = nullptr;
// How many times a counter had to be put back (see restoreProgress). This is what keeps the
// "level record changed: none" line from being vacuous: the guard prevents the recording block,
// and this counts the writes that happen outside it and are undone.
inline long long g_progressRestores = 0;

// Put the level's record back to what it was when the session opened.
//
// Not everything can be blocked at the door: PlayLayer::resetLevel increments
// `m_level->m_attempts` INLINE (measured at PlayLayer::resetLevel+0x9cc on 2.2081 -- the
// SeedValue read-modify-write is emitted straight into the function), so there is no call to
// hook. The same is true of the level's jump counter. For those, GD is allowed to write and the
// value is restored afterwards -- the same "let it happen, then correct it against the truth"
// shape the solver uses elsewhere.
inline void restoreProgress() {
    if (!progressBlockActive()) return;
    if (!g_progressLevel || !g_progressAtStart.valid) return;
    const LevelProgress now = sampleProgress(g_progressLevel);
    if (!now.valid) return;
    const LevelProgress& w = g_progressAtStart;
    if (now.attempts == w.attempts && now.jumps == w.jumps && now.clicks == w.clicks
        && now.normalPercent == w.normalPercent && now.practicePercent == w.practicePercent
        && now.newNormalPercent2 == w.newNormalPercent2
        && now.orbCompletion == w.orbCompletion)
        return;
    ++g_progressRestores;
    g_progressLevel->m_attempts = w.attempts;
    g_progressLevel->m_jumps = w.jumps;
    g_progressLevel->m_clicks = w.clicks;
    g_progressLevel->m_normalPercent = w.normalPercent;
    g_progressLevel->m_practicePercent = w.practicePercent;
    g_progressLevel->m_newNormalPercent2 = w.newNormalPercent2;
    g_progressLevel->m_orbCompletion = w.orbCompletion;
}
// Number of times focus loss nearly stopped us (the main reason workers die silently, so it goes
// into the session log)
inline long long g_bgBlocked = 0, g_resignBlocked = 0;
inline int g_attempt = 0;
// ...and a serial that never restarts (g_attempt does, per level): which attempt a recording and an
// anchor's rows came from, told apart across levels of one session (--recprov, repair.hpp).
inline long long g_attemptSerial = 0;
inline size_t g_nextInput = 0;
// The input cursor as an attempt's first tick on the active layer found it (processCommands), and
// whether that tick has come yet (cleared by the attempt's reset). 0 unless something took the
// plan's inputs before the attempt started -- what a slice's check flight has to rule out before a
// parting from the copy is read as the cut's (level_slice.hpp onVerifyDeath).
inline size_t g_nextInputAtStart = 0;
inline bool g_attemptFed = false;
inline long long g_frame = 0;
inline int g_traceLines = 0;
inline int g_finishedAttempts = 0;
inline bool g_sessionOver = false;
// Saves are refused while a session is RUNNING, not for as long as botDriving() holds. That one stays
// true until the level is left, and a save in that stretch -- the player closing the game over a
// finished solve's results screen -- is the player's own: the memory it writes was audited at the
// session's end (memaudit), and refusing it would drop whatever else they changed since the last
// save. The record the bot made outside the audit's sight (the platformer save) is covered by
// g_quitDriving, which holds across the leave that writes it.
inline bool sessionRunning() { return g_started && !g_sessionOver; }
inline bool saveBlockActive() { return (sessionRunning() || g_quitDriving) && g_cfg.saveBlock; }
inline size_t g_nextToggle = 0;
inline bool g_injecting = false;
// The jump button as GD was last told, tracked in the handleButton hook. This is
// the RAW press, which is not the same thing as the +0x986 latch: that one means
// "a press that has not been consumed yet" and every consumer clears it (the
// grounded jump, rings, taps -- ten of them), so it reads 0 while the button is
// still down. A gate on "is the player pressing" has to read this one.
inline bool g_btnDown = false;
inline std::chrono::steady_clock::time_point g_attemptStart;

// The human takeover's state (the record writing is mod/takeover.hpp). Here, ahead of everything
// that has to ask: the input block, the hotkeys, the HUD and the session reset. While a person
// has the input the session stays open, so botDriving() -- and every block hung on it -- holds.
namespace cpflight { struct Rec; }
namespace takeover {
inline bool armed() { return g_cfg.takeoverAt >= 0 || g_cfg.takeoverX > 0.f || g_cfg.takeoverKey; }
// Armed by the key alone (cfg takeoverkey): no tick or x to fly to. Until the key is pressed the
// session is an ordinary replay.
inline bool keyOnly() { return g_cfg.takeoverKey && g_cfg.takeoverAt < 0 && g_cfg.takeoverX <= 0.f; }
inline bool g_live = false;            // a person has the input (from the take to the mode's end)
inline bool g_takeNow = false;         // the key was pressed: the start point is here, at the next
                                       // update boundary
inline bool g_done = false;            // the mode has ended in this session (it does not re-arm)
inline bool g_restoring = false;       // the reset that loads the checkpoint is running
inline bool g_restorePending = false;  // a reset GD asked for, to be a restore at the frame boundary
inline bool g_waiting = false;         // the stop before the person's run (cfg takeoverwait)
inline std::chrono::steady_clock::time_point g_waitUntil{};
// One strict checkpoint of the mode: the record (cp_flight.hpp), the inputs every attempt started
// from it flies before it -- the plan up to it, and a release there if the button was held --,
// where it was taken (a restore must land exactly there), and the lineage its per-tick rows come
// from.
struct Cp {
    std::shared_ptr<cpflight::Rec> rec;
    std::vector<InputCmd> before;
    long long tick = -1;
    float x = 0.f, y = 0.f, vy = 0.f;
    int n = 0;             // 1 = the start point's, which is never removed
    int dumpAttempt = 0;   // the session attempt that took it (dump.csv's attempt column)
    int parent = 0;        // n of the checkpoint that attempt started from (0: the plan's flight)
    const char* source = "";
    // The game's own checkpoint this one stands for (compared, never dereferenced): when the game
    // takes that one back, this one goes too.
    const void* gdSrc = nullptr;
};
enum class TakeKind { Start, Now, Pending };
inline std::vector<Cp> g_cps;          // the stack; [0] is the start point's
inline int g_from = 0;                 // n of the checkpoint the attempt in progress started from
inline int g_nextN = 0;                // the next checkpoint's n
inline bool g_cpWanted = false;        // the game stored one of its own at once (Z, a checkpoint
                                       // in the level, its on-ground auto checkpoint): a strict one
                                       // at the next update boundary
inline const void* g_wantedSrc = nullptr;   // ...the one it stored (compared, never dereferenced)
// The game's auto checkpoints are two-step (PlayerObject::tryPlaceCheckpoint): created where it
// judges the player safe and kept pending on the player (m_pendingCheckpoint), then stored only
// once the player has outlived it for a while. A restore of it is a restore to the safe moment, so
// the strict one is taken when the pending one appears and kept aside until the game stores that
// one (committed) or lets it go (dropped).
inline Cp g_pending;
inline bool g_hasPending = false;
inline const void* g_pendingSrc = nullptr;   // the game's pending checkpoint g_pending was taken for
inline bool g_commitPending = false;         // ...which the game has just stored
inline long long g_T = -1;             // the start point's tick
inline std::vector<InputCmd> g_human;  // this attempt's own presses (jump, player 1)
// The plan as it was before the start point cut it (g_cfg.inputs / toggles are the checkpoint's
// from then on): the replay key puts it back when it leaves the mode in a session armed by the
// key alone, so the replay plays whole again and the takeover key can be pressed anew.
inline std::vector<InputCmd> g_fullInputs;
inline std::vector<ToggleCmd> g_fullToggles;
inline int g_other = 0;                // presses the plan format cannot carry (other buttons, 2P)
inline int g_attemptNo = 0;            // the mode's attempts: 1 is the one that took the checkpoint
inline int g_deaths = 0, g_restores = 0, g_missed = 0;
inline std::string g_dir;              // this session's folder for the records ("" until the take)
inline bool g_written = false;         // the attempt in progress has been written out
inline std::string g_endWhy;           // why the mode ended (the HUD says so)
// Still flying the plan towards the point (armed, not yet taken, not over).
inline bool approaching() { return armed() && !g_live && !g_done && g_started && !g_sessionOver; }
// A person, not the plan, is pressing now (the input block is lifted for them).
inline bool personDriving() { return g_live && g_started && !g_sessionOver; }
// The mode owns the level: F7 and the seek and speed keys would restart or run it under the person.
// A key-armed session that has not been taken over yet is an ordinary replay and keeps them.
inline bool ownsLevel() { return armed() && !(keyOnly() && !g_live && !g_done); }
// GD asked for a reset while a person plays: it becomes a restore of the checkpoint.
inline bool takesReset() { return g_live && !g_restoring && g_started && !g_sessionOver; }
// The batch of update calls stops here: the checkpoint has just been taken or loaded, and the
// person's run starts after the stop, at the next frame.
inline bool stopsBatch() { return g_live && g_waiting; }
inline void resetState() {
    g_live = g_done = g_restoring = g_restorePending = g_waiting = g_cpWanted = g_takeNow = false;
    g_T = -1;
    g_cps.clear();
    g_pending = Cp{};
    g_hasPending = g_commitPending = false;
    g_pendingSrc = nullptr;
    g_wantedSrc = nullptr;
    g_from = g_nextN = 0;
    g_human.clear();
    g_other = g_attemptNo = g_deaths = g_restores = g_missed = 0;
    g_dir.clear();
    g_written = false;
    g_endWhy.clear();
}
// Defined in mod/takeover.hpp (the key is polled in session_hotkeys.hpp, ahead of it).
inline void endByKey();
inline void keyPressed();
inline void takeNow(const char* by);
inline bool resetByReplayKey();
}  // namespace takeover

// Live commands (data/cmd.txt): pause / resume / step N
inline bool g_paused = false;
// Serve mode (cfg servemode=1): pause at the head of the attempt after a death (tick 0), and on
// "rerun" in cmd.txt swap in the plan from data/plan_in.txt and run again.
// Turns GD into a resident worker so the incremental DP loop does not relaunch GD every
// iteration
inline bool g_serveMode = false;
// Raised while the in-process solve is running (see dpsolve::start / poll)
inline bool g_dpSolving = false;
// ...and while the SECTION search owns the session (src/solver/secsolve.hpp g_active is a
// reference to this one). It lives here, far ahead of the search itself, because the audio
// predicate below has to be able to ask: the search drives the game with tens of thousands of
// checkpoint restores, each of which is a level reset that the sound engine answers with a fresh
// song and every swallowed death with an effect. With the screen on (F5 during a handoff) that
// was tens of thousands of sound calls the operator could hear -- the search is not a thing
// anybody is listening to.
inline bool g_secSearching = false;
inline bool solvingNow() { return botDriving() && (g_serveMode || g_dpSolving || g_secSearching); }
// The whole SESSION is an automated solve -- a serve worker, or the panel's Solve mode. True
// between the search rounds too, when the mod is replaying its own candidate plan and about to
// go back to searching. solvingNow() above is narrower: only while the search itself runs.
//
// The two are not interchangeable. Keys that belong to the operator of a solve (the render
// toggle) have to work for the whole session, or a run that comes back to 1x for a replay can
// never be sent fast again.
inline bool solveSession() { return botDriving() && (g_serveMode || g_cfg.dpSolve); }
// The in-process solve has found a plan that cleared, and the session has turned into a showing
// of it: rendering on, at 1x, with the song. Everything before this -- including the replays the
// loop runs to test its candidates -- is part of the solve and stays fast and silent.
inline bool g_dpShowSolution = false;
// The session is solving, in the sense the screen should say so. A candidate replay in the
// middle of the loop is still solving; only the final showing is a replay.
inline bool showingSolve() { return solveSession() && !g_dpShowSolution; }
inline bool g_serveWait = false;
inline bool g_serveReset = false;  // reset requested by rerun (executed on the physics-loop side)
// HUD values for DP mode. Only reads what the external driver wrote to data/hud.txt
// (the solver itself lives outside GD, so the mod does not know the progress on its own)
inline int g_hudIter = 0;
// fixup usage (n/cap). The driver passes it as `fixups=n/cap` in hud.txt
inline int g_hudFixups = 0, g_hudFixupCap = 0;
inline float g_hudVerifiedX = 0.f, g_hudAnchorX = 0.f;
inline long long g_hudAnchorT = 0;
inline std::string g_hudPhase;
// Tolerance of the false-clear guard (cfg `clearmargin=<px>`, default 400 = from measurements
// on the official levels). In levels whose ending runs backwards the goal is far before
// levelMaxX, so widen it there
inline float g_clearMargin = 400.f;
// Auto-pause once the player's x passes this (cfg `pauseatx`). 0=disabled. For filming
inline float g_pauseAtX = 0.f;
// Latch to make it one-shot. With just "x >= threshold", resuming gets paused again on the next
// frame and can never resume
inline bool g_pauseAtXFired = false;
// The stall guard wants a resetLevel it must not perform where it runs (inside the fast loop --
// the rest of the frame's batch would run the fresh attempt with a stale input cursor; measured
// as garbage deaths at t=660/1,837 on a prefix that had verified clean dozens of times). Set
// there, consumed at the frame boundary next to dpsolve::poll().
inline bool g_stallResetPending = false;
// How many update calls the fast loop's last batch made (cfg framebudgetms cuts batches short;
// the effect sweep advances by what actually ran).
inline int g_lastBatchCalls = 0;
// Whether to show the top-left HUD (cfg `hud=0` hides it). The player ends up behind the HUD
// text, so hide it when filming
inline bool g_hudOn = true;
// F8: the screen is off because the user asked for it. Kept apart from the fast loop's own
// render skip so that "fast but visible" and "slow but blind" are both reachable -- the two used
// to be one cycling key.
inline bool g_renderOff = false;
// F1: the overlays are hidden. This NEVER hides the bot badge (spec §9 requires a replay to be
// visibly a replay), and the key does not hide them while solving (showing them always works).
inline bool g_overlayHidden = false;
// "Solving" as opposed to "replaying": the mod is being driven as a solver worker (serve mode is
// what the DP driver opens), or it is solving the level in-process (cfg `dpsolve` / the panel's
// Solve mode). Hiding the overlays is refused here.
inline bool solvingNow();
// Spectating speed. Left/right arrows (or up/down) step one notch; F8 toggles between FAST and
// realtime + cycles through 1x and above. Physics stays fixed at 1/240 and only "how many
// substeps per frame" changes, so the trajectory and tick numbers are identical to 1x
constexpr float WATCH_SPEEDS[] = { 0.25f, 0.5f, 1.f, 2.f, 4.f, 8.f, 16.f };
constexpr int WATCH_SPEED_N = (int)(sizeof(WATCH_SPEEDS) / sizeof(WATCH_SPEEDS[0]));
constexpr int WATCH_SPEED_1X = 2;               // WATCH_SPEEDS[2] == 1.0
constexpr float WATCH_SPEED_MAX = WATCH_SPEEDS[WATCH_SPEED_N - 1];
inline int g_watchIdx = WATCH_SPEED_1X;
inline float g_watchSpeed = 1.f;
// Move one notch (dir = +1 faster, -1 slower). The ends just saturate; no wrap-around
inline void watchSpeedStep(int dir) {
    int n = g_watchIdx + dir;
    if (n < 0) n = 0;
    if (n > WATCH_SPEED_N - 1) n = WATCH_SPEED_N - 1;
    g_watchIdx = n;
    g_watchSpeed = WATCH_SPEEDS[n];
}
inline void watchSpeedSet(int idx) {
    if (idx < 0) idx = 0;
    if (idx > WATCH_SPEED_N - 1) idx = WATCH_SPEED_N - 1;
    g_watchIdx = idx;
    g_watchSpeed = WATCH_SPEEDS[idx];
}
// Slow motion (cfg `slowmo=<N>`, for watching a replay). Only dt becomes 1/N; the substep
// stays 1/240 = physics is unaffected, and an input sequence recorded at low speed replays
// as-is at 1x. There is no key for it: the replay-watching tools pass it in the session cfg
// (py/watch_plan_replay.py, py/start_manual_worker.py), and a mid-play speed change belongs on
// the spectating notch (the arrow keys), which does not touch dt at all.
inline float g_slowmo = 1.f;
// cfg `vtime=<seconds>` (hooks_vtime.cpp): while the mod drives, the scheduler is handed this fixed dt and the
// action manager is advanced by it once per update call, so a cocos action reads the same on any frame shape.
// Not given (-1, the default): the fast loop's own dt (cfg fastdt) while the fast loop is what runs and nowhere else
// (vtimeNow, fxcensus.hpp), so a replay at the screen's rate keeps the screen's dt. 0 = off. A cube's rotation on the
// tick it starts to fall moved with the frame's wall time and with fastloops, and a plan the fast loop cleared died
// at t=924 at one update a frame (a custom level): a solution found without it did not clear when it was replayed.
inline float g_vtime = -1.f;
inline bool g_camRand = true;   // cfg `camrand=0` (off): the camera shake's rand() is seeded from the tick (hooks_vtime.cpp)
// cfg `vtscale=0` (off; on by default): the fast loop's per-call action update (hooks_gamelayer.cpp) is scaled by the scheduler's time scale
// (GD's TimeWarp), as the scheduler's own update of a 1x frame is. Without it a cocos action reads 1/60 per call where 1x reads 1/60 x the warp
// (a custom level, a dash sprite's rotation at the end of a dash: 96 degrees in the fast loop, 136 in a 1x replay).
inline bool g_vtScale = true;
// cfg `stopfollowers=0` (off; on by default): as a PlayLayer leaves its scene the sprites that follow its objects stop their actions (PlayLayer::onExit,
// stopFollowerActions in helpers_game.hpp).
inline bool g_stopFollowers = true;
// cfg `replaystep=<n>` (0 = off): while the bot drives a replay at the screen's speed (not the fast loop, not a solve), the game is advanced in fixed
// steps of 1/n s taken from the time the frames bring (replaystep.hpp), n = 60 for the 1/60 s update the solver's fast loop uses.
inline int g_replayStep = 0;
inline float g_replayStepDiagDt = 0.f;   // cfg `replaystepdt=<seconds>` (diagnostic, 0 = off): the frame time replaystep.hpp is fed in place of the real one
inline int g_replayStepCfg = -1;   // the value cfg `replaystep` gave (-1: not given); the play menu's Options choice is used when it was not
inline long long g_nodeDbg0 = 0, g_nodeDbg1 = -1;   // cfg `nodedbg=t0,t1` (print only, hooks_player.cpp): the node at PlayerObject+0x740 each tick
inline long long g_rotDbg0 = 0, g_rotDbg1 = -1;   // cfg `rotdbg=t0,t1` (print only, hooks_vtime.cpp)
// Force a recompute of the visible sections right after returning from render skip (old method,
// only with visrefresh=1). While skipping, the visible-section bounds are not updated, so
// objects are not drawn after returning
inline bool g_visRefresh = false;
// Whether to collapse the visible sections to 0 when rendering resumes (cfg `visrefresh`).
// Default OFF: collapsing crashes the code that adds all sections up to the current position
// in one go (kept for A/B isolation)
inline bool g_visRefreshOn = false;
// Whether a render resume was requested. resetLevel at a frame boundary and rebuild the render
// state along with it
inline bool g_visResetPending = false;
// Burning through the end-zone visual effects (after lock). Rendering is stopped meanwhile —
// 1800 updates per frame is too heavy with rendering ON and the screen looks frozen
inline bool g_endzoneBurn = false;
inline std::chrono::steady_clock::time_point g_endzoneStart;
// Whether to burn through the end-zone visual effects in one go (cfg `endburn`). Measurement
// switch
inline bool g_endzoneBurnOn = true;
// Wall-clock time at which the end-zone lock began (not locked = default-constructed zero).
inline std::chrono::steady_clock::time_point g_endzoneLockStart;
// Enter the burn only when the lock has lasted this long in real time without completing
// (a safety net; normally never fires)
constexpr double ENDZONE_BURN_AFTER_SEC = 3.0;
// The stall/overlong guards leave a LOCKED player alone for this much real time, measured
// from g_endzoneLockStart. The end sequence (m_isLocked, pull-in, effects) is driven by the
// scheduler, i.e. by REAL frames -- under the fast loop 3,000 ticks of "stillness" pass in
// under two rendered frames, and the tick-based stall guard was killing legitimate clears
// during the end-zone pin (measured 2026-08-25: the fr110 lv11 solution replays to
// x=29,503/29,803, pins at the end wall, gets stall-killed, and the armed completion then
// leaks into the NEXT attempt as an endscreen at t=0/x=349 -- lv8/11/17's whole in-process
// wall). Past this bound the guards fire as before: a HUNG end sequence (the lv22 wrong-mode
// zombie that never completes) must still be put down.
constexpr double ENDZONE_KILL_AFTER_SEC = 10.0;
// Measure the drift between the song and game time (cfg `syncprobe=1`). Emits real time, game
// time (tick/240), GD's own attemptTime and FMOD's song position side by side
inline bool g_syncProbe = false;
// Record switch (touch trigger) contacts (cfg `trigtrace=1`)
inline bool g_trigTrace = false;
// autorun.cfg keys removed in a release clean-up that this session's file still names
// (session.hpp kRemovedCfg). dpsolve::start refuses to solve with any of them.
inline std::vector<std::string> g_cfgRemoved;
// The press latch around a tick window (cfg `presstrace=t0,t1`, `press:` lines; off when
// t1 < t0). +0x985 is "the button is down", +0x986 "a press not yet consumed" -- pushButton
// sets both at once (0x397fbc) and each consumer clears only 0x986. Printed before and after
// PlayerObject::update and after pushButton, so a consumer's tick shows as 1 -> 0 inside one
// update. Added for a UFO press on lv14 that fires two ticks late in the game.
inline long long g_pressT0 = 0, g_pressT1 = -1;
// The stick re-land of PlayerObject::postCollision around a tick window (cfg
// `sticktrace=t0,t1`, `stick:` lines; off when t1 < t0). postCollision re-lands a player
// whose ground object moved away by at most 5*dt (0x38e76f-0x38ea48) through
// collidedWithObject with dt = 0, and when the ground object's stretched rect misses the
// player it falls back to the object checkCollisions pre-registered at +0x600. Printed on
// entry and exit of postCollision (P1 only) with the ground object +0x608, the fallback
// +0x600 and the stick flag +0x658, plus one line per dt = 0 collision inside it. Added to
// test which object a rider on a sinking floor is tied to when it crosses a seam between
// two blocks in a rotated frame (lv22 t=6,122).
inline long long g_stickT0 = 0, g_stickT1 = -1;
inline bool g_inPostCollP1 = false;
// The positions of listed objects at the end-of-tick record point (cfg
// `areatrace=t0,t1,uid[,uid...]`, `area:` lines; off when t1 < t0). Added to find what a
// level solved after another one in the same game carries over: on lv22 the solids an
// Area Move (3006) pushes sit up to 2 px elsewhere at t=14,275-14,370 when lv21 ran first,
// while everything the dump shows is identical.
// cfg `rngfresh=1`: every level of a session starts GD's own fast_rand seeds where the
// first level found them (the values a fresh process has). 2.2081 keeps three process-wide
// LCG seeds (x*0x343fd + 0x269ec3): 0x6c2e90 for triggers (PlayLayer::resetLevel reseeds it
// each attempt), 0x6c2ee0 drawn by every object's GameObject::resetObject on every attempt,
// and 0x6c2ef8 in teleportPlayer. The last two are never reseeded, so a level solved after
// another one in the same game starts from wherever that one left them: on lv22 the solids an
// Area Move pushes at t=14,275-14,370 then sit up to 2 px elsewhere than in a fresh game.
inline bool g_rngFresh = false;
// cfg `rngseed=<ee0>,<ef8>`: the values rngfix (below) sets the two never-reseeded seeds (see
// g_rngFresh) to, instead of its own. For measuring which objects depend on them: the same
// plan replayed under several seeds shows every position the seeds can move.
inline bool g_rngSeedSet = false;
inline long long g_rngSeedEE0 = 0, g_rngSeedEF8 = 0;
// cfg `rngfix` (on; 0 = off): while the mod drives, the two never-reseeded seeds are set before
// the level is built and again before every reset (checkpoint restores included), so the Area
// Move variance table (filled from 0x6c2ef8 when the level is built) and every object's variance
// index (drawn from 0x6c2ee0 by resetObject on each attempt) are the same on every attempt and
// in every game. The values are those a fresh process has when it builds its first level,
// measured on two launches (e90=0, ee0=2531011, ef8=0; e90 is reseeded by resetLevel itself),
// so the fixed level is the one a freshly started game shows on its first attempt. Measured on
// lv22 before this: the same plan flown as attempt 3, 5, 7 and 11 of a game put group 330's
// blocks up to 42 px apart at t=14,300, and as far apart again with the seeds set only when the
// level was built (rngseed). Playing a level yourself, with no session open, is untouched.
// rngseed=, when given, supplies the values instead.
inline bool g_rngFix = true;
constexpr long long kRngFixEE0 = 2531011, kRngFixEF8 = 0;
// The third seed, 0x6c2e90, drives the Random and Advanced Random triggers, spawn-delay
// variance, Advanced Follow and a few more (every reader: GJBaseGameLayer::tryGetObject,
// processAdvancedFollowAction, modifyGroupPhysics, RandTriggerGameObject / SpawnTriggerGameObject
// / EffectGameObject::triggerObject). PlayLayer::resetLevel (0x3b90f4) reseeds it on every
// attempt from the clock -- gettimeofday, seconds times microseconds -- unless GD's own replay
// flag is set (m_useReplay, +0x3190, with m_replayRandSeed at +0x32f0), and keeps the value it
// drew at +0x32e0 (m_randomSeed). So those triggers fire differently on every attempt of every
// game. rngfix puts both back to kRngFixE90 right after the reset returns. Setting GD's replay
// flag instead would switch on its replay system as well. If anything had drawn from the seed
// between the reseed and the end of the reset, the seed would no longer equal +0x32e0; that is
// counted (g_rngDrawnInReset) rather than assumed not to happen.
constexpr long long kRngFixE90 = 0;
constexpr size_t kRandomSeedOff = gdoff::kLayerRandomSeed;   // GJBaseGameLayer::m_randomSeed
inline long long g_rngDrawnInReset = 0;     // resets whose trigger seed was drawn before rngfix
inline void rngFixAfterReset(void* layer) {
    if (!botDriving() || !g_rngFix || !layer) return;
    auto* base = reinterpret_cast<unsigned char*>(geode::base::get());
    auto& seed = *reinterpret_cast<long long*>(base + gdoff::kSeedTriggerRva);
    auto& kept = *reinterpret_cast<long long*>(reinterpret_cast<char*>(layer) + kRandomSeedOff);
    if (seed != kept) ++g_rngDrawnInReset;
    seed = kRngFixE90;
    kept = kRngFixE90;
}
inline void rngFixApply() {
    if (!botDriving() || !(g_rngFix || g_rngSeedSet)) return;
    auto* base = reinterpret_cast<unsigned char*>(geode::base::get());
    *reinterpret_cast<long long*>(base + gdoff::kSeedVarIndexRva) =
        g_rngSeedSet ? g_rngSeedEE0 : kRngFixEE0;
    *reinterpret_cast<long long*>(base + gdoff::kSeedVarTableRva) =
        g_rngSeedSet ? g_rngSeedEF8 : kRngFixEF8;
}
// cfg `attemptone` (off; 1 = on): while the mod drives, every attempt is the level's first.
// A level can read how many attempts have been made -- an Item Compare or Item Edit with an item
// of type 5, which GJBaseGameLayer::getItemValue (0x2341c0) answers from the layer's attempt count
// (+0x3084); its only callers are activateItemEditTrigger (0x234250) and
// activateItemCompareTrigger (0x234630) -- and Item Persistence (3641) keeps chosen items across
// attempts: GJEffectManager::resetEffects (0x25c020) copies the kept values (+0x378) back into
// the live item map (+0x330) through transferPersistentItems (0x262ed0), and a checkpoint restore
// (loadFromState, 0x2644a0) does the same. The loop records the moving geometry on one attempt and
// flies its plans on later ones, and a solution is replayed as a session's first attempt, so such
// a level is a different level on each of them. With this on, getItemValue answers the attempt
// count a first attempt reads (0: the layer counts the attempts before the current one) and the
// kept values are not copied back: every attempt, and every checkpoint
// restore, sees the items a first attempt does (hooks_attempt.cpp, Windows only). A level that
// opens only after dying in the right places cannot be solved this way; the play menu says the
// solver takes the level as its first attempt. Playing a level yourself is untouched.
inline bool g_attemptOne = false;
// cfg `attemptcount=k` (off: -1): while the mod drives, getItemValue answers the attempt count as k on every
// attempt (k counts the attempts before the current one, so k=1 reads as a session's second attempt) and leaves
// Item Persistence alone. A level whose Item Compare reads the attempt count is then one level on every attempt,
// including the first flight on the level itself after a slice swap, where the layer's own count starts over: on a
// custom level the copy's flights read a later attempt and lived, and the level's first flight read 0 and died on
// tick 1 to a spike the compare switches. k=1 is the world a judge with prelude 1 replays in. attemptone wins.
inline long long g_attemptCount = -1;
// cfg `attemptauto` (1 = on, the default): a level the scan finds attempt-dependent (levelwarn::kAttemptDependent:
// an Item Compare / Edit reading the attempt count, or Item Persistence, whose triggers reach play) is driven as if
// `attemptcount=1` were set, with no cfg line for it. Decided at every level entry (enterConfiguredLevel), so one
// level's pin cannot reach the next. An explicit attemptone or attemptcount wins. Off: -1, the game's own count.
inline bool g_attemptAuto = true;
inline long long g_attemptAutoCount = -1;      // the pin the level entry chose (-1: none)
inline long long g_attemptReadsPinned = 0;    // attempt-count reads answered as on attempt 1 (or as k)
inline long long g_attemptReadsSeen = 0;       // attempt-count reads this session (first 20 printed)
inline long long g_persistTransfersSkipped = 0;   // kept-item copies not made
inline bool attemptPinActive() { return botDriving() && g_attemptOne; }
inline long long g_areaT0 = 0, g_areaT1 = -1;
inline std::vector<int> g_areaUids;
inline std::vector<GameObject*> g_areaObjs;
inline cocos2d::CCArray* g_areaArr = nullptr;
// The slope-ride bytes at the end-of-tick record point (cfg `slopetrace=t0,t1`,
// `slprec:` lines; off when t1 < t0). See anchors::slopeTrace in repair.hpp.
inline long long g_slopeT0 = 0, g_slopeT1 = -1;
// Simulated frame drop (cfg `lagms=<milliseconds>`). Sleep this long at the head of each frame
// (`fps=` has no effect on visitDraws, so it cannot substitute)
inline int g_lagMs = 0;
// Single huge stall (cfg `lagat=<tick>`). Sleep `lagms` once at that tick
// (GD catches up when every frame is slow, but a single big stop can be lost to the dt cap)
inline long long g_lagAtTick = -1;
inline bool g_lagFired = false;
// Counters for isolating "it froze" (is rendering being skipped, or is the update not advancing)
inline long long g_visitSkips = 0;   // times the game-layer draw was skipped
inline long long g_visitDraws = 0;   // times it actually drew
inline long long g_pcCalls = 0;      // times processCommands was called (the physics entry point)
// cfg singlereset: the layer and the g_pcCalls value of the last real PlayLayer::resetLevel, and how many
// resets it skipped.
inline const void* g_resetMarkLayer = nullptr;
inline long long g_resetMarkPc = -1;
inline long long g_resetsSkipped = 0;
// Whether to stop all actions when toggling with F8 (cfg `watchpurge`). For A/B
inline bool g_watchPurge = true;
// Whether to remove all actions in endSession (cfg `endpurge`). Default 0.
// Setting 1 also wipes GD's own result visual effects and reproduces "frozen at the goal"
// (for disproof)
inline bool g_endPurge = false;
// Whether to create death visual effects during fast headless runs (cfg `deathfx`). Default
// 0=do not create (the root cause of the CTD). In fast mode the death effects are born in bulk
// while actions advance only once per rendered frame, so debris sprites pile up and become CTD
// material when rendering resumes. The effects are irrelevant to physics and bookkeeping, so
// rather than cleaning up we stop creating them. While spectating (realtime) they are created
inline bool g_deathFx = false;
inline long long g_deathFxSkipped = 0;  // times not created (so that "zero" is visible)
// Sound that got through while the session was SOLVING (not showing a solution). The gate is
// audio::silent(), which deliberately lets the song back in when the operator puts the screen on
// -- so "a solve made a sound" is not one question but two, and the number of calls and what they
// were is the only way to tell a leak from that decision. Reported at session end.
inline long long g_soundWhileSolving = 0;
// ...and what was asked for and refused, which is the other half of the same question: a solve
// that makes a sound with an EMPTY passed-tally is a sound that never went through the hooks at
// all, and only the refused count can tell that apart from "the hooks are not being called".
inline long long g_soundBlockedWhileSolving = 0;
// Song seeks skipped while a section search ran (the FMODAudioEngine hook in hooks_system.cpp).
inline long long g_secSongSeeksSkipped = 0;
// Times the silence had to be imposed again on something already sounding (audio::sync).
inline long long g_silenceReasserts = 0;
// Reproduce retry (cfg `retryafter=<seconds>`). The retry button is really PlayLayer::resetLevel(),
// so firing it after the session ends walks the same path in batch
inline double g_retryAfterSec = 0.0;
inline bool g_retryDone = false;
inline int g_stepTicks = 0;

// Stop every dangling cocos action. During the fast loop, sprites get destroyed but their
// actions stay in CCActionManager (they only advance on real frames), and the moment rendering
// resumes they write into a dead sprite's atlas and crash. So always sweep right before
// rendering resumes. Actions are visual-effects only and play no part in physics or
// determinism. This is inside drawScene, so it cannot be swallowed with SEH (swallowing leaves
// rendering broken from then on) — do not swallow, prevent.
// Defined below (after DATA_DIR is resolved). Declared here because the hotkey handler calls it
inline void writeResult(const std::string& text, bool truncate = false);

// `CCActionManager::update` is protected, so call it through a derived type that exposes the name.
// (No instance is created; used only for casting pointers)
struct ActionManagerTick : cocos2d::CCActionManager {
    using cocos2d::CCActionManager::update;
};

// Defined in notify.hpp: the purge below takes the whole action manager, and a notification's
// hide is an action chain like any other (see the note there). Declared here for the same reason
// writeResult is -- the caller comes first in the include chain.
inline void notifyActionsPurged();

inline void purgeDanglingActions() {
    if (auto* d = cocos2d::CCDirector::sharedDirector())
        if (auto* am = d->getActionManager()) am->removeAllActions();
    // removeAllActions is not selective: it also takes the actions of everything the DIRECTOR
    // draws outside the scene, and Geode's notifications live there (CCDirector's notification
    // node). A notification whose chain is removed mid-life is left on screen for good AND
    // wedges the shared queue behind it, because the pop is the last link of that same chain.
    // This re-arms ours. Measured cost of not doing it: the F8/F5 toggle is a purge, the toggle
    // is how a solve is watched, and "the notification never went away" was the visible half of
    // "no notification has appeared since".
    notifyActionsPurged();
}

}  // namespace p1

#include "mod/phase.hpp"   // the loop's named phases; needs writeResult's declaration above
