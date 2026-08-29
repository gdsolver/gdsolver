#pragma once
// The iteration map: where the repair loop spent its rounds, drawn over the level.
//
// A cold run reports one number -- "cleared in 129 rounds" -- and that number says nothing about
// WHERE the rounds went. In practice they are never spread evenly: a level is solved in one round
// for nine tenths of its length and then the loop grinds for a hundred rounds against two or three
// walls. Which walls those are, and what the loop was doing at each of them, is currently only
// recoverable by reading tens of thousands of log lines.
//
// So the loop writes down what it already knows, per round, and a replay can draw it back onto the
// level it belongs to:
//
//   * every replay death        -- where GD stopped the plan, and how that round was scored
//   * every fixup               -- where the MODEL was wrong, which is the cause the deaths are
//                                  the symptom of
//   * every phantom veto box    -- the stretches the loop decided were not routes at all
//   * every re-anchor           -- how far back the ladder had to reach to get past a wall
//
// Nothing here feeds anything back: recording is append-to-a-vector, drawing is a CCDrawNode.
// The loop's decisions are untouched by whether this is on.
//
// The file (`<data>/itermap_lv<N>.txt`) is written when the solve ends -- on a clear AND on a
// give-up, because a run that did NOT clear is the one whose map is worth reading. It is also
// producible after the fact from a run's log (py/itermap_from_log.py), so the maps of every run
// already on disk exist without re-solving anything.
#include <mutex>

#include "mod/config.hpp"

