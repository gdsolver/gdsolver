#pragma once
#include <chrono>
// Stage C: the repair loop, inside the game.
//
// The loop is a loop around two things the mod already has: solving (dp/, linked in since
// Stage B) and replaying (a session). It began as a port of the Python driver and is now the
// only copy -- the state a re-anchor needs is read off PlayerObject while the replay runs,
// instead of being parsed back out of dump.csv afterwards.
//
//   solve the whole level  ->  replay it in GD  ->  it dies at tick dt
//                                                       |
//          splice the tail onto the verified prefix  <--+  re-anchor on GD's REAL state
//                                                          some ticks before the death and
//                                                          solve the tail from there
//
// The model is wrong somewhere -- that is the premise of the whole project. What makes the loop
// work is that GD is the authority on where it is wrong: the prefix that GD actually replayed
// is true by construction, so every iteration keeps it and only re-solves what comes after.
//
// This loop clears all 22 cold on its own (py/cold_regress.py, 2026-08-27), so the list that
// used to stand here -- fixups, capacity tiers, needtrig, phantom vetoes, dead bands, moving-
// geometry harvesting, sprite rotation -- is spent; all of it is below, and two rungs the driver
// never had are as well (the unseen-door retry and the forced mode-portal crossing).
//
// WHAT IS STILL ONLY IN THE DRIVER, and why none of it is load-bearing here:
//   * section rescue as an automatic rung. `secsolve` exists (src/solver/secsolve.hpp) and can be
//     started by hand from poll(), but it is a one-way handoff: it never splices its result back
//     and never resumes the loop. The driver escalates entry points and re-solves the previous
//     section on its own. No level in the suite has needed it in-process.
//   * reversible segment boundaries and prefix cutting on frontier width (the driver reads
//     --bands, which this loop does not even ask leveldp for).
//   * lookahead DOUBLING. This loop runs the inverse -- the whole level, shortened to
//     kHorizonShort under stall -- which covers the same ground from the other end.
#include "mod/session.hpp"

