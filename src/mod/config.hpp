#pragma once
// Session configuration (autorun.cfg keys), data root, shared session state.
#include "mod/prelude.hpp"

namespace p1 {

// Root for all input/output. If the launch argument --geode:gdsolver.data-root=<absolute path>
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
    // How many repair iterations the loop may spend before it reports the wall (Stage C).
    // A report, not a failure: "stopped at iteration N, deepest t=..." is the diagnostic the
    // level is asking for. 40 undercounted a level the loop can actually clear: with the phantom
    // veto (2026-08-24), lv16 needs 53 rounds and clears cold at the default cap raised here --
    // at 40 it reported "could not solve" one wall short of the real answer. 200 is comfortably
    // past that with margin for a harder level (lv22 stands at 84% after 184 of a 400-round
    // budget), while an actually-unsolvable wall still gives up in well under a minute of extra
    // wall clock rather than running unbounded.
    int dpMaxIters = 200;
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
    // transition is worth a record. Off by default, and the reason is a measurement rather than
    // caution -- see writeFixup's no-op test, which is where it acts.
    //
    // What it finds is real: on lv16 it promotes 27 transitions that were being filed as "already
    // right" on the strength of the first body alone, and 26 of them are p2 errors of half a
    // pixel or more (up to dvy 11.42, a whole cube jump). It also closes the corpus' largest
    // grind -- t=13,017, where the model then dies on GD's own tick instead of twelve later.
    // What it costs is the level: lv16 goes 66 -> 150 iterations and lv20 38 -> 44. A fixup is a
    // patch applied at matched states, so a model made faithful at 27 points and left wrong
    // between them steers the search into routes it then has to abandon; the fix those 27 records
    // describe belongs in the physics, not in the file.
    // So: ON to regenerate the list (GDSOLVER_LAB/oneoff/py/p2_gap_list.py reads it back out of
    // the log), OFF to solve.
    bool dpFixP2 = false;
    // cfg `dpbandtrack`: hand the search the CAMERA's recorded flight band
    // (--bandtrack). On since 81f2a09; off is how that commit's remaining half is
    // A/B'd, since lv22's second cold regression bisects to it.
    bool dpBandTrack = true;
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
    // Record pad firing (cfg `padtrace=1`). Makes activatedByPlayer (pad types only, with
    // used=m_activatedByPlayer1) and propellPlayer (the launch itself) name themselves.
    // Injection breaks the contact state and changes the result even with the same values, so
    // boundaries are pinned down with this + plan changes
    bool padTrace = false;
    // Observe the stair snap (checkSnapJumpToObject). For measuring the phenomenon where x
    // advances extra on the landing tick
    bool snapTrace = false;
    bool hitboxTrace = false;   // record the hitboxes GD actually uses (cfg hitboxtrace=1)
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
    // Feasibility measurement for the section solver (cfg `restoreloop=N`).
    // Restore N times in a row from the checkpoint created by checkpointat and report the cost
    // per restore. This number decides the design: 300 states per layer x 200 layers = 60,000
    // restores, so 1ms each is 60 seconds, 50ms each is 50 minutes and unusable.
    int restoreLoop = 0;
    bool coinMode = false;   // enable our own coin-pickup detection (for coin verification
                             // during replay)
    // Music handling. continuous=keep the song playing / mute=silent / normal=untouched
    std::string music = "continuous";
    std::vector<InputCmd> inputs;
    std::vector<ToggleCmd> toggles; // for verification D: switch mode at the given tick (calls
                                    // the same function a portal does)
};

inline Config g_cfg;
inline bool g_started = false;
inline bool g_uiSession = false; // this session was started from the on-screen panel
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