namespace p1 {
namespace itermap {

// The two sides of this file run on different threads: the fixup recorder is part of the solver
// job (a detached worker), while the drawing is the game's own frame. So every entry point takes
// this, and everything that touches the vectors is `...Locked`. Uncontended once per frame; the
// alternative was a vector reallocating under a loop that is iterating it.
inline std::mutex g_mu;

// ---- what a round leaves behind ----

// How a round was scored by the loop, in the loop's own words. This is the field that turns a
// column of deaths from "it died here a lot" into a story: a stack of `Rewind` is the loop
// failing to get past a wall, while a stack of `Deeper` is it walking through a hard section one
// step at a time.
enum Kind {
    KindDeeper = 0,   // deeper than anything before -- progress, back-off reset
    KindFollow = 1,   // a regression on a tail the model had solved; followed anyway
    KindForced = 2,   // ...on the route a forced mode-portal crossing opened
    KindRewind = 3,   // no improvement: rewound to the deepest plan, backed off further
    KindWedge  = 4,   // wedged / void: credited where it stopped moving, never ranks
    KindCount  = 5,
};

inline const char* kindName(int k) {
    switch (k) {
        case KindDeeper: return "deeper";
        case KindFollow: return "followed";
        case KindForced: return "forced";
        case KindWedge:  return "wedged";
        default:         return "rewound";
    }
}

struct Death {
    int iter = 0;
    long long tick = 0;
    float x = 0.f, y = 0.f;
    int kind = KindRewind;
    long long anchorT = -1;   // the anchor the plan that died was spliced at (-1 = the first plan)
    float anchorX = 0.f;
    int backoff = 0;
    int killerId = -1;        // GD's own verdict (destroyPlayer's argument), not a guess
    int killerUid = -1;
};

struct Fixup {
    int iter = 0;
    long long tick = 0;
    float x = 0.f, y = 0.f;
    int kill = 0;             // a fixup that records "GD ends the run at this state"
};

struct Veto {
    int iter = 0;
    float x0 = 0.f, x1 = 0.f;
};

// The path a round actually flew, from the anchor it was spliced at to where it died.
//
// ONLY THE TAIL, and that is not a saving -- it is the whole content. Every round keeps the
// prefix GD has already verified and re-solves only what comes after, so the rounds are the SAME
// trajectory up to their splice point and differ only past it. Drawing them whole would draw one
// line sixty times over and a fan at the end; drawing the tails draws the fan, which is the part
// that says anything. It is also bounded: the ladder reaches back at most 3,600 ticks.
//
// This replaces the straight line the map used to draw from anchor to death. That line was a
// chord of this curve -- right about the two ends and about nothing in between.
struct Path {
    int iter = 0;
    int kind = KindRewind;
    std::vector<float> xy;    // flat pairs; every kPathStep ticks
};
// Every eighth tick: about 10 px at speed 1 and 40 at speed 4, which is smooth enough for a line
// and keeps the whole fan of a hard level to a few tens of thousands of segments.
constexpr int kPathStep = 8;
constexpr size_t kPathMaxPts = 1200;   // a runaway tail must not eat the file

inline std::vector<Death> g_deaths;
inline std::vector<Fixup> g_fixups;
inline std::vector<Veto>  g_vetoes;
inline std::vector<Path>  g_paths;
inline int g_rounds = 0;        // rounds the run took (the last round recorded, or from the file)
inline bool g_cleared = false;  // whether the run that produced this map went on to clear
inline int g_mapLevel = -1;     // the level the loaded map belongs to (-1 = nothing loaded)
// The level whose file has already been looked for. Without this, cfg `itermap=1` raised the flag
// and nothing ever read the file -- the key was the only loader, so a session started with the
// map switched on drew an empty level.
inline int g_loadTried = -1;

// Bumped whenever the contents change, so the drawing side knows to rebuild rather than
// re-deciding that question from the vector sizes (which repeat across levels).
inline int g_generation = 0;

// TWO THINGS, deliberately separated.
//
// The MAP is the analysis -- the marks over the level, the histogram in the bar, the summary
// text. It draws on top of the level, which is exactly what you do not want while watching a
// solution and exactly what you do want while one is being searched for. So there is no single
// right default, and it does not have one: see mapWanted below.
//
// The BAR is transport: where you are in the level, and a place to click to go somewhere else.
// That is useful in every replay whether or not anyone is analysing anything, so it is always
// there. F1 (hide the overlay) is the only thing that takes it away.
//
// -1 = nobody has said, 0 / 1 = an explicit answer from F10 or from cfg `itermap`. Three states
// rather than a bool because the answer to "is the map on" is different before anyone has
// expressed a preference (it follows the mode) and after (it is whatever they asked for), and a
// bool cannot tell those apart -- a solve would either override a choice the operator had just
// made, or never get its own default.
inline int g_mapPref = -1;

// [2026-08-28, user direction] A SOLVE SHOWS IT BY DEFAULT. That is the session where the map is
// the thing worth watching: the marks appear as the loop produces them, and a run's walls are
// visible while it grinds at them. A replay is the opposite -- you are there to watch the level,
// and analysis drawn over it is in the way -- so there it stays off until F10 asks.
//
// solveSession(), not showingSolve(): the showing of the solution at the end is part of the same
// session, and a map that switched itself off at that moment would vanish exactly when the run it
// describes has finished. It ends with the session, so the next plain replay is off again.
inline bool mapWanted() { return g_mapPref >= 0 ? g_mapPref != 0 : solveSession(); }

// F1 and cfg `hud=0` reach both, like every other drawn thing except the bot badge (spec §9).
//
// THE BAR IS NOT DRAWN WHILE SOLVING. It is transport for a replay, and the loop owns the level
// while it searches: there is nowhere to seek to, and a press on it is refused (barLive).
//
// THE MAP IS. [2026-08-28, user direction] It was off there too, on the grounds that it is being
// WRITTEN by the run underneath it -- a picture of the rounds so far drawn over the round
// currently being flown. That is the argument for it, not against it: with the screen on (F5) the
// marks appear as the loop produces them, so the wall a run is grinding at is visible while it
// grinds instead of only in the file afterwards. What must not happen while solving is the map
// READING a file over a run that is writing one, and that is ensureLoaded's job, not this one.
inline bool barVisible() {
    return g_hudOn && !g_overlayHidden && botDriving() && !showingSolve();
}
inline bool mapVisible() {
    return mapWanted() && g_hudOn && !g_overlayHidden && botDriving();
}
// Either of them is enough to draw the strip: it carries the map's histogram as well as the
// playhead, so a solve gets a bar with no transport in it.
inline bool stripVisible() { return barVisible() || mapVisible(); }

// ---- seeking (click the strip) ----
//
// Where the click asked to be taken to, in level x. 0 = nothing pending.
//
// HOW IT GETS THERE, and why it is not the obvious way. The obvious way is the one cfg `watchat`
// already implements: run blind at full speed and switch the screen back on at the target. That
// cannot be used here. Resuming rendering after a blind stretch walks through renderTogglePress,
// which raises g_visResetPending, and the update hook consumes that by calling resetLevel()
// BEFORE it resumes -- the order is deliberate and load-bearing (resuming first crashes on the
// visibility state that piled up while stopped). So the screen comes back at the top of the
// level, which is the opposite of a seek.
//
// So this fast-forwards with the screen ON, driving the same "several updates per frame" loop the
// spectating notch drives -- but not through the notch, which stops at 16x. It sets its own loop
// count instead (seekLoops below), leaving whatever speed the arrow keys were on untouched. The
// physics is identical either way: the substep stays 1/240 and only the number of substeps per
// frame changes, so the run that arrives is the same run that would have arrived at 1x. It is
// also honest to watch -- you see it scrub rather than teleport.
// EVERYTHING IS A TICK. The bar used to be a picture of the level's X and clicking it asked for
// an x, which is wrong wherever world x is not monotonic -- lv22's rotated and reversed sections,
// where x stands still or runs backwards while time does not. The playhead wandered there, and an
// arrival test of "has px passed the target" can fire early, late or never. Time has none of
// those properties, the keys already asked in it, and one unit for both is one thing to reason
// about. -1 = nothing pending.
inline long long g_seekTick = -1;
// A seek to a point the run has already passed has to start the level again. Raised here and
// consumed at a frame boundary in the update hook -- resetLevel() from inside touch handling is
// the same mistake the stall guard's deferred reset exists to avoid.
inline bool g_seekRestart = false;
// The game was STOPPED (F2) when this seek was asked for, and has to be stopped again when it
// lands. A backward frame-step while paused is the case: physics cannot run in reverse, so the
// only way to step back ten ticks is to run the level again up to tick-10 -- which needs time to
// flow, and then needs the stop back. Raised by whoever asks; consumed by the arrival.
inline bool g_seekRepause = false;
// A backward frame-step wants an EXACT tick, and a seek cannot give one: a batch is indivisible,
// so it lands within a batch of wherever it aimed (measured: asked for -10, got -6). So the seek
// deliberately aims SHORT of the real target and this holds the real one; the arrival finishes
// the last few ticks with probe::g_step, which is exact by construction. -1 = not a step.
inline long long g_seekStepTo = -1;
constexpr long long kStepUndershoot = 16;   // comfortably more than one batch
// Stop this far short, so the approach to the wall is watchable rather than arriving on top of
// it. Two seconds of level at 1x.
constexpr float kSeekLead = 240.f;
inline bool seeking() { return g_seekTick >= 0 || g_seekRestart; }

// ---- where the bar's playhead belongs ----
//
// While a seek runs the level is being flown through at hundreds of batches a frame, and a
// playhead that follows that sweeps the whole bar in a second, which reads as the level playing
// rather than as a seek. So during a seek the playhead sits at the DESTINATION, the way a video
// scrubber does: it is already showing where playback will resume.
inline long long g_seekBarTick = 0;
// Which way this seek is going, decided ONCE when it starts.
//
// Deriving it per frame from "is the destination ahead of the player" gets both cases wrong. A
// backward seek restarts the level, and from the top of the level the destination is ahead again
// -- so the indicator flipped from back to forward mid-seek. And a forward seek to a tick the run
// has never reached has no known x for the destination, so the comparison ran against a stale
// value and pointed backwards. The answer is known at the moment the seek is asked for, and
// nothing after that changes it.
inline bool g_seekForward = true;

// One x per physics tick of the current attempt, recorded by the game layer. It is what makes a
// tick-based seek ("five seconds earlier") land somewhere the bar can point at, and what makes a
// backward step exact rather than an estimate from the current speed. A full level is ~30,000
// ticks = 120 KB, and it is thrown away at the head of every attempt.
inline std::vector<float> g_xHist;

// The physics tick, mirrored. g_tick itself lives in trace_io.hpp, which is a long way further
// down the include chain than this file -- and this file has to stay near the top of it, because
// the repair loop records into it. trackX is called from the end of every physics tick, so the
// mirror is exact wherever it matters and simply stops moving when nothing is running.
inline long long g_nowTick = 0;
// The deepest tick this session has reached. Half of the bar's axis length: the other half is the
// plan's last input, which for a solution lands within a few seconds of the end, so the scale is
// right from the first frame instead of growing under the reader as the run goes on.
inline long long g_barTicksSeen = 0;

inline void trackX(long long tick, float x) {
    g_nowTick = tick;
    if (tick > g_barTicksSeen) g_barTicksSeen = tick;
    if (tick < 0 || tick > 400000) return;    // the same runaway cap the anchors use
    if ((size_t)tick + 1 > g_xHist.size()) g_xHist.resize((size_t)tick + 1, -1.f);
    g_xHist[(size_t)tick] = x;
}

inline void onAttemptStart() { g_xHist.clear(); g_nowTick = 0; }

// How long the bar's axis is, in ticks. The plan's last input is the anchor -- a solution's
// inputs run to within a few seconds of the end -- so the scale does not move under the reader.
// The deepest tick seen and a fixed two seconds of headroom keep the playhead off the right edge.
inline long long barTicks() {
    long long t = g_barTicksSeen;
    if (!g_cfg.inputs.empty() && (long long)g_cfg.inputs.back().step > t)
        t = (long long)g_cfg.inputs.back().step;
    if (t < 240) t = 240;
    return t + 480;
}

// The x the run was at on that tick, or -1 if it has not been there.
inline float xAtTick(long long tick) {
    if (tick < 0 || (size_t)tick >= g_xHist.size()) return -1.f;
    return g_xHist[(size_t)tick];
}

// ---- dragging the bar ----
// While a finger (or the mouse) is down on the bar, the level is NOT scrubbed: dragging moves a
// ghost marker and the seek happens once, on release. Scrubbing live would mean restarting and
// re-running the level for every pixel of the drag.
inline bool g_dragging = false;
inline float g_dragFrac = 0.f;      // 0..1 along the bar
// The most physics batches one frame may run for a seek. The end-zone burn in the same branch of
// the update hook already uses 1800 -- with the screen OFF, because at that rate a drawn frame
// takes long enough to read as a freeze. A seek keeps the screen on, so this is the value that
// still redraws often enough to look like fast-forward rather than a hang. It is a ceiling, not a
// rate: seekLoops below asks for far less than this as it closes in.
constexpr int kSeekLoopsMax = 400;
// Conservative pixels-per-batch. One batch is update(1/60) = 4 physics ticks, and the player
// covers roughly 5.2 px/tick at the fastest speed portal, so ~10 px per batch at the very most.
// Dividing the remaining distance by a number ABOVE that is what makes the approach geometric:
// each frame closes at most two thirds of what is left, so the seek decelerates into the target
// and can never overshoot it, whatever speed portals the section holds.
constexpr float kSeekPxPerLoop = 16.f;

// Where the strip was last drawn, in the overlay's coordinates. Recorded by the draw so the
// click can be turned back into a level x; a click is only inside the strip if the strip is
// where it was drawn last frame.
inline float g_stripX = 0.f, g_stripY = 0.f, g_stripW = 0.f, g_stripH = 0.f;
inline long long g_barTicks = 0;   // the tick span the bar was drawn against
// The summary label's measured box, so the strip's draw node can lay a backing panel under it.
// White text over a level is unreadable wherever the level happens to be bright, and this text is
// numbers -- the one kind of text you cannot guess from context.
inline float g_labelW = 0.f, g_labelH = 0.f;

inline void clearLocked() {
    g_deaths.clear();
    g_fixups.clear();
    g_vetoes.clear();
    g_paths.clear();
    g_rounds = 0;
    g_cleared = false;
    g_mapLevel = -1;
    g_loadTried = -1;
    g_seekTick = -1;
    g_seekRestart = false;
    g_seekRepause = false;
    g_seekStepTo = -1;
    g_dragging = false;
    g_barTicksSeen = 0;
    ++g_generation;
}

inline void clear() {
    std::lock_guard<std::mutex> lk(g_mu);
    clearLocked();
}

// ---- the loop's side ----
//
// The killer is latched at destroyPlayer (hooks_playlayer) rather than looked up here: GD holds
// it in an argument at that instant and nowhere afterwards, and inferring it from nearby rects is
// the mistake the `killer:` line exists to avoid.
inline long long g_killTick = -1;
inline int g_killId = -1, g_killUid = -1;
inline float g_killY = 0.f;

inline void latchKiller(long long tick, float py, int objId, int objUid) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_killTick = tick;
    g_killY = py;
    g_killId = objId;
    g_killUid = objUid;
}

// One round ended. `y` is the player's y at the death as the recorder saw it; the caller passes it
// because the anchor buffers live further down the include chain than this file.
inline void addDeath(int iter, long long tick, float x, float y, int kind,
                     long long anchorT, float anchorX, int backoff) {
    std::lock_guard<std::mutex> lk(g_mu);
    Death d;
    d.iter = iter;
    d.tick = tick;
    d.x = x;
    // Prefer GD's own y at the kill. The recorder's last row is one tick short of the death (it
    // runs at the END of a tick and the destroy happens inside one), which on a fast fall is tens
    // of pixels away from where the marker belongs.
    d.y = (g_killTick == tick && g_killY != 0.f) ? g_killY : y;
    d.kind = kind;
    d.anchorT = anchorT;
    d.anchorX = anchorX;
    d.backoff = backoff;
    if (g_killTick == tick) { d.killerId = g_killId; d.killerUid = g_killUid; }
    g_deaths.push_back(d);
    if (iter > g_rounds) g_rounds = iter;
    ++g_generation;
}

// Called from the SOLVER THREAD (the fixup pass runs inside the ladder job), which is the whole
// reason for the mutex above.
inline void addFixup(int iter, long long tick, float x, float y, bool kill) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_fixups.push_back(Fixup{iter, tick, x, y, kill ? 1 : 0});
    ++g_generation;
}