namespace p1 {

// ============================================================
// The anchor record
//
// One row per physics tick of the current attempt, holding exactly what `--start` needs to
// resume the model from GD's own state. The driver reads these out of dump.csv; here they are
// taken from the player as the tick ends, which is the same instant the dump row is written
// (and the same reason it is written there: the state has to be settled).
//
// Indexed by tick, not a ring: the ladder backs off up to 28,800 ticks and the driver reads the
// whole attempt out of the dump, so anything shorter would silently make the deep rungs
// unavailable. A full level is ~30,000 rows -- under 3 MB.
// ============================================================
struct AnchorRow {
    bool valid = false;
    float x = 0.f, y = 0.f, vy = 0.f;
    int mode = 0;            // modeIdx() -- the ordering leveldp uses
    int onGround = 0, onGround2 = 0;
    int flip = 0;            // m_isUpsideDown
    int mini = 0;            // vehicle size < 0.9 (a size portal has been passed)
    int dual = 0;
    float y2 = 0.f, v2 = 0.f;
    // g2 / g2b are the SECOND body's two contact flags, kept raw exactly like p1's pair above
    // and filtered by groundedOf2() where the anchor is built.
    int f2 = 0, g2 = 0, g2b = 0;
    // ...and its own MODE and SIZE. The pair used to be anchored with one of each, copied from
    // p1, on the grounds that the halves cannot differ for more than the tick between them
    // reaching the same portal -- measured false on the rig `dualmode` (thousands of ticks
    // apart), true of the official corpus. -1 = "not a dual", which is what the solver reads as
    // "say nothing and keep the copy".
    int m2 = -1, mini2 = -1;
    // A dash is held for hundreds of ticks (lv21 holds one for 300+), and an anchor taken inside
    // one that resumes without it restarts in free fall through the whole thing. The driver has
    // to infer both of these from a 420-tick window of its dump; in the game they are just there
    // to be read.
    int dashing = 0;
    float dashSlope = 0.f;   // dy per px of x, from the ring's own rotation (0 for an upright one)
    // The player's SPRITE angle. State::rot is the only angle the turned-box test uses, and an
    // anchor without it re-accumulates from zero -- which mid-section is tens of degrees away
    // from GD and flips that test's verdict outright.
    float rot = 0.f;
    float speed = 0.f;       // GD's own speed multiplier, so the model need not re-derive it
    int gframe = 0;          // rotated gameplay (0/1/2/3 = 0/90/180/270)
    int snapUid = -1;        // m_objectSnappedTo's uid, and how far it snapped
    float snapDist = 0.f;
    float pmin = 0.f, pmax = 0.f;   // GD's flight band (getMinPortalY / getMaxPortalY)
    // GD's velocity-limit exemption, byte [player+0x952] (set by the slope
    // machinery and the RED ring/pad, cleared when vy re-enters the band;
    // dp/state.hpp State::boost has the disassembly). While set, updateJump
    // skips the terminal clamp, so an anchor without it re-clamps the swing at
    // 8 while GD keeps accelerating.
    int boost = 0;
};

// GD's MAX GAMEPLAY Y (layer+0x36a8), refreshed every recorded tick and passed
// to the solver as --maxplayy. 0 = not read yet (the flag is then withheld).
inline float g_maxPlayYLive = 0.f;

namespace anchors {

// Three buffers.
//
// `live` is the attempt being recorded and `dead` is the one that just ended -- two, not one,
// because GD resets the level about a second after the death and starts refilling the live one
// while the solver thread is still reading the dead one. Swapping costs nothing and removes the
// question entirely.
//
// `deepest` belongs with the deepest plan the loop has kept. When an iteration regresses, the
// plan is rewound to that one and the ladder has to re-anchor on ITS trajectory: a tick number
// means nothing on its own, and the state the dying attempt held at that tick is a state the
// restored plan never passes through. Anchoring there sends the search after a route that does
// not exist.
inline std::vector<AnchorRow> g_live;
inline std::vector<AnchorRow> g_dead;
inline std::vector<AnchorRow> g_deepest;
// Which of the two finished attempts the ladder is reading. Set on the main thread before the
// solver thread starts, never while it runs.
inline const std::vector<AnchorRow>* g_src = &g_dead;

inline void onAttemptStart() { g_live.clear(); }
inline void reset() {
    g_live.clear();
    g_dead.clear();
    g_deepest.clear();
    g_src = &g_dead;
}

// Hand the finished attempt to the solver side.
inline void bank() { g_dead.swap(g_live); g_live.clear(); }

// This attempt got further than any before it: keep its trajectory with its plan.
inline void keepAsDeepest() { g_deepest = g_dead; }

inline void ladderOn(bool useDeepest) { g_src = useDeepest ? &g_deepest : &g_dead; }

inline const AnchorRow* row(long long t) {
    const std::vector<AnchorRow>& v = *g_src;
    if (t < 0 || (size_t)t >= v.size()) return nullptr;
    const AnchorRow& r = v[(size_t)t];
    return r.valid ? &r : nullptr;
}

// How many ticks the current source holds (for walks over the whole recording).
inline long long depth() { return (long long)g_src->size(); }

// Called at the end of every physics tick while an in-process solve session is open. Kept off
// the dump's `noTrace` switch on purpose: this is not a diagnostic, it is what the next
// iteration re-anchors on.
inline void record(GJBaseGameLayer* l, long long t) {
    if (!l || !l->m_player1 || t < 0) return;
    if (t > 400000) return;    // a runaway attempt must not eat memory instead of ending
    if ((size_t)t >= g_live.size()) g_live.resize((size_t)t + 1);
    AnchorRow& r = g_live[(size_t)t];
    auto* p = l->m_player1;
    const auto pos = p->getPosition();
    r.valid = true;
    r.x = pos.x;
    r.y = pos.y;
    r.vy = (float)p->m_yVelocity;
    r.mode = modeIdx(p);
    r.onGround = p->m_isOnGround ? 1 : 0;
    r.onGround2 = p->m_isOnGround2 ? 1 : 0;
    r.flip = p->m_isUpsideDown ? 1 : 0;
    r.mini = (p->m_vehicleSize < 0.9f) ? 1 : 0;
    r.dual = l->m_gameState.m_isDualMode ? 1 : 0;
    r.y2 = (r.dual && l->m_player2) ? l->m_player2->getPositionY() : 0.f;
    r.v2 = (r.dual && l->m_player2) ? (float)l->m_player2->m_yVelocity : 0.f;
    r.f2 = (r.dual && l->m_player2 && l->m_player2->m_isUpsideDown) ? 1 : 0;
    r.g2 = (r.dual && l->m_player2 && l->m_player2->m_isOnGround) ? 1 : 0;
    r.g2b = (r.dual && l->m_player2 && l->m_player2->m_isOnGround2) ? 1 : 0;
    r.m2 = (r.dual && l->m_player2) ? modeIdx(l->m_player2) : -1;
    r.mini2 = (r.dual && l->m_player2)
                  ? ((l->m_player2->m_vehicleSize < 0.9f) ? 1 : 0) : -1;
    r.dashing = p->m_isDashing ? 1 : 0;
    // dp/step.hpp builds this from the ring object's rotation in degrees, so take it from the
    // ring GD says the player is riding rather than from m_dashAngle, whose units and meaning
    // would have to be established first.
    r.dashSlope = 0.f;
    if (r.dashing && p->m_dashRing) {
        const double rot = reinterpret_cast<GameObject*>(p->m_dashRing)->getRotation();
        r.dashSlope = (float)std::tan(-rot * 3.14159265358979 / 180.0);
    }
    r.rot = p->getRotation();   // the dump's `rot` column, from the same call
    r.speed = (float)p->m_playerSpeed;
    r.gframe = g_gameFrame;
    r.snapUid = p->m_objectSnappedTo ? p->m_objectSnappedTo->m_uniqueID : -1;
    r.snapDist = p->m_snapDistance;
    r.pmin = l->getMinPortalY();
    r.pmax = l->getMaxPortalY();
    // The velocity-limit exemption has no bindings name; the offset is the one
    // updateJump's clamp gate reads (0x38ca9f: cmp [player+0x952],0), pinned
    // to 2.2081 like every other raw offset here.
    r.boost = *(reinterpret_cast<uint8_t const*>(p) + 0x952) ? 1 : 0;
    // ...and GD's MAX GAMEPLAY Y, the world-y bound whose crossing (two ticks
    // running) is the environment kill with a NULL object. Written by
    // updateMaxGameplayY into layer+0x36a8; read live rather than re-deriving
    // the formula, because on dynamic-height levels it moves with the world.
    g_maxPlayYLive = *reinterpret_cast<float const*>(
        reinterpret_cast<char const*>(l) + 0x36a8);
}

}  // namespace anchors

// ============================================================
// ---- rotation triggers already consumed before an anchor (--spentrot) ----
//
// A 2900 is one-shot. lv22's maze (t~11,3xx-12,8xx) walks x=15,399..16,131 through frames
// 1/2/3 and consumes the six triggers there; when the SHIP later glides the pinned floor
// through the same x at t=14,3xx, GD ignores them -- but an anchored tail solve knows nothing
// of the maze, sees uid6337 at (16,005,609) six pixels from the ride, and rotates a world GD
// does not (the -7.8 carry fixups at x=16,003 were the model fighting its own phantom turn).
// The anchor recording holds the truth: any 2900 whose travel coordinate the recorded run
// crossed within the firing window BEFORE the anchor is dead by t0 -- it either fired there
// (a gframe change says where) or was already spent even earlier. Marking those spent is
// exactly what the driver's --spentrot carried, and the loop never wired it.
struct RotObj { int uid; double cx, cy; };
inline std::vector<RotObj> g_rotObjs;   // parsed from the level csv at session start

inline void loadRotObjs(const std::string& csv) {
    g_rotObjs.clear();
    std::istringstream in(csv);
    std::string line;
    std::getline(in, line);   // header: id,type,cx,cy,...,uid,...
    // Column positions follow the objrects header (id first, cx/cy 3rd/4th, uid 8th).
    while (std::getline(in, line)) {
        if (line.rfind("2900,", 0) != 0) continue;
        RotObj o{};
        int col = 0;
        size_t p = 0;
        while (p <= line.size() && col < 9) {
            size_t q = line.find(',', p);
            if (q == std::string::npos) q = line.size();
            const std::string f = line.substr(p, q - p);
            if (col == 2) o.cx = std::atof(f.c_str());
            else if (col == 3) o.cy = std::atof(f.c_str());
            else if (col == 7) o.uid = std::atoi(f.c_str());
            p = q + 1;
            ++col;
        }
        if (o.uid > 0) g_rotObjs.push_back(o);
    }
}

// The frame maps of dp/frames.hpp, reduced to what the scan needs.
inline void rotUV(int f, double X, double Y, double& u, double& v) {
    switch (f & 3) {
        case 0:  u = X;  v = Y;  break;
        case 1:  u = -Y; v = X;  break;
        case 2:  u = -X; v = -Y; break;
        default: u = Y;  v = -X; break;
    }
}

inline std::string spentRotArg(long long t0) {
    if (g_rotObjs.empty()) return "";
    std::set<int> spent;
    const AnchorRow* prev = nullptr;
    double puPrev = 0.0;
    int pf = 0;
    for (long long t = 1; t < t0; ++t) {
        const AnchorRow* r = anchors::row(t);
        if (!r) continue;
        if (prev && r->gframe == pf) {
            double u1, v1;
            rotUV(pf, (double)r->x, (double)r->y, u1, v1);
            for (const RotObj& o : g_rotObjs) {
                if (spent.count(o.uid)) continue;
                double ut, vt;
                rotUV(pf, o.cx, o.cy, ut, vt);
                const bool crossed = (puPrev < ut && u1 >= ut)
                                     || (puPrev > ut && u1 <= ut);
                // 150 = kRotPerpWin. Over-marking is the risk to watch: a trigger
                // crossed inside the window before t0 that GD in fact never fires
                // and that a LATER rung needs would be wrongly dead -- no such
                // trigger exists in the corpus (the shaft's 11342 is first crossed
                // at its own firing), and under-marking is the measured disease.
                if (crossed && std::fabs(v1 - vt) <= 150.0) spent.insert(o.uid);
            }
        }
        double u0, v0;
        rotUV((int)r->gframe, (double)r->x, (double)r->y, u0, v0);
        puPrev = u0;
        pf = r->gframe;
        prev = r;
    }
    std::string s;
    for (int uid : spent) {
        if (!s.empty()) s += ",";
        s += std::to_string(uid);
    }
    return s;
}


// The in-process solve loop (`dpsolve`)
// ============================================================
namespace dpsolve {

// Where the search resumes from, relative to the death. 6 ticks is the driver's default: close
// enough that the prefix keeps almost everything GD verified, and the ladder below goes deeper
// whenever that is not enough.
constexpr int kBackOff = 6;
// The rungs the ladder walks when the shallow anchor is doomed. Same list as the driver's.
constexpr int kRungs[] = {24, 96, 240, 600, 1500, 3600, 7200, 14400, 28800};
// Same three knobs the driver passes on every call. They dominate the run time (the flying
// layers saturate), and the values are the ones the cold regression is measured with -- the
// core's own defaults are coarser and larger, so leaving them out would make the mod's solve a
// different search from the driver's.
constexpr long long kCap = 2000;
constexpr double kYq = 0.25, kVq = 1.0;
constexpr const char* kThreads = "8";
// More states per layer, on a coarser grid, for a search that has run out of room. Not
// redundant with the base setting: deduplication keeps one representative per bin, so a coarse
// bin can hold a concrete state a fine grid threw away, and neither set contains the other.
//
// [2026-08-23] Measured and rejected as an ALWAYS-ON ladder -- it cost 4-20x and changed no
// outcome, because at the time every wall the loop met was a missing world rather than a
// missing state (the numbers are in the plan log). Re-asked once fixups, the trigger map and
// the recordings existed, and kept as one of the things escalate() tries when the ladder has
// nothing left: there it costs only the levels that have already failed without it.
constexpr long long kCapTiers[] = {16000, 40000};
constexpr double kTierYq = 0.5, kTierVq = 2.5;
inline int g_capTier = 0;    // 0 = the base setting
// MEASURED AND REJECTED (2026-08-23): the driver retries a doomed anchor with more capacity on
// a coarser grid (16,000 then 40,000 states, yq 0.5 / vq 2.5), and that ladder is what carries
// lv16 there. Ported here it cost 4-20x and changed no outcome:
//
//   lv16   without 41 iterations, deepest t=7,579 in 576s | with 10 iterations, SAME t=7,579,
//                                                           out of wall clock at 1,200s
//   lv20   without 41 iterations, deepest t=3,953 in 325s | with  9 iterations, SAME t=3,953,
//                                                           out of wall clock at 1,200s
//   lv21   without  6 iterations in 43s                   | with  6 iterations in 865s
//
// The capacity WAS binding (capHits > 0) and the verdict still did not move: at these anchors
// the tail is already PARTIAL at t=13,936 while GD dies at t=1,686, so the wall is fidelity,
// not capacity, and no amount of search buys past it. The number is still reported on every
// anchor line below, so whether capacity has become the wall stays visible -- worth asking
// again once fixups exist, because the driver's evidence for the tiers comes from a loop that
// has them.

// ---- the job in flight ----
inline std::atomic<bool> g_running{false};   // a solve has been started and not yet collected
inline std::atomic<bool> g_finished{false};  // the worker thread has set g_rc
// Bumped once per session by start() (never by spawn() -- a session's own ladder rounds share
// one generation). spawn() hands the value it read to the worker thread, which reports it back
// alongside g_finished; poll() only installs a result whose generation still matches. A worker
// is `.detach()`ed and cannot be cancelled (see spawn()), so an ESC-quit mid-solve does not stop
// it -- without this it would report into whatever session is live when it finally finishes.
// Measured (2026-08-24): the orphaned thread's late completion overwrote g_cfg.inputs, force-
// reset whatever level was current, and fired a "solved" Notification whose sound was not muted
// (audio::silent() depends on the NOW-live session's state, which by then had nothing to do with
// the solve that actually finished) -- this is the mechanism behind both "state carries over
// after repeated ESC-quits" and "sound leaks during Solve" reports.
inline std::atomic<int> g_generation{0};
inline int g_resultGeneration = -1;          // set by the worker before g_finished, like g_rc
// A start() arrived while g_running was still held by an earlier session's orphaned worker.
// Starting a second thread there would race the first on g_csv/g_plan/g_planPath (plain globals,
// not per-job), so the request is queued here instead of dropped; poll() retries it once the
// slot frees, re-validated against the session that asked (see poll()).
inline GJBaseGameLayer* g_pendingLayer = nullptr;
inline int g_rc = -1;
inline std::string g_planPath;
inline std::string g_tailPath;
inline std::chrono::steady_clock::time_point g_t0;
inline std::string g_csv;          // the level, built once at session start
inline int g_horizon = 0;

// ---- what the loop knows between iterations ----
inline int g_iter = 0;
inline std::vector<InputCmd> g_plan;      // the plan currently installed
inline std::vector<InputCmd> g_best;      // the deepest plan GD has verified
inline long long g_bestDeath = -1;        // ...and how far it got
inline int g_curBackoff = kBackOff;
inline bool g_lastTailSolved = false;     // the tail spliced last time reached the end
inline int g_followSolved = 0;            // regressions followed on a solved branch
inline int g_followForced = 0;            // ...and on the forced (portal) route -- see the grace
inline long long g_lastDeath = -1;
inline float g_lastDeathX = 0.f;      // ...and where (the void-attempt repeat scoring)
// Deaths per tick bucket (dt/4). The installed deepest plan is restored and
// replayed between failed tails, and its own death never carried veto credit
// (the gate wants a fresh SOLVED tail), so the cycle restore -> replay ->
// same death -> failed tails -> restore ran forever: lv22's t=7,274 death
// repeated across six runs and 200+ iterations without its site ever earning
// a box. NOT a consecutive-run counter: the oscillation alternates between
// two death points (a few 7,274s, then a 6,004), so a consecutiveness
// requirement never fills. A tick bucket that has died 8 times IS refuted --
// the model believed the state before it was alive -- and counts as veto
// credit (checkPhantom), the same standing the wedge already has; the box
// dedupe and the widening ladder absorb the repeated credits.
inline std::map<long long, int> g_deathRuns;
// ---- the phantom veto (the driver's check_phantom) ----
//
// A route the MODEL believes in and the GAME kills at the same spot, over and over. The tail was
// SOLVED, so nothing is recordable from it -- the model is not disagreeing with the game about a
// transition, it is planning through a place the game does not allow at all -- and the loop has
// no way to stop proposing it. Measured on lv22's own 84% run, in its last 13 rounds:
//   iterations 201-204  death x=20,135.0, tail [SOLVED] every time
//   iterations 206-211  death x=17,426.6, tail [SOLVED] every time
// Both sites are one 8px bin, both well past the 4 hits the driver waits for, and the run spent
// those rounds re-deriving the same two dead ends before it ran out of anchors.
//
// The claim a veto makes is only "the game killed a plan the model believed in, here, N times",
// which is a disagreement the run OBSERVED rather than anything read off the level's structure --
// the same footing as a fixup, and it lives and dies with the run in the same way. Not the
// authored per-level bands: those say what a route is, this says where one was refuted.
//
// Not ported with it: the needtrig side effects (un-dropping boxes, pinning the ones just before
// the phantom), because needtrig itself is not in this loop.
constexpr int kPhantomAfter = 4;          // hits at one site before the box is dropped
constexpr double kPhantomDx = 4.0;        // veto box half-width (px)
constexpr double kPhantomDy = 10.0;       // ...and half-height
// A phantom family spread over height needs one box per height it is caught at, so the count is
// bounded here rather than by "one per site". Each box is two argv entries on every later solve;
// this is a runaway guard, not a budget anyone should hit -- lv22's worst site needs about six.
constexpr int kPhantomMaxBands = 64;
inline std::map<long long, int> g_phantomHits;    // site (x/8) -> deaths of a SOLVED tail
// site*16+mode -> how many times the SAME box came back. A wall whose death
// state repeats with identical numbers re-derives an identical band; each
// repeat doubles the box instead of being discarded (see checkPhantom).
inline std::map<long long, int> g_phantomScale;
// ...but only this far. The widening was capped at 5 (128x320 px) on the
// reasoning that the driver's hand-authored corridor band was that big; a
// hand-authored band is aimed, and this one is not. Measured on the runs that
// actually clear: lv22's certified cold run tops out at scale 2 and lv21 never
// leaves 0, while lv16 walked all the way to 5 and sealed itself in -- its
// x=21,865 site (the dual ship, where the known 0.0076 px/tick drift lives)
// grew a 256x640 px no-go zone and every rung then died at x=21,736, one pixel
// short of its left edge, for 100+ rounds. So the ladder stops where the
// evidence that it helps stops.
constexpr int kPhantomScaleMax = 2;
inline std::vector<std::string> g_phantomBands;   // --deadband strings, in the order found
inline bool g_phantomLifted = false;      // released once; never veto again after that
// A KILL-ONLY DEATH IS WORTH TWO ORDINARY ONES. When the recorder finds that the
// game killed the player where the model did not AND the transition physics
// agrees to within kNoopEps on both bodies, there is nothing else that death can
// teach: the model already moves the player exactly as the game does and only
// the kill test differs. That is precisely the veto's own claim -- "the model is
// not disagreeing about a transition, it is planning through a place the game
// does not allow at all" -- established in one round rather than inferred from
// four repeats, so the recorder adds a second hit and the box drops on the
// second round instead of the fourth.
//
// Measured on lv20's cold run (2026-08-29): three sites cost exactly four
// rounds each and every one of them is this shape --
//   [fixup] t=16250 x=24354.3 mode=3 in=0 dy=-1.440 dvy=0.000 kill=1 err 0.000/0.000
//   [fixup] t=16647 x=24994.8 mode=3 in=0 dy=-1.037 dvy=-0.086 kill=1 err -0.000/0.000
//   [fixup] t=16670 x=25031.9 mode=3 in=0 dy=0.733  dvy=-0.129 kill=1 err 0.000/-0.000
// -- all three a moving spike the player grazes by a fraction of a pixel
// (0.203, 0.253 and 0.540 px against the recording's own rects), which no
// margin can close: --dynhazpad kills lv19's and lv21's own solutions at 0.1 px.
// The veto is the mechanism that fits, and it was already firing at all three;
// what it was spending was the four rounds of evidence.
//
// ONE CREDIT PER ITERATION. A multi-tick divergence writes one record per tick,
// and the claim being made is about the DEATH, not about each record.
// (the test itself lives in writeFixup, where kNoopEps and the errors are)
inline int g_killVetoIter = -1;
inline bool g_stop = false;               // the loop has given up; do not start another job
// What the worker thread produced (read on the main thread after g_finished)
inline bool g_haveNewPlan = false;
inline long long g_anchorT = -1;
inline float g_anchorX = 0.f;
// A candidate has cleared and the session should turn into a showing of it. Raised from
// levelComplete, acted on at the next frame boundary (see onCleared / poll).
inline bool g_showRequest = false;
// A no-death pass over the level is running, purely to record the moving geometry to the end.
// A pass over the level whose only purpose is to record where the moving geometry goes.
// Two kinds, and the difference is what is being driven:
//   Bootstrap  no inputs at all, before any solving. The player runs along the ground and the
//              autonomous triggers fire as it crosses them, so the whole level gets recorded
//              once for free. This is what breaks the circle on a level whose first wall is
//              itself a moving part: you cannot record past a wall you cannot pass, and you
//              cannot pass it without the recording.
//   Deep       the deepest verified plan, later, to record what THAT route sets in motion.
enum RecordKind { RecNone = 0, RecBootstrap = 1, RecDeep = 2 };
inline int g_recordKind = RecNone;
inline bool g_recordRequest = false;
inline bool g_deepActive = false;
// Whether the attempt CURRENTLY on screen was started as a recording pass. Latched from
// g_deepActive when the attempt begins (resetLevel) and left alone until the next one starts.
//
// Not the same question as g_deepActive, and levelComplete has to ask this one. The recorder
// retires itself when its tick budget runs out (poll's "did not reach the end"), but that only
// stops the RECORDING -- the pass it started is still physically running, and with the fast loop
// the remainder of that single frame is thousands more ticks. Measured on level 1650666
// (2026-08-24): the budget retired the recorder at t=60,270, the no-input no-death run carried on
// to t=64,792, reached the end, and levelComplete -- seeing g_deepActive already false -- filed it
// as a genuine clear ("cleared after 0 repair rounds") and then "showed" it with the empty input
// list the bootstrap had left behind, which is why it died seconds into the replay.
inline bool g_recordAttempt = false;
inline long long g_deepDoneAt = -1;   // the depth the last one was taken at
inline long long g_deepStartTick = 0;
inline std::string g_groupsBootPath;
// Defined below, next to the deep record it shares its machinery with; start() needs it first.
inline bool startBootstrapRecord();
inline bool g_coldRestarted = false;  // the prefix has been thrown away once
inline int g_escalations = 0;         // how many times the ladder has been given another option

// Nine significant digits, the same precision the dump is written with. Coarser than that and
// the re-anchored x lands on the wrong side of a contact test near x=20,000.
inline std::string num(double v) {
    char b[40];
    snprintf(b, sizeof(b), "%.9g", v);
    return b;
}

inline std::string g_fixupPath;      // declared here; the recorder below fills it

// ---- what the loop has decided it needs, decided while running ----
//
// NOTHING HERE IS PER LEVEL. Every one of these is a switch the loop throws when it can see for
// itself that it needs to, because a table of "lv19 needs this, lv22 needs that" is a table
// somebody has to write for the twenty-third level, and there is no way to write it for a level
// nobody has played. What a level needs shows up in how the run is going, so that is where it is
// read off.
// Anchors that have already been tried and led nowhere. Without this the ladder is a pure
// function of (death tick, settings), so a rewind re-picks the same rung, re-derives the same
// tail, and the game re-plays it to the same death -- for the rest of the budget. Measured on
// lv16: iterations 21 through 25 were byte-identical, all anchored at t=379, all dying at
// t=3,576. Cleared whenever the run learns something, because then it is a different question.
inline std::unordered_set<long long> g_spentAnchors;
// Touch boxes the search is no longer required to enter. A box BEHIND the anchor cannot be
// entered by a tail that starts after it, so requiring one empties the frontier before the
// first tick -- `PARTIAL t=0`, which reads from outside as "this level is impassable" and is
// nothing of the kind. The solver reports which boxes it required and which the anchor is
// already past; this is that answer, fed back into the next call.
inline unsigned g_needTrigDropped = 0;
// ...and the ones merely under suspicion. Dropping a box the moment it looks unsatisfiable is
// measured-harmful: the backoff a demanded-but-unreachable box forces is exactly how lv19 ends
// up deep enough to buy the lift ride it needs, and the driver records that dropping on sight
// took lv19 from 16 rounds cleared to 48 and out. So a box is only released when the run has
// stopped getting anywhere without it -- the pressure first, the release later.
inline unsigned g_needTrigSuspect = 0;
inline int g_stallRuns = 0;          // iterations since the run last got deeper
inline bool g_needUnseen = false;    // go and touch doors whose effect has not been seen
inline int g_horizonFull = 0;        // a plan that covers the level
inline int g_horizonNow = 0;         // ...and what is being asked for right now

// How many fruitless iterations before each switch is thrown. Small: the cost of turning one on
// late is a few iterations, and the cost of having it on when it is not needed is every
// iteration of every level that does not need it (measured: the trigger detours cost lv18 four
// rounds out of five).
constexpr int kStallToUnseen = 2;
constexpr int kStallToShorten = 3;
// Capacity is escalated on a STALL as well as on a failed ladder, because they are different
// failures. A ladder that finds nothing has run out of anchors; a ladder that keeps finding them
// and never gets deeper has run out of room -- the anchors solve, the plans replay, and the run
// dies in the same place because the search cannot hold the states that would go round it.
// Later than the other two: it is by far the most expensive thing here.
constexpr int kStallToCap = 5;
// The bounded lookahead the loop falls back to. A plan that only reaches a little past the
// verified frontier is CHECKED IN THE GAME that much sooner -- and a replay is cheap next to a
// search, so when the model keeps being wrong the answer is to ask the game more often, not to
// spend longer being wrong.
constexpr int kHorizonShort = 3000;

// How far outside GD's own flight band still counts as being on the playfield. Legitimate play
// does leave the band -- verified runs clear its ceiling by up to 1,083 px in rotated sections
// and towers -- so the margin is wide enough not to touch any of that.
constexpr float kOffBoard = 1200.f;

// Where this run left the playfield, or -1 if it never did.
//
// GD does not always kill for it. An inverted cube that misses its landing is not killed -- a
// cube has no invisible ceiling -- so it keeps rising at terminal velocity while x goes on
// growing, and a run like that reads as the deepest one yet while being nowhere at all. Left
// alone, a loop that keeps the deepest plan will refine that arc forever: it is the best thing
// it has ever seen and it leads nowhere.
//
// The driver names the x ranges where this happens per level, by hand, one level at a time.
// This asks the state instead, which needs nothing written down about any particular level:
// the band is the game's own, and being a screen and a half outside it is not a route.
inline long long offBoardTick(long long upTo, float& xAt) {
    for (long long t = 1; t <= upTo; ++t) {
        const AnchorRow* r = anchors::row(t);
        if (!r || r->pmax <= r->pmin) continue;
        if (r->y < r->pmin - kOffBoard || r->y > r->pmax + kOffBoard) {
            xAt = r->x;
            return t;
        }
    }
    return -1;
}

// The rest of the level, beyond the collider table. buildPois already writes these next to
// objrects.txt whenever a solve session opens, so they cost nothing to pass -- and without them
// the model is planning against a different level from the one it is being replayed in:
//
//   triggers + objgroups   which walls are doors that have to be flown into. Without them a
//                          gate that only opens on contact is simply a wall, and the search
//                          spends the whole run proving there is no way through it.
//   obb                    the real corners of rotated hazards. Without it they are their
//                          bounding boxes, which is over-killing -- safe, but it closes gaps
//                          that are actually open.
//
// Both the search and the fixup resim take them: a recorder that replays in a different world
// from the search records divergences that the search never had.
// Where the moving geometry actually was, taken from the run's own replays (see harvestGroups).
inline std::string g_groupsPath;
inline long long g_groupsDepth = -1;
// ...and the same thing recorded in one run that could not die (see startDeepRecord). A live
// recording stops where the player died, and the model holds the last sample it saw forever --
// so a platform that was still moving at the wall reads as one that stopped there, and getting
// past the wall is the only way to record more of it. This breaks that circle.
inline std::string g_groupsDeepPath;

inline void addWorldArgs(std::vector<std::string>& a) {
    std::error_code ec;
    // ...and where the moving parts of it were. The static table holds their positions at level
    // entry only, so without this a sinking platform is a floor that never sinks and a level
    // built on them is planned in a world that does not exist.
    // Order is override order, weakest first. The no-death recordings are a different worldline
    // from the plan (nothing died in them), so they are only trustworthy where the real replays
    // have not reached; wherever one has, it wins. The input-free bootstrap is the weakest of
    // all -- it is what the level does when nobody plays it.
    if (g_cfg.dpGroups && !g_groupsBootPath.empty()
        && std::filesystem::exists(g_groupsBootPath, ec)) {
        a.push_back("--groups");
        a.push_back(g_groupsBootPath);
    }
    if (g_cfg.dpGroups && !g_groupsDeepPath.empty()
        && std::filesystem::exists(g_groupsDeepPath, ec)) {
        a.push_back("--groups");
        a.push_back(g_groupsDeepPath);
    }
    if (g_cfg.dpGroups && !g_groupsPath.empty()
        && std::filesystem::exists(g_groupsPath, ec)) {
        a.push_back("--groups");
        a.push_back(g_groupsPath);
    }
    // ...and where the CAMERA's flight band was (--bandtrack). The band floor is a
    // rideable surface: at lv22's shaft entrance the pan carries a ship up at
    // +1.44/tick with y = pmin(t)+13.5 pinned for 19 straight ticks, no object under
    // it -- grouptraceall tracked all 1.2M rows and found nothing moving there, the
    // hitbox trace showed zero collidedWithObject calls, only pmin moves. dp has
    // carried the seat law for exactly this stretch since r107 (bands.hpp: y =
    // pmin(t+1)+12.15, measured at x 20,000..20,090), but it engages only when
    // g_bandTrack is loaded, the driver fed that from its dump, and in-process
    // nothing did -- so the model got the anchor tick's band as a constant and sank
    // through the floor GD was standing on. The rows come from the same anchor
    // source the --start comes from, so a fixup pass (which retargets g_src to the
    // attempt that just died) writes that attempt's camera along with its anchor.
    {
        const std::string bp = std::string(DATA_DIR) + "/dp_band.txt";
        std::ofstream bf(bp, std::ios::trunc);
        float lf = -1e9f, lc = -1e9f;
        long long rows = 0;
        const long long n = anchors::depth();
        for (long long t = 1; t < n; ++t) {
            const AnchorRow* r = anchors::row(t);
            if (!r || r->pmax <= r->pmin) continue;
            // Only changes: the reader holds the last row's value until the next
            // (bandTrackAt), so flat stretches cost one row instead of thousands.
            if (r->pmin == lf && r->pmax == lc) continue;
            lf = r->pmin; lc = r->pmax;
            bf << t << ',' << r->pmin << ',' << r->pmax << '\n';
            ++rows;
        }
        bf.close();
        // cfg `dpbandtrack=0` withholds it. Passing the recorded band at all is
        // what 81f2a09 started doing in-process, and the bisect puts lv22's
        // second cold regression in that commit -- but NOT in the fly/bandcarry
        // branch it also added (81f2a09 with that branch gated still stops at
        // x=3,633). This switch is how the remaining half of the commit gets
        // measured on its own.
        if (rows > 0 && g_cfg.dpBandTrack) {
            a.push_back("--bandtrack");
            a.push_back(bp);
        }
    }
    if (!g_cfg.dpWorld) return;
    const std::string trig = std::string(DATA_DIR) + "/triggers.txt";
    const std::string grp = std::string(DATA_DIR) + "/objgroups.txt";
    const std::string obb = std::string(DATA_DIR) + "/obb.txt";
    if (std::filesystem::exists(trig, ec) && std::filesystem::exists(grp, ec)) {
        a.push_back("--triggers"); a.push_back(trig);
        a.push_back("--objgroups"); a.push_back(grp);
        // ...and, once the run has shown it needs to, make the search go and TOUCH a box whose
        // effect it has not seen. The loop always keeps the deepest plan, so a detour to open a
        // door looks like a loss until the recording of what it opened exists -- and only that
        // detour can buy the recording.
        //
        // Not from the start, because it is not free: it constrains the frontier, and on a level
        // whose walls are walls the search pays for boxes that lead nowhere. Measured on lv18,
        // which needs none of them: one round with it off, five with it on. Turned on when the
        // run stops getting deeper, which is the only evidence that a wall might be a door.
        if (g_needUnseen) a.push_back("--needtrig-unseen");
        // ...minus the ones this run has established cannot be entered from where it is.
        for (int b = 0; b < 32; ++b)
            if (g_needTrigDropped & (1u << b)) {
                a.push_back("--needtrig-skip");
                a.push_back(std::to_string(b));
            }
    }
    if (std::filesystem::exists(obb, ec)) { a.push_back("--obb"); a.push_back(obb); }
    // GD's MAX GAMEPLAY Y, read live off the layer (g_maxPlayYLive). Closes the
    // sky-escape phantom: without it the DP plans free climbs into y=3,600+
    // that GD environment-kills (dp/speed.hpp g_maxPlayY has the disassembly).
    if (g_maxPlayYLive > 0.f) {
        a.push_back("--maxplayy");
        a.push_back(num(g_maxPlayYLive));
    }
}

inline std::vector<std::string> baseArgs(const std::string& out) {
    const bool tiered = g_capTier > 0;
    std::vector<std::string> a{"--out", out,
                               "--horizon", std::to_string(g_horizonNow),
                               "--cap", std::to_string(tiered ? kCapTiers[g_capTier - 1] : kCap),
                               "--shipyq", num(tiered ? kTierYq : kYq),
                               "--shipvq", num(tiered ? kTierVq : kVq),
                               "--threads", kThreads};
    // Everything the run has learnt about where the model is wrong. Passed to every call, so a
    // gap measured in one iteration is already closed for the next one's first search.
    std::error_code ec;
    if (!g_fixupPath.empty() && std::filesystem::exists(g_fixupPath, ec)) {
        a.push_back("--fixups");
        a.push_back(g_fixupPath);
    }
    // ...and the places the game refuted outright (see checkPhantom). Same reason they go on
    // every call: a box learnt in one iteration must already be closed for the next one's first
    // search, or the ladder spends its rungs re-deriving the dead end that produced it.
    for (const std::string& band : g_phantomBands) {
        a.push_back("--deadband");
        a.push_back(band);
    }
    addWorldArgs(a);
    for (const std::string& s : g_cfg.dpArgs) a.push_back(s);
    // Say once what the solver is actually being told. Everything above is assembled from a
    // dozen switches and files, and when a run behaves unlike another the first question is
    // always which of them differed -- a question that cost a session to answer by inference
    // (2026-08-27: a cfg `dparg` was passed, believed delivered, and the A/B built on that
    // belief was wrong twice). One line, at the first solve of a session.
    {
        static bool s_logged = false;
        if (!s_logged) {
            s_logged = true;
            std::string line = "dpsolve: solver args:";
            for (const std::string& s : a) line += " " + s;
            writeResult(line);
        }
    }
    return a;
}

// Whether the anchor counts as standing. The flying modes keep m_isOnGround set while airborne
// (it is sticky), so there the flag alone is not enough -- the second contact flag and a still
// vertical velocity are what say "resting on something". Mirrors the driver's grounded_of.
inline int groundedOf(const AnchorRow& r) {
    const bool flying = (r.mode == 1 || r.mode == 3 || r.mode == 4);   // ship / ufo / wave
    if (flying)
        return (r.onGround && r.onGround2 && std::fabs(r.vy) < 0.01f) ? 1 : 0;
    return (r.onGround || r.vy == 0.f) ? 1 : 0;
}

// ...AND THE SAME FOR THE SECOND BODY. The stickiness is a property of the flag, not of which
// player owns it, but p2's was going into `--start` raw -- so an anchor taken while the dual's
// second half was flying with a stale m_isOnGround told the model "resting", and the model's
// resting branch restarts the velocity from zero.
// Measured on lv16 t=9,400 (dual ship, p2 flying with m_isOnGround still 1 and vy = +0.069): the
// model's next tick gave p2 0.086 -- one gravity step from rest -- where GD has 0.155 = 0.069 +
// one step. p1 then tracks GD to 0.0000 for the whole section while p2's error grows 0.0193 px a
// tick, which is what four of lv16's ten failing sections were.
inline int groundedOf2(const AnchorRow& r) {
    if (!r.dual) return 0;
    const bool flying = (r.mode == 1 || r.mode == 3 || r.mode == 4);
    if (flying)
        return (r.g2 && r.g2b && std::fabs(r.v2) < 0.01f) ? 1 : 0;
    return (r.g2 || r.v2 == 0.f) ? 1 : 0;
}

// How much of the robot's hover the anchor still has. GD keeps no counter for it, so it is read
// off the trajectory -- the way the driver reads it out of its dump, and for the same reason:
// while hovering, GD holds vy EXACTLY constant, so counting back over "airborne and the same vy"
// gives what has been spent, and the tick where that run breaks is the jump that armed it.
//
// Without this an anchor taken mid-hover starts with an empty budget and only the model falls,
// while GD flies level. Every tick of that is recorded as a divergence by a comparison that is
// itself an anchored resim -- measured by the driver on lv21's x=18,705 wall, where 45 of 63
// records were this, and all 45 then went back into the model as --fixups and overwrote correct
// physics next to the wall.
constexpr int kRobotHoverTicks = 67;   // measured 2026-08-10; see the driver's robot_hover_left

inline int robotHoverLeft(long long t0) {
    const AnchorRow* cur = anchors::row(t0);
    if (!cur || cur->mode != 5 || cur->onGround) return 0;   // grounded = GD has dropped it
    // Falling is not hovering: a robot at terminal velocity holds vy just as flat, and crediting
    // that invents a budget nobody armed. Zero is NOT excluded -- a hover keeps whatever vy it
    // began with, and a pad or orb can set that to zero.
    const float vp = cur->flip ? -cur->vy : cur->vy;
    if (vp < 0.f) return 0;
    long long j = t0;
    for (int i = 0; i <= kRobotHoverTicks; ++i) {
        const AnchorRow* prev = anchors::row(j - 1);
        if (!prev || prev->onGround || prev->vy != cur->vy) break;
        --j;
    }
    const long long left = kRobotHoverTicks - (t0 - j);
    return left > 0 ? (int)left : 0;
}

// The button state the tail inherits: the last input in the plan BEFORE t0. Guessing 0 is not
// free -- in ship it selects a different acceleration branch and the trajectory drifts from the
// first tick. The window has to be the same one the prefix is cut on (`< t0`), or the tail is
// solved on an assumption the plan does not carry.
inline int heldBefore(const std::vector<InputCmd>& plan, long long t0) {
    int held = 0;
    for (const InputCmd& c : plan) {
        if (c.step >= t0) break;
        held = c.down ? 1 : 0;
    }
    return held;
}

// The 27 fields of `--start`, in the order leveldp reads them:
//   t0, x, y, vy, mode, grounded, held, flip, mini, dual, y2, v2, f2, g2, speed,
//   robotHover, dashHeld, dashSlope, snapUid, snapDist, gframe, reversed,
//   rot, rotNeg, boost, mode2, mini2
inline std::string startArg(long long t0, const AnchorRow& r, int held) {
    // GD's rotated frames do not map one-to-one onto the model's. Measured over all 2,069 ticks
    // of lv22's rotated section: GD frame 2 is the model's (frame 0, reversed), and GD frame 3
    // mirrors the vertical so the gravity flag flips with it. Passing the raw number puts the
    // tail in a world whose gravity points the wrong way.
    int gf = r.gframe, rv = 0, flip = r.flip;
    if (gf == 2) { gf = 0; rv = 1; }
    else if (gf == 3) { flip = 1 - flip; }
    // Reversed gameplay (id 2900) is not a column GD has, but it is recoverable: the sign of
    // the movement along this frame's travel axis says which way the player is going.
    if (rv == 0) {
        const AnchorRow* prev = anchors::row(t0 - 1);
        if (prev) {
            const double d = (r.gframe % 2 == 1) ? (r.y - prev->y) : (r.x - prev->x);
            const double fwd = (r.gframe == 0 || r.gframe == 3) ? 1.0 : -1.0;
            if (d * fwd < -1e-6) rv = 1;
        }
    }
    std::string s = std::to_string(t0);
    s += "," + num(r.x) + "," + num(r.y) + "," + num(r.vy);
    s += "," + std::to_string(r.mode) + "," + std::to_string(groundedOf(r));
    s += "," + std::to_string(held) + "," + std::to_string(flip)
       + "," + std::to_string(r.mini) + "," + std::to_string(r.dual);
    s += "," + num(r.y2) + "," + num(r.v2) + "," + std::to_string(r.f2)
       + "," + std::to_string(groundedOf2(r)) + "," + num(r.speed);
    // robotHover, dashHeld, dashSlope. A dash wins: dp gates the hover seed on the 16th field
    // and the dash on the 17th, and crediting a hover to a dash is how the driver once got the
    // right trajectory from the wrong mechanism (lv21 x=18,105 -- a rot=0 ring holds vy at 0 and
    // draws exactly the same flat line as a hover at vy=0, right up to the 68th tick).
    const int hover = r.dashing ? 0 : robotHoverLeft(t0);
    s += "," + std::to_string(hover) + "," + std::to_string(r.dashing)
       + "," + num(r.dashSlope);
    s += "," + std::to_string(r.snapUid) + "," + num(r.snapDist);
    s += "," + std::to_string(gf) + "," + std::to_string(rv);
    // 23rd/24th: the sprite angle and which way it is turning. dp derives the spin direction
    // from the caller, so take it the same way the driver does -- from the sign of the step the
    // angle just made.
    int rotNeg = 0;
    if (const AnchorRow* prev = anchors::row(t0 - 1))
        if (r.rot - prev->rot < 0.f) rotNeg = 1;
    s += "," + num(r.rot) + "," + std::to_string(rotNeg);
    // 25th: the velocity-limit exemption (GD's byte 0x952 -> State::boost).
    s += "," + std::to_string(r.boost);
    // 26th/27th: the SECOND body's own mode and size. -1 outside a dual, which the solver reads
    // as "not told" and answers with the old copy-from-p1 -- so this is inert on every anchor
    // the official corpus produces (measured: lv16's cold run never has the halves differ) and
    // carries the truth on the custom levels where they do.
    s += "," + std::to_string(r.m2) + "," + std::to_string(r.mini2);
    return s;
}

// ============================================================
// Fixups: learning where the model is wrong, instead of walking around it
//
// After every replay, the model is asked to re-simulate THE SAME PLAN from the state GD really
// had a few hundred ticks before the death. Where the two first disagree, that one transition is
// recorded -- the state before it, the input, and GD's outcome as a delta -- and every later
// search substitutes GD's outcome whenever it meets a matching state. GD is the authority; the
// model is the approximation.
//
// The point is arithmetic. Without this a divergence has to be walked around by backing off, and
// the same wall costs iteration after iteration; with it, an observed disagreement costs one.
//
// Deltas, not absolutes, and the match window is one tick wide and gated on input, mode, size,
// gravity and grounded -- a record must never generalise beyond the transition it was measured
// on. The format and the window are dp/fixup.hpp's; this only writes what that file reads.
// ============================================================

// How far before the death to anchor the re-simulation. Far enough to contain the divergence,
// near enough that the phase noise of a long resim does not invent its own.
constexpr long long kFixupWindow = 400;
// A tick counts as diverged when the model's y or vy is off by more than this. This is the
// ACCUMULATED difference -- how far apart the two have drifted by that tick.
constexpr double kDivergeEps = 0.3;
// ...and this is how close the ONE TRANSITION has to be before a delta record would be a no-op.
// Much smaller, and it has to be: the two measure different things. A transition can be right to
// three decimal places while the drift it sits on top of is a third of a pixel, and a delta of
// 0.003 cannot pay off an accumulation of 0.3.
//
// These were the same constant, and that broke the recorder completely. Every divergence it
// found was scored against 0.3, decided to be "already right", filed as a no-op mark -- and the
// mark then blocked any real record at that state. Measured on lv16: fifty passes, walking
// forward one tick at a time, writing marks, producing not one usable fixup, while the driver on
// the same wall recorded two and went past it.
constexpr double kNoopEps = 0.05;
// Beyond this the two are on different trajectories, not a repairable transition: a delta on y
// and vy cannot express an x split. The stair snap's +-1px is a harmless translation; a real
// speed fork grows at about 0.32 px/tick.
constexpr double kForkX = 5.0;
constexpr int kFixupPasses = 4;    // a multi-tick divergence needs one record per tick
// [2026-08-24] 400 was chosen when hard walls looked like one-shot transitions. It is not enough
// for a CONTINUOUS physics gap (an unimplemented force field, a floor lift, a carrier band): each
// such tick eats one record, so a 30-tick force box costs 30 records. lv22's own 84% wall hit the
// ceiling exactly there --
//   [fixup] t=16413..16421  dy 1.4x  dvy +0.108/-0.069     (records 392..400)
//   [fixup] t=16422 differs (dy=-1.365) but the record budget is spent
//   [fixup] t=16423 differs (dy=-2.710) but the record budget is spent   (dy grows linearly)
// -- which looked like the wall, but MEASURED AND NEUTRAL: raised to 2000, the same cold run
// recorded 500+ fixups without ever hitting the new ceiling, and stopped at the same 84%
// (x=20,135, `it ran out of repair rounds` instead of `record budget is spent`). So that
// particular divergence was a real gap the model has now learnt, not the wall's cause; something
// else at x=20,135 refuses every route the search finds. Left raised anyway -- it cost nothing
// and a level with a genuinely long continuous gap may still need the room. Each record is small
// and matched by (t, mode, input, size, gravity, grounded, dual), so cost is linear in the count.
constexpr int kFixupCap = 2000;

inline std::string g_fixupNoopPath;  // records that turned out to change nothing (dedupe marks)
inline int g_fixupCount = 0;
inline int g_fixupNoop = 0;

// One row of the model's own trace: tick,x,y,vy,mode,grounded,dual,y2,vy2,flip2,act
struct TraceRow {
    bool valid = false;
    double x = 0, y = 0, vy = 0, y2 = 0, vy2 = 0;
    int mode = 0, grounded = 0, dual = 0, act = -1;
    // The MODEL's own flip and mini (trace columns 24 and 16; -1 = an old
    // trace without them). The fixup key must be built from these, not from
    // the anchor row: the solver matches records against MODEL states, and at
    // the exact transitions fixups exist for -- a 1-tick flip phase error is
    // the canonical one -- GD's flags and the model's disagree. A record
    // keyed on GD's flip was written once, never fired (applyFixup rejects on
    // f.flip != s.flip), and then blocked every re-record at that key with
    // "a record already covers this state" -- the un-learnable-wall loop of
    // [[gd-fixup-already-covered-skip]], reproduced on lv22 at t=5,944
    // (grounded ball, edvy=+3.426 = the flip impulse, refused 100+ rounds).
    int flip = -1, mini = -1;
};

inline bool loadTrace(const std::string& path, std::map<long long, TraceRow>& out) {
    std::ifstream f(path);
    if (!f) return false;
    out.clear();
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || !isdigit((unsigned char)line[0])) continue;   // header
        std::vector<std::string> c;
        size_t p = 0;
        while (true) {
            size_t q = line.find(',', p);
            c.push_back(line.substr(p, q == std::string::npos ? q : q - p));
            if (q == std::string::npos) break;
            p = q + 1;
        }
        if (c.size() < 11) continue;
        TraceRow r;
        r.valid = true;
        const long long t = std::atoll(c[0].c_str());
        r.x = std::atof(c[1].c_str());
        r.y = std::atof(c[2].c_str());
        r.vy = std::atof(c[3].c_str());
        r.mode = std::atoi(c[4].c_str());
        r.grounded = std::atoi(c[5].c_str());
        r.dual = std::atoi(c[6].c_str());
        r.y2 = std::atof(c[7].c_str());
        r.vy2 = std::atof(c[8].c_str());
        r.act = (c[10] == "0" || c[10] == "1") ? std::atoi(c[10].c_str()) : -1;
        // mini is column 16 and flip column 24 of the model trace; keep -1 on
        // an old/short trace so the caller can fall back to the anchor row.
        if (c.size() > 24) {
            r.mini = std::atoi(c[16].c_str());
            r.flip = std::atoi(c[24].c_str());
        }
        out[t] = r;
    }
    return !out.empty();
}

