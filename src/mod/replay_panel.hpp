#pragma once
// The replay panel: GD's own record of the deepest verified attempt, played in a corner of the screen while the
// search is working. (The pure half -- the readers, the clipping, the silhouettes -- is replay_panel_geom.hpp.)
//
// WHAT IS PLAYED. Not the model's guess: the rows `anchors::g_deepest` holds, which the player wrote as each tick of
// the attempt ended, so any place the model was wrong and GD's state was taken instead is already in them. The one part that is the
// model's is the end of the path: the plan the solver's latest search found, joined on from its anchor and drawn in its own colour
// (with the last few plans as ghosts beside it, each a figure of its player's mode). The level
// is `dpsolve::g_csv` (the table the session opened, not the sliced copy the scene may hold), the moving geometry is the run's
// own recording (`dp_groups.txt`, the deepest replay's), and the points where a fixup says the model parted from GD
// are marked on the bar and ringed as the path passes them.
//
// WHERE. The free area from the middle of the screen to the top-right: the overlay column keeps the left edge, the
// iteration map's strip the bottom-right. One CCDrawNode child of the scene, CPU-clipped to the panel's box.
//
// WHEN. On by default (cfg replaypanel), the key turns it off, and only while a solve runs: the showing of the solution
// is a replay and the panel is not drawn there. Both ends of a solve empty it.
//
// WHAT IT NEVER DOES. It writes nothing the solver reads, holds no pointer into the game, takes the main thread's
// copy of the rows once per change and does its file reading and parsing on one background thread per input. Off, or
// outside a solve, a frame costs a few tests.
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>

#include "mod/itermap.hpp"
#include "mod/replay_panel_geom.hpp"

namespace p1 {
namespace replaypanel {

namespace pg = panelgeom;

// Tags are looked up with getChildByTag on the scene, where every overlay hangs, so they must not repeat one another's:
// 0x51D50.. are hud.hpp's, 0x51D60.. itermap's, 0x51D70 touch_controls' PAD_TAG, 0x51D71 progress_bars' BAR_TAG. (The panel
// first took 0x51D70/71, and its label lookup returned the progress bar: a CCLabelBMFont cast on another node.)
constexpr int PANEL_TAG = 0x51D80;
constexpr int LABEL_TAG = 0x51D81;
constexpr int TOUCH_TAG = 0x51D82;

// What the panel plays, and whether: cfg `replaypanel` is the start (0 off, 1 the whole verified path -- the default --, 2 its last
// stretch; 3 was the model's plan as a mode of its own and now means 1) and the key (Tab) goes round full -> recent -> off -> full.
// -1 = follow cfg. The model's plan is not a mode: whenever the solver has found one, the path played is GD's own up to the anchor of
// the solver's latest search and, from there, the walk the model made of the plan (`dp_tail.txt.trace.csv`) -- what the model believes
// the plan does, drawn in its own colour -- joined to the verified path as its last part. It may differ from what GD will do; it is a
// view of the planning, and nothing the solver reads.
enum Mode { Full = 0, Recent = 1, Off = 2 };
inline int g_modePref = -1;
inline int mode() {
    if (g_modePref >= 0) return g_modePref;
    return g_cfg.replayPanel == 0 ? Off : g_cfg.replayPanel == 2 ? Recent : Full;
}
inline bool wanted() { return mode() != Off; }
inline const char* modeName() {
    return mode() == Full ? "full" : mode() == Recent ? "recent" : "off";
}

// ------------------------------------------------------------------ reading a file the solver may be writing

// The whole file, opened so that the solver can still replace or delete it while this holds it (a plain ifstream shares
// read and write but not delete, and the loop removes and re-copies its recordings). Empty when it cannot be read.
inline std::string readShared(const std::string& path, size_t cap = (size_t)512 << 20) {
    std::string out;
#ifdef GEODE_IS_WINDOWS
    const int wn = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wn <= 0) return out;
    std::wstring w((size_t)wn, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), wn);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER sz{};
    if (GetFileSizeEx(h, &sz) && (unsigned long long)sz.QuadPart <= cap) {
        out.resize((size_t)sz.QuadPart);
        size_t got = 0;
        while (got < out.size()) {
            DWORD n = 0;
            const DWORD ask = (DWORD)std::min<size_t>(out.size() - got, 1u << 24);
            if (!ReadFile(h, out.data() + got, ask, &n, nullptr) || n == 0) break;
            got += n;
        }
        out.resize(got);
    }
    CloseHandle(h);
#else
    std::ifstream f(path, std::ios::binary);
    if (!f) return out;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n < 0 || (unsigned long long)n > cap) return out;
    f.seekg(0);
    out.resize((size_t)n);
    f.read(out.data(), n);
    out.resize((size_t)f.gcount());
#endif
    return out;
}

// ------------------------------------------------------------------ what the panel holds

struct LevelJob {
    std::string sig;
    std::shared_ptr<const pg::Level> level;
    size_t obb = 0;
    double ms = 0.0;
};
struct MoversJob {
    std::shared_ptr<const pg::Level> level;     // the level the rows' object indices are into
    std::shared_ptr<const pg::Movers> movers;
    long long serial = -1, depth = -1;
    double ms = 0.0;
};

struct PlanJob {
    std::shared_ptr<const pg::Ghost> model;
    long long size = -1, mtime = 0;
    long long gen = -1;                          // State::planGen when the job was started
    double ms = 0.0;
};