inline void addVeto(int iter, float x0, float x1) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_vetoes.push_back(Veto{iter, x0, x1});
    ++g_generation;
}

// The round's tail, handed over a point at a time. The caller walks the recorder, which is where
// the trajectory lives; this only keeps it.
inline void beginPath(int iter, int kind) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_paths.push_back(Path{iter, kind, {}});
}
inline void addPathPoint(float x, float y) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_paths.empty()) return;
    std::vector<float>& v = g_paths.back().xy;
    if (v.size() >= kPathMaxPts * 2) return;
    v.push_back(x);
    v.push_back(y);
}
inline void endPath() {
    std::lock_guard<std::mutex> lk(g_mu);
    // A tail of one point is a dot the death mark already draws.
    if (!g_paths.empty() && g_paths.back().xy.size() < 4) g_paths.pop_back();
    ++g_generation;
}

// ---- the file ----

inline std::string pathFor(int levelId) {
    char name[64];
    snprintf(name, sizeof(name), "/itermap_lv%d.txt", levelId);
    return std::string(DATA_DIR) + name;
}

// Written at the end of a solve, whichever way it ended. `key=value` lines in the same style as
// the plan files, so it stays greppable and a converter can produce one from a log.
inline bool save(int levelId, bool cleared) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_deaths.empty() && g_fixups.empty()) return false;
    g_cleared = cleared;
    const std::string path = pathFor(levelId);
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << "# gdsolver iteration map\n";
    f << "level=" << levelId << "\n";
    f << "rounds=" << g_rounds << "\n";
    f << "cleared=" << (cleared ? 1 : 0) << "\n";
    for (const Death& d : g_deaths)
        f << "death=" << d.iter << ',' << d.tick << ',' << d.x << ',' << d.y << ','
          << d.kind << ',' << d.anchorT << ',' << d.anchorX << ',' << d.backoff << ','
          << d.killerId << ',' << d.killerUid << "\n";
    for (const Fixup& x : g_fixups)
        f << "fixup=" << x.iter << ',' << x.tick << ',' << x.x << ',' << x.y << ','
          << x.kill << "\n";
    for (const Veto& v : g_vetoes)
        f << "veto=" << v.iter << ',' << v.x0 << ',' << v.x1 << "\n";
    // round, kind, then x,y pairs every kPathStep ticks. One line per round; long, but a path is
    // one object and splitting it would need a way to say which pieces belong together.
    for (const Path& p : g_paths) {
        if (p.xy.size() < 4) continue;
        f << "path=" << p.iter << ',' << p.kind;
        for (float v : p.xy) f << ',' << v;
        f << "\n";
    }
    g_mapLevel = levelId;
    return true;
}

// Read a map back. Returns false when there is none, which is the ordinary case for a level that
// was solved before this existed -- the overlay then says so rather than drawing an empty level.
inline bool load(int levelId) {
    std::ifstream f(pathFor(levelId));
    if (!f) return false;
    std::lock_guard<std::mutex> lk(g_mu);
    clearLocked();
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = line.substr(0, eq);
        const char* v = line.c_str() + eq + 1;
        if (key == "rounds") { g_rounds = std::atoi(v); }
        else if (key == "cleared") { g_cleared = (*v == '1'); }
        else if (key == "death") {
            Death d;
            double x = 0, y = 0, ax = 0;
            long long tick = 0, at = -1;
            if (sscanf(v, "%d,%lld,%lf,%lf,%d,%lld,%lf,%d,%d,%d", &d.iter, &tick, &x, &y,
                       &d.kind, &at, &ax, &d.backoff, &d.killerId, &d.killerUid) >= 5) {
                d.tick = tick; d.x = (float)x; d.y = (float)y;
                d.anchorT = at; d.anchorX = (float)ax;
                g_deaths.push_back(d);
                if (d.iter > g_rounds) g_rounds = d.iter;
            }
        } else if (key == "fixup") {
            Fixup x;
            double fx = 0, fy = 0;
            long long tick = 0;
            if (sscanf(v, "%d,%lld,%lf,%lf,%d", &x.iter, &tick, &fx, &fy, &x.kill) >= 4) {
                x.tick = tick; x.x = (float)fx; x.y = (float)fy;
                g_fixups.push_back(x);
            }
        } else if (key == "path") {
            Path p;
            const char* q = v;
            char* end = nullptr;
            p.iter = (int)std::strtol(q, &end, 10);
            if (end == q || *end != ',') continue;
            q = end + 1;
            p.kind = (int)std::strtol(q, &end, 10);
            if (end == q) continue;
            q = end;
            while (*q == ',') {
                const double d = std::strtod(q + 1, &end);
                if (end == q + 1) break;
                p.xy.push_back((float)d);
                q = end;
            }
            if (p.xy.size() >= 4) {
                p.xy.resize(p.xy.size() & ~size_t(1));   // pairs only
                g_paths.push_back(std::move(p));
            }
        } else if (key == "veto") {
            Veto vt;
            double a = 0, b = 0;
            if (sscanf(v, "%d,%lf,%lf", &vt.iter, &a, &b) == 3) {
                vt.x0 = (float)a; vt.x1 = (float)b;
                g_vetoes.push_back(vt);
            }
        }
    }
    g_mapLevel = levelId;
    ++g_generation;
    return !g_deaths.empty() || !g_fixups.empty();
}

// Called from the overlay pass (main thread) and from the key. Reads the file at most once per
// level, and never over a run that is recording its own map.
inline void ensureLoaded(int levelId) {
    if (levelId <= 0 || g_loadTried == levelId) return;
    // A SOLVE IS THE AUTHOR, never a reader. The "this run owns the map" test below only works
    // once the run has recorded something, and at the head of a solve it has not -- so the map
    // read the PREVIOUS run's file and the solve then appended its own rounds to it. Measured
    // 2026-08-28 on lv21: 78 rounds reported against 156 deaths, exactly double.
    if (g_cfg.dpSolve && !g_dpShowSolution) {
        g_loadTried = levelId;
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_deaths.empty() || !g_fixups.empty()) {         // this run owns the map
            g_loadTried = levelId;
            return;
        }
    }
    load(levelId);
    // AFTER the load, not before it: load() clears first, and clearLocked() puts g_loadTried
    // back to -1 -- so stamping it up front means the file is opened and parsed again on the
    // very next frame, for as long as the overlay is up.
    g_loadTried = levelId;
}

// ---- buckets ----
//
// One GD block wide. Finer than that and a wall that the loop attacked from three entry heights
// splits into three thin columns that read as three separate walls; coarser and two genuinely
// different walls a block apart merge into one.
constexpr float kBucket = 30.f;

struct Hot {
    float x0 = 0.f;      // the bucket's left edge in level coordinates
    int deaths = 0;
    int fixups = 0;
    int firstIter = 0, lastIter = 0;
    float yLo = 0.f, yHi = 0.f;
    int dominant = KindRewind;   // the kind most of this bucket's deaths were scored as
    int byKind[KindCount] = {0, 0, 0, 0, 0};
};

// Deaths and fixups gathered per bucket, in x order. Rebuilt only when the map changes: a level's
// worth of rounds is a few hundred entries and this runs behind the draw, not per tick.
inline std::vector<Hot> g_hot;
inline int g_hotGen = -1;
inline int g_hotMax = 0;         // the busiest bucket's death count (the scale for every bar)

// The caller holds g_mu.
inline void rebuildHotLocked() {
    if (g_hotGen == g_generation) return;
    g_hotGen = g_generation;
    g_hot.clear();
    g_hotMax = 0;
    std::map<int, Hot> by;
    for (const Death& d : g_deaths) {
        const int b = (int)std::floor(d.x / kBucket);
        auto it = by.find(b);
        if (it == by.end()) {
            Hot h;
            h.x0 = b * kBucket;
            h.firstIter = h.lastIter = d.iter;
            h.yLo = h.yHi = d.y;
            it = by.emplace(b, h).first;
        }
        Hot& h = it->second;
        ++h.deaths;
        if (d.kind >= 0 && d.kind < KindCount) ++h.byKind[d.kind];
        if (d.iter < h.firstIter) h.firstIter = d.iter;
        if (d.iter > h.lastIter) h.lastIter = d.iter;
        if (d.y < h.yLo) h.yLo = d.y;
        if (d.y > h.yHi) h.yHi = d.y;
    }
    for (const Fixup& x : g_fixups) {
        const int b = (int)std::floor(x.x / kBucket);
        auto it = by.find(b);
        if (it == by.end()) {
            Hot h;
            h.x0 = b * kBucket;
            h.firstIter = h.lastIter = x.iter;
            h.yLo = h.yHi = x.y;
            it = by.emplace(b, h).first;
        }
        ++it->second.fixups;
    }
    g_hot.reserve(by.size());
    for (auto& kv : by) {
        Hot& h = kv.second;
        for (int k = 0; k < KindCount; ++k)
            if (h.byKind[k] > h.byKind[h.dominant]) h.dominant = k;
        g_hot.push_back(h);
        if (h.deaths > g_hotMax) g_hotMax = h.deaths;
    }
}