// Everything the solver matches a record on (dp/fixup.hpp, fixupMatches). Kept as one struct so
// that "would the solver treat these two as the same transition" is asked in one place.
struct FixupKey {
    double x = 0, y = 0, vy = 0, y2 = 0, vy2 = 0;
    int in = 0, mode = 0, mini = 0, flip = 0, g = 0, kill = 0, dual = 0;
};

// Read a record back into its key. `dual` is not a column -- the second body rides in a named
// tail, so its presence IS the flag, which is also how dp/fixup.hpp reads these files.
inline bool parseFixupKey(const std::string& line, FixupKey& k,
                          double* dyOut = nullptr, double* dvOut = nullptr) {
    double dy, dvy;
    int g2;
    if (std::sscanf(line.c_str(),
                    "x=%lf,in=%d,mode=%d,mini=%d,flip=%d,g=%d,"
                    "y=%lf,vy=%lf,dy=%lf,dvy=%lf,g2=%d,kill=%d",
                    &k.x, &k.in, &k.mode, &k.mini, &k.flip, &k.g, &k.y, &k.vy,
                    &dy, &dvy, &g2, &k.kill) < 12)
        return false;
    if (dyOut) *dyOut = dy;
    if (dvOut) *dvOut = dvy;
    const size_t p = line.find(",dual2=");
    if (p != std::string::npos) {
        double d1, d2;
        int gg;
        if (std::sscanf(line.c_str() + p, ",dual2=%lf,%lf,%lf,%lf,%d",
                        &k.y2, &k.vy2, &d1, &d2, &gg) == 5)
            k.dual = 1;
    }
    return true;
}

// The solver's match window, applied to two records instead of to a record and a state.
//
// This has to be EXACTLY dp/fixup.hpp's fixupMatches. A looser one refuses to write a record
// because of an entry the solver will never apply, and then the wall it was measured on can
// never be learnt -- the recorder reports "a record already covers this state" every round while
// the model goes on getting that transition wrong. Measured on lv16's dual section: the same
// three ticks (t=8,567..8,569) were refused every pass for the whole budget because x, y and vy
// matched an entry whose mode, gravity or second body did not.
inline bool sameTransition(const FixupKey& a, const FixupKey& b) {
    if (a.in != b.in || a.kill != b.kill || a.mode != b.mode || a.mini != b.mini
        || a.flip != b.flip || a.g != b.g || a.dual != b.dual)
        return false;
    if (std::fabs(a.x - b.x) > 1.2 || std::fabs(a.y - b.y) > 4.0
        || std::fabs(a.vy - b.vy) > 1.0)
        return false;
    if (a.dual && (std::fabs(a.y2 - b.y2) > 4.0 || std::fabs(a.vy2 - b.vy2) > 1.0))
        return false;
    return true;
}

// Is a record the solver would already apply to this transition on file --
// AND does applying it produce what GD was just observed to do? A record
// whose deltas disagree with the observation is NOT coverage: it is the
// previous worldline's answer sitting on this one's key. Refusing to
// re-record on such a hit was the second half of the un-learnable-wall loop:
// the key window (x1.2/y4/vy1) spans neighbouring worldlines, the stored
// delta cured one of them, and every later pass through the same window was
// refused while the model went on getting ITS transition wrong (lv22
// t=11,251..11,253: dvy 2.5/3.2/3.6 observed on different rounds, all
// refused by one record). The 0.2 tolerance keeps adjacent-tick drift of a
// decaying curve (0.03..0.1 between neighbours) counting as covered, so
// records do not churn, while real disagreements (0.3+) re-record; the
// solver picks the nearest match (findFixup), so the refinement wins where
// it was measured and the old record keeps its own neighbourhood.
inline bool fixupOnFile(const std::string& path, const FixupKey& k,
                        double dyG, double dvG) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        FixupKey o;
        double dy = 0, dvy = 0;
        if (parseFixupKey(line, o, &dy, &dvy) && sameTransition(o, k)
            && std::fabs(dy - dyG) < 0.2 && std::fabs(dvy - dvG) < 0.2)
            return true;
    }
    return false;
}

// What writeFixup did. Two of these mean something went on file; the rest are refusals, and each
// says WHICH refusal -- "the transition cannot be expressed" covering six different causes is
// how a dual-phase mismatch and an exhausted cap look identical in a log.
// A MARK is not an answer either: it only says "this tick has been looked at, and it was already
// right", so callers must not treat it as one.
enum FixupWrite {
    FixupReal,       // a record that changes what the model does
    FixupMark,       // ...and one that only says "looked at"
    FixupCapped,     // the run has recorded all it is allowed to
    FixupNoTrace,    // the model's own resim has no row on one side of the transition
    FixupNoRow,      // ...and neither does GD's recording
    FixupHalfPair,   // a dual, with only one of the two bodies observed
    FixupNoInput,    // the resim did not write down which way the button was
    FixupOnFile,     // an existing record already covers this state
    FixupIoError,
};
inline const char* fixupWhy(int w) {
    switch (w) {
        // The two that are not refusals. A caller reporting a refusal should never see these,
        // and if it does, the sentence has to say so rather than fall through to "could not be
        // written" -- a mark IS on file, it just answers nothing.
        case FixupReal:     return "it went on file";
        case FixupMark:     return "it was already right, so only a mark went on file";
        case FixupCapped:   return "the record budget is spent";
        case FixupNoTrace:  return "the resim has no row for one side of it";
        case FixupNoRow:    return "the game's recording has no row for one side of it";
        case FixupHalfPair: return "a dual with only one body observed";
        case FixupNoInput:  return "the resim did not record the input";
        case FixupOnFile:   return "a record already covers this state";
        default:            return "it could not be written";
    }
}

