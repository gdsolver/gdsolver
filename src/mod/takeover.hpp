#pragma once
// ============================================================
// The human takeover: a debugging mode (cfg `takeover=<tick>` or `takeoverx=<x>`, off by default).
// With cfg `takeoverkey=1` the point is not fixed ahead: the plan plays as an ordinary replay, and
// the end key (F8) takes the input over where it stands -- the checkpoint is put there and the mode
// is the same from then on. The key then ends the mode, as it does after a fixed point.
//
// Where the repair loop or a section search stalls, a person can often see a way through that the
// search does not take. This mode lets them show it. The plan is flown to a point; the strict
// checkpoint the loop's checkpoint flights use (cp_flight.hpp -- taken by takeoverTake and loaded
// by takeoverRestore in hooks_gamelayer, through the same cpCaptureState / cpRestoreState) is put
// there; and from that tick on the jump button is the person's.
//
// From there the game's own checkpoints are the person's, and every one is a strict one. Practice
// mode is switched on at the point, so the game places checkpoints the way it always does -- its
// own auto-checkpoint setting, Z, a checkpoint object in the level -- and each gets a strict
// checkpoint taken between two update calls where the physics stands: one the game stores at once
// (Z, a level checkpoint) at the next update boundary; an auto checkpoint, which the game creates
// where it judges the player safe and stores only once they have outlived it, at the boundary
// after it is created, kept aside and committed when the game stores its own (dropped if the game
// lets it go). The takeover's own records are created and not stored (takeoverMark), so the
// game's practice state -- the queued Z, the markers -- stays the game's. X removes the newest,
// except the start point's. Every death goes back to the newest, never to the head. A clear, or
// the end key (setting key-takeover-end), ends the mode.
//
// What it is for, and what it is not:
//   - It is an instrument. Each attempt is written out as a whole plan from tick 0 -- the plan's
//     own inputs before the point, then the person's -- in the format every replay tool reads, so
//     the route can be flown again in GD and in the model and the first parting measured.
//   - It is never a seed. Nothing here writes where a solve reads (solutions, resumes, fixups),
//     and a person's route is no evidence that the solver can find one.
//   - The session stays open the whole time, so botDriving() holds and nothing is recorded:
//     achievements, statistics, coins and the level's own record stay blocked, and the badge says
//     HUMAN INPUT.
//
// The tick of a press is the substep GD applies it in: the handleButton hook notes g_tick, which
// is the step an injected press of the same plan is fed at (processCommands feeds step <= g_tick
// before the substep runs). Whether a recorded attempt really flies again the same way is not
// assumed: the py driver replays every attempt and compares it with this session's dump.
//
// Files, under the data root's takeover/<session>/:
//   base.plan.txt       what every attempt from the start point flies before the person
//   att_NNN.plan.txt    one per attempt, the whole plan from tick 0
//   summary.txt         one line per event (start, checkpoint, removal, restore, attempt, end)
// The per-tick state is the session's dump.csv (its attempt column). An attempt's run is its own
// rows after the checkpoint it started from, and before that the rows of the attempt that took
// that checkpoint, and so on back to the plan's own flight: each checkpoint line names the
// attempt that took it (dump=) and the checkpoint that attempt had started from (parent=).
// ============================================================
#include "mod/level_slice.hpp"
#include <ctime>