// The busiest bucket, for the one-line summary. Returns nullptr on an empty map.
// The caller holds g_mu and must not keep the pointer past the lock.
inline const Hot* hottestLocked() {
    rebuildHotLocked();
    const Hot* best = nullptr;
    for (const Hot& h : g_hot)
        if (!best || h.deaths > best->deaths) best = &h;
    return best;
}

// The bucket the player is standing in right now (or nullptr). Same contract.
inline const Hot* atLocked(float px) {
    rebuildHotLocked();
    for (const Hot& h : g_hot)
        if (px >= h.x0 && px < h.x0 + kBucket) return &h;
    return nullptr;
}

// ---- drawing ----

constexpr int WORLD_TAG = 0x51D60;   // the CCDrawNode parented to the object layer
constexpr int STRIP_TAG = 0x51D61;   // ...and the one in the bottom-right corner
constexpr int LABEL_TAG = 0x51D62;   // the summary text that goes with it
constexpr int TOUCH_TAG = 0x51D63;   // the layer that turns a click on the strip into a seek
constexpr int COUNT_TAG = 0x51D64;   // holds the per-column death counts, in level coordinates
constexpr int COVER_TAG = 0x51D65;   // the blackout drawn over the level while seeking

// Colours, in one place so the legend and the marks cannot drift apart.
inline cocos2d::ccColor4F kindColor(int k, float a) {
    switch (k) {
        case KindDeeper: return {0.30f, 0.95f, 0.45f, a};   // progress
        case KindFollow: return {0.35f, 0.80f, 1.00f, a};   // followed a solved branch
        case KindForced: return {0.70f, 0.55f, 1.00f, a};   // the forced portal route
        case KindWedge:  return {1.00f, 0.45f, 0.95f, a};   // wedged, never ranks
        default:         return {1.00f, 0.30f, 0.25f, a};   // rewound: the loop is stuck here
    }
}

// Fetch or create one of our draw nodes. Looked up by tag every time and never cached in a raw
// pointer -- the same rule the HUD labels follow, and for the same reason: a scene rebuild frees
// the node while a cached pointer keeps pointing at it.
// `id` is the Geode node ID. The lookup stays by tag; the name is there because these hang off
// layers the game owns, where another mod has no other way to refer to them.
inline cocos2d::CCDrawNode* drawNode(cocos2d::CCNode* parent, int tag, const std::string& id,
                                     int z) {
    using namespace cocos2d;
    if (!parent) return nullptr;
    auto* n = static_cast<CCDrawNode*>(parent->getChildByTag(tag));
    if (!n) {
        n = CCDrawNode::create();
        if (!n) return nullptr;
        n->setTag(tag);
        n->setID(id);
        n->setZOrder(z);
        parent->addChild(n);
    }
    return n;
}

inline void fillRect(cocos2d::CCDrawNode* n, float x0, float y0, float x1, float y1,
                     const cocos2d::ccColor4F& c) {
    using namespace cocos2d;
    CCPoint v[4] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    n->drawPolygon(v, 4, c, 0.f, ccColor4F{0.f, 0.f, 0.f, 0.f});
}

// The marks, in the level's own coordinates. Parented to the object layer, so they pan, zoom and
// rotate with the world exactly as the level does -- which is the only way a mark at x=3,497 is
// still at x=3,497 inside lv22's rotated shaft.
//
// Rebuilt only when the map or the parent changes. A few hundred primitives in one draw node cost
// nothing to draw and are not worth re-emitting every frame.
inline void drawWorld(cocos2d::CCNode* objectLayer) {
    using namespace cocos2d;
    if (!objectLayer) return;
    auto* n = drawNode(objectLayer, WORLD_TAG, "itermap-world"_spr, 1 << 20);
    if (!n) return;
    // The counts hang off a plain node beside the draw node: a CCDrawNode cannot hold text, and
    // the two have to appear and disappear together.
    auto* counts = objectLayer->getChildByTag(COUNT_TAG);
    if (!counts) {
        counts = CCNode::create();
        if (!counts) return;
        counts->setTag(COUNT_TAG);
        counts->setID("itermap-counts"_spr);
        counts->setZOrder((1 << 20) + 1);
        objectLayer->addChild(counts);
    }
    n->setVisible(mapVisible());
    counts->setVisible(mapVisible());
    if (!mapVisible()) return;
    // The generation covers content; the parent is compared separately so that re-entering a
    // level (a fresh object layer, a fresh node) redraws even though the map is unchanged.
    std::lock_guard<std::mutex> lk(g_mu);
    static int s_gen = -1;
    static CCNode* s_parent = nullptr;
    if (s_gen == g_generation && s_parent == objectLayer) return;
    s_gen = g_generation;
    s_parent = objectLayer;
    rebuildHotLocked();
    n->clear();
    counts->removeAllChildrenWithCleanup(true);
    // 1. the veto boxes, underneath everything: stretches the loop ruled out as routes
    for (const Veto& v : g_vetoes)
        fillRect(n, v.x0, -300.f, v.x1, 3200.f, ccColor4F{0.55f, 0.15f, 0.65f, 0.10f});
    // 2. the columns. Alpha is the share of the run's worst bucket, so the picture answers "where
    //    did the rounds go" at a glance instead of "was anything ever wrong here"
    for (const Hot& h : g_hot) {
        if (h.deaths <= 0) continue;
        const float f = g_hotMax > 0 ? (float)h.deaths / (float)g_hotMax : 0.f;
        const float lo = h.yLo - 200.f, hi = h.yHi + 200.f;
        // Kept under a third opaque even at the worst wall: the column has to be findable at a
        // glance without hiding the geometry the run died on, which is the thing you came to look
        // at once the column has told you where to look.
        fillRect(n, h.x0, lo, h.x0 + kBucket, hi,
                 ccColor4F{1.00f, 0.25f, 0.20f, 0.05f + 0.25f * f});
    }
    // 3. THE TAILS -- what each round actually flew, from the anchor it was spliced at to where
    //    it died. Faint on purpose: they overlap, and where several rounds took the same line the
    //    overlap is meant to build up into a brighter one on its own. Coloured by the round, so a
    //    fan of red tails is the loop trying the same wall a dozen ways.
    //    A FIXED INK BUDGET, not a fixed alpha per tail. Rounds share the verified prefix by
    //    construction, so their tails lie on top of each other for most of their length, and at a
    //    flat 20% a fan of sixty is opaque long before the sixtieth. Worse than opaque: the kinds
    //    are different colours, so what they mix to is not "a brighter red" but grey, and the
    //    colour key -- the whole reason the tails are coloured -- stops meaning anything. Dividing
    //    by the count keeps the total roughly constant, so a fan stays a fan and a lone tail is as
    //    visible as it ever was. The floor keeps a hundred-round map from vanishing.
    const float ta = std::max(0.05f, std::min(0.20f, 2.0f / std::max(1.f, (float)g_paths.size())));
    for (const Path& p : g_paths) {
        const ccColor4F c = kindColor(p.kind, ta);
        for (size_t i = 2; i + 1 < p.xy.size(); i += 2)
            n->drawSegment({p.xy[i - 2], p.xy[i - 1]}, {p.xy[i], p.xy[i + 1]}, 0.8f, c);
    }
    //    ...and a tick at each anchor, which is where its tail begins.
    for (const Death& d : g_deaths) {
        if (d.anchorT < 0 || d.anchorX <= 0.f) continue;
        n->drawSegment({d.anchorX, d.y - 16.f}, {d.anchorX, d.y + 16.f}, 1.2f,
                       ccColor4F{0.35f, 0.60f, 1.00f, 0.45f});
    }
    // 4. the fixups: where the MODEL was wrong. These are the cause; the deaths are the symptom,
    //    and on a level whose wall is really a fidelity hole they sit hundreds of pixels apart.
    for (const Fixup& x : g_fixups) {
        const ccColor4F c = x.kill ? ccColor4F{1.00f, 0.35f, 0.85f, 0.85f}
                                   : ccColor4F{1.00f, 0.80f, 0.20f, 0.75f};
        n->drawDot({x.x, x.y}, 3.5f, c);
    }
    // 5. the deaths themselves, on top, coloured by how the round was scored
    for (const Death& d : g_deaths) {
        const ccColor4F c = kindColor(d.kind, 0.90f);
        n->drawSegment({d.x, d.y - 26.f}, {d.x, d.y + 26.f}, 1.4f, kindColor(d.kind, 0.45f));
        n->drawDot({d.x, d.y}, 5.f, c);
    }
    // 6. how many rounds died in each column, in figures.
    //
    //    Opacity alone answers "is this one of the bad ones" and nothing else -- two columns that
    //    are both near the maximum look identical whether they cost 11 rounds or 3. The figure is
    //    the thing you actually compare, so it is printed.
    //
    //    EVERY COLUMN, including the ones that cost a single round. This skipped 1 at first, on
    //    the grounds that a "1" over every isolated death is noise and that "no figure" is a
    //    convention costing nothing to learn. Both were wrong, and the data says so: ten of lv20's
    //    sixteen columns are singletons, so the unlabelled case is the COMMON one -- and a reader
    //    who meets it first reads a column with no number as a number that failed to draw, which
    //    is exactly how it was reported (2026-08-28). A count you have to know a rule to read is
    //    not a count.
    //
    //    Placed just above the topmost death in the column rather than at the column's own top
    //    edge -- the box runs 200px past the marks into empty sky, which on a tall section is off
    //    the screen you are reading the number from.
    for (const Hot& h : g_hot) {
        if (h.deaths <= 0) continue;
        char cb[16];
        snprintf(cb, sizeof(cb), "%d", h.deaths);
        auto* lbl = CCLabelBMFont::create(cb, "bigFont.fnt");
        if (!lbl) continue;
        lbl->setAnchorPoint({0.5f, 0.f});
        lbl->setScale(0.45f);
        lbl->setPosition({h.x0 + kBucket * 0.5f, h.yHi + 44.f});
        const ccColor4F c = kindColor(h.dominant, 1.f);
        lbl->setColor({(GLubyte)(c.r * 255.f), (GLubyte)(c.g * 255.f), (GLubyte)(c.b * 255.f)});
        lbl->setOpacity(235);
        counts->addChild(lbl);
    }
}