// Record the transition into tick t: the model's state at t-1 with GD's observed deltas.
inline int writeFixup(long long t, int kill, const std::map<long long, TraceRow>& m) {
    if (g_fixupCount >= kFixupCap) return FixupCapped;
    auto mPrev = m.find(t - 1), mCur = m.find(t);
    if (mPrev == m.end() || mCur == m.end()) return FixupNoTrace;
    const AnchorRow* gPrev = anchors::row(t - 1);
    const AnchorRow* gCur = anchors::row(t);
    // A KILL record is the one case that needs no `after` state. GD ended the run on this
    // transition, so whatever is recorded at the death tick is a dead player, not a physical
    // state -- and the record does not want one: applyFixup reads only the verdict for a kill,
    // and the state it is MATCHED on is the one before the transition. In practice the row is
    // usually there (the recording runs on through the death animation), but requiring it would
    // make the class depend on an accident of when the attempt is banked.
    if (!gPrev || (!gCur && kill == 0)) return FixupNoRow;
    // A dual transition is recordable now (dp/fixup.hpp), but only if BOTH bodies were observed
    // on both sides of it -- a record with half a pair in it would be applied to a pair.
    const bool dual = (mPrev->second.dual != 0);
    if (dual && !(gPrev->dual && mCur->second.dual && (!gCur || gCur->dual))) return FixupHalfPair;
    const int act = mCur->second.act;
    if (act < 0) return FixupNoInput;
    // Deltas describe what GD did instead of the model. A kill has no "instead" -- the run ended
    // -- so it carries zeroes and leaves the grounded flag alone; dp/fixup.hpp reads neither.
    const double dyG = gCur ? gCur->y - gPrev->y : 0.0;
    const double dvG = gCur ? gCur->vy - gPrev->vy : 0.0;
    // Flying modes keep GD's onGround set while airborne, so the model's own flag is the one to
    // leave in place; 255 is dp/fixup.hpp's "do not touch it".
    const bool flying = (mPrev->second.mode == 1 || mPrev->second.mode == 3
                         || mPrev->second.mode == 4);
    const int g2 = (flying || !gCur) ? 255 : (gCur->onGround ? 1 : 0);
    // A transition the model already gets right cannot be expressed as a delta -- `c.y = s.y +
    // dy` with the model's own dy is literally nothing. It still goes on file, in a separate
    // one, because the record is also the mark that says "this tick has been looked at"; drop it
    // and the recorder examines the same tick forever.
    const double eDy = dyG - (mCur->second.y - mPrev->second.y);
    const double eDvy = dvG - (mCur->second.vy - mPrev->second.vy);
    // ...AND THE SECOND BODY'S OWN TRANSITION ERROR. The scan that picks the tick to record
    // already compares both bodies (dy2/dv2 above); this test, which decides whether there is
    // anything TO record, asked only the first -- so in a dual the second body's error was
    // invisible here even though the record format carries dy2/dvy2 and applyFixup applies them.
    //
    // The failure it produces is not a missed record, it is a POISONED one: p1's transition is
    // perfect, the record is filed as a no-op mark, and from the next pass that mark answers
    // "a record already covers this state" at the one key that could have carried the fix. The
    // pass then walks forward one tick and marks that one too.
    //
    // Measured on lv16 t=13,017, the corpus' largest grind (11 of 66 cold iterations). The scan
    // finds the pair parting at t=12,991 every round; writeFixup answers `err 0.000/0.000` --
    // p1's -- and marks it; the next round marks 12,992, then 12,993, one tick per iteration,
    // while p2's accumulated gap runs out to 15.73 px and vy 2.83 by the death tick. Every one
    // of those marks is on file saying the model was already right about a transition it was
    // getting wrong by 0.6 px a tick.
    // Same blindness as the report's `apart there by` line, in the same function, found the same
    // day and by the same measurement.
    //
    // The two numbers are computed and PRINTED unconditionally -- that is the instrument, and it
    // costs nothing. Whether they may VETO the no-op is cfg `dpfixp2`, ON since 2026-08-29.
    //
    // It was off for most of that day, and both settings were measurements rather than policy:
    // with the physics as it stood in the morning, promoting those 27 transitions to records
    // cost lv16 66 -> 150 iterations, because a patch applied at 27 matched states leaves the
    // model wrong between them and the search plans through the gaps. Once the ramp-ride and
    // second-body fixes landed, the same 27 became 7 and the same flag became a win
    // (lv16 95 -> 55). WHAT A FLAG IS WORTH DEPENDS ON THE PHYSICS UNDERNEATH IT, so re-measure
    // before reading either number as settled. config.hpp carries the table.
    const double eDy2 = dual ? (gCur ? gCur->y2 - gPrev->y2 : 0.0)
                                   - (mCur->second.y2 - mPrev->second.y2) : 0.0;
    const double eDvy2 = dual ? (gCur ? gCur->v2 - gPrev->v2 : 0.0)
                                   - (mCur->second.vy2 - mPrev->second.vy2) : 0.0;
    const bool noop = (kill == 0 && std::fabs(eDy) < kNoopEps
                       && std::fabs(eDvy) < kNoopEps
                       && (!g_cfg.dpFixP2
                           || (std::fabs(eDy2) < kNoopEps
                               && std::fabs(eDvy2) < kNoopEps)));
    // The key of the record about to be written -- every field the solver matches on, taken from
    // the same variables the line below is built out of.
    // flip and mini come from the MODEL's trace row when it carries them (see
    // TraceRow): the solver matches records against model states, and at a
    // divergent transition GD's flags are exactly what the model's may not be.
    const int keyFlip = mPrev->second.flip >= 0 ? mPrev->second.flip : gPrev->flip;
    const int keyMini = mPrev->second.mini >= 0 ? mPrev->second.mini : gPrev->mini;
    FixupKey key;
    key.x = mPrev->second.x;
    key.y = mPrev->second.y;
    key.vy = mPrev->second.vy;
    key.in = act;
    key.mode = mPrev->second.mode;
    key.mini = keyMini;
    key.flip = keyFlip;
    key.g = mPrev->second.grounded;
    key.kill = kill;
    key.dual = dual ? 1 : 0;
    key.y2 = mPrev->second.y2;
    key.vy2 = mPrev->second.vy2;
    if (fixupOnFile(g_fixupPath, key, dyG, dvG)
        || fixupOnFile(g_fixupNoopPath, key, dyG, dvG))
        return FixupOnFile;
    char dual2[160] = "";
    if (dual) {
        // The second body's own before-state and GD's own deltas for it. Named tail, so a
        // reader that predates duals sees a record it understands and treats as single-body.
        snprintf(dual2, sizeof(dual2), ",dual2=%.4f,%.4f,%.4f,%.4f,%d",
                 mPrev->second.y2, mPrev->second.vy2,
                 gCur ? gCur->y2 - gPrev->y2 : 0.0, gCur ? gCur->v2 - gPrev->v2 : 0.0,
                 (flying || !gCur) ? 255 : (gCur->g2 ? 1 : 0));
    }
    char line[700];
    snprintf(line, sizeof(line),
             "x=%.4f,in=%d,mode=%d,mini=%d,flip=%d,g=%d,y=%.4f,vy=%.4f,"
             "dy=%.4f,dvy=%.4f,g2=%d,kill=%d,edy=%.4f,edvy=%.4f%s",
             mPrev->second.x, act, mPrev->second.mode, keyMini, keyFlip,
             mPrev->second.grounded, mPrev->second.y, mPrev->second.vy,
             dyG, dvG, g2, kill, eDy, eDvy, dual2);
    {
        std::ofstream f(noop ? g_fixupNoopPath : g_fixupPath, std::ios::app);
        if (!f) return FixupIoError;
        f << line << "\n";
    }
    char b[600];
    // Both lines carry the second body's error too, for the same reason the test above now
    // consults it: `err 0.000/0.000` next to a record that was decided by a body those two
    // numbers do not describe is how this went unread for a day.
    char e2[48] = "";
    if (dual) snprintf(e2, sizeof(e2), " p2 %.3f/%.3f", eDy2, eDvy2);
    if (noop) {
        ++g_fixupNoop;
        snprintf(b, sizeof(b), "dpsolve:   [fixup] t=%lld already right (err %.3f/%.3f%s) "
                 "- marked only (%d)", t, eDy, eDvy, e2, g_fixupNoop);
    } else {
        ++g_fixupCount;
        // ...and on the map (itermap.hpp). A fixup is where the MODEL was wrong, which is the
        // cause the deaths around it are the symptom of -- and the two are often hundreds of
        // pixels apart, which is the single most useful thing the picture says.
        itermap::addFixup(g_iter, t, (float)mPrev->second.x, (float)mPrev->second.y, kill != 0);
        snprintf(b, sizeof(b), "dpsolve:   [fixup] t=%lld x=%.1f mode=%d in=%d "
                 "dy=%.3f dvy=%.3f kill=%d err %.3f/%.3f%s (%d total)",
                 t, mPrev->second.x, mPrev->second.mode, act, dyG, dvG, kill,
                 eDy, eDvy, e2, g_fixupCount);
    }
    writeResult(b);
    // KILL-ONLY: the second veto hit (see g_killVetoIter). The p2 halves are
    // consulted whatever cfg dpFixP2 says -- unlike the noop test above, being
    // strict here only ever withholds credit.
    const bool physAgrees =
        std::fabs(eDy) < kNoopEps && std::fabs(eDvy) < kNoopEps
        && (!dual || (std::fabs(eDy2) < kNoopEps && std::fabs(eDvy2) < kNoopEps));
    if (!noop && kill != 0 && physAgrees && g_lastTailSolved && !g_phantomLifted
        && g_killVetoIter != g_iter) {
        g_killVetoIter = g_iter;
        const long long site = (long long)std::floor((double)g_lastDeathX / 8.0);
        const int n = ++g_phantomHits[site];
        char kb[224];
        snprintf(kb, sizeof(kb), "dpsolve:   [veto] t=%lld is kill-only (the "
                 "physics agrees) - counting it twice at x=%.0f (%d/%d)",
                 t, (double)g_lastDeathX, n, kPhantomAfter);
        writeResult(kb);
    }
    return noop ? FixupMark : FixupReal;
}

// One pass: re-simulate the plan from the anchor and record the first divergence found.
// Returns how many records went on file.
inline int fixupPass(long long t0, const std::string& startArgStr, const std::string& band,
                     long long deathTick) {
    const std::string base = std::string(DATA_DIR) + "/dp_fixup";
    std::vector<std::string> a{"--replay", g_planPath, "--start", startArgStr,
                               "--out", base,
                               "--cap", std::to_string(kCap),
                               "--shipyq", num(kYq), "--shipvq", num(kVq),
                               "--threads", kThreads};
    if (!band.empty()) { a.push_back("--startband"); a.push_back(band); }
    {   // the resim must not fire 2900s the recorded run already consumed either --
        // a phantom rotation in the REFERENCE side of the diff writes fixups against
        // a world GD does not have (the -7.8 carry family at x=16,003)
        const std::string sr = spentRotArg(t0);
        if (!sr.empty()) { a.push_back("--spentrot"); a.push_back(sr); }
        // ...and in that same world the autonomous triggers take the
        // recording's tick, not the x-crossing estimate (--trigraw): inside a
        // rotated maze world-x does not advance, so the crossing estimate
        // fires whole sections early (cli.hpp r99: Toggle uid6439 1,598 ticks
        // early, and the block GD lands on is gone). The driver passed this
        // for every section anchor of a level that has a 2900; the loop never
        // wired it. Anchored calls only -- a from-head solve estimating from
        // its own plan keeps the crossing rule.
        if (!g_rotObjs.empty()) a.push_back("--trigraw");
    }
    std::error_code ec;
    if (std::filesystem::exists(g_fixupPath, ec)) {
        a.push_back("--fixups");
        a.push_back(g_fixupPath);
    }
    addWorldArgs(a);
    for (const std::string& s : g_cfg.dpArgs) a.push_back(s);
    std::filesystem::remove(base + ".trace.csv", ec);
    dpbridge::solveInProcess(g_csv, a);
    const long long modelDied = dpbridge::outcome().replayDiedT;
    std::map<long long, TraceRow> m;
    if (!loadTrace(base + ".trace.csv", m)) {
        writeResult("dpsolve:   [fixup] the resim wrote no trace - skipping");
        return 0;
    }
    // The resim's first row is t0+1: the model is PLACED at GD's state at t0 and the trace holds
    // the ticks it went on to compute. So the transition into t0+1 -- the first one after the
    // anchor, and the only place a divergence can be when the model and the game are identical
    // at t0 by construction -- has no `before` row, and the recorder reported it as unrecordable
    // forever. The row is not missing information: `--start` puts the model exactly where GD
    // was, so GD's own record of t0 IS the model's state at t0.
    // Measured on lv16's second wall: the divergence was at t=8,383 with the anchor at t=8,382,
    // every round, and the residual it left then matched an existing record at every later tick.
    if (m.find(t0) == m.end()) {
        if (const AnchorRow* a = anchors::row(t0)) {
            TraceRow r;
            r.valid = true;
            r.x = a->x;
            r.y = a->y;
            r.vy = a->vy;
            r.mode = a->mode;
            r.grounded = groundedOf(*a);
            r.dual = a->dual;
            r.y2 = a->y2;
            r.vy2 = a->v2;
            r.act = -1;    // no transition ENDS at t0, so no input is attributed to it
            // model == GD at t0 by construction, so GD's flags stand in for the
            // model's -- with the same frame-3 flip mirror startArg applies.
            r.mini = a->mini;
            r.flip = (a->gframe == 3) ? 1 - a->flip : a->flip;
            m[t0] = r;
        }
    }
    int made = 0;     // anything went on file, marks included -- drives the pass loop
    int real = 0;     // ...and of those, the ones that change what the model does
    int skipped = 0;
    long long lastCommon = -1;
    for (auto it = m.begin(); it != m.end(); ++it) {
        const long long t = it->first;
        // Nothing past GD's death is a state. The recording runs on for the length of the death
        // animation, and those rows are a corpse being tidied up, not physics -- comparing the
        // model against them invents divergences and, worse, makes the model look like the one
        // that died first. Measured on lv22: GD died at t=2,659 and the two were still being
        // compared at t=3,012.
        if (t > deathTick) break;
        const AnchorRow* gr = anchors::row(t);
        if (!gr) continue;
        lastCommon = t;
        const double dy = it->second.y - gr->y;
        const double dv = it->second.vy - gr->vy;
        // ...and the SECOND body, wherever there is one. The record format has carried y2/vy2
        // since duals became recordable, but the search for where the two first parted still
        // only looked at the first player -- so in a dual section a divergence that starts in
        // the second body is invisible until it reaches the first, and what gets written down
        // is wherever the symptom surfaced rather than where the cause was. lv16's wall is in a
        // dual ship section, which is why this asymmetry is worth closing before anything else.
        const bool pair = (it->second.dual != 0 && gr->dual != 0);
        const double dy2 = pair ? it->second.y2 - gr->y2 : 0.0;
        const double dv2 = pair ? it->second.vy2 - gr->v2 : 0.0;
        if (std::fabs(dy) <= kDivergeEps && std::fabs(dv) <= kDivergeEps
            && std::fabs(dy2) <= kDivergeEps && std::fabs(dv2) <= kDivergeEps) continue;
        if (std::fabs(it->second.x - gr->x) > kForkX) {
            char b[200];
            snprintf(b, sizeof(b), "dpsolve:   [fixup] t=%lld is an x fork (%.1f px) - "
                     "not a transition a delta can carry", t, it->second.x - gr->x);
            writeResult(b);
            return made;
        }
        const int w = writeFixup(t, 0, m);
        if (w == FixupReal || w == FixupMark) {
            ++made;
            if (w == FixupReal) ++real;
            // One record per pass: past here the two are on different trajectories. A MARK stops
            // the scan for the same reason a record does (it is on file now, and re-scanning the
            // whole stretch every pass is what this early exit is for) -- but it is not an
            // answer, so `real` stays 0 and the verdict question below still gets asked.
            break;
        }
        // Refused. KEEP LOOKING: returning here stalls the recorder completely -- it re-finds the
        // same tick every pass, records nothing, and the run learns nothing for the rest of its
        // budget. Measured on lv16, which reported the identical unrecordable tick 33 times.
        if (++skipped <= 3) {
            char b[240];
            snprintf(b, sizeof(b), "dpsolve:   [fixup] t=%lld differs (dy=%.3f dvy=%.3f) but %s "
                     "- looking further", t, dy, dv, fixupWhy(w));
            writeResult(b);
        }
    }
    // A kill record says "at THIS state, with THIS input, GD ends the run". That is only true if
    // the model was actually AT that state -- the class it was built for is the one where the two
    // agree to the last tick and only the verdict differs. When they have already drifted apart,
    // the death belongs to the drift, and recording a kill blames a state that is perfectly safe
    // in the game and prunes it out of every later search.
    //
    // Measured on lv16's dual section: the loop wrote `t=8773 kill=1 err=-2.398` -- a kill on a
    // transition the model was 2.4 px away from -- and the driver, which reaches the identical
    // wall at t=8,768, records nothing there at all and re-anchors past it on the third rung.
    // BOTH bodies. A kill record says "this state dies"; a state with a second body that is
    // already somewhere else is not the state GD killed, and writing the verdict against it
    // prunes a first player that was perfectly fine.
    bool agreedAtDeath = false;
    double gapY = -1.0, gapVy = -1.0;   // how far apart they were there, for the report
    // ...AND THE SAME FOR THE SECOND BODY, because the report was printing the first body's gap
    // and calling it the answer. On a pair the verdict test below is an AND over both, so a run
    // where p1 matches to the digit and p2 does not reads as
    //     nothing recordable: ... apart there by 0.00/0.00
    // with no record written and no reason given -- the two numbers on the line say "they agree"
    // while the decision they are attached to says the opposite. Measured on lv16 t=13,017, the
    // corpus' largest single grind (11 of the run's 66 iterations): every census taken of that
    // site, including this session's, recorded it as "y/vy agree 0.00/0.00" ON THE STRENGTH OF
    // THIS LINE, and the second body was never in it. -1 = there was no row to compare.
    double gapY2 = -1.0, gapVy2 = -1.0;
    {
        auto mp = m.find(deathTick - 1);
        const AnchorRow* gp = anchors::row(deathTick - 1);
        if (mp != m.end() && gp) {
            gapY = std::fabs(mp->second.y - gp->y);
            gapVy = std::fabs(mp->second.vy - gp->vy);
            agreedAtDeath = gapY <= kDivergeEps && gapVy <= kDivergeEps;
            if (mp->second.dual != 0 && gp->dual != 0) {
                gapY2 = std::fabs(mp->second.y2 - gp->y2);
                gapVy2 = std::fabs(mp->second.vy2 - gp->v2);
                if (agreedAtDeath)
                    agreedAtDeath = gapY2 <= kDivergeEps && gapVy2 <= kDivergeEps;
            }
        }
    }
    // Whatever the trajectories did, the two runs also disagree about WHO DIED, and that is a
    // separate question with its own record. It is asked whenever this pass found no real
    // divergence to blame -- including when the pass ended on a no-op mark, which answers
    // nothing. Gating it on `made` instead starved it completely: measured on lv16's second
    // wall, four passes in a row ended by marking t=8,615..8,618 as already-right and the death
    // at t=8,768 was never once asked about.
    // WHY THE VERDICT RECORD DID NOT GO ON FILE. The two writes below are the only records that
    // can express "the two ran the same trajectory and disagreed about who died", and their
    // return value was thrown away -- so a refusal was indistinguishable from never trying, and
    // the same tick was ground for iteration after iteration with the log saying only "nothing
    // recordable". Measured on lv16 t=13,017: 11 of the run's 66 iterations died there, and every
    // one of them called writeFixup(t, kill=1) and discarded the answer.
    // Kept as one number plus fixupWhy()'s sentence; -1 = neither branch was entered, which is
    // itself an answer (the preconditions on this if are the ones that failed).
    int verdictWhy = -1;
    if (real == 0) {
        if (modelDied >= 0 && modelDied < deathTick && anchors::row(modelDied + 1)) {
            // The model killed a run GD carried on: the delta record revives it
            verdictWhy = writeFixup(modelDied, 0, m);
            if (verdictWhy == FixupReal) ++real, ++made;
        } else if ((modelDied < 0 || modelDied > deathTick) && agreedAtDeath
                   && anchors::row(deathTick - 1) && m.find(deathTick) != m.end()) {
            // ...and the mirror: GD ended the run here and the model let it live. The
            // trajectories agree to the last tick and only the verdict differs, so there is
            // nothing to patch but the verdict.
            //
            // "The model let it live" means AT THIS TICK -- not "the model never died". It was
            // written as `modelDied < 0`, and that is a different claim: the resim runs on to
            // the end of the plan, so a model that outlives GD's death by thousands of ticks
            // and then dies somewhere else failed the test and the record was never written.
            // That is the whole of lv16's wall. Measured: GD died at t=4,753, the model at
            // t=7,903, they agreed on every tick they shared, and the loop replayed the same
            // plan ten more times because the one record that could express the disagreement
            // was gated on where the model happened to die 3,150 ticks later.
            verdictWhy = writeFixup(deathTick, 1, m);
            if (verdictWhy == FixupReal) ++real, ++made;
        }
    }
    // A pass that records nothing and says nothing is indistinguishable from one that was never
    // run, and that is exactly how the kill class stayed invisible: the loop replayed the same
    // doomed plan for a whole budget while this function returned 0 in silence. Say why.
    if (real == 0) {
        char b[280];
        char p2[64] = "";
        if (gapY2 >= 0.0) snprintf(p2, sizeof(p2), " (p2 %.2f/%.2f)", gapY2, gapVy2);
        snprintf(b, sizeof(b), "dpsolve:   [fixup] nothing recordable: GD died t=%lld, model died "
                 "t=%lld, apart there by %.2f/%.2f%s, compared to t=%lld, marks %d, skipped %d, "
                 "trace %lld..%lld",
                 deathTick, modelDied, gapY, gapVy, p2, lastCommon, made, skipped,
                 m.empty() ? -1 : m.begin()->first, m.empty() ? -1 : m.rbegin()->first);
        writeResult(b);
        // ...and the verdict record's own answer, with enough of its key to find the entry that
        // blocked it. `a record already covers this state` and `the resim did not record the
        // input` are different bugs with the same silence, and the difference is one grep.
        const long long vt = (verdictWhy >= 0 && modelDied >= 0 && modelDied < deathTick)
                                 ? modelDied : deathTick;
        auto vp = m.find(vt - 1);
        char c[320];
        if (verdictWhy < 0)
            // Every gate on the two branches, so the one that closed is readable without the
            // source open. `died same tick` is the healthy case -- there is no disagreement to
            // record -- and the rest are not.
            snprintf(c, sizeof(c), "dpsolve:   [fixup] no verdict record was attempted: "
                     "model died %s, agreed=%d, GD row at model death+1=%d, GD row at t-1=%d, "
                     "model row at t=%d",
                     modelDied < 0 ? "not at all"
                                   : modelDied < deathTick ? "first"
                                   : modelDied > deathTick ? "later" : "on the same tick",
                     agreedAtDeath ? 1 : 0,
                     (modelDied >= 0 && anchors::row(modelDied + 1)) ? 1 : 0,
                     anchors::row(deathTick - 1) ? 1 : 0,
                     m.find(deathTick) != m.end() ? 1 : 0);
        else if (vp == m.end())
            snprintf(c, sizeof(c), "dpsolve:   [fixup] the verdict record at t=%lld was refused: "
                     "%s", vt, fixupWhy(verdictWhy));
        else
            snprintf(c, sizeof(c), "dpsolve:   [fixup] the verdict record at t=%lld was refused: "
                     "%s (key x=%.4f y=%.4f vy=%.4f in=%d mode=%d mini=%d flip=%d g=%d dual=%d)",
                     vt, fixupWhy(verdictWhy), vp->second.x, vp->second.y, vp->second.vy,
                     m.count(vt) ? m.at(vt).act : -1, vp->second.mode, vp->second.mini,
                     vp->second.flip, vp->second.grounded, vp->second.dual);
        writeResult(c);
    }
    return made;
}