namespace p1 {
namespace takeover {

// The last two seconds before the point run at the person's speed, so they see what they are
// about to play; before that the plan is flown at up to this many update calls a frame (4 ticks
// each, dt pinned as a seek pins it).
constexpr long long kLeadTicks = 2 * 240;
constexpr int kApproachMax = 250;
// By x there is no tick to aim at: a fixed pace until this far short of the point (two seconds at
// the fastest speed portal is about 1,150 px; at 16 calls a frame a frame covers at most ~150).
constexpr float kLeadPx = 1200.f;
constexpr int kApproachByX = 16;

// Every line goes to result.txt, and once the folder exists to summary.txt as well.
inline void line(const std::string& s) {
    if (!g_dir.empty()) {
        std::ofstream f(g_dir + "/summary.txt", std::ios::app);
        if (f) f << s << "\n";
    }
    writeResult("takeover: " + s);
}

inline int presses(const std::vector<InputCmd>& v) {
    int n = 0;
    for (const InputCmd& c : v) n += (c.down && !c.p2) ? 1 : 0;
    return n;
}

inline std::string attemptName(int n) {
    char b[32];
    snprintf(b, sizeof(b), "att_%03d.plan.txt", n);
    return b;
}

inline const Cp* cpByN(int n) {
    for (const Cp& c : g_cps)
        if (c.n == n) return &c;
    return nullptr;
}

// The attempt in progress as a whole plan: what its checkpoint's attempts fly before the person,
// then the person's presses as GD got them.
inline std::vector<InputCmd> attemptPlan() {
    std::vector<InputCmd> plan;
    if (const Cp* c = cpByN(g_from)) plan = c->before;
    plan.insert(plan.end(), g_human.begin(), g_human.end());
    return plan;
}

inline void startWait() {
    g_waiting = true;
    g_paused = true;
    g_waitUntil = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds((long long)(std::max(0.0, g_cfg.takeoverWait) * 1000.0));
}

// How many update calls this frame flies towards the point (0: the person's speed, or no takeover).
inline int approachLoops(long long tick, float x) {
    if (!approaching()) return 0;
    if (g_cfg.takeoverAt >= 0) {
        const long long rem = g_cfg.takeoverAt - kLeadTicks - tick;
        if (rem < 8) return 0;
        return (int)std::min<long long>(rem / 4, kApproachMax);
    }
    return (x < g_cfg.takeoverX - kLeadPx) ? kApproachByX : 0;
}

inline std::string cpLine(const char* what, const Cp& c) {
    char b[256];
    snprintf(b, sizeof(b), "%s n=%d t=%lld x=%.4f y=%.4f vy=%.4f held=%d dump=%d parent=%d source=%s",
             what, c.n, c.tick, (double)c.x, (double)c.y, (double)c.vy,
             (c.rec && c.rec->held) ? 1 : 0, c.dumpAttempt, c.parent, c.source);
    return b;
}

// The start point's checkpoint is in (takeoverTake): the person has the input from its tick.
inline void begin(Cp&& c) {
    char stamp[32];
    {
        std::time_t now = std::time(nullptr);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &now);
#else
        localtime_r(&now, &tmv);
#endif
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
    }
    g_dir = std::string(DATA_DIR) + "/takeover/" + stamp;
    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);
    if (ec) writeResult("takeover: could not create " + g_dir + " (" + ec.message() + ")");
    c.n = g_nextN = 1;
    ++g_nextN;
    g_T = c.tick;
    writeInputsFile(g_dir + "/base.plan.txt", c.before);
    g_live = true;
    g_from = c.n;
    g_attemptNo = 1;
    g_human.clear();
    g_written = false;
    char b[320];
    snprintf(b, sizeof(b),
             "start level=%d t=%lld x=%.4f y=%.4f vy=%.4f held=%d base=%zu wait=%.2fs dump=%d "
             "end-key=%s - the input is the person's from t=%lld",
             g_cfg.levelId, c.tick, (double)c.x, (double)c.y, (double)c.vy,
             (c.rec && c.rec->held) ? 1 : 0, c.before.size(), g_cfg.takeoverWait, g_attempt,
             keyName(Key::TakeoverEnd).c_str(), c.tick);
    line(b);
    line("folder " + g_dir);
    line(cpLine("checkpoint", c));
    g_cps.push_back(std::move(c));
    startWait();
}

// A strict checkpoint in place of one the game placed.
inline void addCp(Cp&& c) {
    c.n = g_nextN++;
    line(cpLine("checkpoint", c));
    g_cps.push_back(std::move(c));
}

// The strict checkpoint kept aside for the game's pending one: let go (the game dropped or
// replaced its own, or the attempt died), or committed (the game stored its own).
inline void dropPending() {
    g_pending = Cp{};
    g_hasPending = g_commitPending = false;
    g_pendingSrc = nullptr;
}

