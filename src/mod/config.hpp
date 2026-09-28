#pragma once
// Session configuration (autorun.cfg keys), data root, shared session state.
#include "mod/prelude.hpp"

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

struct InputCmd { int step; bool down; };
struct ToggleCmd { int step; std::string mode; };

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
    // cfg `dpcaptiers=0` (1 = on, the default): escalate() skips the capacity tiers (kCapTiers in
    // repair.hpp), so the restart from the start comes next and runs at the base setting instead
    // of inheriting the top tier's 40,000 states for the whole level. Asked because, over the
    // spinoff cold logs (2026-09-26), a restart at the top tier cost 16,263 s in 125 runs for one
    // real step forward, the escalation job itself returned no plan 159 times in 170, and a
    // model-only first plan from t=0 never needed more than 2,000 on official lv1-22 (lv20-22 stop
    // at the same tick at 40,000). The tiers did carry SubZero 4003 and two custom levels to the
    // end in runs without a section solve -- which is what an A/B with this off has to answer.
    int dpCapTiers = 1;
    // cfg `dpsectierfirst=1` (0 = off, the default): when the ladder finds no anchor, the wall goes
    // to a section solve before the capacity is raised (escalate() in repair.hpp) -- one window per
    // escalation, each further back, as autoFire draws them -- and only a wall none of those
    // windows crossed goes on to the next tier. The user's order (2026-09-26): a full frontier is
    // usually a divergence, so the game's own search goes first, and more states only after it.
    int dpSecTierFirst = 0;
    // cfg `dpsecmaxback=N` (0 = no limit, the default): no section-solve window begins more than N
    // ticks before its wall (repair.hpp autoWindow, where the measurement is); a wall whose next
    // window would is done, and with dpsectierfirst the capacity tier comes next.
    int dpSecMaxBack = 0;
    // cfg `dpwalkgates=1`: the single-trajectory walks fire the item gates the search's step does
    // (dp --walkgates) -- the witness resim inside every search, and the fixup recorder's replay,
    // which under coinroute also takes --coins and --items so it has the gate tables and the same
    // item givers the search reads. Off: the replay stays as it was, with no coin arguments.
    // ON BY DEFAULT (`dpwalkgates=0` turns it off). Without coinroute
    // it only adds --walkgates, which dp now defaults to anyway and which is inert without --coins.
    bool dpWalkGates = true;
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
    // cfg `routeprereq` (off; 1 = on; only a coin session builds the census): a coin that needs
    // something entered first (solver/route.hpp: the
    // touch box, key or toggle block whose chain switches the coin, or a platform/orb near it, on)
    // is ranked, in an attempt that has not switched that on, at the attempt's closest approach to
    // the BOX rather than to the coin (coinmisspost's rank), and the search anchored before the box
    // is told to enter it (--needtrig-uid). SubZero 4002's second coin: its platforms come on only
    // from the key 4,314 px before it, and every repair went to the coin. Until a coin is engaged
    // (the fallback below) nothing reads the census, so the loop and the DP argv are the old ones.
    // OFF BY DEFAULT: with the SubZero coin set on, SubZero 4001's coin-on cold took 33 rounds with
    // it on and with it off (four runs, one last [fp]), and over the official 22 it never engaged
    // either way. It stays for what the chain rig route2 shows: on, 9 rounds; off, no anchor past
    // the second key.
    bool routePrereq = false;
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
    // cfg `dpsecchain` (on; 0 = off; needs dpsecauto): a wall right behind the pin -- the splice died
    // within kAutoCreep ticks of its end, which leaves the ladder nothing to plan with -- doubles
    // the next window's margin, and doubles it again for each such wall in a row (up to 8x).
    // Coin-off SubZero lv4001 with dpsecauto: 43 rungs, 30 of them 2-9 ticks past the last pin,
    // each window 254 deep and each advancing ~57 ticks -- 10,922 layers searched for 3,732 ticks.
    bool dpSecChain = true;
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
    // cfg `dplearnresets` (-1 = no limit, the old behaviour): how many times one wall (one value of
    // the best death) may send the ladder back to its shallowest rung because a round recorded
    // fixups. On a custom level, x=29,779: a ship ring GD fires a tick before the model re-fired in the
    // model after every re-anchor, so each round wrote one record one tick later and reset the
    // ladder -- 13 rounds in a row, the backoff never past 96, while the route needed the anchor
    // 105-119 ticks back (repair.hpp, the reset). 1 = the first lesson at a wall reopens it.
    // Off by default (-1): the stall it was written for was not reproduced in a cold run, so
    // there is no consumer yet whose wall is shown to be this reset repeating. On official lv16
    // a cap of 1 took 21 rounds to 19, which alone does not justify changing the default.
    int dpLearnResets = -1;
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
    int dpSecRungPrior = 15;
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
    // cfg `slicecount=1`: print what the cut would remove and end the session without solving
    // (a census of levels against the threshold).
    bool sliceCount = false;
    // cfg `slicenoposition=1`: the cut drops the decorations kept for being read as a position
    // too -- wrong on purpose, to exercise the verification on the level and the add-back.
    bool sliceNoPosition = false;
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
    // What the solve loop gives the search besides the collider table. All on by default; each
    // is separately switchable because they interact, and "which of these three did that" is a
    // question that comes up every time one of them changes.
    //   dpfixups  learn the first model/GD divergence of each replay and apply it (repair.hpp)
    //   dpworld   the trigger map, the group table and the turned hitboxes (buildPois writes
    //             them); implies --needtrig-unseen
    //   dpgroups  where the moving geometry went, recorded from the run's own replays
    bool dpFixups = true;
    bool dpWorld = true;
    bool dpGroups = true;
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
    // On by default (at A), paired with dpRotQToggle below: the queue
    // without the toggle rule fires rotations the game only consumes (lv22 uid5809),
    // so the two go on and off together. On by default (at S) too, with dpTouchSeedNow:
    // on lv22 every anchored call's seed is derivable (122 of 122 against 46 of 157 at
    // A) -- derivable from S's rule, not independently checked in the game.
    // `dprotseed=A dptouchseednow=0` is the previous default, `dprotseed=0
    // dprotqtoggle=0` the one before it.
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
    // cfg `dpinputgrid` (1 = off): pass --inputgrid <n> to the searches, so the button changes only
    // on every n-th tick; repair.hpp baseArgs lifts it again once the loop escalates to a
    // capacity tier or restarts. 2 by default, measured with dpcapladder above.
    int dpInputGrid = 2;
    // cfg `dpphaseprof` (print only): time every call into dp (`dpcall:`) and pass --phaseprof to
    // the searches (`phaseprof: ... prep=`) -- repair.hpp timedSolve.
    bool dpPhaseProf = false;
    // cfg `dpwatchfired=<uid>[,<uid>...]`: the dump's `firedw` column carries each listed
    // object's +0x28e byte (the flag checkSpawnObjects tests before it calls triggerObject)
    // on every tick. Empty by default, and then the column reads "-". Print only.
    std::vector<int> dpWatchFired;
    // cfg `dprotqtoggle=1`: pass --rotqtoggle to the anchored solves and the fixup resims,
    // with --touchseed naming the touch Toggles the attempt had already entered by t0 (a
    // geometric test on its own recorded positions -- GD's touch recorder never sees a
    // touch Toggle). On by default with dpRotSeed; `dprotqtoggle=0`
    // turns it off.
    bool dpRotQToggle = true;
    // cfg `dpspentpad` (on by default since 2026-09-26; `dpspentpad=0` turns it off): pass dp's --spentpad to the anchored
    // solves and the fixup resims -- the pads GD had latched by t0 in this attempt (padseed,
    // recorded where GD sets the latch, activatedByPlayer). GD fires a pad once per attempt;
    // the model's State::usedPad starts empty at an anchor, so a pad fired and stepped off
    // before t0 comes back live and the anchored model fires it again. On an old custom level,
    // t=6,960: pad uid 2158 fired at ~6,922, the loop's anchor at 6,944 re-fired it at 6,960
    // (the kitref replay parts from GD there; with --spentpad 2158 it follows GD to its death).
    // The pads come from the attempt the anchor row belongs to (anchors::seeds, banked with the
    // rows), not from the live recorder the next attempt's reset clears.
    bool dpSpentPad = true;
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
    // OFF for now, and the reason is that no instrument here can see it. dp's
    // State::portalLatch starts empty at an anchor, and empty means "nothing
    // spent" -- which is exactly the behaviour before the latch existed, so an
    // anchored section can only fail to inherit a refusal, never invent one.
    // Measured: lv22's quick_regress sections are byte-identical with the latch
    // on and off. The payload's effect is therefore invisible to the anchored
    // suites and shows up only in a serial cold run, so it is turned on when
    // there is a cold run to judge it rather than on the strength of the
    // argument.
    bool portalPayload = false;
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
    // cfg `coinwatch=1`: repeat the `coinlive:` line every 1,200 ticks. For the
    // question "when does this coin's collision rect move", which the once-per
    // -coin default cannot answer.
    bool coinWatch = false;
    // cfg `areaenv=0` turns off the recording of where GD's own random numbers can put an Area
    // Move's objects (solver/areaenv.hpp); the recording then holds this game's rects, as before.
    // A diagnostic switch: without it the plan depends on the seeds of the game that recorded it.
    bool areaEnv = true;
    // Music handling. continuous=keep the song playing / mute=silent / normal=untouched
    std::string music = "continuous";
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