struct State {
    // the level
    std::string levelSig;                        // dpsolve::g_csvSig of the table in `level` ("" = none read)
    std::shared_ptr<const pg::Level> level;
    std::future<LevelJob> levelJob;
    // GD's path (the deepest verified attempt), and the path shown: that with the model's plan joined on from the anchor
    std::shared_ptr<const pg::Track> gdTrack;
    std::shared_ptr<const pg::Track> track;
    // The model's plans: a ring of the last cfg `replaypanelplans` of them (newest first), each a walk the solver's search made
    // of the plan it found; the deepest is the one in use (`planShown`: the path is built from it and the camera follows it, the
    // rest are drawn beside it as ghosts). A change of the one in use waits for the clock to start over, so a viewer is not taken
    // off a replay half way. `planStub` is a walk of a few ticks (a search doomed from its anchor), kept for when there is nothing
    // else. The file's stamp lets it be read again only when it changed, and `planStale` is the stamp it had when the solve (or
    // the level) began: a file that old is the last level's, and is never read.
    std::vector<std::shared_ptr<const pg::Ghost>> ring;
    std::shared_ptr<const pg::Ghost> planStub, planShown;
    const pg::Ghost* lastPlan = nullptr;         // the plan the shown path was built with (compared, never read)
    std::future<PlanJob> planJob;
    long long planGen = 0;                       // bumped by resetPath(): a job started before it read the last level's file
    long long planSize = -1;
    long long planMtime = 0;
    long long planStaleSize = -2, planStaleMtime = 0;
    long long plansRead = 0;
    bool planSaid = false;                       // the first plan that was long enough to keep has been logged
    std::chrono::steady_clock::time_point planPolled = std::chrono::steady_clock::now();
    long long trackSerial = -2;
    size_t trackSrcRows = 0;
    long long staleSerial = -3;                  // the solver's rows when the last level / solve ended: not the new one's
    size_t staleRows = 0;
    // the run's recording of the moving geometry
    std::shared_ptr<const pg::Movers> movers;
    std::vector<int> moverOf;                    // object index -> index in movers->m, -1 for a static one
    long long moversSerial = -1, moversDepth = -1;   // the recording's attempt and depth that were read
    long long askedSerial = -2, askedDepth = -2;     // ...and the pair last asked for (so one failure is not retried every frame)
    std::future<MoversJob> moversJob;
    // the corrections
    std::vector<pg::Mark> marks;
    size_t fixupsSeen = (size_t)-1;
    // the clock and the camera
    bool clockOn = false;
    pg::Clock clock;                             // where in the stretch the picture is
    std::chrono::steady_clock::time_point last;
    pg::Camera cam;                              // eased toward the player's y; off = snap on the next frame
    // where the panel's left edge is: it keeps clear of the overlay's text and only moves right within a solve, so a
    // line that grows and shrinks does not make the picture breathe
    float leftEdge = 0.f;
    // bookkeeping
    bool wasSolving = false;
    int levelId = -1;                            // g_cfg.levelId the panel is holding data for (-1 = none yet)
    bool visible = false;                        // the nodes are shown (so they need hiding when it goes off)
    long long frames = 0;
    double usSum = 0.0;
    long long runFrames = 0;                     // frames drawn and their cost since the panel was last emptied
    double runUs = 0.0;
    long long runSkipped = 0;                    // ...and frames left alone because the game was busy
    // The frame interval: a frame that follows a long one is not drawn (see update)
    std::chrono::steady_clock::time_point lastFrame = std::chrono::steady_clock::now();
    bool drawn = false;                          // something has been emitted since the panel was last emptied
    bool held = false;                           // the picture is held because the game is busy
    // The frame census of the last kStatEverySec seconds the panel was on screen: the gaps between frames, and how many
    // were drawn / held. Logged now and then, so a run's log says how the panel kept up without waiting for the solve to
    // end (a worker's session ends without the panel being emptied).
    std::chrono::steady_clock::time_point statSince = std::chrono::steady_clock::now();
    long long statFrames = 0, statHeld = 0, statOver40 = 0;   // ...and the frames over 40 ms (what the old threshold held)
    double statGapSum = 0.0, statGapMax = 0.0;
    // what the keys do to it while the screen is dark
    bool paused = false;                         // F2: the clock stands still
    int speedIdx = -1;                           // the pace, a notch of WATCH_SPEEDS (-1 = from cfg when first wanted)
    bool owning = false;                         // on screen with a path to play: the transport keys are the panel's
    std::string lastText;                        // the status line that goes with the held picture
    // the mouse: where the panel stood on the last frame it was drawn (a press is tested against it), and a drag in progress
    cocos2d::CCRect box;
    bool boxValid = false;
    bool dragging = false;
    bool dragWasPaused = false;                  // F2's stop, put back when the drag ends (a drag stops the clock under the finger)
    long long dragEvents = 0;                    // presses taken since the panel was last emptied (the log says the first few)
};
inline State g_s;

// A frame that comes this long after the one before it is a frame of a game that is busy (the fast loop's batches, a section
// search's slices, a recording pass: 50 ms to seconds a frame on a heavy level, against 16.7 ms when it is only waiting for the
// search). The panel is an animation, and an animation drawn on frames like those jerks; it is held where it was instead.
//
// How long that is follows the frame budget (cfg framebudgetms): a batch is cut at the budget, so a frame of a game that
// is flying lasts the budget plus what the frame costs; only a frame well past that is the game being busy with something
// else. The panel used to hold at 40 ms: with a 100 ms budget that was every frame of a flight, and with a section search
// running (it hands the frame back every 50 ms, cfg secslicems, so its frames come at 54 ms) every frame of the search.
// With the budget at 30 ms a flight's frames come at 30-45 ms, and 80 ms leaves the search's slices a margin: the panel
// keeps moving through both, at 20-30 frames a second.
inline double busyMs() { return std::max(80.0, 2.0 * g_cfg.frameBudgetMs); }
inline std::string planPath() { return std::string(DATA_DIR) + "/dp_tail.txt.trace.csv"; }
constexpr double kStatEverySec = 30.0;           // the frame census is logged this often while the panel is on screen

// The stretch played: the whole path (mode full) or its last `replaypanelspan` ticks, ending at the frontier (recent). The path is
// GD's verified one with the model's plan joined to it when there is one, so the frontier is the end of the plan.
inline void stretch(const pg::Track& tr, long long& a, long long& b) {
    pg::stretchOf(tr, mode() == Recent ? g_cfg.replayPanelSpan : 0, a, b);
}

inline void say(const std::string& line) {
    writeResult("replaypanel: " + line);
}

// ------------------------------------------------------------------ taking the inputs in

// The rows of the deepest attempt, as the few numbers a picture needs. Main thread (g_deepest is written there, in
// onDeath), once per change.
inline std::shared_ptr<const pg::Track> copyTrack(const std::vector<AnchorRow>& rows, long long serial) {
    auto tr = std::make_shared<pg::Track>();
    tr->f.resize(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        const AnchorRow& r = rows[i];
        if (!r.valid) continue;
        pg::Frame& f = tr->f[i];
        f.x = r.x; f.y = r.y; f.vy = r.vy;
        f.x2 = r.x2; f.y2 = r.y2;
        f.mode = (signed char)r.mode;
        f.flip = (signed char)(r.flip != 0);
        f.mini = (signed char)(r.mini != 0);
        f.dual = (signed char)(r.dual != 0);
        f.og = (signed char)(r.onGround != 0);
        f.gframe = (signed char)(r.gframe & 3);
        f.valid = true;
    }
    tr->serial = serial;
    tr->finish();
    return tr;
}