// Find and record where this replay's model and GD first parted. Runs on the worker thread,
// before the ladder, because a record made now is used by the ladder's very first solve.
inline void recordFixups(long long deathTick) {
    if (!g_cfg.dpFixups || deathTick < 2 || g_fixupCount >= kFixupCap) return;
    // ALWAYS the attempt that just died. The ladder may have been pointed at the deepest
    // attempt's trajectory instead (a rewind), and comparing the model against a run it is not
    // replaying would record divergences that never happened.
    const std::vector<AnchorRow>* saved = anchors::g_src;
    anchors::ladderOn(false);
    struct Restore {
        const std::vector<AnchorRow>* s;
        ~Restore() { anchors::g_src = s; }
    } restore{saved};
    long long t0 = deathTick - kFixupWindow;
    if (t0 < 1) t0 = 1;
    while (t0 < deathTick && !anchors::row(t0)) ++t0;
    const AnchorRow* r = anchors::row(t0);
    if (!r) {
        writeResult("dpsolve:   [fixup] no recorded state near the anchor - skipping");
        return;
    }
    // No dual guard here. The driver has one, because ITS resim passes zeroes for the second
    // body and so cannot start inside a dual at all. startArg above passes the real pair, which
    // is the whole point of reading the state out of the game rather than out of a dump -- so a
    // dual anchor is just an anchor.
    const std::string arg = startArg(t0, *r, heldBefore(g_plan, t0));
    std::string band;
    if (r->pmax > r->pmin) band = num(r->pmin) + "," + num(r->pmax);
    // TIME IT. Every other second of the loop is on the record -- the search prints `done in Ns`
    // and each replay prints its own wallMs -- and those two are what "the loop spends its time
    // chasing fidelity" was measured from. The recorder was not among them, and it is not small:
    // each pass is a full `--replay` of the plan from t0 to the end of the level, run up to
    // kFixupPasses times per death. On lv16 the two add up to 58% of a cold run's wall clock,
    // which leaves 42% that no line accounts for -- and that gap is this loop.
    // Printed per death, so a change can be judged on what it costs rather than on the iteration
    // count alone (iterations are not equal: an early death re-solves a long tail, a late one
    // does not).
    const auto fxT0 = std::chrono::steady_clock::now();
    int passes = 0;
    for (int pass = 1; pass <= kFixupPasses; ++pass) {
        // A divergence spanning several ticks needs one delta each, and the next one only
        // becomes visible once the previous is being applied -- hence the passes.
        ++passes;
        if (fixupPass(t0, arg, band, deathTick) == 0) break;
    }
    {
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - fxT0).count();
        char b[160];
        snprintf(b, sizeof(b), "dpsolve:   [fixup] recorder: %d pass(es) in %.0f ms "
                 "(anchor t=%lld, death t=%lld)", passes, ms, t0, deathTick);
        writeResult(b);
    }
}

// ---- mode portals the run went past without taking ----
//
// A route that crosses a mode portal and does not come out in that mode is flying a section
// built for something else. That is NOT a verdict on the route: a level may hold a portal
// nothing can reach -- lv18 has a decorative pair placed out of contact, and every correct route
// passes their x without firing them -- so this must never score a run or forbid a branch.
//
// It is only a HINT ABOUT WHERE TO RE-ANCHOR, and it is asked only once the ladder has already
// run out of anchors. On a level whose portals are decoration it costs a few doomed anchors; on
// lv22 it is the difference between planning the robot section as a robot and flying through it
// as a ship (GD's own recording reads mode=1 from x=18,180 to x=20,121, and the robot portal is
// at x=19,005).
struct ModePortal {
    double x;
    int mode;
};
inline std::vector<ModePortal> g_modePortals;
inline bool g_triedMissedPortal = false;
// The forcing slit for the forced rung (see the hint in escalate): one --deadband value,
// consumed by exactly the ladder rung that g_forceAnchorT points at, then dropped.
inline std::string g_forcePortalBand;
inline long long g_forceAnchorT = -1;

// dp's own type -> mode map (step.hpp's wantMode). Only the non-cube portals: a cube portal is
// not identifiable from the type alone here, and missing one costs nothing but a hint.
inline void loadModePortals(const std::string& csv) {
    g_modePortals.clear();
    std::istringstream in(csv);
    std::string line;
    std::getline(in, line);   // header
    while (std::getline(in, line)) {
        const size_t p1 = line.find(',');
        if (p1 == std::string::npos) continue;
        const size_t p2 = line.find(',', p1 + 1);
        if (p2 == std::string::npos) continue;
        int mode = -1;
        switch (std::atoi(line.c_str() + p1 + 1)) {
            case 5:  mode = 1; break;   // ship
            case 16: mode = 2; break;   // ball
            case 19: mode = 3; break;   // UFO
            case 26: mode = 4; break;   // wave
            case 27: mode = 5; break;   // robot
            case 33: mode = 6; break;   // spider
            case 41: mode = 7; break;   // swing
            default: break;
        }
        if (mode < 0) continue;
        g_modePortals.push_back({std::atof(line.c_str() + p2 + 1), mode});
    }
    std::sort(g_modePortals.begin(), g_modePortals.end(),
              [](const ModePortal& a, const ModePortal& b) { return a.x < b.x; });
}

// How long after crossing to ask what mode the run came out in. A portal fires on contact, and
// the mode is settled well inside this.
constexpr long long kPortalSettle = 30;

// The tick the run crossed the DEEPEST mode portal it did not come out of, or -1. Read entirely
// off the run's own recording plus where the portals are.
// ...and which portal that was (position and the mode it sets), for the caller that wants to
// FORCE the crossing (see the forcing band at the hint). Valid only when the last call
// returned a tick.
inline double g_missedPortalX = 0.0;
inline int g_missedPortalMode = -1;

inline long long missedPortalTick() {
    if (g_modePortals.empty() || !anchors::g_src) return -1;
    const long long end = (long long)anchors::g_src->size();
    size_t idx = 0;
    long long best = -1, crossT = -1;
    int wantMode = -1;
    double crossX = 0.0;
    for (long long t = 1; t < end; ++t) {
        const AnchorRow* r = anchors::row(t);
        if (!r) continue;
        if (crossT >= 0 && t >= crossT + kPortalSettle) {
            if (r->mode != wantMode) {
                best = crossT;
                g_missedPortalX = crossX;
                g_missedPortalMode = wantMode;
            }
            crossT = -1;
        }
        while (idx < g_modePortals.size() && (double)r->x >= g_modePortals[idx].x) {
            crossT = t;
            wantMode = g_modePortals[idx].mode;
            crossX = g_modePortals[idx].x;
            ++idx;
        }
    }
    return best;
}

// ---- the ladder (runs on the worker thread) ----
//
// Walk anchors backwards from the death until one of them can be solved. An anchor is usable
// when the model reaches the end from it (SOLVED), or when it dies LATER than GD did -- a plan
// that is doomed further along still moves the verified prefix forward, and replaying it is how
// the next wall gets found at all. A tail that dies before the current death teaches nothing.
inline bool runLadder(long long dt) {
    std::vector<int> rungs;
    // A forced rung goes first: escalate() has picked a tick to go back to (a mode portal the
    // run went past without taking), and the point of it is to be tried before the usual walk.
    size_t nForced = 0;
    if (g_forceAnchorT > 0 && g_forceAnchorT < dt - kBackOff) {
        rungs.push_back((int)(dt - g_forceAnchorT));
        g_spentAnchors.erase(g_forceAnchorT);   // it has never been tried for THIS reason
        nForced = 1;
    }
    g_forceAnchorT = -1;
    // One-shot: the slit belongs to this hint's rung alone.
    const std::string forceBand = g_forcePortalBand;
    g_forcePortalBand.clear();
    rungs.push_back(g_curBackoff);
    for (int r : kRungs) if (r >= g_curBackoff) rungs.push_back(r);

    std::vector<InputCmd> tail;
    long long chosenT = -1;
    int chosenBackoff = 0;
    bool solvedTail = false;
    // Indexed rather than range-for: a rung that turns out to have been asked an unanswerable
    // question is retried once, after the question is fixed (see the needtrig drop below).
    for (size_t rungIdx = 0; rungIdx < rungs.size(); ++rungIdx) {
        const int bo = rungs[rungIdx];
        const long long t0 = dt - bo;
        if (t0 < 2) {
            writeResult("dpsolve:   ladder exhausted: backoff " + std::to_string(bo)
                        + " reaches before the start of the run");
            break;
        }
        // An anchor this far back is not repairing a tail, it is re-solving the level: at
        // t0=379 against a wall at t=7,579 the splice kept 4 of 360 inputs and threw away
        // everything the game had verified. The plans that came out died in the opening
        // sections, round after round. Re-solving the level IS one of the things this loop can
        // do -- it is the cold restart in escalate() -- but it should be chosen deliberately,
        // not arrived at by a back-off that quadruples until it reaches the start.
        // An anchor this far back throws away most of what the game has verified. That is
        // sometimes exactly right -- lv19 only reaches the lift ride it needs by re-anchoring
        // deep -- so it is not forbidden; it just has to be worth it. A doomed tail from here
        // buys nothing and costs the whole prefix, and taking one is how the loop ended up
        // replaying 231-input plans that died in the opening sections. Below, `deepOnlyIfSolved`
        // makes such a rung acceptable only on a tail that reaches the end.
        const bool restartScale = (dt > 200 && t0 < dt / 2);
        if (g_spentAnchors.count(t0)) {
            writeResult("dpsolve:   anchor t=" + std::to_string(t0)
                        + " has already been tried and led nowhere - skipping this rung");
            continue;
        }
        const AnchorRow* r = anchors::row(t0);
        if (!r) {
            writeResult("dpsolve:   no recorded state for t=" + std::to_string(t0)
                        + " - skipping this rung");
            continue;
        }
        // An anchor far outside GD's own flight band is a state that is alive but no longer on
        // the board (a cube that missed its landing is not killed; it rises forever while x
        // keeps growing). Re-anchoring there sends the search off after a route that does not
        // exist. The margin is wide because legitimate play does leave the band: verified runs
        // clear its ceiling by up to 1,083px in rotated and tower sections.
        if (r->pmax > r->pmin
            && !(r->y >= r->pmin - 1200.f && r->y <= r->pmax + 1200.f)) {
            writeResult("dpsolve:   anchor t=" + std::to_string(t0) + " y="
                        + std::to_string((int)r->y) + " is off the board (band "
                        + std::to_string((int)r->pmin) + ".."
                        + std::to_string((int)r->pmax) + ") - skipping this rung");
            continue;
        }
        const int held = heldBefore(g_plan, t0);
        const std::string arg = startArg(t0, *r, held);
        std::vector<std::string> a = baseArgs(g_tailPath);
        // The forcing slit rides ONLY the forced rung (the hint's re-anchor); every other
        // rung and every later solve is free to answer differently.
        if (rungIdx < nForced && !forceBand.empty()) {
            a.push_back("--deadband");
            a.push_back(forceBand);
        }
        {   // 2900s the recorded run consumed before this anchor (see spentRotArg)
            const std::string sr = spentRotArg(t0);
            if (!sr.empty()) { a.push_back("--spentrot"); a.push_back(sr); }
            // ...and the recording's trigger ticks in rotated territory
            // (--trigraw; same wiring as the fixup resim above)
            if (!g_rotObjs.empty()) a.push_back("--trigraw");
        }
        a.push_back("--start");
        a.push_back(arg);
        std::string band;
        if (r->pmax > r->pmin) {
            band = num(r->pmin) + "," + num(r->pmax);
            a.push_back("--startband");
            a.push_back(band);
        }
        writeResult("dpsolve:   [anchor] --start " + arg
                    + (band.empty() ? "  (band guessed from x)" : "  --startband " + band));
        std::error_code ec;
        std::filesystem::remove(g_tailPath, ec);   // a stale tail must not read as this call's
        const int rc = dpbridge::solveInProcess(g_csv, a);
        const dpbridge::SolveOutcome o = dpbridge::outcome();
        std::vector<InputCmd> cand;
        loadInputsFile(g_tailPath, cand);
        // A tail with no inputs at all is a legal plan -- "never press" is what some stretches
        // want -- so the file's EXISTENCE is the test, not whether it parsed any lines.
        // loadInputsFile answers the second question, and using it for the first would throw
        // away exactly the plans that are already right.
        const bool haveFile = std::filesystem::exists(g_tailPath, ec);
        // The FORCED rung is exempt from the depth bar: its entire point is to let the
        // game answer whether the portal route works, and the model's opinion of that
        // route's depth is the one thing not trusted here -- the door-chain effects
        // past the portal only exist in the model AFTER a replay records them, so the
        // model's robot tail dying at t=16,221 says nothing (and the bar itself was a
        // wedge credit at 17,097: the sterile plan gate-keeping its own replacement).
        const bool forcedRung = (rungIdx < nForced);
        // The restart rung used to bin every PARTIAL ("only a complete route
        // is worth the prefix") -- and binned one reaching x=4,389 while the
        // wedged prefix it was protecting had never verified past x=2,201
        // (lv22's rotation gate, 2026-08-26; the ladder then gave up at 9%).
        // A partial that OUT-REACHES everything the game has verified cannot
        // be a downgrade: adopting it queues a replay, the game rules on it,
        // and a phantom comes back as veto credit. The +40 keeps equal-depth
        // churn out (and rotated sections, where x is not monotone, from
        // flapping on noise).
        const bool usable = haveFile
            && (o.verdict == dpbridge::OutcomeSolved
                || (forcedRung && o.verdict == dpbridge::OutcomePartial
                    && o.deepT > 0 && !cand.empty())
                || (!restartScale && o.verdict == dpbridge::OutcomePartial
                    && o.deepT > dt)
                // No deepT bar here: in rotated territory x is not monotone
                // in t, and the x=2,331 partial that finally out-reached the
                // gate peaked at t=1,795 -- EARLIER than the t=1,838 death it
                // was replacing (measured on the 194-round run this line
                // ended). The depth bar alone carries the claim.
                || (restartScale && o.verdict == dpbridge::OutcomePartial
                    && o.deepT > 0 && o.deepX > (double)g_hudVerifiedX + 40.0));
        char b[280];
        char deep[64] = "";
        if (o.deepT >= 0) snprintf(deep, sizeof(deep), " t=%lld x=%.0f", o.deepT, o.deepX);
        // capHits is reported even though nothing acts on it: it is the one number that says
        // whether the search ran out of capacity or out of physics, and the rejected tier
        // ladder above is the reason that distinction has to stay visible
        snprintf(b, sizeof(b), "dpsolve:   [%s%s] rc=%d inputs=%zu capHits=%lld%s",
                 o.verdict == dpbridge::OutcomeSolved ? "SOLVED"
                     : (o.verdict == dpbridge::OutcomePartial ? "PARTIAL" : "FAILED"),
                 deep, rc, cand.size(), o.capHits,
                 usable ? "" : (!haveFile ? " - no tail written"
                              : (restartScale ? " - doomed, and this far back only a complete "
                                                "route is worth the prefix"
                                              : " - doomed, backing off")));
        writeResult(b);
        // The frontier was empty before a single tick ran, and the call required a box the
        // anchor is already past. That is not the level being impassable, it is a requirement
        // that cannot be met from here -- drop the box and let the ladder ask again.
        const unsigned stuckBoxes = o.needTrigMask & o.needTrigPassed & ~g_needTrigDropped;
        if (!usable && stuckBoxes && o.deepT <= 0 && !(g_needTrigSuspect & stuckBoxes)) {
            g_needTrigSuspect |= stuckBoxes;
            snprintf(b, sizeof(b), "dpsolve:   the frontier was empty before tick 1: the search "
                     "was told to enter a box this anchor has already passed (mask 0x%x) - "
                     "noted, released only if the run stops getting anywhere", stuckBoxes);
            writeResult(b);
        }
        // ...and the AHEAD-of-the-anchor version of the same hole. A box that moves nothing
        // recordable is UNSEEN forever (seen = its objects moved in a recording), so needtrig
        // keeps demanding it on every rung -- and a demand the route cannot meet (a skull box,
        // a box below the floor) EMPTIES the frontier at the box's expiry instead of before
        // tick 1. Measured on lv22's cold run (2026-08-26 night): boxes (2807,143)/(2961,197)/
        // (3049,259) plus the three red skulls steered every plan into the hazard carpet,
        // every rung came back doomed with deepT>0, and the release below never armed because
        // only the deepT<=0 path fed it. Suspicion is cheap: escalate() still gates the actual
        // release behind "the run has stopped getting anywhere", so a door that is genuinely
        // needed keeps its pressure until the run is already out of other options.
        const unsigned aheadBoxes =
            o.needTrigMask & ~o.needTrigPassed & ~g_needTrigDropped & ~g_needTrigSuspect;
        if (!usable && aheadBoxes && o.deepT > 0) {
            g_needTrigSuspect |= aheadBoxes;
            snprintf(b, sizeof(b), "dpsolve:   a doomed rung was still being asked for boxes "
                     "ahead of it (mask 0x%x) - noted, released only if the run stops getting "
                     "anywhere", aheadBoxes);
            writeResult(b);
        }
        if (!usable) continue;
        tail = std::move(cand);
        chosenT = t0;
        chosenBackoff = bo;
        solvedTail = (o.verdict == dpbridge::OutcomeSolved);
        // The forced (portal) rung's tail gets a follow grace -- see the branch at the
        // rewind decision. Set on the choice, not the splice, so only this rung grants it.
        if (rungIdx < nForced) g_followForced = 6;
        break;
    }
    if (chosenT < 0) return false;

    // Splice. The prefix is cut at `< t0`, the same window heldBefore uses -- cutting one tick
    // earlier drops an input the tail was told is still held.
    //
    // Nothing is inserted at the seam. Releasing the button there (which an earlier version of
    // the driver did) breaks a tail that was solved on the assumption the button is down, and
    // the fixed position it used was one tick early for every mode with input latency 1. The
    // tail emits its own release at whatever tick its mode calls for.
    std::vector<InputCmd> next;
    for (const InputCmd& c : g_plan) {
        if (c.step >= chosenT) break;
        next.push_back(c);
    }
    const size_t nPrefix = next.size();
    next.insert(next.end(), tail.begin(), tail.end());
    g_plan.swap(next);
    g_curBackoff = chosenBackoff;
    g_lastTailSolved = solvedTail;
    g_anchorT = chosenT;
    g_anchorX = anchors::row(chosenT) ? anchors::row(chosenT)->x : 0.f;
    char b[224];
    snprintf(b, sizeof(b), "dpsolve:   re-anchored at t=%lld (backoff %d): %zu kept + %zu new "
             "= %zu inputs", chosenT, chosenBackoff, nPrefix, tail.size(), g_plan.size());
    writeResult(b);
    return true;
}

// ---- starting a job ----
// Two jobs: the first solve of the level, and a repair. Both leave the level frozen while the
// solver thread works and hand the answer back at a frame boundary, which is the only place it
// is safe to touch the level.
// JobSeedPlan: [2026-08-24] `dpseedplan=<path>` skips the FIRST leveldp call and installs a plan
// from disk instead -- GD still verifies it for real (the loop's own invariant, "the prefix that
// GD actually replayed is true by construction", is untouched), only the redundant search that
// would have re-derived the first N iterations' worth of fixups and vetoes is skipped.
//
// Why this exists: lv22's 84% wall took ~170 iterations and ~25 minutes to REACH on every cold
// run, before any change AT the wall could be measured at all. Testing an idea about what happens
// past the wall meant paying that cost every time. Development is explicitly allowed to use a
// resumed/seeded plan (CLAUDE.local.md's 2026-08-12 ruling); only the accepted, reported result
// has to come from a cold run with this left unset.
//
// The seed is a plain plan file (loadInputsFile's format, same as a saved solution) -- typically
// an earlier run's dp_plan.txt/dp_best.txt or a saved solution_lvN_dp.txt. If GD does not reach at
// least as far with it as the seed's own history says it should, that is itself a finding (the
// level, the mod, or the seed changed) and the run proceeds from wherever GD actually put it.
enum JobKind { JobFirstSolve = 0, JobLadder = 1, JobSeedPlan = 2 };