// The strip: the whole level as one bar, so the shape of the run is visible without flying to
// each wall. Laid out in the top-left overlay column (the one region of the window measured to be
// reliably on screen -- see the note in hud.hpp), and given the level length and the player's x by
// the caller so this file needs nothing from the game layer.
//
// PLACED AGAINST THE VISIBLE RECTANGLE, in the bottom-right corner.
//
// Not getWinSize(). The note in hud.hpp records that the right and bottom of getWinSize() are not
// reliably on screen, and the first version of this strip measured the same thing from the other
// side: 0.52 of getWinSize().width drew across 78% of the visible width. getVisibleOrigin() /
// getVisibleSize() are the pair that answers this question properly -- they are the rectangle the
// resolution policy actually shows -- so corners are addressable and none of it has to be guessed.
//
// The bottom-right, because the top-left is the overlay column and everything between them is
// where the level is. A timeline over the playfield is a timeline you have to look past.
// One line naming every candidate for "how big is the screen, in the coordinates this node draws
// in". The first attempt at a bottom-right corner put the strip entirely off screen because
// getVisibleSize() turned out to report exactly getWinSize() here, which is not the rectangle the
// window shows -- and the note in hud.hpp records the same surprise from the other end. Rule 2 of
// this project: ask the game.
inline void logScreenMetrics(cocos2d::CCNode* parent) {
    using namespace cocos2d;
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    auto* d = CCDirector::sharedDirector();
    const CCSize win = d->getWinSize();
    const CCSize vis = d->getVisibleSize();
    const CCPoint vo = d->getVisibleOrigin();
    CCSize frame{0.f, 0.f};
    float sx = 0.f, sy = 0.f;
    if (auto* v = CCEGLView::sharedOpenGLView()) {
        frame = v->getFrameSize();
        sx = v->getScaleX();
        sy = v->getScaleY();
    }
    // ...and what the node we draw into is actually doing, which is the part that turns design
    // units into pixels for US rather than for the game.
    CCPoint bl{0.f, 0.f}, tr{0.f, 0.f};
    float pscale = 0.f;
    if (parent) {
        bl = parent->convertToNodeSpace({0.f, 0.f});
        tr = parent->convertToNodeSpace({win.width, win.height});
        pscale = parent->getScale();
    }
    // ...and the window itself, in real pixels. GD is DPI-aware, so this is the number a
    // DPI-unaware tool disagrees with, and the disagreement is the whole story (see stripBox).
    float cw = 0.f, ch = 0.f;
#ifdef GEODE_IS_WINDOWS
    RECT rc{};
    if (HWND h = GetActiveWindow())
        if (GetClientRect(h, &rc)) {
            cw = (float)(rc.right - rc.left);
            ch = (float)(rc.bottom - rc.top);
        }
#endif
    char b[448];
    snprintf(b, sizeof(b),
             "itermap: win=%.1fx%.1f visOrigin=%.1f,%.1f visSize=%.1fx%.1f frame=%.1fx%.1f "
             "viewScale=%.3f,%.3f parentScale=%.3f nodeRect=%.1f,%.1f..%.1f,%.1f "
             "client=%.0fx%.0f",
             win.width, win.height, vo.x, vo.y, vis.width, vis.height,
             frame.width, frame.height, sx, sy, pscale, bl.x, bl.y, tr.x, tr.y, cw, ch);
    writeResult(b);
}

// The bottom-right corner of getWinSize(), which is the whole design rectangle.
//
// THE CORNER IS FINE. The note that used to stand in hud.hpp -- that the right and bottom of
// getWinSize() are not reliably on screen, measured in a "1706x960 worker window" -- was an
// artefact of the instrument, not a fact about GD. The display is at 144 DPI, GD is DPI-aware,
// and the capture tool was not: the window's real client area is 2560x1440 and every screenshot
// was silently cropped to the top-left 1706x960 of it. Anything drawn in the bottom-right was on
// screen the whole time and simply outside the picture. Settled 2026-08-28 by asking Windows the
// same question with and without DPI awareness (2560x1440 vs 1706x960, GetDpiForWindow 144);
// py/gdtas/window.py now declares awareness, and logScreenMetrics above prints the numbers this
// conclusion rests on once per session.
//
// The bottom-right, because the top-left is the overlay column and everything between them is
// where the level is. A timeline over the playfield is a timeline you have to look past.
inline cocos2d::CCRect stripBox() {
    using namespace cocos2d;
    const CCSize v = CCDirector::sharedDirector()->getWinSize();
    const float m = v.width * 0.015f;              // margin off both edges
    const float w = v.width * 0.46f;
    const float h = v.height * 0.055f;
    return CCRect(v.width - w - m, m, w, h);
}