inline void commitPending() {
    g_commitPending = false;
    if (!g_hasPending) return;
    Cp c = std::move(g_pending);
    dropPending();
    addCp(std::move(c));
}

// The newest goes, except the start point's.
inline void removeLast(const char* why) {
    if (g_cps.size() <= 1) {
        line(std::string("remove refused (") + why + "): the start point's checkpoint stays");
        return;
    }
    line(cpLine((std::string("removed (") + why + ")").c_str(), g_cps.back()));
    g_cps.pop_back();
}

inline bool g_xRemoving = false;   // the game's removeCheckpoint below is the X key's

// The game took one of its own checkpoints out (PlayLayer::removeCheckpoint, the hook calls this
// after the game's own work). Its auto checkpoint marks itself just placed (PlayerObject+0x7e9)
// and checkCollisions takes it back if the player is hit while that holds -- the game's guard
// against respawning into the hit. The strict one follows: not taken, if the game took its own
// back before the next update boundary, or removed with it.
inline void gameRemoved(const void* gone) {
    if (!personDriving() || g_restoring || !gone) return;
    if (g_cpWanted && gone == g_wantedSrc) {
        g_cpWanted = false;
        g_wantedSrc = nullptr;
        line("the game took back the checkpoint it had just placed - no strict one for it");
        return;
    }
    if (g_cps.size() > 1 && g_cps.back().gdSrc == gone)
        removeLast(g_xRemoving ? "X" : "the game took its own back");
}

// X while the person plays. When the newest strict checkpoint stands for the game's newest, the
// game's own removal runs (its marker goes, and gameRemoved takes the strict one with it);
// otherwise the strict one goes alone. The start point's never goes.
inline void removeByKey(PlayLayer* pl) {
    if (g_cps.size() <= 1) {
        removeLast("X");
        return;
    }
    const void* src = g_cps.back().gdSrc;
    if (pl && src && pl->m_checkpointArray && pl->m_checkpointArray->count() > 0
        && pl->m_checkpointArray->lastObject() == src) {
        const size_t before = g_cps.size();
        g_xRemoving = true;
        pl->removeCheckpoint(false);
        g_xRemoving = false;
        if (g_cps.size() < before) return;
    }
    removeLast("X");
}

// The attempt in progress, written out whole.
inline void writeAttempt(const char* outcome, long long tick, float x) {
    if (g_written) return;
    g_written = true;
    const std::string name = attemptName(g_attemptNo);
    if (!writeInputsFile(g_dir + "/" + name, attemptPlan()))
        writeResult("takeover: could not write " + g_dir + "/" + name);
    const Cp* c = cpByN(g_from);
    // from= the checkpoint the attempt started from (cp= its n); dump= the session's own attempt
    // number, the dump.csv column this attempt's rows carry.
    char b[288];
    snprintf(b, sizeof(b),
             "attempt=%d outcome=%s tick=%lld x=%.3f cp=%d from=%lld presses=%d other=%d dump=%d file=%s",
             g_attemptNo, outcome, tick, (double)x, g_from, c ? c->tick : -1LL, presses(g_human),
             g_other, g_attempt, name.c_str());
    line(b);
}

// The restore landed (takeoverRestore): where, against where the checkpoint was taken.
inline void restored(const Cp& c, float x, float y, float vy) {
    ++g_restores;
    ++g_attemptNo;
    g_from = c.n;
    g_human.clear();
    g_other = 0;
    g_written = false;
    const bool exact = x == c.x && y == c.y && vy == c.vy;
    if (!exact) ++g_missed;
    char b[256];
    snprintf(b, sizeof(b), "restore attempt=%d cp=%d t=%lld dump=%d %s", g_attemptNo, c.n, c.tick,
             g_attempt, exact ? "landed where the checkpoint was taken" : "");
    std::string s = b;
    if (!exact) {
        char m[200];
        snprintf(m, sizeof(m), "MISSED: x=%.4f y=%.4f vy=%.4f, taken at x=%.4f y=%.4f vy=%.4f",
                 (double)x, (double)y, (double)vy, (double)c.x, (double)c.y, (double)c.vy);
        s += m;
    }
    line(s);
    startWait();
}