inline void spawn(int kind, long long arg, const char* phase) {
    g_running = true;
    g_finished = false;
    g_rc = -1;
    g_haveNewPlan = false;
    g_t0 = std::chrono::steady_clock::now();
    g_dpSolving = true;    // the badge and the session HUD say SOLVING from here
    // Hold the level still while the solver works. Without this the player runs into the first
    // hazard over and over for the length of the solve, which is exactly what it looks like
    // when the mod is doing nothing at all -- the one impression this must not give
    g_paused = true;
    g_hudPhase = phase;
    const int gen = g_generation.load();   // this session's generation, read on the main thread
    std::thread([kind, arg, gen]() {
        bool ok = false;
        try {
            if (kind == JobFirstSolve) {
                std::error_code ec;
                std::filesystem::remove(g_planPath, ec);
                g_rc = dpbridge::solveInProcess(g_csv, baseArgs(g_planPath));
                ok = loadInputsFile(g_planPath, g_plan);
            } else if (kind == JobSeedPlan) {
                g_rc = 0;
                ok = loadInputsFile(g_cfg.dpSeedPlan, g_plan);
            } else {
                // Put the plan GD just replayed on disk. The fixup resim runs it through the
                // model with `--replay`, and the file is the only way to hand it over -- while
                // the plan itself lives in memory and is respliced every iteration. Without
                // this the resim replays whatever the last solver call happened to write, so
                // the model is running one plan and GD ran another, and EVERY tick of the
                // comparison is a divergence. Measured on lv18: it cleared in one round without
                // fixups and ground out all 41 with them.
                writeInputsFile(g_planPath, g_plan);
                // Learn first, then search. The recorder reads the trajectory GD just flew, so
                // it has to run before anything resets the level -- and the ladder's first call
                // should already have the benefit of it.
                const int fixBefore = g_fixupCount;
                recordFixups(g_lastDeath);
                if (g_fixupCount > fixBefore) {
                    // The model is not what it was. Anchors that were doomed under the old one
                    // are open questions again, and the shallow rungs -- the ones that keep the
                    // most of the verified prefix -- deserve the first look.
                    g_spentAnchors.clear();
                    g_curBackoff = kBackOff;
                    writeResult("dpsolve:   the model learnt something - every anchor is worth "
                                "another look, starting from the shallowest");
                }
                ok = runLadder(arg);
            }
        } catch (...) {
            ok = false;    // never let an exception cross back into the game's frame
        }
        g_haveNewPlan = ok;
        g_resultGeneration = gen;   // set before g_finished, same ordering convention as g_rc
        g_finished = true;
    }).detach();
}

// What begins the run: a real search, or a seeded plan waiting to be verified. Both call sites
// that used to spawn(JobFirstSolve, ...) directly go through this instead (see JobSeedPlan).
inline void beginFirstAttempt() {
    if (!g_cfg.dpSeedPlan.empty()) {
        writeResult("dpsolve: seeding the plan from " + g_cfg.dpSeedPlan
                    + " instead of solving for it - the game still verifies it for real");
        spawn(JobSeedPlan, 0, "verifying a seeded plan before searching from where it lands");
    } else {
        spawn(JobFirstSolve, 0, "the level is held still until the plan is ready");
    }
}

// Kick off the first solve for the level currently loaded. Safe to call more than once: only
// the first one runs immediately -- a call that arrives while the previous session's worker
// thread is still occupying the job slot is queued instead (see g_pendingLayer) rather than
// silently dropped, which used to leave a new Solve session's HUD reading SOLVING forever with
// nothing behind it.
// A PLATFORMER LEVEL IS OUTSIDE THE FORMULATION, not merely unmeasured, and the
// difference matters more than the word does. Everything below rests on two
// properties a platformer does not have: the level sets the forward speed, so
// the plan is one binary decision per physics tick and nothing else; and the run
// is one pass, so a tick is reached once. A platformer has steering, the player
// can stop, turn round and re-cross the same x, and "the input at tick t" no
// longer describes the run at all.
//
// Handed one anyway, the solver used to SOLVE IT -- badly, silently, and with a
// straight face. Refusing is not a smaller failure than being wrong here, it is
// a different kind: an answer nobody can check versus a sentence saying there is
// no answer. Being wrong slowly is the thing this project is built to avoid, and
// this was its sharpest remaining instance.
//
// Both flags are read. m_isPlatformer is the layer's own, i.e. what the physics
// is actually running; isPlatformer() is the level's. They should agree, and the
// point of asking twice is that if they ever do not, the refusal is the safe
// side of the disagreement.
inline bool refuseIfPlatformer(GJBaseGameLayer* l) {
    bool plat = l->m_isPlatformer;
    if (auto* pl = PlayLayer::get())
        if (pl->m_level && pl->m_level->isPlatformer()) plat = true;
    if (!plat) return false;
    if (g_sessionOver) return true;   // one ending per session
    const char* note =
        "gdsolver: this is a PLATFORMER level - not solving it";
    writeResult("dpsolve: refusing - a platformer has steering and no forced "
                "scroll, so \"the input at tick t\" does not describe the run. "
                "The solver is not wrong here, it does not apply. Nothing was "
                "searched and no plan was produced.");
    notify::show(note, NotificationIcon::Error, 6.f);
    g_paused = true;
    endSession("platformer_unsupported");
    return true;
}

inline void start(GJBaseGameLayer* l) {
    if (!l) return;
    if (refuseIfPlatformer(l)) return;
    // Bumped on every request, queued or not: this generation identifies the SESSION that is
    // asking, not the job currently occupying the slot. Bumping it only in the non-queued branch
    // (as an earlier version of this fix did) left it unchanged while a request sat queued --
    // so when the OLD session's orphaned worker finally reported in, its generation still
    // matched (nothing had moved it), poll()'s discard check let it through, and its plan/g_iter/
    // g_best (a different level's) got installed into THIS session. Measured (2026-08-24): a
    // fresh Solve queued behind a still-running previous one inherited iter=24/best=521 from the
    // level before it, with no "dpsolve: start" line ever logged for the new session -- the
    // giveaway that start() had taken the queued branch and skipped its own reset.
    ++g_generation;
    if (g_running.load()) { g_pendingLayer = l; return; }
    g_pendingLayer = nullptr;
    std::ostringstream oss;
    solver::writeObjRects(oss, l);
    g_csv = oss.str();
    loadModePortals(g_csv);    // where the mode portals are; see missedPortalTick
    loadRotObjs(g_csv);        // ...and the 2900s, for --spentrot (see spentRotArg)
    g_triedMissedPortal = false;
    g_forceAnchorT = -1;
    g_planPath = std::string(DATA_DIR) + "/dp_plan.txt";
    g_tailPath = std::string(DATA_DIR) + "/dp_tail.txt";
    // Fixups are per RUN. A solve starts from the untouched model, so what a previous run
    // measured must not be lying around -- that is the cold rule, and a stale file would make
    // the next solve start with knowledge it did not earn.
    g_fixupPath = std::string(DATA_DIR) + "/dp_fixups.txt";
    g_fixupNoopPath = std::string(DATA_DIR) + "/dp_fixups_noop.txt";
    g_groupsPath = std::string(DATA_DIR) + "/dp_groups.txt";
    g_groupsDeepPath = std::string(DATA_DIR) + "/dp_groups_deep.txt";
    g_groupsBootPath = std::string(DATA_DIR) + "/dp_groups_boot.txt";
    {
        std::error_code ec;
        std::filesystem::remove(g_fixupPath, ec);
        std::filesystem::remove(g_fixupNoopPath, ec);
        std::filesystem::remove(g_groupsPath, ec);   // the recordings are this run's, too
        std::filesystem::remove(g_groupsDeepPath, ec);
        std::filesystem::remove(g_groupsBootPath, ec);
    }
    g_fixupCount = 0;
    g_fixupNoop = 0;
    g_groupsDepth = -1;
    g_deepActive = false;
    g_recordAttempt = false;
    g_deepDoneAt = -1;
    g_recordKind = RecNone;
    g_recordRequest = false;
    g_coldRestarted = false;
    g_escalations = 0;
    g_capTier = 0;
    g_needTrigDropped = 0;
    g_needTrigSuspect = 0;
    g_spentAnchors.clear();
    // The vetoes are THIS run's observations, the same as the fixups above: a box the game
    // refuted on one level says nothing about the next, and carrying one into a second solve in
    // the same GD session would close ground that was never tested.
    g_phantomHits.clear();
    g_phantomScale.clear();
    g_phantomBands.clear();
    g_phantomLifted = false;
    g_killVetoIter = -1;
    g_forcePortalBand.clear();
    g_cfg.noDeath = false;
    // How long a plan to ask for. Ticks, not pixels: the player advances between 1.01 px
    // (speed 0.7) and 2.8 px per tick, so "one tick per pixel of level" is the worst case with
    // room to spare. The search stops early when it reaches the end, so over-asking is free;
    // under-asking is what makes a solved plan end in mid-air.
    g_horizonFull = g_cfg.dpHorizon;
    if (g_horizonFull <= 0) {
        g_horizonFull = (int)(solver::g_levelMaxX) + 2000;
        if (g_horizonFull < 3000) g_horizonFull = 3000;
    }
    g_horizon = g_horizonFull;     // the bound the no-death pass is allowed
    g_horizonNow = g_horizonFull;
    g_stallRuns = 0;
    g_needUnseen = false;
    g_iter = 0;
    g_plan.clear();
    g_best.clear();
    g_bestDeath = -1;
    g_lastDeath = -1;
    g_deathRuns.clear();
    g_curBackoff = kBackOff;
    g_lastTailSolved = false;
    g_followSolved = 0;
    g_followForced = 0;
    g_anchorT = -1;
    g_stop = false;
    g_showRequest = false;
    g_dpShowSolution = false;
    anchors::reset();
    writeResult("dpsolve: start level=" + std::to_string(g_cfg.levelId)
                + " csv=" + std::to_string(g_csv.size()) + " bytes horizon="
                + std::to_string(g_horizon));
    // On a level with moving parts, look before solving: one input-free pass records what the
    // level does by itself, and the first search then plans in a world that moves. Without it
    // the first plan is made against geometry frozen where it started, and on a level whose
    // first wall IS a moving part there is no way to earn the recording -- passing the wall is
    // the only thing that would record it. The solve is started when that pass finishes.
    if (!startBootstrapRecord())
        beginFirstAttempt();
}

// One line per iteration that pins the whole state of the loop, so that a change meant to be
// behaviour-preserving can be proven against a previous run instead of asserted.
//
// The fields are the installed plan, the game's verdict on it,
// the flight band the attempt started in (GD carries pmin/pmax across retries, so this changes
// what the DP is told and belongs in the print), the fixup file, the two ladder dials, and the
// two recordings the next solve will overlay.
//
// Files are summed as size/hash rather than compared whole -- the recordings run to tens of MB
// and the point is only whether two runs fed the solver the same bytes.
inline std::string fileSig(const std::string& path) {
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec)) return "-";
    std::ifstream f(path, std::ios::binary);
    if (!f) return "-";
    // FNV-1a over the file, printed like the driver's md5 prefix: the value only has to be
    // stable within one build, never portable, and the recordings are far too big for a real
    // digest to be free here.
    uint64_t h = 1469598103934665603ULL;
    size_t n = 0;
    char buf[65536];
    while (f.read(buf, sizeof(buf)) || f.gcount()) {
        const size_t got = (size_t)f.gcount();
        n += got;
        for (size_t i = 0; i < got; ++i) {
            h ^= (uint8_t)buf[i];
            h *= 1099511628211ULL;
        }
    }
    char out[48];
    snprintf(out, sizeof(out), "%zu/%08x", n, (unsigned)(h & 0xffffffffULL));
    return out;
}

inline void logFingerprint(long long dt, double deathX) {
    if (!g_cfg.dpFingerprint) return;
    // The plan is in memory, so it is summed the same way over its own edges.
    uint64_t ph = 1469598103934665603ULL;
    for (const InputCmd& c : g_plan) {
        const uint64_t v = (uint64_t)c.step * 2u + (uint64_t)(c.down ? 1 : 0);
        for (int i = 0; i < 8; ++i) {
            ph ^= (uint8_t)(v >> (i * 8));
            ph *= 1099511628211ULL;
        }
    }
    // The band the ATTEMPT started in, not the one it died in (see the note above).
    const std::vector<AnchorRow>* savedSrc = anchors::g_src;
    anchors::ladderOn(false);
    const AnchorRow* r1 = anchors::row(1);
    anchors::g_src = savedSrc;
    char band[48] = "-";
    if (r1) snprintf(band, sizeof(band), "%.1f,%.1f", (double)r1->pmin, (double)r1->pmax);
    char b[384];
    snprintf(b, sizeof(b),
             "dpsolve:   [fp] it=%d plan=%zu/%08x att=%d t=%lld x=%.3f band1=%s "
             "fix=%d/%s horizon=%d backoff=%d live=%s deep=%s",
             g_iter, g_plan.size(), (unsigned)(ph & 0xffffffffULL), g_attempt,
             dt, deathX, band, g_fixupCount, fileSig(g_fixupPath).c_str(),
             g_horizonNow, g_curBackoff, fileSig(g_groupsPath).c_str(),
             fileSig(g_groupsDeepPath).c_str());
    writeResult(b);
}

// Stop, and say where it got to rather than why the machinery stopped.
//
// "no anchor on this prefix can be solved" is an accurate sentence about the ladder and tells a
// player nothing: not which level, not how far it got, not whether it was close. What is worth
// putting on screen is the distance -- a run that stops at 5% has hit something the model does
// not implement, and one that stops at 95% is a different conversation. The internal reason
// stays in result.txt, where it is the first thing anyone debugging would read.
inline void giveUp(const char* reason, const char* sessionWhy) {
    if (g_sessionOver) return;   // one ending per session
    double lvl = 0.0;
    if (auto* pl = PlayLayer::get()) lvl = pl->m_levelLength;
    const double pct = (lvl > 1.0) ? (double)g_hudVerifiedX / lvl * 100.0 : 0.0;
    char note[224];
    snprintf(note, sizeof(note),
             "gdsolver: could not solve lv%d - stopped at %.0f%% (x %.0f) after %d round%s",
             g_cfg.levelId, pct, (double)g_hudVerifiedX, g_iter, g_iter == 1 ? "" : "s");
    writeResult(std::string("dpsolve: giving up - ") + reason + " | " + note);
    // Keep the iteration map. A run that did NOT clear is the one whose map is worth reading --
    // the level is left standing where the loop stopped, and F10 now draws every round it spent
    // getting there over the top of it.
    // "itermap", not "iteration map": py/cold_regress.py matches `^dpsolve: iter (\d+):` for the
    // round count, and a line that starts `dpsolve: iteration ...` is one loosened regex away
    // from being counted as a round.
    if (itermap::save(g_cfg.levelId, false))
        writeResult("dpsolve: itermap saved -> " + itermap::pathFor(g_cfg.levelId));
    notify::show(note, NotificationIcon::Error, 6.f);
    // Stop the level where it is and leave it there. endSession gives the screen back, so what
    // was a black window during the solve becomes the level at the point the bot stopped, with
    // the overlay saying how far it got. Whether to leave is the player's call (F9); the run is
    // over either way and nothing is being driven.
    g_paused = true;
    endSession(sessionWhy);
}