// THE AXIS IS TIME, not the level's x. World x is not monotonic -- lv22's rotated and reversed
// sections run it backwards or hold it still while the run carries on -- so an x axis made the
// playhead wander there and made "click here" mean a place the run passes more than once. Ticks
// have none of that, and they are what the keys were already asking in.
inline void drawStrip(cocos2d::CCNode* parent) {
    using namespace cocos2d;
    auto* n = drawNode(parent, STRIP_TAG, "itermap-strip"_spr, 1 << 20);
    if (!n) return;
    const bool on = stripVisible();
    n->setVisible(on);
    if (!on) { g_stripW = 0.f; return; }   // not drawn = not clickable
    const CCRect box = stripBox();
    const float kStripW = box.size.width, kStripH = box.size.height;
    g_stripX = box.origin.x; g_stripY = box.origin.y;
    g_stripW = kStripW;      g_stripH = kStripH;
    g_barTicks = barTicks();
    std::lock_guard<std::mutex> lk(g_mu);
    rebuildHotLocked();
    n->setPosition({box.origin.x, box.origin.y});
    n->clear();
    // Everything inside is a fraction of the box for the same reason the box is a fraction of the
    // window: none of these are pixels, and a hairline in one coordinate space is a slab in
    // another. `thin` is the narrowest mark that still shows up.
    const float thin = kStripW * 0.004f;
    const float pad = kStripH * 0.07f;
    // The backing panel for the summary text, drawn first so everything else sits on top of it.
    // It reaches from the strip's own top edge up past the label, and leftwards to wherever the
    // longest line ends -- the label is anchored by its bottom-right corner to the strip's right
    // edge, so the lines grow left and the panel has to follow them.
    if (g_labelH > 0.f) {
        const float lp = kStripH * 0.25f;
        fillRect(n, kStripW - g_labelW - lp, kStripH,
                 kStripW + lp * 0.5f, kStripH + kStripH * 0.25f + g_labelH + lp,
                 ccColor4F{0.f, 0.f, 0.f, 0.78f});
    }
    // the trough
    fillRect(n, 0.f, 0.f, kStripW, kStripH, ccColor4F{0.f, 0.f, 0.f, 0.78f});
    fillRect(n, 0.f, 0.f, kStripW, pad * 0.4f, ccColor4F{1.f, 1.f, 1.f, 0.25f});
    const float st = kStripW / (float)std::max(1LL, g_barTicks);   // ticks -> bar
    // Everything up to the playhead is the MAP's layer of the bar. Without F10 the bar is pure
    // transport: a trough and a playhead, and nothing that needs explaining.
    //
    // The deaths and fixups carry their own tick, so they land on this axis directly. The VETOES
    // do not -- a veto is a box in x and nothing else -- so they are drawn in the world only.
    // Inventing a tick for them by searching the position history would be a guess in exactly the
    // sections the tick axis exists to survive.
    if (mapVisible()) {
        // One column per bucket of ticks, wide enough to see: the axis is a whole level, so a
        // single tick is far thinner than a pixel.
        const long long tb = std::max(1LL, g_barTicks / 200);
        std::map<long long, std::pair<int, int>> col;   // bucket -> (deaths, dominant kind)
        std::map<long long, int[KindCount]> kinds;
        for (const Death& d : g_deaths) {
            const long long b = d.tick / tb;
            ++col[b].first;
            if (d.kind >= 0 && d.kind < KindCount) ++kinds[b][d.kind];
        }
        int most = 0;
        for (auto& kv : col) if (kv.second.first > most) most = kv.second.first;
        for (auto& kv : col) {
            int dom = KindRewind;
            auto it = kinds.find(kv.first);
            if (it != kinds.end())
                for (int k = 0; k < KindCount; ++k)
                    if (it->second[k] > it->second[dom]) dom = k;
            const float f = most > 0 ? (float)kv.second.first / (float)most : 0.f;
            const float x0 = (float)(kv.first * tb) * st;
            fillRect(n, x0, pad, x0 + std::max(thin, (float)tb * st),
                     pad + (kStripH - pad * 3.f) * f, kindColor(dom, 0.85f));
        }
        // fixups on their own row along the top edge -- they are a different question from the
        // deaths and must not be added into the same bar
        for (const Fixup& x : g_fixups) {
            const float fx = (float)x.tick * st;
            fillRect(n, fx, kStripH - pad, fx + thin, kStripH,
                     x.kill ? ccColor4F{1.00f, 0.35f, 0.85f, 0.9f}
                            : ccColor4F{1.00f, 0.80f, 0.20f, 0.9f});
        }
    }
    // Where the drag will land, if one is in progress. Amber and full height so it reads as "this
    // is what you are about to do", distinct from the white playhead saying where you are.
    if (g_dragging) {
        const float gx = g_dragFrac * kStripW;
        fillRect(n, gx - thin, -pad * 2.f, gx + thin, kStripH + pad * 2.f,
                 ccColor4F{1.00f, 0.75f, 0.15f, 0.95f});
    }
    // The playhead. During a seek it shows the DESTINATION rather than the live position: the
    // level is being flown through at hundreds of batches a frame and a playhead following that
    // sweeps the bar in a second, which reads as playback rather than as a seek.
    const long long headT = seeking() ? g_seekBarTick : g_nowTick;
    const float hx = std::min(kStripW, std::max(0.f, (float)headT * st));
    fillRect(n, hx - thin * 0.5f, -pad, hx + thin * 0.5f, kStripH + pad,
             ccColor4F{1.f, 1.f, 1.f, 0.95f});
}

// While a seek runs, the level is being drawn at hundreds of physics batches a frame: the picture
// is legible for none of it and reads as a strobe. Covering it is the only option -- turning
// rendering OFF is the path that crashes on the way back (see the note at g_seekTick) -- so the
// level is painted over and a transport indicator drawn on top, which is also what the reader
// expects "fast-forwarding" to look like.
//
// Chevrons rather than a glyph: the fonts GD ships have no play/rewind character, and three
// triangles pointing the right way need no font at all.
inline void drawCover(cocos2d::CCNode* parent, bool forward) {
    using namespace cocos2d;
    auto* n = drawNode(parent, COVER_TAG, "seek-cover"_spr, (1 << 20) - 1);
    if (!n) return;
    const bool on = (seeking() || g_dragging) && barVisible();
    n->setVisible(on);
    if (!on) return;
    const CCSize v = CCDirector::sharedDirector()->getWinSize();
    n->setPosition({0.f, 0.f});
    n->clear();
    // Fully opaque. There is nothing to see behind it -- the level under a seek is a strobe --
    // and a partly transparent cover just makes the strobe dimmer.
    fillRect(n, 0.f, 0.f, v.width, v.height, ccColor4F{0.f, 0.f, 0.f, 1.f});
    // Three chevrons, centred, pointing the way the level is about to move.
    const float s = v.height * 0.055f;                 // one chevron's half-height
    const float cy = v.height * 0.5f;
    const float dir = forward ? 1.f : -1.f;
    for (int i = 0; i < 3; ++i) {
        const float cx = v.width * 0.5f + dir * (float)(i - 1) * s * 1.5f;
        CCPoint tri[3] = {{cx - dir * s * 0.55f, cy + s},
                          {cx + dir * s * 0.55f, cy},
                          {cx - dir * s * 0.55f, cy - s}};
        n->drawPolygon(tri, 3, ccColor4F{1.f, 1.f, 1.f, 0.30f + 0.22f * (float)i},
                       0.f, ccColor4F{0.f, 0.f, 0.f, 0.f});
    }
}

// The text that goes above the strip. Three lines the reader can act on: what the run cost
// overall and its worst wall, what is under the playhead right now, and what the colours mean --
// the last one because a picture in five colours with no key is a picture of nothing. The key is
// spelled out in words rather than drawn as swatches: a CCLabelBMFont takes one colour for the
// whole string, so the swatches would have to be a fourth draw node kept in step with this text.
inline void summary(char* out, size_t cap, float px, float levelLen) {
    if (cap == 0) return;
    std::lock_guard<std::mutex> lk(g_mu);
    // WHERE YOU ARE, in the unit the bar and the keys work in. This line is the bar's own and is
    // there with or without the map.
    //
    // It leads with the TICK because x cannot answer the question on its own. Reported
    // 2026-08-28: a ten-tick step back at x=19,242 came out at x=19,303 and read as the level
    // moving forward -- but that stretch of lv22 runs in reverse, so ten ticks earlier IS 61 px
    // further right, and elsewhere in the rotated frame x does not move at all while time does.
    // With the tick on screen both of those read as what they are.
    const long long total = std::max(1LL, g_barTicks);
    int n = snprintf(out, cap, "t=%lld / %lld  (%.0f%%)   x=%.0f\n",
                     (long long)g_nowTick, total,
                     100.0 * (double)g_nowTick / (double)total, (double)px);
    if (n < 0 || (size_t)n >= cap) return;
    if (!mapWanted()) return;       // the bar alone says where you are and stops there
    // "Nothing recorded and nothing loaded", not "no file loaded": a solve in progress has rounds
    // in memory and no file yet, and that is exactly when watching the map grow is worth
    // something.
    if (g_deaths.empty() && g_fixups.empty()) {
        snprintf(out + n, cap - n,
                 "ITERATION MAP  nothing recorded for this level\n"
                 "  (solve it once, or build a map from a log with py/itermap_from_log.py)");
        return;
    }
    rebuildHotLocked();
    const Hot* top = hottestLocked();
    char worst[96] = "";
    if (top && g_rounds > 0)
        snprintf(worst, sizeof(worst), "   worst x=%.0f (%d %s, %.0f%%)",
                 (double)(top->x0 + kBucket * 0.5f), top->deaths, kindName(top->dominant),
                 100.0 * top->deaths / (double)std::max(1, (int)g_deaths.size()));
    // g_mapLevel is set only by save/load, so it doubles as "this run is over": a map still being
    // recorded has not failed to clear, it has not finished.
    const char* state = (g_mapLevel < 0) ? "   [in progress]"
                                         : (g_cleared ? "" : "   [did not clear]");
    // A map with deaths but no tails is not a map whose tails failed to draw -- it is a map that
    // never had any. Only a run records them; py/itermap_from_log.py cannot, because the tail is a
    // trajectory and the log holds one line per round, not one per tick. Every file in data/ came
    // from that rebuild, so "the trajectories are missing" is what every stored map looks like, and
    // it was reported as a drawing bug (2026-08-29) exactly because nothing said otherwise.
    const char* tails = (!g_deaths.empty() && g_paths.empty())
                        ? "   [no tails in this map - rebuilt from a log]" : "";
    n += snprintf(out + n, cap - n, "ITERATION MAP  %d rounds  %zu deaths  %zu fixups%s%s%s\n",
                  g_rounds, g_deaths.size(), g_fixups.size(), worst, state, tails);
    if (n < 0 || (size_t)n >= cap) return;
    const Hot* here = atLocked(px);
    if (here) {
        n += snprintf(out + n, cap - n,
                      "  here x=%.0f: %d death%s (%s), %d fixup%s  (rounds %d-%d)\n",
                      (double)px, here->deaths, here->deaths == 1 ? "" : "s",
                      kindName(here->dominant),
                      here->fixups, here->fixups == 1 ? "" : "s",
                      here->firstIter, here->lastIter);
    } else {
        n += snprintf(out + n, cap - n,
                      "  here x=%.0f: clear on the first plan   (%.0f%% of the level)\n",
                      (double)px, levelLen > 1.f ? 100.0 * px / levelLen : 0.0);
    }
    if (n < 0 || (size_t)n >= cap) return;
    // Two lines, not one. Measured on a 1706x960 worker: the single 118-character line ran off
    // the right edge and lost its last item. The overlay column has no wrapping, so the length of
    // every line here is a fixed budget of about 80 characters.
    // The last item is transport, and while the loop owns the level there is none -- saying so
    // beats offering a click that barLive() will refuse.
    snprintf(out + n, cap - n,
             "  deaths: green deeper / cyan followed / violet forced / red rewound / pink wedged\n"
             "  amber fixup, magenta kill fixup, purple veto   -   %s",
             barVisible() ? "click the strip to seek" : "live: the solve is writing this");
}