// A press reached GD while the person plays.
//
// A plan carries player 1's jump, and outside a two-player level every jump press is that:
// GJBaseGameLayer::handleButton (0x2338e0) sends a press that arrives as player 2's to player 1's
// pushButton / releaseButton all the same unless the level's settings say two-player mode
// (LevelSettingsObject+0x154), and the second body of a dual follows from there either way. Which
// key, pad or click it came from does not reach the physics, so it is not kept. (Measured on a
// person's first session: every press arrived as player1=0 and the cube jumped on each.)
inline void noteInput(bool down, int button, bool isPlayer1) {
    bool twoPlayer = false;
    if (auto* pl = PlayLayer::get())
        if (pl->m_levelSettings) twoPlayer = pl->m_levelSettings->m_twoPlayerMode;
    if (button == 1 && (isPlayer1 || !twoPlayer)) {
        g_human.push_back({(int)g_tick, down});
        return;
    }
    // Not a jump of player 1 -- a two-player level's second player, or a platformer button. A plan
    // has no place for it: counted, and the first few named, so an attempt that leaned on one is
    // not mistaken for a route the plan reproduces.
    if (++g_other <= 3) {
        char b[160];
        snprintf(b, sizeof(b), "a press the plan format cannot carry: button=%d player1=%d down=%d t=%lld",
                 button, isPlayer1 ? 1 : 0, down ? 1 : 0, (long long)g_tick);
        line(b);
    }
}

inline void finish(const std::string& why) {
    g_live = false;
    g_done = true;
    g_waiting = false;
    g_endWhy = why;
    char b[200];
    snprintf(b, sizeof(b), "end %s attempts=%d deaths=%d restores=%d missed=%d checkpoints=%zu",
             why.c_str(), g_attemptNo, g_deaths, g_restores, g_missed, g_cps.size());
    line(b);
}

inline void died(long long tick, float x) {
    ++g_deaths;
    writeAttempt("death", tick, x);
}

// The plan died before it reached the point: there is nothing to hand over, and every attempt
// would fly the same death again.
inline void diedBeforePoint(long long tick, float x) {
    g_done = true;
    g_endWhy = "the plan died before the point";
    char b[200];
    if (g_cfg.takeoverAt >= 0)
        snprintf(b, sizeof(b), "takeover: the plan died at t=%lld x=%.1f, before the point t=%lld - "
                 "nothing was handed over", tick, (double)x, g_cfg.takeoverAt);
    else
        snprintf(b, sizeof(b), "takeover: the plan died at t=%lld x=%.1f, before the point x=%.1f - "
                 "nothing was handed over", tick, (double)x, (double)g_cfg.takeoverX);
    writeResult(b);
    endSession("takeover_plan_died");
}

inline void cleared(long long tick, float x) {
    writeAttempt("clear", tick, x);
    finish("clear");
}

// The end key. Outside a takeover session the key is not polled.
inline void endByKey() {
    if (g_done || !g_started || g_sessionOver) return;
    if (g_live) {
        auto* pl = PlayLayer::get();
        const float x = (pl && pl->m_player1) ? pl->m_player1->getPositionX() : -1.f;
        writeAttempt("ended", g_tick, x);
        finish("end key");
    } else {
        g_done = true;
        g_endWhy = "end key before the point";
        writeResult("takeover: end key before the point was reached - nothing was handed over");
    }
    g_paused = false;
    endSession("takeover_end_key");
}

// cfg `takeoverkey`: hand the input over where the replay stands now. The checkpoint itself is put
// there at the next update boundary (takeoverAfterUpdate), where the physics stands between two
// update calls, as at a fixed point. Only before the mode has begun, in a plain replay.
inline void takeNow(const char* by) {
    if (!keyOnly() || !g_started || g_sessionOver || g_live || g_done) {
        writeResult(std::string("takeover: ") + by + " ignored - only an armed replay (cfg takeoverkey) "
                    "that has not been taken over yet can be");
        return;
    }
    if (g_cfg.dpSolve || g_serveMode) {
        writeResult(std::string("takeover: ") + by + " ignored - a solve owns the level");
        return;
    }
    g_takeNow = true;
    writeResult(std::string("takeover: ") + by + " at t=" + std::to_string((long long)g_tick)
                + " - the input is yours from the next update boundary");
}