// The path that is shown: GD's own with, when the solver has found a plan, the model's plan joined on from the anchor of the
// latest search (GD's frames after the anchor are left out). Called when either changes (and when the mode does); a change of
// what is shown starts the clock over at the start of its stretch, the first time it is on at all included.
inline void rebuildShown(bool restart) {
    std::shared_ptr<const pg::Track> next = g_s.gdTrack;
    const pg::Ghost* plan = (g_s.planShown && !g_s.planShown->f.empty()) ? g_s.planShown.get() : nullptr;
    if (plan) {
        auto sp = std::make_shared<pg::Track>(pg::spliceGhost(g_s.gdTrack.get(), *plan));
        sp->serial = g_s.gdTrack ? g_s.gdTrack->serial : -2;
        next = sp;
    }
    const bool changed = next != g_s.track;
    g_s.track = next;
    if (!g_s.track) return;
    if (changed) g_s.fixupsSeen = (size_t)-1;
    // A path of GD's that grows (the deepest attempt changes at every death) does not take the viewer back to the start of
    // the stretch; a change of what the picture is does: the mode, a new plan, the first path of a solve.
    const bool planChanged = plan != g_s.lastPlan;
    g_s.lastPlan = plan;
    if (!g_s.clockOn || restart || planChanged) {
        long long a, z;
        stretch(*g_s.track, a, z);
        g_s.clock = pg::Clock{(double)a, 0.0};
        g_s.clockOn = true;
        g_s.cam.on = false;
        g_s.last = std::chrono::steady_clock::now();
    }
}
// Show this path from now on (the clock starts at the start of its stretch the first time).
inline void adoptTrack(std::shared_ptr<const pg::Track> tr, size_t srcRows, const char* what) {
    g_s.gdTrack = std::move(tr);
    g_s.trackSerial = g_s.gdTrack->serial;
    g_s.trackSrcRows = srcRows;
    char b[160];
    snprintf(b, sizeof(b), "path of the %s (attempt %lld): ticks %lld..%lld", what, g_s.trackSerial, g_s.gdTrack->first,
             g_s.gdTrack->last);
    say(b);
    rebuildShown(false);
}

// What belongs to the path and the recording (not the level): drop it, and refuse the rows the solver still holds --
// they are the last level's until its next attempt replaces them.
inline void resetPath() {
    g_s.gdTrack.reset();
    g_s.track.reset();
    g_s.ring.clear();
    g_s.planStub.reset();
    g_s.planShown.reset();
    g_s.planSize = -1;
    g_s.planMtime = 0;
    g_s.plansRead = 0;
    g_s.planSaid = false;
    ++g_s.planGen;                               // a read still running is the last level's (or solve's): pollPlan drops it
    // the plan file as it stands now is the last level's (or solve's): it is not read until a search writes it again
    {
        long long sz = 0, mt = 0;
        std::error_code ec;
        const auto fs = std::filesystem::file_size(planPath(), ec);
        const auto ft = std::filesystem::last_write_time(planPath(), ec);
        if (!ec) { sz = (long long)fs; mt = (long long)ft.time_since_epoch().count(); g_s.planStaleSize = sz; g_s.planStaleMtime = mt; }
        else g_s.planStaleSize = -2;
    }
    g_s.lastPlan = nullptr;
    g_s.trackSerial = -2;
    g_s.trackSrcRows = 0;
    g_s.staleSerial = anchors::g_deepestSerial;
    g_s.staleRows = anchors::g_deepest.size();
    g_s.movers.reset();
    g_s.moverOf.clear();
    g_s.moversSerial = g_s.moversDepth = -1;
    g_s.askedSerial = g_s.askedDepth = -2;
    g_s.marks.clear();
    g_s.fixupsSeen = (size_t)-1;
    g_s.clockOn = false;
    g_s.cam.on = false;
}

// A solve has ended, or one has begun: whatever the panel holds is the last solve's. The panel is shown only while a
// solve runs, so after the clear (the showing of the solution is a replay) it is emptied, and the next solve starts
// from nothing -- not from the last level's picture for the moment before its own data arrives.
inline void retire() {
    const bool had = g_s.track || g_s.level || g_s.movers;
    resetPath();
    g_s.level.reset();
    g_s.levelSig.clear();
    g_s.leftEdge = 0.f;
    g_s.levelId = -1;
    g_s.drawn = false;
    g_s.held = false;
    g_s.paused = false;
    g_s.owning = false;
    g_s.boxValid = false;
    g_s.dragging = false;
    g_s.dragEvents = 0;
    g_s.lastText.clear();
    if (had) {
        char b[160];
        snprintf(b, sizeof(b), "emptied (%lld frames drawn, %lld held while the game was busy, %.0f us a frame on average)",
                 g_s.runFrames, g_s.runSkipped, g_s.runFrames > 0 ? g_s.runUs / (double)g_s.runFrames : 0.0);
        say(b);
    }
    g_s.runFrames = 0;
    g_s.runUs = 0.0;
    g_s.runSkipped = 0;
}

inline void startLevelJob() {
    if (g_s.levelJob.valid()) return;
    const std::string sig = dpsolve::g_csvSig;
    std::string csv = dpsolve::g_csv;
    const std::string obbPath = std::string(DATA_DIR) + "/obb.txt";
    g_s.levelJob = std::async(std::launch::async, [sig, csv = std::move(csv), obbPath]() {
        const auto t0 = std::chrono::steady_clock::now();
        LevelJob j;
        j.sig = sig;
        const std::string obbText = readShared(obbPath, (size_t)64 << 20);
        const pg::ObbMap obb = pg::parseObb(obbText);
        j.obb = obb.size();
        j.level = std::make_shared<const pg::Level>(pg::parseObjrects(csv, &obb));
        j.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return j;
    });
}