// The replay just died. Decide which plan the next iteration starts from, then re-solve.
//
// Three cases, and the middle one is the reason this is not just "keep the deepest":
//   * deeper than anything before  -> that is progress; keep it and reset the back-off
//   * a regression, but the tail we spliced last time reached the end -> this is not the same
//     plan doing worse, it is a DIFFERENT branch that the model carried all the way. GD dying
//     early on it is a fidelity hole at the seam, which is precisely what re-anchoring absorbs.
//     Rewinding here would go back to a branch the model already knows dies.
//   * otherwise -> restore the deepest verified plan and back off further.
// Take this replay's record of what the moving geometry did, if it is the deepest one so far.
//
// DEEPEST, not latest. The loop backs off constantly, so most iterations are short replays of a
// prefix; overwriting with the latest would keep throwing away what a deep run saw, and a door
// the player once opened would go back to being shut. The recording is the run's own -- its own
// replay of its own plan -- so nothing external enters the cold loop.
inline void harvestGroups() {
    if (!grouptrace::g_on || g_groupsPath.empty()) return;
    const grouptrace::Roll r = grouptrace::g_lastRoll;
    if (r.rows <= 0 || r.depth <= g_groupsDepth) return;
    std::error_code ec;
    std::filesystem::copy_file(std::string(DATA_DIR) + "/grouptrace_last.txt", g_groupsPath,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return;
    g_groupsDepth = r.depth;
    char b[192];
    snprintf(b, sizeof(b), "dpsolve:   moving geometry: kept this run's record "
             "(%lld rows, depth t=%lld)", r.rows, r.depth);
    writeResult(b);
}

// "The model was sure, the game said no, here, again." Count it, and once a site has said it
// kPhantomAfter times, refuse to plan through that small box for the rest of the run.
//
// Only a SOLVED tail counts. A PARTIAL tail is the model agreeing that the route dies, which is
// not a phantom -- it is the search doing its job -- and vetoing there would close ground the run
// still has to cross. The box is drawn around the state the GAME was in at the death (anchors::
// row is that recording), not around the model's idea of it.
inline void checkPhantom(long long dt, double deathX, bool wedged = false,
                         bool force = false) {
    // A WEDGE needs no tail verdict: whatever plan drove it expected to keep advancing and
    // the game froze the player instead -- as strong a refutation as a killed SOLVED tail.
    // Gated only on g_lastTailSolved, 128 wedge deaths at one site counted for nothing,
    // because the deepest-plan replays that produce them splice no new tail and the stale
    // verdict from the last real splice was PARTIAL.
    if ((!g_lastTailSolved && !wedged) || g_phantomLifted) return;
    const long long site = (long long)std::floor(deathX / 8.0);
    // FORCE = the tick-bucket credit fired (8 deaths in one dt/4 bucket). That
    // evidence is already past the 4 hits this counter waits for, but counted
    // as ONE hit it needs 3 more rounds the ladder may not have: lv22's
    // rotation gate died 8x at t=1838, the credit fired on the last death, and
    // the ladder burnt its whole no-anchor escape sequence and gave up at 9%
    // with no box ever dropped (2026-08-26). A refuted bucket fills the
    // counter outright; the box dedupe and the widening ladder above still
    // absorb the repeats.
    const int n = (g_phantomHits[site] += (force ? kPhantomAfter : 1));
    if (n < kPhantomAfter) return;
    if ((int)g_phantomBands.size() >= kPhantomMaxBands) return;
    // Pinned to the attempt that just died, like every other reader of a death tick (the
    // credits above, recordFixups). Through the ladder's leftover g_src this mostly worked
    // by accident -- the loop usually follows the deepest plan, so dead and deepest agree --
    // but a credited death tick (wedge/off-board) has no row at all on a stale trajectory.
    const std::vector<AnchorRow>* savedSrc = anchors::g_src;
    anchors::ladderOn(false);
    const AnchorRow* r = anchors::row(dt);   // AnchorRow is p1's, the accessor is anchors'
    anchors::g_src = savedSrc;
    if (!r) return;
    // Counting starts again toward the NEXT box at this site (see the dedupe below).
    g_phantomHits[site] = 0;
    char band[96];
    // mode -2-M = "only mode M dies here" (dp/modifiers.hpp) -- exactly what the evidence
    // supports: the game refuted a tail that was in mode M at this state, and it says nothing
    // about the other modes. The old -1 ("nothing survives") closed lv22's shaft wedge point
    // for the ROBOT too: the reference route climbs through the very box the wedged SHIP
    // earns, so the portal re-anchor's robot branch was killed by the ship's own veto and
    // could never out-live it.
    // [2026-08-26] A REPEATED identical refutation WIDENS the box (x2 per
    // repeat, capped at x32). The dedupe below used to just return on an
    // identical band -- and the hit counter was already reset -- so a wall
    // whose death state recurs with identical numbers (the lv22 corridor:
    // (8,286, y~1,076) and (9,827, y~1,738), the same figures across six
    // runs) could never escalate past its first 8x20 box, and the run
    // oscillated between two already-boxed points for 70+ minutes at a time.
    // Growing the box is the same claim made honestly at larger scale ("the
    // game killed a believed plan HERE, and point-closure did not move the
    // route") -- it is what the driver's hand-authored corridor band did by
    // hand -- and liftPhantomVetoes stays as the insurance if a wide box
    // ever swallows the live route.
    const long long scaleKey = site * 16 + r->mode;
    const int scale = g_phantomScale[scaleKey];
    const double pdx = kPhantomDx * (double)(1 << scale);
    const double pdy = kPhantomDy * (double)(1 << scale);
    snprintf(band, sizeof(band), "%.1f,%.1f,%d,%.1f,%.1f",
             (double)r->x - pdx, (double)r->x + pdx,
             -2 - r->mode,
             (double)r->y - pdy, (double)r->y + pdy);
    // [2026-08-24] Deduped on the BOX, not on the site. One site can host a whole FAMILY of
    // phantom routes, and one box per site closes exactly one of them however many times the
    // rest come back -- the driver's hand-authored band for lv22's dead end spans y 0..1,300 for
    // that reason, against the 20px this draws. Repeats of the SAME state still cost nothing,
    // and each new box is still only ever "the game killed a plan the model believed in, here";
    // the claim does not weaken by being made twice.
    //
    // NOT DEMONSTRATED to move lv22's 84% wall, which is what prompted it: the box is drawn
    // around GD's OWN death state, and there GD died at y=625.3 every single time, so the second
    // box never had different numbers to hold. (The wall turned out to be upstream of the veto
    // entirely -- the model never fires the 2900 at (20115,723); see the handoff.) Kept because
    // the one-per-site limit is real regardless of whether this level exercises it.
    for (const std::string& prev : g_phantomBands)
        if (prev == band) {
            // the same box again: escalate the NEXT derivation at this site,
            // as far as kPhantomScaleMax (see its note for why that is 2 and
            // not the 5 this started at)
            if (g_phantomScale[scaleKey] < kPhantomScaleMax)
                ++g_phantomScale[scaleKey];
            return;
        }
    g_phantomBands.emplace_back(band);
    // On the map as a wash over the stretch the box covers (itermap.hpp). Only the x extent: the
    // y bounds and the mode are what makes the box honest, but on a picture of the level they
    // would draw a shape that reads as "the route goes around here", which is the one thing a
    // veto explicitly does not claim.
    itermap::addVeto(g_iter, (float)((double)r->x - pdx), (float)((double)r->x + pdx));
    char b[224];
    snprintf(b, sizeof(b), "dpsolve:   [veto] a SOLVED tail died in the game %d times at t=%lld "
             "(x=%.0f) - dropping the box: --deadband %s [%d]",
             n, dt, (double)r->x, band, (int)g_phantomBands.size());
    writeResult(b);
    // ...and a confirmed phantom is NEW EVIDENCE THAT THIS WALL IS A ROUTE ERROR, so the missed
    // mode portal is worth asking about again. That hint is otherwise once per wall, and at a
    // wall that never moves "once per wall" is once per run: lv22's 84% wall was named correctly
    // ("it crossed a mode portal at t=15,858 and came out in the wrong mode") and then never
    // revisited, while 180 rounds went by. Re-asking is not repeating the same question either --
    // the boxes above are in force now, so the solve from the crossing searches a space the first
    // attempt did not have.
    g_triedMissedPortal = false;
}

// Before declaring the level unsolved, give every veto back once. A veto is a box the game was
// SEEN to kill in, so letting it back in cannot make the game less true -- this is insurance
// against a box that was drawn wide enough to swallow a live route with the dead one. Once only,
// or the run can cycle veto -> stuck -> release -> phantom -> veto forever.
inline bool liftPhantomVetoes(const char* where) {
    if (g_phantomBands.empty() || g_phantomLifted) return false;
    char b[192];
    snprintf(b, sizeof(b), "dpsolve:   [veto] out of anchors at %s - giving all %d boxes back for "
             "one last attempt (no more vetoes after this)", where, (int)g_phantomBands.size());
    writeResult(b);
    g_phantomBands.clear();
    g_phantomLifted = true;
    return true;
}

inline void onDeath(long long dt, float deathX) {
    // Once the solve is over, a death is just a death: the showing of the solution is a plain
    // replay and must not restart the search behind it
    if (!g_cfg.dpSolve || g_dpShowSolution || g_stop || g_running.load()) return;
    // A VOID attempt: a death in the first moments of a run that recorded (nearly)
    // nothing, while the banked recording is rich. The measured producer (lv8/11/17,
    // 2026-08-25) is the stall guard's deferred reset on a zombie attempt: the level's
    // completion state leaks across resetLevel, the endscreen fires at the NEXT
    // attempt's t=0, the false-clear gate refuses it and reports death t=0 -- and the
    // empty recording then BANKED OVER the ladder's anchors, so every rung read "no
    // recorded state" and the run gave up at 40-85% with a healthy best. Scored as a
    // repeat of the last real death instead, with the buffers untouched: the ladder,
    // the wedge credit and the follow logic keep working off real data, and the next
    // attempt replays the installed plan normally.
    const bool voidAttempt =
        anchors::g_live.size() < 50 && anchors::g_dead.size() >= 500
        && dt < 600 && g_lastDeath > 3000;
    if (voidAttempt) {
        char vb[192];
        snprintf(vb, sizeof(vb), "dpsolve:   void attempt (t=%lld, %zu rows recorded) - "
                 "scored as a repeat of the last real death (t=%lld)",
                 dt, (size_t)anchors::g_live.size(), (long long)g_lastDeath);
        writeResult(vb);
        anchors::g_live.clear();
        dt = g_lastDeath;
        deathX = g_lastDeathX;
    } else {
        anchors::bank();
    }
    harvestGroups();     // rollGroupTrace has already committed this run's recording
    ++g_iter;
    // A run that left the playfield is credited where it left it, not where it eventually
    // stopped. Everything after that point is an arc through empty sky: it gains x, so it looks
    // like the best run yet, and refining it is refining nothing. Scored here, once, so every
    // decision below -- deepest, rewind, ladder, fixups -- works off the same number.
    {
        // Same source pinning as the wedge credit below: this asks about the attempt that
        // just died, not about whatever trajectory the previous ladder pointed g_src at.
        const std::vector<AnchorRow>* savedSrcOb = anchors::g_src;
        anchors::ladderOn(false);
        float offX = 0.f;
        const long long offT = offBoardTick(dt, offX);
        anchors::g_src = savedSrcOb;
        if (offT > 0 && offT < dt) {
            char ob[224];
            snprintf(ob, sizeof(ob), "dpsolve:   left the playfield at t=%lld x=%.0f - crediting "
                     "the death there, not at t=%lld x=%.0f", offT, (double)offX, dt,
                     (double)deathX);
            writeResult(ob);
            dt = offT;
            deathX = offX;
        }
    }
    bool wasWedged = false;
    // ...and a WEDGED run is credited where it stopped moving, for the same reason. The stall
    // guard (hooks_gamelayer) ends an attempt whose player has not moved for 30,000 ticks, so
    // the death: line arrives with a tick inflated by the whole idle stretch -- and the loop
    // scores depth by tick, so one wedge at x=20,115 (t_stall+30,000) outranks every honest
    // death in the level forever and the deepest-plan rewind pins the run to it. lv22 grows
    // these naturally: the rotated-ship glitch route survives the shaft, never completes, and
    // ends only when the guard fires. 1,000 ticks of stillness is far beyond any legitimate
    // stand that ends in a death (the longest such in the corpus is the end-of-level pin at
    // 668), and a real wedge has 30,000 by construction.
    {
        // Explicitly against the attempt that just died. g_src still points at whatever the
        // previous iteration's ladder left it on (usually the deepest), whose rows end at the
        // honest depth -- row(inflated dt) came back null there and this whole credit was
        // silent while the trap it was written for ran free (measured: best t=57,751 and
        // climbing, all wedges).
        const std::vector<AnchorRow>* savedSrc = anchors::g_src;
        anchors::ladderOn(false);
        // The death tick itself has no row -- record() runs at the END of a physics tick and
        // the destroy happens inside one -- and past t=400,000 the recorder stops entirely
        // (its runaway cap). Walk down to the last recorded row first; on a wedge the position
        // there is the same frozen point, which is the whole premise of the scan.
        long long d0 = std::min(dt, 400000LL);
        while (d0 > 1 && !anchors::row(d0)) --d0;
        const AnchorRow* rd = anchors::row(d0);
        if (rd) {
            long long k = d0;
            while (k > 1) {
                const AnchorRow* rk = anchors::row(k - 1);
                if (!rk || std::fabs(rk->x - rd->x) > 0.5f
                    || std::fabs(rk->y - rd->y) > 0.5f)
                    break;
                --k;
            }
            if (dt - k >= 900) {
                char wb[224];
                snprintf(wb, sizeof(wb), "dpsolve:   wedged since t=%lld (x=%.0f, %lld still "
                         "ticks) - crediting the death there, not at t=%lld", k,
                         (double)rd->x, dt - k, dt);
                writeResult(wb);
                dt = k;
                deathX = rd->x;
                wasWedged = true;
            }
        }
        anchors::g_src = savedSrc;
    }
    // An attempt that had to be forced over (the moving zombie: alive, advancing nowhere
    // meaningful, never completing -- the overlong guard ends it at 40k ticks) is
    // wedge-class for every purpose: never ranks, counts toward the veto, re-arms the
    // portal hint. No legitimate attempt exceeds ~22k ticks on these levels.
    if (dt > 30000) wasWedged = true;
    // The HUD's iteration block is fed by the external driver through hud.txt. There is no
    // driver here, so the loop fills the same fields itself -- otherwise the panel's Solve mode
    // sits on "iter 0 starting" for the whole run
    g_hudIter = g_iter;
    if (deathX > g_hudVerifiedX) g_hudVerifiedX = deathX;
    char b[256];
    snprintf(b, sizeof(b), "dpsolve: iter %d: death t=%lld x=%.1f (best t=%lld)",
             g_iter, dt, (double)deathX, g_bestDeath);
    writeResult(b);
    logFingerprint(dt, deathX);
    // Another death in the same tick bucket; enough of them are veto credit
    // on their own (the note at g_deathRuns).
    const int sameDeaths = ++g_deathRuns[dt / 4];
    checkPhantom(dt, (double)deathX, wasWedged || sameDeaths >= 8,
                 sameDeaths >= 8);
    // A wedge IS the signature the missed-portal hint waits for -- a run alive past a mode
    // portal in a mode the section was not built for, frozen instead of killed -- so the hint
    // fires here directly instead of waiting the hours it takes the ladder to exhaust every
    // other idea first (measured: the previous run reached that escalation at round 334).
    // Same one-shot as the escalation path; if the crossing was actually taken,
    // missedPortalTick returns -1 and this stays silent.
    if (wasWedged && !g_triedMissedPortal) {
        anchors::ladderOn(false);
        const long long mt = missedPortalTick();
        if (mt > 0) {
            g_triedMissedPortal = true;
            g_forceAnchorT = mt - kPortalSettle;
            char fb[96];
            snprintf(fb, sizeof(fb), "%.1f,%.1f,%d,-1e18,1e18",
                     g_missedPortalX + 45.0, g_missedPortalX + 75.0, g_missedPortalMode);
            g_forcePortalBand = fb;
            char mb[256];
            snprintf(mb, sizeof(mb), "dpsolve:   the wedge names it - the run crossed a mode "
                     "portal at t=%lld in the wrong mode; going back there and forcing the "
                     "crossing (--deadband %s)", mt, fb);
            writeResult(mb);
        }
    }
    if (g_iter > g_cfg.dpMaxIters) {
        writeResult("dpsolve: iteration budget exhausted (" + std::to_string(g_cfg.dpMaxIters)
                    + ") - stopping");
        g_stop = true;
        g_hudPhase = "gave up: out of iterations";
        giveUp("it ran out of repair rounds", "dpsolve_budget");
        return;
    }
    long long useT = dt;
    // How this round gets scored, kept for the iteration map (itermap.hpp). A column of deaths
    // says where the loop spent its rounds; the kind is what says whether it was working through
    // a hard section or stuck against a wall, and only the branch below knows which.
    int outcome = itermap::KindRewind;
    // !wasWedged: a wedge never ranks. Its credited tick measures when the route ARRIVED at
    // the dead point, not how far it got -- any route that dawdles before wedging at the same
    // frozen state scores higher (measured: best walked 16,538 -> 17,237 -> 20,920, every one
    // of them the same (20115, 829) wedge), and the sterile wedge plan then owns the deepest
    // slot forever. The veto and the portal hint still fire off a wedge; only the depth
    // ranking is closed to it.
    if (dt > g_bestDeath && !wasWedged) {
        // The run got deeper. Whatever was turned on to get it moving has done its job, so the
        // expensive part of it is given back -- the full lookahead costs nothing while the model
        // is right, and a run that keeps a bounded one after it starts working pays for it on
        // every later search.
        g_stallRuns = 0;
        // Capacity is the expensive one -- the same anchor solves in 8 seconds at the base
        // setting and 56 at the top tier, for the same answer -- so it is the first thing given
        // back once the run is moving again.
        if (g_capTier != 0) {
            writeResult("dpsolve:   deeper - back to the cheap search");
            g_capTier = 0;
        }
        if (g_horizonNow != g_horizonFull) {
            snprintf(b, sizeof(b), "dpsolve:   deeper at t=%lld - back to planning the whole "
                     "level (%d ticks)", dt, g_horizonFull);
            writeResult(b);
            g_horizonNow = g_horizonFull;
        }
        g_bestDeath = dt;
        g_best = g_plan;
        g_spentAnchors.clear();     // a different wall; nothing is known about its anchors
        // ...and the run has crossed ground it had not crossed before, so there may be a mode
        // portal in it that it did not take. The hint is once per wall, not once per run.
        g_triedMissedPortal = false;
        // ...and so is the escalation BUDGET. The 12 in escalate() is a runaway backstop, but
        // it was counted over the whole run while every option it guards is chosen per wall --
        // so a level with several walls spent the budget on the early ones and reached the late
        // ones with nothing left to try. Measured on lv22: the runs that cleared the early
        // sections outright ground on to 184-213 rounds and reached x=20,135, while a run that
        // has to fight at x=2,266 and x=3,689 first gives up at round 99 with x=9,827 -- same
        // binary, same wall, different history. Progress is what re-arms it, so this cannot
        // loop forever: a wall that yields nothing still stops after 12.
        g_escalations = 0;
        anchors::keepAsDeepest();   // the plan and the trajectory that produced it, together
        anchors::ladderOn(false);
        g_curBackoff = kBackOff;
        g_followSolved = 0;
        g_followForced = 0;    // the route made progress; the normal flow owns it now
        outcome = itermap::KindDeeper;
    } else if (g_lastTailSolved && (dt > g_lastDeath || g_followSolved < 4)) {
        outcome = itermap::KindFollow;
        g_followSolved = (dt > g_lastDeath) ? 0 : g_followSolved + 1;
        snprintf(b, sizeof(b), "dpsolve:   regression to t=%lld on a solved branch - following "
                 "it (%d/4, best %lld)", dt, g_followSolved, g_bestDeath);
        writeResult(b);
        anchors::ladderOn(false);
        g_curBackoff = kBackOff;
    } else if (g_followForced > 0 && (dt > g_lastDeath || g_followForced > 1)) {
        // The FORCED route (the portal the hint made the solve take) gets the same grace a
        // solved branch does, for the same reason: a brand-new route's first death is always
        // shallower than the incumbent -- especially when the incumbent is a WEDGE, whose
        // credited depth can never be extended (its anchors are sterile) but outranks any
        // honest death near the portal. Without this the forced robot tail was verified once
        // (died at x=19,407, 400px past the portal -- a real robot run), lost the depth
        // comparison to the ship's wedge at t=16,538, and was thrown away the same round.
        // Following it keeps the fixup recorder and the ladder on ITS trajectory, which is
        // how a new route gets developed at all.
        // Progress along the forced line keeps the grace whole (the same shape as the
        // solved-branch follow); only actual stagnation spends it.
        outcome = itermap::KindForced;
        if (dt <= g_lastDeath) --g_followForced;
        snprintf(b, sizeof(b), "dpsolve:   %s t=%lld on the forced route - "
                 "following it (%d left, best %lld)",
                 dt > g_lastDeath ? "progress to" : "regression to",
                 dt, g_followForced, g_bestDeath);
        writeResult(b);
        anchors::ladderOn(false);
        g_curBackoff = kBackOff;
    } else {
        // No progress: go back to the deepest plan GD verified, and to the trajectory that plan
        // actually flew, and reach further back for the anchor.
        // The anchor this plan was spliced at is spent: it was tried, the game replayed what
        // came out of it, and the run is no deeper for it. Taking it again produces the same
        // tail and the same death.
        if (g_anchorT > 0) g_spentAnchors.insert(g_anchorT);
        if (!g_best.empty()) g_plan = g_best;
        anchors::ladderOn(true);
        g_curBackoff = std::min(g_curBackoff * 4, 3600);
        useT = g_bestDeath;
        snprintf(b, sizeof(b), "dpsolve:   no improvement (best %lld) - rewound to the deepest "
                 "plan (t=%lld), backoff %d", g_bestDeath, g_bestDeath, g_curBackoff);
        writeResult(b);
        ++g_stallRuns;
        // Getting nowhere. Two things are worth trying, in the order of what they cost, and
        // both are things the loop can tell it needs from the run itself rather than from a
        // table of levels.
        if (g_stallRuns >= kStallToUnseen && !g_needUnseen && g_cfg.dpWorld) {
            g_needUnseen = true;
            writeResult("dpsolve:   stuck - from here the search must also enter the doors "
                        "whose effect it has not seen");
        }
        if (g_stallRuns >= kStallToShorten && g_horizonNow == g_horizonFull) {
            // Ask for less, and find out sooner whether it is true. Planning the whole level
            // each time is only worth it while the model is right about the whole level; once
            // it is demonstrably wrong somewhere, a shorter plan gets checked in the game
            // sooner and the loop learns from the game instead of from the model.
            g_horizonNow = kHorizonShort;
            snprintf(b, sizeof(b), "dpsolve:   %d rounds without progress - planning %d ticks "
                     "at a time so the game checks each step", g_stallRuns, kHorizonShort);
            writeResult(b);
        }
        // [2026-08-23] Capacity was escalated here too, on a plain stall. Reverted: it is the
        // 4-20x cost measured and rejected earlier, and a level that stalls pays it on every
        // round for as long as the stall lasts -- which on lv16 was the whole budget, visibly
        // slowing the search and buying nothing. It stays in escalate(), where it is only
        // reached after the ladder has actually run out of anchors.
    }
    // Put the round on the map (itermap.hpp). Recording only -- nothing below reads it back, so
    // whether the overlay is on cannot change what the loop does.
    //
    // A wedge overrides whatever branch it fell down: it is credited where it stopped moving,
    // never ranks, and drawing it as an ordinary death would put a mark at a point no route ever
    // reached. The anchor is the one THIS plan was spliced at (the previous round's choice), so
    // the line the map draws from it to the death is the causal one: "the ladder reached back to
    // here, and what came out of it died there".
    if (wasWedged) outcome = itermap::KindWedge;
    {
        // The recorder's last row is one tick short of the death, and the death tick itself has
        // no row at all -- so walk down to the last one there is. Explicitly against the attempt
        // that just died: g_src points at whatever the branch above left it on.
        const std::vector<AnchorRow>* savedSrcIm = anchors::g_src;
        anchors::ladderOn(false);
        float dy = 0.f;
        for (long long k = std::min(dt, 400000LL); k > 0 && k > dt - 8; --k)
            if (const AnchorRow* r = anchors::row(k)) { dy = r->y; break; }
        // The tail this round flew, straight out of the recorder that is already sitting here --
        // the same rows the ladder re-anchors on. From the splice point, because that is where
        // this round stops being every other round (see itermap::Path).
        {
            const long long p0 = (g_anchorT > 0) ? g_anchorT : 0;
            const long long p1 = std::min(dt, 400000LL);
            itermap::beginPath(g_iter, outcome);
            for (long long k = p0; k <= p1; k += itermap::kPathStep)
                if (const AnchorRow* r = anchors::row(k)) itermap::addPathPoint(r->x, r->y);
            itermap::endPath();
        }
        anchors::g_src = savedSrcIm;
        itermap::addDeath(g_iter, dt, deathX, dy, outcome, g_anchorT, g_anchorX, g_curBackoff);
    }
    g_lastDeath = dt;
    g_lastDeathX = deathX;
    // The fixup pass runs on the same worker job, before the ladder: it uses THIS replay's
    // recorded states, and what it learns is in force for the ladder's very first solve.
    spawn(JobLadder, useT, "learning where the model was wrong, then re-solving");
}

// A candidate has cleared the level. That ends the solving and begins the one thing worth
// watching: the solution, at 1x, with the artwork and the song.
//
// ---- the deep record ----
//
// Send the deepest verified plan through the level once with dying switched off, only to see
// where the moving geometry goes. Collisions, portals and triggers all still fire -- only the
// kill is swallowed -- so the trajectory up to the wall is the same one, and the ride the plan
// started carries on to the end instead of stopping at the wall with it.
//
// This is not external data: it is the run's own plan, replayed by the run, in the run's own
// game. It is a DIFFERENT WORLDLINE though (nothing died in it), which is why it is only ever
// the base layer -- see addWorldArgs.
//
// Fires when the ladder has run out of anchors, and again only once the plan has got at least
// this much deeper; re-recording the same depth would just cost a pass over the level.
constexpr long long kDeepRefire = 300;

// Is there anything to record? A level with no grouped objects has no moving geometry, and a
// pass over it would only cost a run of the level to write an empty file.
inline bool levelHasMovingParts() {
    return g_cfg.dpGroups && grouptrace::g_on && !grouptrace::g_objs.empty();
}

// The very first thing a solve does on a level that has moving parts: run it once with no
// inputs and no dying, to see what the level does on its own. The reset happens at the next
// frame boundary (poll), like every other level change here.
inline bool startBootstrapRecord() {
    if (!levelHasMovingParts()) return false;
    g_recordKind = RecBootstrap;
    g_deepActive = true;
    g_cfg.noDeath = true;
    g_cfg.inputs.clear();
    g_recordRequest = true;
    g_hudPhase = "watching the level run itself, to see what moves";
    writeResult("dpsolve: recording what the level does on its own (no inputs, nodeath) "
                "before solving");
    return true;
}

inline bool startDeepRecord() {
    if (!levelHasMovingParts() || g_deepActive) return false;
    if (g_best.empty() || g_bestDeath < 0) return false;
    if (g_deepDoneAt >= 0 && (g_bestDeath - g_deepDoneAt) < kDeepRefire) return false;
    g_deepDoneAt = g_bestDeath;
    g_recordKind = RecDeep;
    g_deepActive = true;
    g_deepStartTick = 0;
    g_cfg.noDeath = true;
    g_cfg.inputs = g_best;
    g_hudPhase = "recording the moving geometry to the end of the level";
    char b[192];
    snprintf(b, sizeof(b), "dpsolve: no anchor left - re-recording the moving geometry with "
             "nodeath over the deepest plan (t=%lld)", g_bestDeath);
    writeResult(b);
    g_recordRequest = true;
    return true;
}

// The no-death pass is over (it reached the end, or it stopped getting anywhere). Take the
// recording and put the loop back where it was.
inline void finishDeepRecord(const char* why) {
    if (!g_deepActive) return;
    const int kind = g_recordKind;
    g_deepActive = false;
    g_recordKind = RecNone;
    g_cfg.noDeath = false;
    const grouptrace::Roll r = grouptrace::g_lastRoll;
    const std::string dst = (kind == RecBootstrap) ? g_groupsBootPath : g_groupsDeepPath;
    const char* what = (kind == RecBootstrap) ? "bootstrap record" : "deep record";
    char b[240];
    // A near-empty recording is worse than none: it would become a base layer asserting that
    // everything is where it started.
    if (r.rows >= 100) {
        std::error_code ec;
        std::filesystem::copy_file(std::string(DATA_DIR) + "/grouptrace_last.txt", dst,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        snprintf(b, sizeof(b), "dpsolve:   %s (%s): %lld samples to t=%lld - %s",
                 what, why, r.rows, r.depth, ec ? "COPY FAILED" : "adopted");
    } else {
        snprintf(b, sizeof(b), "dpsolve:   %s (%s): only %lld samples - discarded",
                 what, why, r.rows);
    }
    writeResult(b);
    if (kind == RecBootstrap) {
        // Now solve (or verify the seed), in a world that is already known to move.
        beginFirstAttempt();
        return;
    }
    // Back to the plan the loop was working on, and re-ladder from its wall -- now against a
    // world that keeps moving past it.
    g_plan = g_best;
    anchors::ladderOn(true);
    g_curBackoff = kBackOff;
    spawn(JobLadder, g_bestDeath, "re-solving against the recorded geometry");
}

// ---- when the ladder finds nothing ----
//
// "No anchor on this prefix can be solved" is a statement about the search AS IT IS CURRENTLY
// SET UP, not about the level. Before that becomes a verdict, everything that changes the setup
// gets a turn, cheapest first. Each is once-only, so this terminates.
//
// None of it is per level. Every option here is one the run asks for by failing, and the same
// sequence runs on every level -- the ones that never need it never reach this code.
inline bool escalate() {
    ++g_escalations;
    if (g_escalations > 12) return false;    // backstop; the flags below already bound this
    if (g_cfg.dpWorld && !g_needUnseen) {
        g_needUnseen = true;
        writeResult("dpsolve:   no anchor - trying again with the doors the search has not "
                    "entered");
    } else if (g_horizonNow == g_horizonFull) {
        g_horizonNow = kHorizonShort;
        char b[192];
        snprintf(b, sizeof(b), "dpsolve:   no anchor - trying again in %d-tick steps, so the "
                 "game checks each one", kHorizonShort);
        writeResult(b);
    } else if (g_needTrigSuspect & ~g_needTrigDropped) {
        // Now. The run has stopped getting anywhere, so the pressure those boxes were applying
        // has had its chance and is only emptying frontiers.
        const unsigned rel = g_needTrigSuspect & ~g_needTrigDropped;
        g_needTrigDropped |= rel;
        char b[192];
        snprintf(b, sizeof(b), "dpsolve:   no anchor - releasing the doors it cannot reach "
                 "from here (mask 0x%x)", rel);
        writeResult(b);
    } else if (startDeepRecord()) {
        return true;    // that one drives the level itself; it will come back through poll
    } else if (!g_triedMissedPortal) {
        // Everything that changes HOW the search is set up has been tried. Ask WHERE instead:
        // if the run went past a mode portal and did not come out in that mode, it has been
        // flying a section built for something else, and the place to go back to is the
        // crossing. See g_modePortals -- this is a hint, not a verdict.
        g_triedMissedPortal = true;
        const long long mt = missedPortalTick();
        if (mt <= 0) {
            writeResult("dpsolve:   no anchor - the run took every mode portal it crossed, so "
                        "the route is not the thing that is wrong");
        } else {
            g_forceAnchorT = mt - kPortalSettle;
            // ...and this time the crossing is FORCED, not merely offered. The DP has no
            // reason of its own to prefer the portal: on lv22's shaft the model can climb
            // frame 3 in either mode, so from the pre-portal anchor the ship branch ties or
            // beats the robot every round, and the game refutes it 60px at a time forever
            // (measured: best t=21,534 was still a ship, wedged at yet another point of the
            // same band ceiling). A forcing slit just past the portal -- "only the portal's
            // mode lives here", the deadband's >= 0 semantics -- makes the one solve from
            // this anchor answer the question the hint is actually asking: IS there a route
            // through the portal? If there is none, the tail comes back empty, the slit is
            // dropped with the rung, and nothing else ever sees it. The slit spans all
            // heights: dodging the portal in y is the same wrong answer as flying past it.
            char fb[96];
            snprintf(fb, sizeof(fb), "%.1f,%.1f,%d,-1e18,1e18",
                     g_missedPortalX + 45.0, g_missedPortalX + 75.0, g_missedPortalMode);
            g_forcePortalBand = fb;
            char b[256];
            snprintf(b, sizeof(b), "dpsolve:   no anchor - it crossed a mode portal at t=%lld "
                     "and came out in the wrong mode; going back there and forcing the "
                     "crossing (--deadband %s)", mt, fb);
            writeResult(b);
        }
    } else if (g_capTier < (int)(sizeof(kCapTiers) / sizeof(kCapTiers[0]))) {
        ++g_capTier;
        char b[192];
        snprintf(b, sizeof(b), "dpsolve:   no anchor - trying again with room for %lld states "
                 "on a coarser grid", kCapTiers[g_capTier - 1]);
        writeResult(b);
    } else if (!g_coldRestarted) {
        // Last resort: throw the prefix away and solve the level again from the start -- but
        // with everything this run has learnt (the fixups, the recording of the moving
        // geometry, the doors). It is a different search from the first one for exactly that
        // reason. Once per run: it is a whole-level solve, and if the second one cannot do it
        // a third will not either.
        g_coldRestarted = true;
        writeResult("dpsolve:   no anchor - solving the level again from the start, with what "
                    "this run has learnt");
        g_horizonNow = g_horizonFull;   // a whole-level attempt, not a step
        g_curBackoff = kBackOff;
        g_stallRuns = 0;
        anchors::ladderOn(false);
        // THE DEEPEST VERIFIED PLAN IS KEPT. An earlier version cleared it, on the theory that
        // the point of a restart is to escape the prefix -- and that threw lv22 from x=3,615
        // back to x=622 and straight into giving up, because a plan the model can build alone
        // is not the plan the loop built by re-anchoring on the game a dozen times. If the
        // fresh attempt is worse, the next round simply reads as no progress and the loop goes
        // back to what it had, with the rest of its budget intact.
        spawn(JobFirstSolve, 0, "solving again from the start");
        return true;
    } else if (liftPhantomVetoes("no solvable anchor")) {
        // Last of all, and after the restart, exactly as the driver orders it: the boxes are the
        // cheapest thing to give back, but giving them back is also the only step that can make
        // the search WORSE (the phantoms come straight back), so nothing else should still be
        // waiting behind it.
    } else {
        return false;
    }
    // The setup changed; ask the same wall again.
    g_curBackoff = kBackOff;
    spawn(JobLadder, g_bestDeath >= 0 ? g_bestDeath : g_lastDeath,
          "re-solving with more of the search turned on");
    return true;
}

// Whether this clear is going to be taken over by the showing. Asked BEFORE GD's own completion
// path runs, because the answer decides whether to run it at all.
//
// Only a session someone is watching gets a showing: a headless run (a worker driven by
// autorun.cfg) has no screen to come back to, and the extra replay at 1x would be 90 seconds of
// nothing before the session could close. cfg `dpshow` forces it either way.
inline bool wantsShow() {
    if (!g_cfg.dpSolve || g_dpShowSolution) return false;
    return (g_cfg.dpShow >= 0) ? (g_cfg.dpShow == 1) : g_uiSession;
}

// Whether the mod is taking this clear over instead of letting GD end the level on it. Either
// it is about to show the solution, or the run that reached the end was the no-death recording
// pass, which is not a clear at all -- nothing survived it, dying was simply switched off.
// g_recordAttempt, not g_deepActive: a recording pass that outlived its own recorder (see the
// declaration) still must not be handed to GD's completion path, or the end screen fires on a run
// that only reached the end because dying was switched off.
inline bool takesOverClear() { return g_recordAttempt || g_deepActive || wantsShow(); }

// Returns whether it took the clear over. Called from levelComplete, which is the middle of GD's
// own completion path, so nothing is touched here beyond raising the flag -- the restart happens
// at a frame boundary in poll(), the rule every level change in this file follows.
inline bool onCleared() {
    if (!wantsShow()) return false;
    g_dpShowSolution = true;   // badge -> REPLAY, audio stops being silenced
    g_showRequest = true;
    writeResult("dpsolve: cleared after " + std::to_string(g_iter)
                + " repair rounds - replaying the solution at 1x");
    return true;
}

// Called once per frame. Installs whatever the worker thread produced, at a frame boundary.
inline void poll() {
    // A start() queued behind an earlier session's still-running orphaned worker (see
    // g_pendingLayer) -- retry now that the slot is free. Re-validated against the session that
    // asked: g_started/g_cfg.dpSolve alone would still be true for a THIRD Solve session queued
    // behind this one, so the layer itself has to match too, and comparing pointers never
    // dereferences the possibly-stale one.
    if (!g_running.load() && g_pendingLayer) {
        GJBaseGameLayer* l = g_pendingLayer;
        g_pendingLayer = nullptr;
        if (g_started && !g_sessionOver && g_cfg.dpSolve && PlayLayer::get() == l) start(l);
    }
    // A no-death pass that is not getting to the end. It cannot die, so nothing else will stop
    // it: a player wedged against a wall runs the tick counter forever. The level's own length
    // bounds an honest pass, so twice that is generous and still finite.
    // A recording pass has been asked for. Restarting the level is a frame-boundary job.
    if (g_recordRequest) {
        g_recordRequest = false;
        g_paused = false;
        if (auto* pl = PlayLayer::get()) pl->resetLevel();
        return;
    }
    if (g_deepActive && g_horizon > 0 && g_tick > (long long)g_horizon * 2) {
        finishDeepRecord("did not reach the end");
        return;
    }
    // ...and the same guard for the FULL-LEVEL horizon (g_horizon == 0), where
    // the cap above never fires. Measured on lv22 (2026-08-26 18:28): the deep
    // pass over a 758-input plan fell out of the rotated section, drifted to
    // x=-71,196 / t=57,834 with nothing left to kill it, and the run sat in
    // the fastloop for half an hour while the census heartbeats kept the
    // harness's stall guard fed. Two ends a completion check can never reach:
    //   - the player has left the board on the wrong side (x < -50): a
    //     forced-scroll game cannot come back from there;
    //   - x has not advanced for 3,600 ticks (15 s of game time): the wedge
    //     the note above describes, now unbounded under the full horizon.
    if (g_deepActive) {
        static long long lastMoveTick = 0;
        static double lastMoveX = -1e18;
        double px = 0.0;
        if (auto* pl = PlayLayer::get())
            if (pl->m_player1) px = pl->m_player1->getPositionX();
        if (std::fabs(px - lastMoveX) > 1.0) { lastMoveX = px; lastMoveTick = g_tick; }
        if (px < -50.0) {
            finishDeepRecord("fell off the board");
            return;
        }
        if (g_tick - lastMoveTick > 3600) {
            finishDeepRecord("stopped getting anywhere");
            return;
        }
    }
    // The finale. Back to 1x from the spectating speed the solve ran at, and the level is
    // restarted so the solution is watched from the beginning rather than joined at the finish
    // line. If the operator had sent the screen to the fast loop with F5, it comes back here.
    if (g_showRequest) {
        g_showRequest = false;
        g_paused = false;
        clearManualPause();          // do not start the showing frozen on someone's F2
        watchSpeedSet(WATCH_SPEED_1X);
        g_hudPhase = "replaying the solution";
        g_cfg.maxAttempts = g_finishedAttempts + 1;   // show it once, then the session ends
        // Turning the screen back on goes through the render key's own path, which does NOT
        // resume drawing here: it asks for a level reset first and resumes on the far side of it
        // (resuming before the reset makes the next updateVisibility walk state that piled up
        // while stopped, and that crashes). That deferred reset is also the restart we want, so
        // when the screen is already on there is nothing to defer and the reset is direct.
        if (!renderingOn()) {
            renderTogglePress();
        } else {
            g_realtimeOverride = true;
            if (auto* pl = PlayLayer::get()) pl->resetLevel();
        }
        notify::show("gdsolver: solved - replaying the solution",
                     NotificationIcon::Success, 4.f);
        return;
    }
    // ---- section-solver handoff (cmd `secsolve <start> <targetX> [horizon] [cap]`) ----
    // The loop retires and the session becomes a section solve: the deepest VERIFIED plan
    // replays to the requested tick, a practice checkpoint is dropped there, and
    // hooks_gamelayer's cfg-driven dispatcher runs runSectionSolve -- the same machinery the
    // py-served path (py/secsolve_run.py) exercises; nothing below is new search code.
    // Deliberately one-way: the search drives the world arbitrarily and ends the session
    // itself ("secsolve"), so the loop is stopped for good with g_stop rather than suspended.
    // Waits for an idle boundary -- a solver worker cannot be cancelled (spawn() detaches),
    // so the handoff never runs while one is out.
    if (g_secReqPending && (!g_started || g_sessionOver)) g_secReqPending = false;
    // Stop the loop from starting anything new, FIRST, and take over once the solver thread in
    // flight has finished. Waiting for an idle thread without this never fires: one frame of a
    // fast loop installs a plan, replays the whole attempt, books the death and spawns the next
    // solve, so g_running is true at every poll() a caller could observe. Measured on 1474319
    // (2026-08-27): the command was accepted and the handoff never came. g_stop closes
    // onDeath's spawn, so the flag goes false and STAYS false.
    if (g_secReqPending && g_cfg.dpSolve && !g_dpShowSolution && !g_stop) {
        g_stop = true;
        writeResult("secsolve handoff: the repair loop stops after the solve in flight");
    }
    if (g_secReqPending && g_cfg.dpSolve && !g_dpShowSolution
        && !g_running.load() && !g_deepActive) {
        g_secReqPending = false;
        if (!g_best.empty() && g_secReqStart >= g_bestDeath) {
            writeResult("secsolve handoff: refused - start t=" + std::to_string(g_secReqStart)
                        + " is past the verified depth t=" + std::to_string(g_bestDeath));
            return;
        }
        g_stop = true;               // no more spawns; onDeath's guard makes deaths inert
        if (g_best.empty())
            writeResult("secsolve handoff: no verified plan yet - the prefix run is inputless");
        std::sort(g_best.begin(), g_best.end(),
                  [](const InputCmd& a, const InputCmd& c) { return a.step < c.step; });
        g_cfg.inputs = g_best;
        // Write down the prefix this hands to the search. Without it the section can only ever
        // be run again through the handoff: the served path (py/secsolve_run.py) takes a plan
        // FILE, and the plan here exists only in memory, so "does the same section behave the
        // same outside a solve session" -- the first question to ask when a handoff's search
        // returns something a replay will not reproduce -- could not be asked at all.
        {
            const std::string pp = std::string(DATA_DIR) + "/secsolve_prefix.txt";
            if (writeInputsFile(pp, g_best))
                writeResult("secsolve handoff: prefix written to " + pp);
        }
        g_cfg.practiceAt = (int)std::max(1LL, g_secReqStart - 100);
        g_cfg.checkpointAt = (int)g_secReqStart;
        // The checkpoint is placed at a frame boundary, so boundaries must be tick-fine --
        // the proven cfg of the served path. The search itself steps with secdt directly and
        // does not care what this is.
        g_cfg.fastloops = 1;
        // If the prefix somehow keeps dying before the section head, let the attempt cap end
        // the session instead of replaying forever (the loop is stopped and will not repair).
        g_cfg.maxAttempts = g_finishedAttempts + 5;
        secsolve::g_on = true;
        secsolve::g_done = false;
        secsolve::g_startTick = g_secReqStart;
        secsolve::g_targetX = g_secReqTarget;
        if (g_secReqHorizon > 0) secsolve::g_horizon = (int)g_secReqHorizon;
        if (g_secReqCap > 0) secsolve::g_cap = (size_t)g_secReqCap;
        secsolve::g_log = true;      // without seclayer: lines, a capped frontier and a real
                                     // wall cannot be told apart
        char hb[224];
        snprintf(hb, sizeof(hb),
                 "secsolve handoff: loop stopped after iter %d - replaying %zu inputs to "
                 "t=%lld, then solving to x=%.1f (horizon=%d cap=%zu)",
                 g_iter, g_cfg.inputs.size(), (long long)g_secReqStart,
                 secsolve::g_targetX, secsolve::g_horizon, secsolve::g_cap);
        writeResult(hb);
        g_hudPhase = "secsolve: replaying to the section head";
        g_paused = false;
        if (auto* pl = PlayLayer::get()) pl->resetLevel();
        return;
    }
    if (!g_running.load() || !g_finished.load()) return;
    g_running = false;
    g_finished = false;
    if (g_resultGeneration != g_generation.load()
        || !g_started || g_sessionOver || !g_cfg.dpSolve) {
        // An orphaned worker (spawn() detaches; nothing can cancel it) from a session that has
        // since ended finally reported in. Its plan and its csv are for a level that is gone --
        // discard it without touching the game: no resetLevel, no Notification, no
        // g_cfg.inputs. Whatever is happening right now, if anything, continues undisturbed.
        writeResult("dpsolve: discarded a stale result from a session that already ended (gen "
                    + std::to_string(g_resultGeneration) + " != "
                    + std::to_string(g_generation.load()) + ")");
        return;
    }
    const double sec = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - g_t0).count();
    char b[256];
    // rc is the solver core's own exit code. It only means anything for the first solve (the
    // ladder makes several calls), but a core that failed to start at all says so here and
    // nowhere else
    char rcs[24] = "";
    if (g_iter == 0) snprintf(rcs, sizeof(rcs), " rc=%d", g_rc);
    snprintf(b, sizeof(b), "dpsolve: done in %.1fs, %zu inputs%s%s",
             sec, g_plan.size(), rcs, g_haveNewPlan ? "" : " (NO PLAN)");
    writeResult(b);
    g_dpSolving = false;
    if (!g_haveNewPlan || g_plan.empty()) {
        g_paused = false;      // never leave the game frozen because the solve failed
        // Before conceding: everything that changes how the search is set up gets a turn.
        if (g_iter > 0 && escalate()) return;
        g_stop = true;
        const char* why = g_iter == 0 ? "the model cannot leave the start of this level"
                                      : "no anchor on this prefix can be solved";
        g_hudPhase = why;
        giveUp(why, "dpsolve_stuck");
        return;
    }
    std::sort(g_plan.begin(), g_plan.end(),
              [](const InputCmd& a, const InputCmd& c) { return a.step < c.step; });
    g_cfg.inputs = g_plan;
    g_paused = false;
    // The screen stays off here. These replays are the loop TESTING a candidate, not showing a
    // result -- most of them die -- and each one drawn at 1x costs the length of the song. The
    // run comes back to the screen once, when a candidate has actually cleared (showSolution).
    g_hudPhase = g_iter == 0 ? "verifying the first plan in the game"
                             : "verifying the repaired plan in the game";
    g_hudAnchorT = g_anchorT;
    g_hudAnchorX = g_anchorX;
    if (auto* pl = PlayLayer::get()) pl->resetLevel();
    if (g_iter == 0) {
        // What has happened here is that the MODEL produced its first plan -- not that the level
        // is solved. Nothing has been verified yet, and on most levels this plan dies and the
        // repair loop runs for dozens of rounds afterwards. Saying "solved" (which this said,
        // with the success icon) is the single most misleading thing the mod can put on screen:
        // it is indistinguishable from the real ending, which is the one fired from g_showRequest
        // above after GD has actually played a candidate to the end.
        char note[192];
        snprintf(note, sizeof(note), "gdsolver: first plan in %.0fs (%zu inputs) - now testing "
                 "it in the game", sec, g_plan.size());
        notify::show(note, NotificationIcon::Info, 4.f);
    }
}

}  // namespace dpsolve

}  // namespace p1