// The replay key (F7) while a person plays in a session armed by the key alone: the attempt in
// progress is written out, the mode is left (the session stays open, nothing is ended), the plan
// is put back whole and practice mode is switched off -- leaving it restarts the level -- so the
// replay plays from the start and the takeover key takes the input over again, anywhere. Returns
// true when it did. At a fixed point the key stays off (the point is the checkpoint).
inline bool resetByReplayKey() {
    if (!keyOnly() || !g_live || g_done || !g_started || g_sessionOver) return false;
    auto* pl = PlayLayer::get();
    if (!pl) return false;
    const float x = pl->m_player1 ? pl->m_player1->getPositionX() : -1.f;
    writeAttempt("reset", g_tick, x);
    char b[256];
    snprintf(b, sizeof(b),
             "reset by the replay key attempts=%d deaths=%d restores=%d checkpoints=%zu - the mode is "
             "left, the replay restarts from the start and the takeover key arms it again",
             g_attemptNo, g_deaths, g_restores, g_cps.size());
    line(b);
    g_cfg.inputs = g_fullInputs;
    g_cfg.toggles = g_fullToggles;
    g_live = g_waiting = g_cpWanted = g_takeNow = g_restorePending = g_restoring = false;
    g_paused = false;
    g_T = -1;
    g_cps.clear();
    dropPending();
    g_from = g_nextN = 0;
    g_human.clear();
    g_other = g_attemptNo = g_deaths = g_restores = g_missed = 0;
    g_dir.clear();
    g_written = false;
    g_endWhy.clear();
    if (pl->m_isPracticeMode) pl->togglePracticeMode(false);
    return true;
}

// The takeover key. Before the mode has begun, in a session armed by the key alone, it takes the
// input here; once a person plays (or at a fixed point's mode) it is the end key, as it was.
inline void keyPressed() {
    if (keyOnly() && !g_live && !g_done) takeNow("the key");
    else endByKey();
}

// The HUD's takeover line (hud.hpp), "" when there is nothing to say.
inline std::string hudText() {
    if (!armed() || !g_started) return "";
    char b[384];
    if (keyOnly() && !g_live && !g_done) {
        snprintf(b, sizeof(b), "TAKEOVER armed: %s hands the input over here (now t=%lld)",
                 keyName(Key::TakeoverEnd).c_str(), (long long)g_tick);
        return b;
    }
    if (g_live) {
        char wait[48] = "";
        if (g_waiting) {
            const double left = std::chrono::duration<double>(
                g_waitUntil - std::chrono::steady_clock::now()).count();
            snprintf(wait, sizeof(wait), "   starts in %.1fs", left > 0.0 ? left : 0.0);
        }
        char miss[48] = "";
        if (g_missed) snprintf(miss, sizeof(miss), "\nRESTORE MISSED %d time(s) - see summary", g_missed);
        const long long at = g_cps.empty() ? g_T : g_cps.back().tick;
        snprintf(b, sizeof(b),
                 "HUMAN INPUT from t=%lld   attempt %d   deaths %d%s\n"
                 "checkpoints %zu (strict; back to t=%lld)   Z place   X remove (the start stays)\n"
                 "%s   end the takeover   -   nothing is recorded%s",
                 g_T, g_attemptNo, g_deaths, wait, g_cps.size(), at,
                 keyName(Key::TakeoverEnd).c_str(), miss);
    } else if (g_done) {
        snprintf(b, sizeof(b), "TAKEOVER over (%s)", g_endWhy.c_str());
    } else if (g_cfg.takeoverAt >= 0) {
        snprintf(b, sizeof(b), "TAKEOVER: flying the plan to t=%lld (now t=%lld)",
                 g_cfg.takeoverAt, (long long)g_tick);
    } else {
        snprintf(b, sizeof(b), "TAKEOVER: flying the plan to x=%.0f", (double)g_cfg.takeoverX);
    }
    return b;
}

}  // namespace takeover
}  // namespace p1