inline void startMoversJob(long long tMax) {
    if (g_s.moversJob.valid() || !g_s.level || dpsolve::g_groupsPath.empty()) return;
    g_s.askedSerial = dpsolve::g_groupsSerial;
    g_s.askedDepth = dpsolve::g_groupsDepth;
    const std::string path = dpsolve::g_groupsPath;
    auto level = g_s.level;
    const long long serial = dpsolve::g_groupsSerial, depth = dpsolve::g_groupsDepth;
    g_s.moversJob = std::async(std::launch::async, [path, level, serial, depth, tMax]() {
        const auto t0 = std::chrono::steady_clock::now();
        MoversJob j;
        j.level = level;
        j.serial = serial;
        j.depth = depth;
        const std::string text = readShared(path);
        j.movers = std::make_shared<const pg::Movers>(pg::parseGroups(text, *level, tMax));
        j.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return j;
    });
}

inline bool ready(const std::future<LevelJob>& f) {
    return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}
inline bool ready(const std::future<MoversJob>& f) {
    return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}
inline bool ready(const std::future<PlanJob>& f) {
    return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

// ------------------------------------------------------------------ the model's plans

// The walk the solver's latest search made of its plan. The solver writes it next to the plan (`<out>.trace.csv`), once per
// call; it is read again when its size or time changed, on a background thread, polled once a second. Nothing here writes.
// Each walk read goes into the ring (the last cfg `replaypanelplans` of them), the deepest is the one the path is built from
// and the camera follows, and the rest are drawn beside it.
constexpr long long kPlanMinTicks = 120;      // a walk shorter than this is a stub: a search that is doomed from its anchor writes one

inline bool planStampOf(long long& size, long long& mtime) {
    std::error_code ec;
    const std::string path = planPath();
    const auto sz = std::filesystem::file_size(path, ec);
    if (ec) return false;
    const auto mt = std::filesystem::last_write_time(path, ec);
    if (ec) return false;
    size = (long long)sz;
    mtime = (long long)mt.time_since_epoch().count();
    return true;
}

inline void startPlanJob(long long size, long long mtime) {
    const std::string path = planPath();
    const long long gen = g_s.planGen;
    g_s.planJob = std::async(std::launch::async, [path, size, mtime, gen]() {
        const auto t0 = std::chrono::steady_clock::now();
        PlanJob j;
        j.size = size;
        j.mtime = mtime;
        j.gen = gen;
        const std::string text = readShared(path, (size_t)256 << 20);
        const pg::Track tr = pg::parseModelTrace(text);
        if (tr.first >= 0) j.model = pg::ghostOf(tr);
        j.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return j;
    });
}

// The deepest of the ring: the walk that gets furthest in x (the newest of equals), or the stub when the ring is empty.
inline std::shared_ptr<const pg::Ghost> deepestPlan() {
    std::shared_ptr<const pg::Ghost> best;
    for (const auto& g : g_s.ring)
        if (g && !g->f.empty() && (!best || g->endX() > best->endX())) best = g;
    return best ? best : g_s.planStub;
}

// The deepest plan becomes the one in use when the clock starts over (or at once when none is in use yet): a viewer is not
// taken off a replay half way by a new plan coming in, whose ghost is drawn the moment it is read all the same.
inline void applyPlan() {
    const auto best = deepestPlan();
    if (!best || best == g_s.planShown) return;
    g_s.planShown = best;
    if (wanted()) rebuildShown(false);           // (the path is built when the panel comes on, if it is off now)
}

inline void pushPlan(const std::shared_ptr<const pg::Ghost>& g) {
    ++g_s.plansRead;
    if (g->last() - g->first() < kPlanMinTicks) {
        if (g_s.ring.empty()) g_s.planStub = g;
        return;
    }
    // the same walk again (a search that was asked twice with the same anchor and found the same plan) is not a new plan
    if (!g_s.ring.empty()) {
        const pg::Ghost& h = *g_s.ring.front();
        if (h.first() == g->first() && h.last() == g->last() && h.endX() == g->endX() && h.f.back().y == g->f.back().y) return;
    }
    g_s.ring.insert(g_s.ring.begin(), g);
    const size_t cap = (size_t)std::max<long long>(1, g_cfg.replayPanelPlans);
    if (g_s.ring.size() > cap) g_s.ring.resize(cap);
    g_s.planStub.reset();
    if (!g_s.planSaid) {
        g_s.planSaid = true;
        char b[200];
        snprintf(b, sizeof(b), "plan of the model: ticks %lld..%lld (from the anchor); the last %zu are drawn together, the deepest followed",
                 g->first(), g->last(), cap);
        say(b);
    }
}

inline void pollPlan() {
    if (ready(g_s.planJob)) {
        PlanJob j = g_s.planJob.get();
        if (j.gen != g_s.planGen) return;        // read before the path was reset: the last level's plan, never shown
        g_s.planSize = j.size;
        g_s.planMtime = j.mtime;
        if (j.model) {
            pushPlan(j.model);
            if (!g_s.planShown) applyPlan();
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (g_s.planJob.valid() || std::chrono::duration<double>(now - g_s.planPolled).count() < 1.0) return;
    g_s.planPolled = now;
    long long size = 0, mtime = 0;
    if (!planStampOf(size, mtime)) return;
    if (size == g_s.planSize && mtime == g_s.planMtime) return;
    if (size == g_s.planStaleSize && mtime == g_s.planStaleMtime) return;   // the file the last level (or solve) left
    startPlanJob(size, mtime);
}
// The level in `level` is the one the solver holds now.
inline bool levelCurrent() { return g_s.level && g_s.levelSig == dpsolve::g_csvSig; }

// Everything the panel reads, brought up to date. Cheap when nothing changed: a few comparisons.
inline void refresh() {
    // the level: a table the panel has not read (a new level, or the same one again after retire())
    if (!dpsolve::g_csv.empty() && dpsolve::g_csvSig != g_s.levelSig && !g_s.levelJob.valid()) {
        if (!g_s.levelSig.empty()) resetPath();      // another table in the middle of a session: another level
        startLevelJob();
    }
    if (ready(g_s.levelJob)) {
        LevelJob j = g_s.levelJob.get();
        if (j.sig == dpsolve::g_csvSig) {            // (a table that has been replaced since is read again next frame)
            g_s.level = j.level;
            g_s.levelSig = j.sig;
            g_s.movers.reset();
            g_s.moverOf.clear();
            g_s.askedSerial = g_s.askedDepth = -2;
            char b[160];
            snprintf(b, sizeof(b), "level %zu boxes (%zu not drawn), %zu oriented boxes, read in %.1f ms",
                     j.level->objs.size(), j.level->hidden, j.obb, j.ms);
            say(b);
        }
    }
    // GD's path: the deepest attempt's rows, when they have changed
    const std::vector<AnchorRow>& rows = anchors::g_deepest;
    if (!rows.empty() && (anchors::g_deepestSerial != g_s.trackSerial || rows.size() != g_s.trackSrcRows)
        && !(anchors::g_deepestSerial == g_s.staleSerial && rows.size() == g_s.staleRows)) {
        adoptTrack(copyTrack(rows, anchors::g_deepestSerial), rows.size(), "deepest attempt");
    }
    // the moving geometry: the run's recording, read again whenever the solver has kept a new one
    if (ready(g_s.moversJob)) {
        MoversJob j = g_s.moversJob.get();
        if (j.level == g_s.level) {
            g_s.movers = j.movers;
            g_s.moversSerial = j.serial;
            g_s.moversDepth = j.depth;
            g_s.moverOf.assign(g_s.level->objs.size(), -1);
            for (size_t k = 0; k < g_s.movers->m.size(); ++k) g_s.moverOf[(size_t)g_s.movers->m[k].obj] = (int)k;
            char b[200];
            snprintf(b, sizeof(b), "moving geometry of attempt %lld: %zu objects, %lld rows up to tick %lld, read in %.1f ms",
                     g_s.moversSerial, g_s.movers->m.size(), g_s.movers->rowsRead, g_s.movers->cover, j.ms);
            say(b);
        }
    }
    if (g_s.level && g_s.track && !g_s.moversJob.valid() && !dpsolve::g_groupsPath.empty() && dpsolve::g_groupsDepth >= 0
        && (dpsolve::g_groupsSerial != g_s.askedSerial || dpsolve::g_groupsDepth != g_s.askedDepth))
        startMoversJob(g_s.track->last + 600);
    // the corrections: fixups of the run that lie on this path
    if (g_s.track) {
        std::lock_guard<std::mutex> lk(itermap::g_mu);
        if (itermap::g_fixups.size() != g_s.fixupsSeen) {
            g_s.fixupsSeen = itermap::g_fixups.size();
            g_s.marks.clear();
            for (const itermap::Fixup& fx : itermap::g_fixups) {
                const pg::Frame* f = g_s.track->at(fx.tick);
                if (f && std::fabs(f->x - fx.x) < 3.f && std::fabs(f->y - fx.y) < 3.f)
                    g_s.marks.push_back(pg::Mark{fx.tick, fx.x, fx.y, fx.kill != 0});
            }
            std::sort(g_s.marks.begin(), g_s.marks.end(),
                      [](const pg::Mark& a, const pg::Mark& b) { return a.t < b.t; });
        }
    }
}

// ------------------------------------------------------------------ the panel's place and the clock

// The free area, from the middle of the screen to the top-right. The overlay column keeps the left edge and the
// iteration map's strip the bottom-right; getWinSize() is the whole design rectangle (see itermap::stripBox).
//
// The left edge is the middle of the screen unless the overlay's own text reaches past it: the elapsed time at the end of
// the first line and the render state at the end of the keys line do, on a long phase or a long held-state line, and a
// panel drawn over the end of a word hides it. So the edge is moved right of the right-most of those lines that stands on
// the panel's rows (`textTags` are their tags; getChildByTag is the lookup the overlay itself uses), and kept there.
inline cocos2d::CCRect panelBox(cocos2d::CCNode* parent, std::initializer_list<int> textTags) {
    using namespace cocos2d;
    const CCSize v = CCDirector::sharedDirector()->getWinSize();
    const float m = v.width * 0.012f;
    const float y0 = v.height * 0.50f, y1 = v.height - m;
    float right = 0.f;
    for (const int tag : textTags) {
        CCNode* n = parent->getChildByTag(tag);
        if (!n || !n->isVisible()) continue;
        const CCRect b = n->boundingBox();
        if (b.getMaxY() < y0 || b.getMinY() > y1) continue;     // not on the panel's rows
        right = std::max(right, b.getMaxX());
    }
    const float want = std::min(v.width * 0.60f, right + 5.f);  // (a line past 60% is let overlap: the picture needs room)
    g_s.leftEdge = std::max(g_s.leftEdge, std::max(v.width * 0.42f, want));
    return CCRect(g_s.leftEdge, y0, v.width - m - g_s.leftEdge, y1 - y0);
}

// Where the pace comes from: the notch of the game's own spectating speeds nearest to cfg `replaypanelspeed` the first time
// it is wanted, then the arrows move it.
inline void initSpeed() {
    if (g_s.speedIdx >= 0) return;
    const float want = g_cfg.replayPanelSpeed > 0.f ? g_cfg.replayPanelSpeed : 1.f;
    int best = WATCH_SPEED_1X;
    for (int i = 0; i < WATCH_SPEED_N; ++i)
        if (std::fabs(std::log(WATCH_SPEEDS[i] / want)) < std::fabs(std::log(WATCH_SPEEDS[best] / want))) best = i;
    g_s.speedIdx = best;
}

// The tick kept inside the stretch (the mode can change it under a clock that stands still).
inline void clampTick() {
    if (!g_s.track) return;
    long long a, z;
    stretch(*g_s.track, a, z);
    g_s.clock.tick = std::max((double)a, std::min((double)z, g_s.clock.tick));
}

inline void advanceClock(const pg::Track& tr) {
    using clk = std::chrono::steady_clock;
    const auto now = clk::now();
    double dt = std::chrono::duration<double>(now - g_s.last).count();
    g_s.last = now;
    if (dt > 0.1) dt = 0.1;                     // a stalled frame (a level swap, a breakpoint) is not a jump
    if (g_s.paused) return;                     // F2 stopped it: the clock stands still
    initSpeed();
    long long a, z;
    stretch(tr, a, z);
    if (g_s.clock.advance(dt, (double)WATCH_SPEEDS[g_s.speedIdx], a, z)) {
        g_s.cam.on = false;                     // started over: the camera snaps
        applyPlan();                            // ...and a newer plan, if the solver found one, is the one that plays now
    }
}

// ------------------------------------------------------------------ the keys, while the screen is dark

// With the screen off (a solve's fast loop) the transport keys have nothing of the game to move: the level is the loop's own
// verification replay, held between its rounds. They are the panel's then -- F2 stops and starts it, F3 / F4 step it one
// tick / ten (a step stops it), the arrows seek it (a tap is five seconds, holding accelerates) and change its pace. With the
// screen on the panel is not shown and the keys are the game's, as ever. The panel's state (mode, place, stop, pace) lives
// through that, so turning the screen off again finds it where it was left.
//
// `owning` is set by update(): the panel is on screen with a path to play. Without one the keys stay the game's.
inline bool owns() { return g_s.owning; }

inline void keyPause() {
    g_s.paused = !g_s.paused;
    say(g_s.paused ? "paused (F2)" : "playing (F2)");
}
inline void keyStep(int ticks) {
    g_s.paused = true;
    g_s.clock.hold = 0.0;
    g_s.clock.tick += (double)ticks;
    clampTick();
}
inline void keySeek(long long ticks) {
    g_s.clock.hold = 0.0;
    g_s.clock.tick += (double)ticks;
    clampTick();
    g_s.cam.on = false;
}
inline void keySpeed(int dir) {
    initSpeed();
    g_s.speedIdx = std::max(0, std::min(WATCH_SPEED_N - 1, g_s.speedIdx + dir));
    char b[48];
    snprintf(b, sizeof(b), "pace %gx", (double)WATCH_SPEEDS[g_s.speedIdx]);
    say(b);
}

// ------------------------------------------------------------------ the mouse
//
// The panel's own clock can be put anywhere in the stretch with a click or a drag on its bar: the playhead goes to where the
// pointer is, the picture follows it, and the replay goes on from there (or stays stopped, if F2 had stopped it). It is the
// panel's clock and nothing else: the level behind a solve is the loop's own verification replay, and a press never reaches it --
// a press is claimed only while the panel is on screen with a path to play, only on the bar and the strip of picture just above it,
// and every other click is left to whatever would have had it (see PanelTouch). Nothing the solver reads is written.
//
// The bar is drawn along the bottom edge of the panel, 3 units thick (replay_panel_geom.hpp, buildFrame): thin enough that a press
// has to be forgiven, so the strip that takes one is the bottom quarter of the panel, the whole width of it.
constexpr float kBarBandFrac = 0.25f;
constexpr float kBarInset = 3.f;                // the bar's own margin at either end (buildFrame)

inline bool onBar(float x, float y) {
    if (!g_s.boxValid || !g_s.owning) return false;
    const cocos2d::CCRect& b = g_s.box;
    const float slack = 4.f;
    return x >= b.getMinX() - slack && x <= b.getMaxX() + slack && y >= b.getMinY() - slack
        && y <= b.getMinY() + b.size.height * kBarBandFrac;
}

// Where along the stretch a pointer x is, 0..1 (clamped: a drag may leave the panel sideways and keeps its end).
inline float barFrac(float x) {
    const float x0 = g_s.box.getMinX() + kBarInset, x1 = g_s.box.getMaxX() - kBarInset;
    const float f = x1 > x0 ? (x - x0) / (x1 - x0) : 0.f;
    return std::max(0.f, std::min(1.f, f));
}

// The tick at fraction f of the stretch, and the clock put there. The camera snaps (a jump is not an ease).
inline long long seekToFrac(float f) {
    if (!g_s.track) return -1;
    long long a, z;
    stretch(*g_s.track, a, z);
    g_s.clock.hold = 0.0;
    g_s.clock.tick = (double)a + (double)f * (double)(z - a);
    clampTick();
    g_s.cam.on = false;
    return (long long)std::floor(g_s.clock.tick);
}

// A press: taken only on the bar. The clock stops under the finger and the playhead goes to it at once; the answer says whether
// the press was claimed (false leaves it to the game).
inline bool pressAt(float x, float y) {
    if (!onBar(x, y) || !g_s.track) return false;
    g_s.dragging = true;
    g_s.dragWasPaused = g_s.paused;
    g_s.paused = true;
    const long long t = seekToFrac(barFrac(x));
    if (++g_s.dragEvents <= 5) {
        char b[96];
        snprintf(b, sizeof(b), "mouse: press on the bar at %.0f%% -> tick %lld", (double)(barFrac(x) * 100.f), t);
        say(b);
    }
    return true;
}
inline void dragTo(float x) {
    if (g_s.dragging) seekToFrac(barFrac(x));
}
// The finger is up (or was never down): the clock goes on from where it was put, unless F2 had stopped it.
inline void letGo(float x, bool cancelled) {
    if (!g_s.dragging) return;
    long long t = (long long)std::floor(g_s.clock.tick);
    if (!cancelled) t = seekToFrac(barFrac(x));
    g_s.dragging = false;
    g_s.paused = g_s.dragWasPaused;
    g_s.last = std::chrono::steady_clock::now();           // the held time is not played catching up
    if (g_s.dragEvents <= 5) {
        char b[96];
        snprintf(b, sizeof(b), "mouse: %s at tick %lld (%s)", cancelled ? "cancelled" : "released", t,
                 g_s.paused ? "stopped" : "playing");
        say(b);
    }
}

// cfg `panelmouse`: the test hook's presses, one entry at a time, counted from the first frame the panel can take one. A click is a press and
// a release; a drag moves over 8 frames and releases at its end. Every call is a frame of the panel.
inline void autoMouse() {
    const auto& v = itermap::g_panelMouseAuto;
    static size_t idx = 0;
    static int dragFrame = -1;
    static bool armed = false;
    static std::chrono::steady_clock::time_point t0;
    if (v.empty() || !g_s.boxValid || !g_s.owning) {
        armed = false;
        return;
    }
    if (!armed) { armed = true; idx = 0; dragFrame = -1; t0 = std::chrono::steady_clock::now(); }
    if (idx * 3 >= v.size()) return;
    const double s = v[idx * 3], f0 = v[idx * 3 + 1], f1 = v[idx * 3 + 2];
    const float w = g_s.box.size.width - 2.f * kBarInset;
    auto X = [&](double f) { return g_s.box.getMinX() + kBarInset + (float)f * w; };
    const float y = g_s.box.getMinY() + 2.f;
    if (dragFrame < 0) {
        if (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < s) return;
        if (!pressAt(X(f0), y)) { ++idx; return; }
        if (f1 < 0.0) { letGo(X(f0), false); ++idx; return; }
        dragFrame = 0;
        return;
    }
    ++dragFrame;
    const double f = f0 + (f1 - f0) * std::min(1.0, dragFrame / 8.0);
    dragTo(X(f));
    if (dragFrame >= 8) { letGo(X(f1), false); dragFrame = -1; ++idx; }
}

// The touch target. A targeted delegate far below anything the game uses (itermap's seek bar sits at -0x7000 for the same reason:
// -500 lost to the end-level screen), which swallows a press ONLY when it lands on the bar; returning false from ccTouchBegan
// leaves the touch to whatever would have had it, so a click anywhere else changes nothing about the game's input.
class PanelTouch : public cocos2d::CCLayer {
public:
    static PanelTouch* create() {
        auto* r = new PanelTouch();
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
        cocos2d::CCDirector::sharedDirector()->getTouchDispatcher()->addTargetedDelegate(this, -0x7001, true);
    }
    bool ccTouchBegan(cocos2d::CCTouch* t, cocos2d::CCEvent*) override {
        if (!t) return false;
        const cocos2d::CCPoint p = t->getLocation();
        return pressAt(p.x, p.y);
    }
    void ccTouchMoved(cocos2d::CCTouch* t, cocos2d::CCEvent*) override {
        if (t) dragTo(t->getLocation().x);
    }
    void ccTouchEnded(cocos2d::CCTouch* t, cocos2d::CCEvent*) override {
        letGo(t ? t->getLocation().x : g_s.box.getMinX(), t == nullptr);
    }
    void ccTouchCancelled(cocos2d::CCTouch*, cocos2d::CCEvent*) override { letGo(0.f, true); }
};

// ------------------------------------------------------------------ one frame

// A CCDrawNode blends with (GL_ONE, GL_ONE_MINUS_SRC_ALPHA), the game's premultiplied-alpha default, so a colour handed to
// it must already be multiplied by its alpha: a plain (1, 1, 1, 0.12) adds a full white and the faint bar under the path
// came out solid white (seen in the game, 2026-10-17). Black and fully opaque colours were already right.
inline cocos2d::ccColor4F premul(const pg::Color& c) {
    return cocos2d::ccColor4F{c.r * c.a, c.g * c.a, c.b * c.a, c.a};
}

inline void emit(cocos2d::CCDrawNode* n, const pg::Scene& sc, float ox, float oy) {
    using namespace cocos2d;
    static thread_local std::vector<CCPoint> v;
    n->clear();
    for (const pg::Prim& p : sc.prims) {
        const float* q = sc.pts.data() + p.off;
        switch (p.kind) {
            case pg::Prim::Poly: {
                v.resize((size_t)p.n);
                for (int i = 0; i < p.n; ++i) v[(size_t)i] = CCPoint(ox + q[2 * i], oy + q[2 * i + 1]);
                n->drawPolygon(v.data(), (unsigned)p.n, premul(p.fill), p.lw, premul(p.line));
                break;
            }
            case pg::Prim::Seg:
                n->drawSegment(CCPoint(ox + q[0], oy + q[1]), CCPoint(ox + q[2], oy + q[3]), p.lw, premul(p.line));
                break;
            case pg::Prim::Dot:
                n->drawDot(CCPoint(ox + q[0], oy + q[1]), p.lw, premul(p.line));
                break;
        }
    }
}

inline cocos2d::CCLabelBMFont* label(cocos2d::CCNode* parent) {
    using namespace cocos2d;
    auto* lbl = static_cast<CCLabelBMFont*>(parent->getChildByTag(LABEL_TAG));
    if (!lbl) {
        lbl = CCLabelBMFont::create("", "chatFont.fnt");
        if (!lbl) return nullptr;
        lbl->setTag(LABEL_TAG);
        lbl->setID("replay-panel-label"_spr);
        lbl->setAnchorPoint({0.f, 1.f});
        lbl->setScale(0.34f);
        lbl->setZOrder((1 << 20) + 3);
        parent->addChild(lbl);
    }
    return lbl;
}

// Hidden is emptied as well: a node that keeps its drawing in the scene shows wherever its visibility is not honoured. The panel's own state
// (g_s) is kept, and the picture is drawn afresh every frame it is shown, so emptying costs nothing.
inline void hide(cocos2d::CCNode* parent) {
    if (auto* n = parent->getChildByTag(PANEL_TAG)) {
        n->setVisible(false);
        static_cast<cocos2d::CCDrawNode*>(n)->clear();
    }
    if (auto* l = parent->getChildByTag(LABEL_TAG)) {
        l->setVisible(false);
        static_cast<cocos2d::CCLabelBMFont*>(l)->setString("");
    }
}

// Called once per frame from the overlay pass. `show` is the overlay pass's own (F1 hides it with the rest); `textTags`
// the overlay's own text lines, which the panel keeps clear of.
//
// On screen only while a solve runs (showingSolve(): a candidate replay in the loop is still the solve, the showing of the
// solution is not) AND the screen is dark: with the screen on, the game is what is watched and the keys are its, so the
// panel is hidden -- not emptied: its state is kept and it comes back as it was. Both ends of a solve empty it, so a level's
// picture is never what the next level starts with.
inline void update(cocos2d::CCNode* parent, bool show, std::initializer_list<int> textTags) {
    using namespace cocos2d;
    if (!parent) return;
    const auto tNow = std::chrono::steady_clock::now();
    const double gapMs = std::chrono::duration<double, std::milli>(tNow - g_s.lastFrame).count();
    g_s.lastFrame = tNow;
    const bool solving = showingSolve();
    if (solving != g_s.wasSolving) {
        g_s.wasSolving = solving;
        retire();
    }
    // A suite goes from one level to the next without ever leaving the solve: there the level the session is asked for
    // changing is the edge, and the picture is emptied the same way.
    if (solving && g_s.levelId != g_cfg.levelId) {
        const bool changed = g_s.levelId != -1;
        if (changed) retire();
        g_s.levelId = g_cfg.levelId;
    }
    // The model's plans are read whenever a solve runs, the panel on screen or not: the level bar shows how far the deepest gets
    if (solving) pollPlan();
    if (!solving || !show || !wanted() || renderingOn()) {
        g_s.owning = false;
        g_s.boxValid = false;
        if (g_s.dragging) letGo(0.f, true);                       // the panel went away under the finger
        if (g_s.visible) { hide(parent); g_s.visible = false; }     // off costs this test, not two tree lookups
        return;
    }
    if (!g_s.visible) {                          // back from hidden (the screen went off again, or the key): no jump
        g_s.last = tNow;
        g_s.cam.on = false;
    }
    g_s.visible = true;
    g_s.owning = (bool)g_s.track;
    // A busy game (a long gap since the last frame): hold the picture, say so, and let the clock stand still so that it does
    // not jump when the frames come back. The first frame of a solve is drawn whatever the gap, and so is any frame on a scene
    // that has none of the panel's nodes yet: the scene is replaced when the level is (and the slice swaps it), and a held frame
    // there would leave nothing on screen for as long as the game stays busy.
    ++g_s.statFrames;
    g_s.statGapSum += gapMs;
    g_s.statGapMax = std::max(g_s.statGapMax, gapMs);
    if (gapMs > 40.0) ++g_s.statOver40;
    if (std::chrono::duration<double>(tNow - g_s.statSince).count() >= kStatEverySec) {
        char b[260];
        snprintf(b, sizeof(b), "last %.0f s: %lld frames, %lld held while the game was busy (gap > %.0f ms), "
                 "%lld of them over 40 ms, mean gap %.1f ms, longest %.0f ms (budget %.0f ms); plans read %lld, %zu in the ring",
                 std::chrono::duration<double>(tNow - g_s.statSince).count(), g_s.statFrames, g_s.statHeld, busyMs(),
                 g_s.statOver40, g_s.statFrames > 0 ? g_s.statGapSum / (double)g_s.statFrames : 0.0, g_s.statGapMax, g_cfg.frameBudgetMs,
                 g_s.plansRead, g_s.ring.size());
        say(b);
        g_s.statSince = tNow;
        g_s.statFrames = g_s.statHeld = g_s.statOver40 = 0;
        g_s.statGapSum = g_s.statGapMax = 0.0;
    }
    if (gapMs > busyMs() && g_s.drawn && parent->getChildByTag(PANEL_TAG) && parent->getChildByTag(LABEL_TAG)) {
        ++g_s.runSkipped;
        ++g_s.statHeld;
        g_s.last = tNow;
        if (!g_s.held) {
            g_s.held = true;
            if (auto* l = static_cast<CCLabelBMFont*>(parent->getChildByTag(LABEL_TAG))) {
                const std::string held = g_s.lastText + "  [held: the game is busy]";
                l->setString(held.c_str());
            }
        }
        return;
    }
    g_s.held = false;
    const auto c0 = std::chrono::steady_clock::now();
    refresh();

    auto* n = itermap::drawNode(parent, PANEL_TAG, "replay-panel"_spr, (1 << 20) + 2);
    auto* lbl = label(parent);
    if (!n || !lbl) return;
    n->setVisible(true);
    lbl->setVisible(true);

    const CCRect box = panelBox(parent, textTags);
    g_s.box = box;
    g_s.boxValid = true;
    // The touch target for the bar, attached to the scene the panel is drawn into and only when absent (a per-frame add would
    // register the delegate again each time); the scene is replaced with the level, and it comes back with the next frame.
    if (!parent->getChildByTag(TOUCH_TAG)) {
        if (auto* tl = PanelTouch::create()) {
            tl->setTag(TOUCH_TAG);
            tl->setID("replay-panel-touch"_spr);
            parent->addChild(tl, (1 << 20) + 4);
        }
    }
    static pg::Scene S;                          // the buffers are kept from frame to frame
    std::string text;
    const pg::Track* tr = g_s.track.get();
    if (!tr || tr->last < 0 || !levelCurrent()) {
        S.reset(pg::Box{0.f, 0.f, box.size.width, box.size.height});
        S.rect(0.f, 0.f, box.size.width, box.size.height, pg::Color{0.f, 0.f, 0.f, 0.72f});
        text = !levelCurrent() ? "replay panel: reading the level..."
                               : "replay panel: waiting for a verified path";
    } else {
        advanceClock(*tr);
        tr = g_s.track.get();                    // (a newer plan may have replaced the path when the clock started over)
        clampTick();
        pg::FrameIn in;
        in.level = g_s.level.get();
        in.movers = g_s.movers.get();
        in.moverOf = &g_s.moverOf;
        in.moversOfThisAttempt = g_s.movers && g_s.moversSerial == tr->serial;
        in.track = tr;
        in.marks = &g_s.marks;
        if (!g_s.ring.empty()) {                                  // the ring of plans beside the path, the deepest being the path
            in.ghosts = &g_s.ring;
            in.focus = g_s.planShown.get();
        }
        stretch(*tr, in.a, in.z);
        in.t = std::max(in.a, std::min(in.z, (long long)std::floor(g_s.clock.tick)));
        in.W = box.size.width;
        in.H = box.size.height;
        text = pg::buildFrame(S, in, g_s.cam);
        initSpeed();
        char st[64];
        snprintf(st, sizeof(st), "  %s %gx%s", modeName(), (double)WATCH_SPEEDS[g_s.speedIdx],
                 g_s.paused ? " [paused]" : "");
        text += st;
    }
    autoMouse();                                 // cfg panelmouse (a test hook; nothing without it)
    emit(n, S, box.origin.x, box.origin.y);
    g_s.drawn = true;
    g_s.lastText = text;
    if (text != lbl->getString()) lbl->setString(text.c_str());
    lbl->setPosition({box.origin.x + 3.f, box.origin.y + box.size.height - 2.f});

    // what a frame costs, now and then (the search's overlay timer counts it too)
    ++g_s.frames;
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - c0).count();
    g_s.usSum += us;
    ++g_s.runFrames;
    g_s.runUs += us;
    if (g_s.frames % 2000 == 0) {
        char b[160];
        snprintf(b, sizeof(b), "%lld frames, %.0f us a frame on average, %zu primitives in the last",
                 g_s.frames, g_s.usSum / 2000.0, S.prims.size());
        say(b);
        g_s.usSum = 0.0;
    }
}

// Tab: the whole path -> its last stretch -> off -> the whole path.
inline void toggle() {
    static const int order[] = {Full, Recent, Off};
    int i = 0;
    while (i < 2 && order[i] != mode()) ++i;
    g_modePref = order[(i + 1) % 3];
    if (wanted()) rebuildShown(true);   // the picture starts over
    say(modeName());
}

// How far in x the model's deepest plan gets, or -1 when there is none (or it ends in a turned gameplay frame, where x is not
// the way along the level): the level bar draws the part of it past the verified point in the plan's colour.
inline double unconfirmedX() {
    const auto g = deepestPlan();
    if (!g || g->f.empty() || g->f.back().gframe != 0) return -1.0;
    return (double)g->endX();
}

}  // namespace replaypanel
}  // namespace p1