// Number of blocked achievements / statistics / coins (logged at every session end to make the
// feature visible)
inline long long g_blockedAch = 0, g_blockedStat = 0, g_blockedCoin = 0;
// Number of blocked level-progress records: the percent / attempt / reward writes that GD does
// from destroyPlayer and levelComplete (GJGameLevel::savePercentage and friends). Kept separate
// from the statistics counter because these are the paths that write the LEVEL's own record --
// the progress bar percentage, the attempt count, orbs, diamonds, the completion flag.
inline long long g_blockedProgress = 0;

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
inline size_t g_nextInput = 0;
inline long long g_frame = 0;
inline int g_traceLines = 0;
inline int g_finishedAttempts = 0;
inline bool g_sessionOver = false;
inline size_t g_nextToggle = 0;
inline bool g_injecting = false;
// The jump button as GD was last told, tracked in the handleButton hook. This is
// the RAW press, which is not the same thing as the +0x986 latch: that one means
// "a press that has not been consumed yet" and every consumer clears it (the
// grounded jump, rings, taps -- ten of them), so it reads 0 while the button is
// still down. A gate on "is the player pressing" has to read this one.
inline bool g_btnDown = false;
inline std::chrono::steady_clock::time_point g_attemptStart;

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
// visibly a replay), and the key is ignored while solving.
inline bool g_overlayHidden = false;
// "Solving" as opposed to "replaying": the mod is being driven as a solver worker (serve mode is
// what the DP driver opens), or it is solving the level in-process (cfg `dpsolve` / the panel's
// Solve mode). The overlay toggle is refused here.
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