// ---- the block in the corner ----
//
// The label lives with the strip rather than in the overlay column, and is anchored by its
// BOTTOM-RIGHT corner to the strip's right edge. Two things follow from that and both are the
// point: the lines grow leftwards, so the longest of them (the colour key) cannot run off the
// right edge the way it did in the column; and turning the map on no longer moves any of the
// lines in the top-left column, which is what an overlay that reshuffles what you were reading
// costs you.
inline void drawLabel(cocos2d::CCNode* parent, float px, float levelLen) {
    using namespace cocos2d;
    auto* lbl = static_cast<CCLabelBMFont*>(parent->getChildByTag(LABEL_TAG));
    // The bar gets a readout of its own -- where you are, in ticks -- and the map adds its
    // analysis under it. summary() decides which of the two it is writing.
    if (!stripVisible()) {
        if (lbl) lbl->setVisible(false);
        g_labelW = g_labelH = 0.f;      // ...so the strip stops painting a panel for it
        return;
    }
    if (!lbl) {
        lbl = CCLabelBMFont::create("", "chatFont.fnt");
        if (!lbl) return;
        lbl->setTag(LABEL_TAG);
        lbl->setID("itermap-readout"_spr);
        lbl->setAnchorPoint({1.f, 0.f});
        lbl->setScale(0.5f);
        // ABOVE the strip, which is where its backing panel is drawn. Both sat at 1<<20, and
        // cocos breaks a tie by insertion order -- the strip is added second, so its 78% black
        // panel was painted over the text. That is what made the line read as grey next to the
        // white key legend: not the colour, the thing on top of it.
        lbl->setZOrder((1 << 20) + 2);
        lbl->setColor({255, 255, 255});
        lbl->setOpacity(255);
        parent->addChild(lbl);
    }
    lbl->setVisible(true);
    const CCRect box = stripBox();
    lbl->setPosition({box.origin.x + box.size.width,
                      box.origin.y + box.size.height + box.size.height * 0.25f});
    // Measured every frame, not only when the string changes: the box is what the strip paints
    // its backing panel against, and a stale one leaves text hanging off the panel's edge.
    g_labelW = lbl->getContentSize().width * lbl->getScale();
    g_labelH = lbl->getContentSize().height * lbl->getScale();
    // setString rebuilds every glyph sprite, so it is throttled the same way the session HUD is.
    static auto s_last = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - s_last).count() < 120
        && *lbl->getString())
        return;
    s_last = now;
    char b[512];
    summary(b, sizeof(b), px, levelLen);
    lbl->setString(b);
}

// ---- clicking the strip ----

// Start a seek to a level x. Called from the touch handler, so it only raises flags; the level
// change itself is a frame-boundary job (the update hook consumes g_seekRestart).
// A replay whose session has ended (the level was cleared, or it died out) is revived exactly the
// way F7 revives it, so that the bar keeps working on the results screen -- which is precisely
// when you want to go back and look at something.
inline bool g_seekRevive = false;

// Take GD's end-of-level screen off, before restarting the level under it.
//
// GD adds it and never removes it: resetLevel() starts a new attempt with the old screen still
// parented, so replaying a level you have cleared stacks another one on every clear. It is a
// GJDropDownLayer with no member on PlayLayer pointing at it, so it is found by type -- and both
// PlayLayer and the scene are searched, because which of the two it hangs off is GD's business
// and not something to hard-code.
inline void dismissEndScreen() {
    using namespace cocos2d;
    auto* pl = PlayLayer::get();
    if (!pl) return;
    CCNode* hosts[2] = {pl, pl->getParent()};
    for (CCNode* host : hosts) {
        if (!host) continue;
        CCArray* kids = host->getChildren();
        if (!kids) continue;
        // Collected first: removing while walking the array is what invalidates it.
        std::vector<CCNode*> doomed;
        for (unsigned i = 0; i < kids->count(); ++i) {
            auto* n = static_cast<CCNode*>(kids->objectAtIndex(i));
            if (geode::cast::typeinfo_cast<EndLevelLayer*>(n)) doomed.push_back(n);
        }
        for (CCNode* n : doomed) n->removeFromParentAndCleanup(true);
        if (!doomed.empty())
            writeResult("itermap: removed " + std::to_string(doomed.size())
                        + " end-of-level screen(s) before restarting");
    }
}

// Seek to a physics tick. The one entry point: the bar and the keys both ask in ticks now.
inline void seekToTick(long long targetTick) {
    if (targetTick < 0) targetTick = 0;
    g_seekTick = targetTick;
    g_seekForward = (targetTick >= g_nowTick);
    // The playhead parks at the destination while the seek runs -- which is a tick, so unlike the
    // old x version there is nothing to look up and nothing to be unknown about a place the run
    // has not been yet.
    g_seekBarTick = targetTick;
    // Forced scroll has no reverse, so the only way back is to run the level again from the top.
    g_seekRestart = (targetTick <= g_nowTick);
    if (g_sessionOver) g_seekRevive = true;
    char b[192];
    snprintf(b, sizeof(b), "itermap: seek to t=%lld from t=%lld (%s%s; speed stays %gx)",
             (long long)targetTick, (long long)g_nowTick,
             g_seekRestart ? "restarting the level" : "fast-forwarding",
             g_seekRevive ? ", reviving the replay" : "", (double)g_watchSpeed);
    writeResult(b);
}

// Stop seeking. The spectating notch is deliberately NOT touched here or when starting: the seek
// runs on its own loop count, so whatever speed the up/down keys were on is what the level is
// running at the moment it arrives. Forcing 1x here would silently undo the user's own setting,
// which is the same mistake the note at g_visResetPending records.
inline void endSeek(const char* why) {
    if (!seeking()) return;
    char b[160];
    snprintf(b, sizeof(b), "itermap: seek ended (%s) at t=%lld, back to %gx",
             why, (long long)g_nowTick, (double)g_watchSpeed);
    g_seekTick = -1;
    g_seekRestart = false;
    writeResult(b);
}

// How many physics batches this frame should run for the seek. 0 = nothing pending, or the target
// is reached and the arrival check below is about to end it.
//
// `dt` IS THE FRAME'S OWN dt, and it has to be, because a batch is one update(dt) call and dt is
// not a constant: slow motion divides it, the slow half of the spectating notch multiplies it,
// and a heavy frame simply is longer. Two bugs came out of assuming 1/60 (2026-08-28):
//   * at 0.25x the seek ran at a quarter speed, because a batch was one tick instead of four --
//     "the ultra-fast playback is slower in slow motion", exactly as reported;
//   * during a long seek the frames get slow, dt grows, and each batch then covered far more
//     than four ticks -- so the seek shot past its target, and the next key step found the
//     target BEHIND the player and restarted the level to get back to it.
// Deriving ticks-per-batch from dt removes both: the seek covers the same amount of level per
// frame whatever the clock is doing.
inline int seekLoops(float dt) {
    if (g_seekRestart || g_seekTick < 0) return 0;
    float ticksPerLoop = dt * 240.f;
    if (!(ticksPerLoop > 0.5f)) ticksPerLoop = 1.f;    // also catches NaN
    const long long remT = g_seekTick - g_nowTick;
    if (remT <= 0) return 0;
    const int n = (int)((float)remT / ticksPerLoop);
    // Less than one batch left: run NONE. A batch is indivisible, so rounding up here overshoots
    // by up to a batch -- and during a seek the frames are heavy, so a batch can be a hundred
    // ticks. Measured: a ten-tick step back landed 166 ticks past its target. seekArrived carries
    // the matching tolerance, so stopping short still lands.
    return n < 1 ? 0 : (n > kSeekLoopsMax ? kSeekLoopsMax : n);
}

// Whether the seek has arrived (or can no longer arrive). Asked once per frame.
//
// The tick target lands within ONE BATCH, not exactly: a batch is indivisible and seekLoops
// refuses to run a whole one for a part-batch remainder (the note there). This is the other half
// of that -- without it the seek would sit one part-batch short of its target for ever.
inline bool seekArrived(bool dead, float dt) {
    if (g_seekRestart || g_seekTick < 0) return false;
    if (dead) return true;
    const long long slack = (long long)std::max(1.f, dt * 240.f);
    return g_nowTick + slack > g_seekTick;
}

// ---- the keys' step ----
//
// A tap is five seconds; holding accelerates. Each step is instant (one frame covers up to 1,600
// ticks), so holding is really "repeat the step, getting bigger" -- which means the acceleration
// curve IS the feel of the key, and it is easy to make useless by being too eager. Measured with
// a step every 110 ms and the multiplier growing every four repeats: 1.6 seconds of holding
// crossed the whole of lv22 and ran off the end of the level. Slower on both counts, so that a
// second of holding is worth about twenty seconds of level and crossing a level takes a
// deliberate hold.
constexpr long long kSeekStepTicks = 5 * 240;
inline long long seekStep(int repeats) {
    long long mult = 1 + repeats / 6;
    if (mult > 6) mult = 6;
    return kSeekStepTicks * mult;
}

// Step by seconds from wherever the seek is currently aimed, so holding the key accumulates
// instead of fighting the seek already in flight.
//
// A FORWARD STEP NEVER RESTARTS THE LEVEL. Fast-forward has no reason to: the place asked for is
// ahead. It used to be able to, because a pending target the run had already overshot is behind
// the player and the general rule sent it back to the top to reach it -- so holding the right
// arrow restarted the level every few steps. Anchoring a forward step to whichever of the two is
// further on makes it impossible by construction.
inline void seekBy(long long deltaTicks) {
    long long from = (g_seekTick >= 0) ? g_seekTick : g_nowTick;
    if (deltaTicks > 0 && from < g_nowTick) from = g_nowTick;
    g_seekStepTo = -1;
    seekToTick(from + deltaTicks);
}

// A frame-step BACKWARDS, for the arrows while the game is stopped. Physics has no reverse, so
// this is a replay to just before where it wants to be, finished exactly with probe::g_step.
inline void stepBack(long long ticks) {
    const long long want = g_nowTick - ticks;
    g_seekStepTo = want < 0 ? 0 : want;
    g_seekRepause = true;
    seekToTick(g_seekStepTo - kStepUndershoot);
    // Every number the decision was made from, in one line. A step back that visibly moves the
    // player FORWARD can be any of: the tick it started from being stale, the target coming out
    // ahead, the restart not being asked for, or the finishing step overshooting -- and from
    // outside the process they all look the same.
    char b[224];
    snprintf(b, sizeof(b),
             "itermap: [stepback] from t=%lld by %lld -> want t=%lld, seek t=%lld, restart=%d",
             (long long)g_nowTick, ticks, (long long)g_seekStepTo,
             (long long)g_seekTick, (int)g_seekRestart);
    writeResult(b);
}

// Whether the bar can be driven at all right now.
inline bool barLive() {
    if (!barVisible() || g_stripW <= 0.f || g_barTicks <= 1) return false;
    if (solvingNow()) return false;         // the loop owns the level while it is solving
    auto* pl = PlayLayer::get();
    return pl && pl->m_player1;
}

// Is this point on the bar? A band of slack above and below, because the bar is deliberately thin
// and a scrubber you have to hit exactly is a scrubber you stop using.
inline bool onBar(float x, float y) {
    const float slack = g_stripH * 0.9f;
    return x >= g_stripX - slack && x <= g_stripX + g_stripW + slack
        && y >= g_stripY - slack && y <= g_stripY + g_stripH + slack;
}

inline float barFrac(float x) {
    float f = (x - g_stripX) / g_stripW;
    if (f < 0.f) f = 0.f;
    if (f > 1.f) f = 1.f;
    return f;
}

// The touch target. A targeted delegate ahead of the game's own handlers, which swallows a touch
// ONLY when it lands on the bar -- returning false from ccTouchBegan leaves the touch to whatever
// would have had it, so a click anywhere else changes nothing about the game's input.
//
// A press starts a DRAG rather than a seek. The level is not scrubbed while the finger is down --
// a backward move means restarting and re-running the level, which is not something to do per
// pixel of a drag -- so the drag moves a marker and the seek happens once, on release.
class StripTouch : public cocos2d::CCLayer {
public:
    static StripTouch* create() {
        auto* r = new StripTouch();
        if (r && r->init()) { r->autorelease(); return r; }
        CC_SAFE_DELETE(r);
        return nullptr;
    }
    bool init() {
        if (!CCLayer::init()) return false;
        this->setTouchEnabled(true);
        return true;
    }
    void registerWithTouchDispatcher() override {
        // Far below anything GD uses. -500 was measured (2026-08-28) to lose to the end-level
        // screen: a press on the bar there never reached this delegate at all, and went to GD's
        // own menu underneath. Targeted delegates are served in priority order and ties go to
        // insertion order, so the fix is to stop tying.
        cocos2d::CCDirector::sharedDirector()->getTouchDispatcher()
            ->addTargetedDelegate(this, -0x7000, true);
    }
    bool ccTouchBegan(cocos2d::CCTouch* t, cocos2d::CCEvent*) override {
        if (!t) return false;
        const cocos2d::CCPoint p = t->getLocation();
        const bool hit = onBar(p.x, p.y);
        // A press that lands on the bar and is refused anyway is the one case worth a line: it
        // means the bar is drawn and looks live and is not, and there is no other way to tell
        // that apart from "the click never arrived" from outside the process.
        if (hit && !barLive()) {
            auto* pl = PlayLayer::get();
            char b[192];
            snprintf(b, sizeof(b), "itermap: press on the bar refused - vis=%d w=%.1f len=%.1f "
                     "solving=%d play=%d player=%d",
                     (int)barVisible(), (double)g_stripW, (double)g_barTicks,
                     (int)solvingNow(), (int)(pl != nullptr),
                     (int)(pl && pl->m_player1));
            writeResult(b);
        }
        if (!hit || !barLive()) return false;
        g_dragging = true;
        g_dragFrac = barFrac(p.x);
        return true;
    }
    void ccTouchMoved(cocos2d::CCTouch* t, cocos2d::CCEvent*) override {
        if (!t || !g_dragging) return;
        g_dragFrac = barFrac(t->getLocation().x);
    }
    void ccTouchEnded(cocos2d::CCTouch* t, cocos2d::CCEvent*) override {
        if (!g_dragging) return;
        if (t) g_dragFrac = barFrac(t->getLocation().x);
        g_dragging = false;
        if (!barLive()) return;
        auto* pl = PlayLayer::get();
        seekToTick((long long)(g_dragFrac * (float)g_barTicks));
    }
    void ccTouchCancelled(cocos2d::CCTouch*, cocos2d::CCEvent*) override {
        g_dragging = false;   // no seek: a cancelled drag asked for nothing
    }
};

// ---- one call from the overlay pass ----
inline void draw(cocos2d::CCNode* parent, PlayLayer* pl) {
    if (!parent) return;
    const float levelLen = pl ? pl->m_levelLength : 0.f;
    const float px = (pl && pl->m_player1) ? pl->m_player1->getPositionX() : 0.f;
    if (mapWanted() && pl && pl->m_level) ensureLoaded(pl->m_level->m_levelID.value());
    if (mapWanted()) logScreenMetrics(parent);
    // The touch target is attached to whatever node the overlay is drawing into, and re-attached
    // if that node changes -- which it does when the results screen replaces the scene, and which
    // is exactly when you want the bar (to go back and look at something).
    // Re-added only when ABSENT: adding it per frame would re-register the touch delegate.
    if (barVisible() && !parent->getChildByTag(TOUCH_TAG)) {
        if (auto* tl = StripTouch::create()) {
            tl->setTag(TOUCH_TAG);
            tl->setID("seek-bar-touch"_spr);
            parent->addChild(tl, (1 << 20) + 1);
        }
    }
    // Which way the chevrons point. A drag is live (the marker moves under the finger); a seek
    // decided its direction when it was asked for and keeps it -- see g_seekForward.
    drawCover(parent, g_dragging ? (g_dragFrac * (float)g_barTicks > (float)g_nowTick) : g_seekForward);
    drawLabel(parent, px, levelLen);
    drawStrip(parent);
    drawWorld(pl ? pl->m_objectLayer : nullptr);
}

// The key. Refused with nothing to show rather than silently drawing an empty level -- "I pressed
// it and nothing happened" is the same picture as "there were no iterations here".
inline void toggle(int levelId) {
    // ensureLoaded is the one loader: it refuses to read a file over a run that is recording its
    // own map, which is what the key would otherwise do on the victory lap of a solve.
    ensureLoaded(levelId);
    // Flip whatever is on screen NOW, which during a solve is the mode's default rather than
    // anything anyone chose -- so the first press there turns the map OFF, as it looks like it
    // should. From here on the answer is explicit and the mode stops deciding.
    g_mapPref = mapWanted() ? 0 : 1;
    char b[192];
    {
        std::lock_guard<std::mutex> lk(g_mu);
        ++g_generation;   // force a rebuild: the parent is unchanged but the visibility is not
        snprintf(b, sizeof(b), "itermap: %s (level %d, %zu deaths, %zu fixups, %d rounds)",
                 mapWanted() ? "shown" : "hidden", levelId, g_deaths.size(), g_fixups.size(),
                 g_rounds);
    }
    writeResult(b);
}

}  // namespace itermap
}  // namespace p1
