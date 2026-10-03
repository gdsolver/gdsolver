#pragma once
#include <chrono>
#include <mutex>         // the checkpoint death handover (g_ckMx)
#include <map>           // cfgDiffLine
#include <set>
#include <sstream>
#include <thread>        // the solver worker
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#include <xmmintrin.h>   // _mm_getcsr: see the fpenv line in logSolverArgs
#endif
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
// geometry harvesting, sprite rotation -- is spent; all of it is below, and rungs the driver
// never had are as well (the unseen-door retry, the forced mode-portal crossing, and the section
// solve: cfg `dpsecauto`, which searches the game itself over a stretch the model cannot cross
// and splices the answer back -- see autoFire).
//
// WHAT IS STILL ONLY IN THE DRIVER, and why none of it is load-bearing here:
//   * reversible segment boundaries and prefix cutting on frontier width (the driver reads
//     --bands, which this loop does not even ask leveldp for).
//   * lookahead DOUBLING. This loop runs the inverse -- the whole level, shortened to
//     kHorizonShort under stall -- which covers the same ground from the other end.
#include "mod/session.hpp"
#include "solver/repair_progress.hpp"
#include "solver/failed_plan.hpp"
#include "solver/attempt_end.hpp"

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
    float rot2 = 0.f;        // ...and the second body's (the dump's p2rot; cfg dpanchorrot2)
    float speed = 0.f;       // GD's own speed multiplier, so the model need not re-derive it
    int gframe = 0;          // rotated gameplay (0/1/2/3 = 0/90/180/270)
    int snapUid = -1;        // m_objectSnappedTo's uid, and how far it snapped
    float snapDist = 0.f;
    float pmin = 0.f, pmax = 0.f;   // GD's flight band (getMinPortalY / getMaxPortalY)
    // GD's Free Mode byte [layer+0x311]: while set, checkCollisions (0x2139b8) clamps no
    // mode to the band at all. Copied from each mode portal the player takes, so it is
    // history --start does not carry; it rides in the hist payload (version 2).
    int freeMode = 0;
    // GD's velocity-limit exemption, byte [player+0x952] (set by the slope
    // machinery and the RED ring/pad, cleared when vy re-enters the band;
    // dp/state.hpp State::boost has the disassembly). While set, updateJump
    // skips the terminal clamp, so an anchor without it re-clamps the swing at
    // 8 while GD keeps accelerating.
    int boost = 0;
    // GD's m_controlsDisabled (the dump's ctrlOff column, same source). While
    // set, the button is ignored entirely (id 2899, frames.hpp's g_ctrlWin note);
    // addWorldArgs turns runs of it into --ctrlwin under cfg `dpctrlwin`.
    int ctrlOff = 0;
    // The press latch, bytes [player+0x985] (held) and [player+0x986] (a press
    // not yet consumed), per body. dp's State::pressSpent is "held, and the
    // press already spent" -- set by a consumer, cleared on release -- so it is
    // 0x985 && !0x986 (histPayload). -1 = no second body.
    int b985 = 0, b986 = 0, b985_2 = -1, b986_2 = -1;
    // cfg `coinroute`: the coins GD had credited by the end of this tick (bit i = the i-th coin
    // in x order, the order dp's L.coins has), from the pickupItem hook. An anchor passes it as
    // --coinmask, or the anchored search would read every coin behind it as missed.
    int coins = 0;
    // ...and GD's item counters at the same instant: the two lowest item ids it
    // has touched (no level in the corpus uses more -- lv21 counts item 1, lv22
    // items 1 and 2). A Count trigger compares these, so an anchored search that
    // is not told them plans a world where that gate never opens. -1 = none.
    int item1 = -1, cnt1 = 0, item2 = -1, cnt2 = 0;
    // The slope ride, raw 2.2081 facts for the anchor seeding: +0x9b0 is on a
    // ramp, +0x9b8 the underside branch, +0x678 the ramp (its uid, -1 for none)
    // and +0x598 the ride's start stamp, with +0xaa0 (the attempt clock) beside it
    // so an age can be read off. Sent in the hist payload's version 4 under cfg
    // `histride` (histPayload). Measured with cfg `slopetrace`: this point reads
    // +0x9b0 = 1 on every tick of a ride, though it is outside checkCollisions.
    int onSlope = 0, slopeUnder = 0, slopeUid = -1;
    double slopeStart = 0.0, totalTime = 0.0;
    // The time of the spider's last teleport, the double spiderTestJumpInternal writes at
    // +0x820 and collidedWithObjectInternal holds against +0xaa0: under 0.04 s after it, a
    // solid's side does not kill (0x393756, the model's State::spiderJumpT). Sent as its age in
    // ticks in the hist payload (version 3).
    double spiderStamp = 0.0;
};

// GD's MAX GAMEPLAY Y (layer+0x36a8), refreshed every recorded tick and passed
// to the solver as --maxplayy. 0 = not read yet (the flag is then withheld).
inline float g_maxPlayYLive = 0.f;

// GD's own state at tick 1 of the level: the row the recorder took on the first attempt of this
// session to reach it. The first solve starts from here when it is not the model's own default
// start (see firstStartArgs) -- the model otherwise always begins as a normal-speed cube on the
// floor at (0, 105), and a 2.2 level can put the player anywhere: the level setting kA36 (GD's
// LevelSettingsObject+0x1c4, the spawn group) moves the spawn to the height of that group's
// object (7 of 7 such levels measured, 2026-09-25), and kA2/kA3/kA4 start it in another mode,
// size or speed. Only a checkpoint restore keeps g_tick running, so tick 1 is always the level's
// own first tick. Cleared per session in start().
inline AnchorRow g_spawnRow;

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
inline std::vector<std::vector<solver::CoinPos>> g_deepestCoinPos;   // see keepAsDeepest
// Which of the two finished attempts the ladder is reading. Set on the main thread before the
// solver thread starts, never while it runs.
inline const std::vector<AnchorRow>* g_src = &g_dead;

// ...and the per-attempt seeds the solver's arguments carry (sweep.hpp: the pads and rings the
// attempt fired, the touch triggers it entered, the gravity portals it spent), banked with the
// rows for the same reason. They are recorded live and the level reset that starts the next
// attempt clears them, so an argument built from them after that reset described an attempt
// the anchor row does not belong to. Measured: two cold runs of SubZero 4001 on one geode parted
// at round 7 because the ladder's second call from t=18,760 was built after the reset in one
// run (--spentorb empty, the log's `[anchor] att=14`) and before it in the other (att=13, ten
// rings) -- a race with the game's clock, not a property of the level. A plan rewound to the
// deepest one re-anchors on that attempt's trajectory, so it takes that attempt's seeds too.
struct Seeds {
    std::unordered_map<int, int> pads;                // padseed::g_first
    std::vector<std::pair<int, int>> rings;           // ringseed::g_fired
    std::unordered_map<int, int> touch;               // touchseed::g_first
    std::unordered_map<int, int> portal, portal2;     // portalseed::g_first / g_first2
};
inline Seeds g_seedsDead;
inline Seeds g_seedsDeepest;
// Follows g_src rather than keeping a pointer of its own: several callers save g_src, call
// ladderOn(false) and put g_src back by hand, and a second pointer would be left behind.
inline const Seeds& seeds() { return g_src == &g_deepest ? g_seedsDeepest : g_seedsDead; }

inline void onAttemptStart() { g_live.clear(); }
inline void reset() {
    g_live.clear();
    g_dead.clear();
    g_deepest.clear();
    g_deepestCoinPos.clear();
    g_src = &g_dead;
    g_seedsDead = Seeds{};
    g_seedsDeepest = Seeds{};
}

// Hand the finished attempt to the solver side.
inline void bank() {
    g_dead.swap(g_live);
    g_live.clear();
    g_seedsDead = Seeds{padseed::g_first, ringseed::g_fired, touchseed::g_first,
                        portalseed::g_first, portalseed::g_first2};
}

// This attempt got further than any before it: keep its trajectory with its plan.
// ...and the coins' own track from that same attempt (solver::g_coinPosLog, still the ended
// attempt's here -- the next attempt's start clears it). The deepest trajectory is measured against
// ITS coins, not the latest attempt's: two routes can reach a coin's triggers 1,901 ticks apart
// (SubZero 4002, a skipped speed portal), and mixing the two clocks files an approach that never
// happened.
inline void keepAsDeepest() {
    g_deepest = g_dead;
    g_deepestCoinPos = solver::g_coinPosLog;
    route::g_onTickDeepest = route::g_onTick;   // cfg routeprereq: its switches, same attempt
    g_seedsDeepest = g_seedsDead;
}

inline void ladderOn(bool useDeepest) { g_src = useDeepest ? &g_deepest : &g_dead; }

inline const AnchorRow* row(long long t) {
    const std::vector<AnchorRow>& v = *g_src;
    if (t < 0 || (size_t)t >= v.size()) return nullptr;
    const AnchorRow& r = v[(size_t)t];
    return r.valid ? &r : nullptr;
}

// How many ticks the current source holds (for walks over the whole recording).
inline long long depth() { return (long long)g_src->size(); }

// cfg `slopetrace=t0,t1`: the slope-ride bytes record() takes, printed at the same instant
// (`slprec:` lines) whether or not a solve session is open, so a plain replay can answer
// whether this point sees the ride at all (AnchorRow::onSlope).
inline void slopeTrace(GJBaseGameLayer* l, long long t) {
    if (!l || !l->m_player1 || t < g_slopeT0 || t > g_slopeT1) return;
    auto const* pb = reinterpret_cast<uint8_t const*>(l->m_player1);
    auto* ramp = *reinterpret_cast<GameObject* const*>(pb + gdoff::kPlayerCurrentSlope);
    const double st = *reinterpret_cast<double const*>(pb + gdoff::kPlayerSlopeStartTime);
    const double tt = *reinterpret_cast<double const*>(pb + gdoff::kPlayerTotalTime);
    char b[256];
    // `att=` is the identity, and it is not decoration: a cold run puts dozens of attempts
    // through the same tick -- measured, 43 of them at t=4,353 of one lv16 run -- so joining
    // these lines to a re-anchor on the tick alone picks whichever attempt was written last.
    // A reader that wants "the ride the anchor was taken from" joins on (att, t).
    snprintf(b, sizeof(b),
             "slprec: att=%d t=%lld onslp=%d under=%d uid=%d slpst=%.6f ttime=%.6f age=%.3f "
             "og=%d y=%.4f vy=%.4f",
             g_attempt, t, pb[gdoff::kPlayerOnSlope] ? 1 : 0, pb[gdoff::kPlayerUpsideDownSlope] ? 1 : 0,
             ramp ? ramp->m_uniqueID : -1, st, tt,
             (tt - st) * 240.0, l->m_player1->m_isOnGround ? 1 : 0,
             (double)l->m_player1->getPositionY(), (double)l->m_player1->m_yVelocity);
    writeResult(b);
}

// Called at the post-input, pre-physics command boundary, labelled S(input tick + 1).
// Kept off `noTrace`: this is the state the next repair iteration re-anchors on.
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
    r.rot2 = (r.dual && l->m_player2) ? l->m_player2->getRotation() : 0.f;   // `p2rot`
    r.speed = (float)p->m_playerSpeed;
    r.gframe = g_gameFrame;
    r.snapUid = p->m_objectSnappedTo ? p->m_objectSnappedTo->m_uniqueID : -1;
    r.snapDist = p->m_snapDistance;
    r.pmin = l->getMinPortalY();
    r.pmax = l->getMaxPortalY();
    r.freeMode = *(reinterpret_cast<uint8_t const*>(l) + gdoff::kLayerFreeMode) ? 1 : 0;
    // The velocity-limit exemption has no bindings name; the offset is the one
    // updateJump's clamp gate reads (0x38ca9f: cmp [player+0x952],0), pinned
    // to 2.2081 like every other raw offset here.
    r.boost = *(reinterpret_cast<uint8_t const*>(p) + gdoff::kPlayerAccelerating) ? 1 : 0;
    r.ctrlOff = p->m_controlsDisabled ? 1 : 0;
    // The press latch (see AnchorRow::b985). Raw 2.2081 offsets: pushButton sets
    // both at 0x397fbc, releaseButton clears them, a consumer clears 0x986.
    r.b985 = *(reinterpret_cast<uint8_t const*>(p) + gdoff::kPlayerJumpBuffered) ? 1 : 0;
    r.b986 = *(reinterpret_cast<uint8_t const*>(p) + gdoff::kPlayerStateRingJump) ? 1 : 0;
    if (r.dual && l->m_player2) {
        auto const* q = reinterpret_cast<uint8_t const*>(l->m_player2);
        r.b985_2 = q[gdoff::kPlayerJumpBuffered] ? 1 : 0;
        r.b986_2 = q[gdoff::kPlayerStateRingJump] ? 1 : 0;
    } else {
        r.b985_2 = r.b986_2 = -1;
    }
    // The slope ride (see AnchorRow::onSlope). Raw offsets, like the press latch.
    {
        auto const* pb = reinterpret_cast<uint8_t const*>(p);
        r.onSlope = pb[gdoff::kPlayerOnSlope] ? 1 : 0;
        r.slopeUnder = pb[gdoff::kPlayerUpsideDownSlope] ? 1 : 0;
        auto* ramp = *reinterpret_cast<GameObject* const*>(pb + gdoff::kPlayerCurrentSlope);
        r.slopeUid = ramp ? ramp->m_uniqueID : -1;
        r.slopeStart = *reinterpret_cast<double const*>(pb + gdoff::kPlayerSlopeStartTime);
        r.totalTime = *reinterpret_cast<double const*>(pb + gdoff::kPlayerTotalTime);
        r.spiderStamp = *reinterpret_cast<double const*>(pb + gdoff::kPlayerLastSpiderFlipTime);
    }
    // ...and GD's MAX GAMEPLAY Y, the world-y bound whose crossing (two ticks
    // running) is the environment kill with a NULL object. Written by
    // updateMaxGameplayY into layer+0x36a8; read live rather than re-deriving
    // the formula, because on dynamic-height levels it moves with the world.
    g_maxPlayYLive = *reinterpret_cast<float const*>(
        reinterpret_cast<char const*>(l) + gdoff::kLayerMaxGameplayY);
    r.coins = 0;
    r.item1 = r.item2 = -1;
    r.cnt1 = r.cnt2 = 0;
    if (g_cfg.coinRoute) {
        for (size_t i = 0; i < solver::g_coinGdTick.size() && i < 8; ++i)
            if (solver::g_coinGdTick[i] >= 0 && solver::g_coinGdTick[i] <= t)
                r.coins |= 1 << i;
        // solver::g_itemCounts is ordered, so "the two lowest ids" is its first
        // two entries.
        int n = 0;
        for (const auto& kv : solver::g_itemCounts) {
            if (n == 0) { r.item1 = kv.first; r.cnt1 = kv.second; }
            else if (n == 1) { r.item2 = kv.first; r.cnt2 = kv.second; }
            else break;
            ++n;
        }
    }
    if (t == 1 && !g_spawnRow.valid) g_spawnRow = r;   // see g_spawnRow
}

}  // namespace anchors

}  // namespace p1
#include "mod/cp_flight.hpp"   // follows the anchors' buffers (cfg cpflight)
namespace p1 {

// ============================================================
// ---- rotation triggers already consumed before an anchor (--spentrot) ----
//
// A 2900 is one-shot. lv22's maze (t~11,3xx-12,8xx) walks x=15,399..16,131 through frames
// 1/2/3 and consumes the six triggers there; when the SHIP later glides the pinned floor
// through the same x at t=14,3xx, GD ignores them -- but an anchored tail solve knows nothing
// of the maze, sees uid6337 at (16,005,609) six pixels from the ride, and rotates a world GD
// does not (the -7.8 carry fixups at x=16,003 were the model fighting its own phantom turn).
// The anchor recording holds the truth, and the only part of it that says a 2900 FIRED is a
// gframe change -- see spentRotArg for why crossing its coordinate is not enough.
struct RotObj { int uid; double cx, cy; };
inline std::vector<RotObj> g_rotObjs;   // parsed from the level csv at session start

// Touch Toggles (1049 with touch=1), for --rotqtoggle's anchor seed. Parsed from the same
// csv in the same pass: GD's own touch recorder (activatedByPlayer, hooks_player.cpp) never
// sees one -- 0 of lv22's three -- so whether the attempt entered one has to be read off its
// recorded positions instead (touchSeedArg).
struct TouchTog { int uid; double cx, cy, hw, hh; };
inline std::vector<TouchTog> g_touchToggles;
// Every touch-triggered object (touch=1, any id), for --touchentered.
inline std::vector<TouchTog> g_touchBoxes;

inline void loadRotObjs(const std::string& csv) {
    g_rotObjs.clear();
    g_touchToggles.clear();
    g_touchBoxes.clear();
    std::istringstream in(csv);
    std::string line;
    std::getline(in, line);   // header: id,type,cx,cy,...,uid,...
    // Column positions follow the objrects header (id first, cx/cy 3rd/4th, w/h 5th/6th, uid
    // 8th, touch 44th).
    while (std::getline(in, line)) {
        const bool rot = line.rfind("2900,", 0) == 0;
        const bool tog = line.rfind("1049,", 0) == 0;
        {
            // Every touch-triggered object: split once, read cx/cy/w/h/uid/touch.
            double f[44] = {};
            int col = 0;
            size_t p = 0;
            while (p <= line.size() && col < 44) {
                size_t q = line.find(',', p);
                if (q == std::string::npos) q = line.size();
                f[col] = std::atof(line.substr(p, q - p).c_str());
                p = q + 1;
                ++col;
            }
            if (col == 44 && (int)f[43] == 1 && (int)f[7] > 0)
                g_touchBoxes.push_back({(int)f[7], f[2], f[3], f[4] * 0.5, f[5] * 0.5});
        }
        if (!rot && !tog) continue;
        RotObj o{};
        double w = 0.0, h = 0.0;
        int touch = 0;
        int col = 0;
        size_t p = 0;
        while (p <= line.size() && col < 44) {
            size_t q = line.find(',', p);
            if (q == std::string::npos) q = line.size();
            const std::string f = line.substr(p, q - p);
            if (col == 2) o.cx = std::atof(f.c_str());
            else if (col == 3) o.cy = std::atof(f.c_str());
            else if (col == 4) w = std::atof(f.c_str());
            else if (col == 5) h = std::atof(f.c_str());
            else if (col == 7) o.uid = std::atoi(f.c_str());
            else if (col == 43) touch = std::atoi(f.c_str());
            p = q + 1;
            ++col;
        }
        if (o.uid <= 0) continue;
        if (rot) g_rotObjs.push_back(o);
        else if (touch == 1) g_touchToggles.push_back({o.uid, o.cx, o.cy, w * 0.5, h * 0.5});
    }
}

// --rotqtoggle: the touch Toggles this attempt had entered by t0, as dp's --touchseed
// (`uid:tick` of the first entry). The test is dp's own box entry (markTouched, step.hpp):
// |x - cx| < hw + half and the player's y, or its y before the move (the previous row), within
// hh + half, with half = dp's playerHalf for the recorded mode and size. Only rows in gframe 0,
// where the recorded position and the trigger's box share world axes -- a Toggle inside a
// turned section is not seeded by this.
// --touchentered: every touch box the attempt's recorded positions overlapped by t0, as
// dp's --touchentered (trigger uids; "-" = none). dp's anchor scan then opens only these from
// the recording: an object that moved before t0 says something moved it, and when its group
// is also driven by an autonomous Move (lv22's ceiling, group 265, Move 18311) that something
// is not the box -- an anchor at x=2,954 opened seven boxes 240 px and more ahead of the
// player. The test is markTouched's (the row's own y, --touchprey=button) in world axes,
// which holds in every frame because the player's box is square.
inline std::string touchEnteredArg(long long t0) {
    std::string out;
    for (const TouchTog& T : g_touchBoxes) {
        for (long long t = 1; t <= t0; ++t) {
            const AnchorRow* r = anchors::row(t);
            if (!r) continue;
            const double half = (r->mode == 4) ? (r->mini ? 2.0 : 5.0)
                              : (r->mode == 6) ? (r->mini ? 8.1 : 13.5)
                                               : (r->mini ? 9.0 : 15.0);
            if (std::fabs((double)r->x - T.cx) < T.hw + half
                && std::fabs((double)r->y - T.cy) < T.hh + half) {
                if (!out.empty()) out += ",";
                out += std::to_string(T.uid);
                // ...and WHEN, as `uid:tick` (dp dates the box from it instead of from
                // an object's first recorded motion).
                out += ":" + std::to_string(t);
                break;
            }
        }
    }
    return out.empty() ? std::string("-") : out;
}

// cfg dpspentpad: dp's --spentpad, the pads this attempt had latched before t0 (padseed). Empty
// when there are none -- dp then seeds only the pads in contact at t0, as without the cfg.
inline std::string spentPadArg(long long t0) {
    std::string out;
    for (const auto& kv : anchors::seeds().pads)
        if (kv.second < t0) out += (out.empty() ? "" : ",") + std::to_string(kv.first);
    return out;
}

// dp's --spentorb, the rings this attempt fired before t0 (ringseed), oldest first -- dp replays
// them through noteRingFired, so the order is the fire order. Always passed since 2026-09-26 (it
// was cfg `dpspentorb`).
inline std::string spentOrbArg(long long t0) {
    std::string out;
    for (const auto& f : anchors::seeds().rings)
        if (f.second < t0) out += (out.empty() ? "" : ",") + std::to_string(f.first);
    return out;
}

// cfg dpxtrack: dp's --xtrack, GD's x on every tick of the anchor's attempt up to t0, so dp dates
// the autonomous triggers behind the anchor by where GD's player actually was (dp g_xTrack). The
// file name carries the source buffer and t0, and it is written to a temporary and renamed: the
// ladder's parallel arms can build the same argument at once, and a reader must never see half
// of one. Empty when the source holds no row.
inline std::string xtrackArg(long long t0) {
    const char* src = (anchors::g_src == &anchors::g_deepest) ? "deep" : "dead";
    const std::string path = std::string(DATA_DIR) + "/dp_xtrack_" + src + "_"
                             + std::to_string(t0) + ".txt";
    std::string body = "tick,x\n";
    long long n = 0;
    char b[64];
    for (long long t = 1; t <= t0; ++t) {
        const AnchorRow* r = anchors::row(t);
        if (!r) continue;
        snprintf(b, sizeof(b), "%lld,%.4f\n", t, (double)r->x);
        body += b;
        ++n;
    }
    if (n == 0) return "";
    const std::string tmp = path + ".tmp" + std::to_string(
        (unsigned long long)std::hash<std::thread::id>{}(std::this_thread::get_id()));
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << body;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return std::filesystem::exists(path) ? path : "";
    }
    return path;
}

inline std::string touchSeedArg(long long t0) {
    std::string out;
    for (const TouchTog& T : g_touchToggles) {
        for (long long t = 1; t <= t0; ++t) {
            const AnchorRow* r = anchors::row(t);
            if (!r || r->gframe != 0) continue;
            const AnchorRow* pr = anchors::row(t - 1);
            const double half = (r->mode == 4) ? (r->mini ? 2.0 : 5.0)
                              : (r->mode == 6) ? (r->mini ? 8.1 : 13.5)
                                               : (r->mini ? 9.0 : 15.0);
            const bool inX = std::fabs((double)r->x - T.cx) < T.hw + half;
            // cfg dptouchseednow: the row's own y, as markTouched reads it under the
            // default --touchprey=button (the pre-button y differs from the row only on a
            // tap tick, which the recording does not keep). The previous row is the
            // --touchprey=parent reading: lv22's reference route "enters" 5831 at t=5,939
            // on it alone (current row 46.2 against 45.39) and GD then fires 5809, which
            // that Toggle would have switched off.
            const bool inY = std::fabs((double)r->y - T.cy) < T.hh + half
                             || (!g_cfg.dpTouchSeedNow && pr
                                 && std::fabs((double)pr->y - T.cy) < T.hh + half);
            if (inX && inY) {
                if (!out.empty()) out += ",";
                out += std::to_string(T.uid) + ":" + std::to_string(t);
                break;
            }
        }
    }
    return out;
}

// The 2900s GD visibly fired at or before t0: at every gframe change in the anchor
// recording, the rotate-gameplay object whose point lies within 2 px of the player, on
// that tick, along the travel axis of the frame being LEFT (x for gframe 0/2, y for 1/3).
//
// The axis matters. Matching on either axis also picks up objects that merely share the
// other coordinate: on lv22, t=12,325 (1->0 at 15,404.7,615.0) is uid 6308 on y, but uid
// 6307 (15,405,859) matched too, on x alone; t=16,447 (0->3 at 20,116.0,607.8) is 11342
// on x, with 6337 (16,005,609) along for the ride on y. 30 of 568 changes in one cold run
// matched two objects that way. Each coincidental uid had already fired earlier in those
// attempts, so no set changed -- by luck of order, not by construction.
//
// This used to mark any 2900 whose travel coordinate the recording crossed within 150 px
// perpendicular -- "crossed" standing in for "fired". Measured on lv22 (one cold run, 41
// anchors), that was wrong in both directions:
//   - OVER: a 2900 on a channel GD has not switched to yet is crossed well before it fires
//     (uid 1215 crossed at t~1,653, fired at 1,817). Spent early, the replay never turns
//     there -- 4 uids at 3 anchors. The old note here said no such trigger existed in the
//     corpus.
//   - UNDER: the crossing test only ran between two rows in the SAME gframe, so the tick on
//     which a 2900 actually turned the frame was never looked at -- 91 uids at 36 anchors
//     (uid 4355 at t=4,646, uid 6286 at t=11,332).
// Replaying all 41 anchors with the witnessed set instead moved no first divergence
// earlier, and five later or away.
//
// Only a 2900 that rotates can be witnessed. A 2899 and a channel-only 2900 never change
// the frame, so they are left out (rotgameplay.txt names both), as is a reverse toggle that
// keeps the frame -- --spentrot only ever set firedT, never revT (cli.hpp).
inline std::string spentRotArg(long long t0) {
    if (g_rotObjs.empty()) return "";
    struct Pt { int uid; double cx, cy; };
    std::vector<Pt> pts;
    {
        // Read per call, not cached: the file is rewritten at every level's start, and a
        // cache would carry one level's triggers into the next in a one-session run.
        const std::string path = std::string(DATA_DIR) + "/rotgameplay.txt";
        std::ifstream f(path);
        if (!f) {
            log::warn("spentrot: {} missing -- no 2900 can be witnessed, none marked", path);
            return "";
        }
        std::string line;
        std::getline(f, line);   // uid,id,cx,cy,chan,ord,sord,sordd,spx,target,chanChanged,swarm,chanOnly,swch
        while (std::getline(f, line)) {
            std::vector<std::string> c;
            std::stringstream ss(line);
            for (std::string v; std::getline(ss, v, ',');) c.push_back(v);
            if (c.size() < 14 || c[1] != "2900" || c[12] != "0") continue;
            pts.push_back({std::atoi(c[0].c_str()), std::atof(c[2].c_str()),
                           std::atof(c[3].c_str())});
        }
    }
    std::set<int> spent;
    int pf = -1;
    for (long long t = 1; t <= t0; ++t) {
        const AnchorRow* r = anchors::row(t);
        if (!r) continue;
        if (pf >= 0 && r->gframe != pf)
            for (const Pt& p : pts) {
                const double d = (pf & 1) ? std::fabs(p.cy - (double)r->y)
                                          : std::fabs(p.cx - (double)r->x);
                if (d <= 2.0) spent.insert(p.uid);
            }
        pf = r->gframe;
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
// cfg dpfineretry (runLadder): the fine retry's search. The bins are the cube's (dp keyOf), the
// window runs kFineLead ticks past GD's death, and a ladder asks it at most kFineTriesPerLadder
// times (one per doomed rung, shallow to deep).
constexpr long long kFineCap = 50000;
constexpr double kFineYq = 2.0, kFineVq = 10.0;
constexpr long long kFineLead = 200;
constexpr int kFineTriesPerLadder = 4;
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
inline int g_topTierIter = 0;  // the round the ladder reached its last tier (cfg dptopstop)
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
inline bool g_argsLogged = false;  // the `solver args:` line has been printed for THIS session
inline std::string g_argsLast;     // ...and what it said, so a CHANGE gets its own line

// ---- checkpoint flights: the game checks the search while it runs (cfg `dpcheck`) ----
// The argument is at ckConsider() below and in dp/progress.hpp; these are the fields. All of them
// belong to the MAIN thread except where a comment says otherwise.
inline bool g_ckFlying = false;          // an attempt flying a checkpoint is in the air
inline bool g_ckInstalled = false;       // ...and g_cfg.inputs is that flight, not g_plan
inline unsigned long long g_ckCall = 0;  // the solve call that published it
inline size_t g_ckIndex = 0;             // ...its position in that call's order
inline long long g_ckT0 = -1;            // the anchor it was searched from
inline long long g_ckEnd = -1;           // the last tick its inputs cover
inline std::vector<InputCmd> g_ckPlan;   // exactly what GD was handed
// The plan the ladder splices its tails into, read ON THE MAIN THREAD at spawn(): runLadder reads
// g_plan for the same purpose on the worker, and a flight is spliced exactly the same way.
inline std::vector<InputCmd> g_ckBase;
inline size_t g_ckFlights = 0, g_ckPassed = 0, g_ckDeaths = 0;   // per job, for the summary line
// A refuted checkpoint, handed from the death hook (main thread) to the worker, which runs the
// fixup recorder on it once the search has stopped. The attempt's recorded rows travel with it:
// GD resets the level about a second after a death and starts refilling anchors::g_live.
struct CkDeath {
    bool ready = false;
    unsigned long long call = 0;
    size_t index = 0;
    long long t0 = 0, end = 0, deathT = 0;
    float deathX = 0.f;
    std::vector<InputCmd> plan;
    std::vector<AnchorRow> rows;
};
inline std::mutex g_ckMx;                // guards g_ckDeath and g_ckDeaf
inline CkDeath g_ckDeath;
// Death ticks this job has already cancelled and learnt from once (see ckLearn for why once). A
// flight that dies on one of them again is passed instead. A fixed rule, so it is as deterministic
// as the rest; cleared with the job.
inline std::set<long long> g_ckDeaf;
// cfg dpcheckobs: the job's first flight death, kept for the comparison with the plan the job
// ends up installing (main thread only). Reset at spawn; read once more at the loop's own death.
struct CkObs {
    bool have = false;
    size_t index = 0;
    long long t0 = -1, end = -1, deathT = -1;
    std::vector<InputCmd> plan;   // exactly what GD was handed for that flight
    double atSec = 0.0;           // seconds into the job when the death was seen (print only)
    bool compared = false, same = false;
};
inline CkObs g_ckObs;
inline size_t g_ckObsSkipped = 0;   // checkpoints passed without a flight
// Worker thread only: the recorder is running on a checkpoint death, not on the loop's own. Keeps
// the loop's per-iteration side effects (the kill-only veto credit) out of it.
inline bool g_ckInner = false;
// ---- what the loop knows between iterations ----
inline int g_iter = 0;
inline std::vector<InputCmd> g_plan;      // the plan currently installed
inline std::vector<InputCmd> g_best;      // the deepest plan GD has verified
// The plan of the attempt that just died, taken before the rewind can replace
// it. What the fixup recorder must replay: the model has to run the SAME inputs
// the game ran, or the difference it measures is between two plans.
inline std::vector<InputCmd> g_flownPlan;
inline long long g_bestDeath = -1;        // ...and how far it got
inline int g_curBackoff = kBackOff;
inline bool g_lastTailSolved = false;     // the tail spliced last time reached the end
// ...and whether the installed plan, as a whole, claims to reach the goal. The
// first solve sets it too (g_lastTailSolved is about a SPLICE and stays false
// there), and under cfg coinroute the goal includes every coin -- which is what
// makes this the right gate for the missed-coin request.
inline bool g_planClaimsGoal = false;
inline int g_followSolved = 0;            // regressions followed on a solved branch
// The deepest death a solved-branch follow has reached on THIS WALL (since the run last got
// deeper), -1 = none yet. What counts as progress along the branch is beating this, not beating
// the round before -- see the follow branch in onDeath for the two-death cycle it closes.
inline long long g_followPeak = -1;
inline int g_followForced = 0;            // ...and on the forced (portal) route -- see the grace
inline long long g_lastDeath = -1;
inline bool g_lastDeathNoCollision = false;   // timeouts and deferred resets are not p1 collisions
inline float g_lastDeathX = 0.f;      // ...and where (the void-attempt repeat scoring)
// cfg coinroute: the attempt was ENDED at a coin GD had not credited (hooks_gamelayer.cpp), not
// killed by the level. Set just before that destroyPlayer, consumed by onDeath. Such a death
// counts like any other for depth, the ladder and the rewind -- but it is no evidence against the
// MODEL, which disagreed with GD about nothing physical: the fixup recorder would write a kill
// where GD "died", and the phantom veto would close the band after the coin to every route. Both
// stay out of it (g_lastDeathCoinMiss is what the recorder's call site reads).
inline bool g_coinMissPending = false;
inline bool g_lastDeathCoinMiss = false;
// ...and the same, filed after the fact (cfg coinmisspost): a clear refused for a missing coin,
// moved back to the attempt's closest approach to that coin. A coin miss in every respect above,
// except that the plan need not have claimed the coin, so the coin margin does not grow for it.
inline bool g_coinMissPostPending = false;
// ...and which coin that filing was for (index into solver::g_coins; -1 = none). The coin wall
// (cfg dpseccoinrung) takes it when no cut named one: a coin the cut spares (a reverser past it,
// a Move carrying it) is only ever missed this way.
inline int g_postMissCoin = -1;
// cfg routeprereq: the box the next searches are told to enter (--needtrig-uid), -1 = none. Set when
// a coin's rank is moved to its prerequisite's box (coinApproachRoute) by the death that becomes the
// wall; the next deeper death decides it again.
inline int g_routeNeedUid = -1;
inline int g_routeNeedDeath = -1;   // the box the death being scored was ranked at (-1 = none)
// cfg routeprereqafter: the coins whose prerequisites rank (bit c), and the rounds each missed coin
// has gone since its filing without GD crediting it.
inline uint32_t g_routeEngaged = 0;
inline int g_routeRounds[8] = {0, 0, 0, 0, 0, 0, 0, 0};
inline bool routeEngaged(size_t ci) {
    return g_cfg.routePrereqAfter <= 0 || (ci < 32 && ((g_routeEngaged >> ci) & 1));
}
// ...and where the coin wall goes for it: the first tick the attempt crossed the coin's x while the
// coin sat live at its FINAL position (-1 = never). The closest approach can be a later pass the coin
// cannot be taken on -- SubZero 4002's third is filed 38 px away on the third pass (t=23,791), where
// no section search found it (3 rungs, 30 leaves without it), while the pass it is taken on comes
// back along the lower tier 2,400 ticks earlier, outside any rung's window.
inline long long g_postMissCrossT = -1;
inline long long firstFinalCrossing(const std::vector<AnchorRow>& rows,
                                    const std::vector<solver::CoinPos>& track, long long upto) {
    if (track.empty()) return -1;
    const solver::CoinPos fin = track.back();
    if (!fin.on) return -1;
    size_t k = 0;
    float px = 0.f;
    bool have = false;
    for (size_t i = 0; i < rows.size() && (long long)i < upto; ++i) {
        while (k + 1 < track.size() && track[k + 1].t <= (long long)i) ++k;
        const AnchorRow& r = rows[i];
        if (!r.valid) { have = false; continue; }
        const bool atFinal = track[k].t <= (long long)i && track[k].on && track[k].x == fin.x
                             && track[k].y == fin.y;
        if (have && atFinal && (px - fin.x) * (r.x - fin.x) <= 0.f && px != r.x) return (long long)i;
        px = r.x;
        have = true;
    }
    return -1;
}
// ...and the coins a refused clear has shown this run missing (bit i = solver::g_coins[i]), which
// every later death is then ranked against (scoreByPostCoins). Per level.
inline int g_postCoins = 0;
// ...and the tick at which that clear's route passed each such coin. A later death is ranked
// against a coin only when the attempt lived past this tick: one that died before it has not
// reached the coin yet, and its closest approach would be some unrelated early point
// (4003: a death at t=3,153 was ranked at t=1,660). Per level; -1 = none.
inline long long g_postCoinT[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
// Where the attempt ranked by scoreByPostCoins actually died. The ranking moves only the
// ladder's tick; the fixup recorder and the phantom veto are about the physical death and read
// this. Equal to g_lastDeath for every other death.
inline long long g_lastDeathPhys = -1;
// How far inside a coin this run now asks its plans to be (dp --coinmargin).
// Starts at GD's own rule -- the boundary counts -- and grows only where the
// game has actually refused a coin the plan claimed. Measured on lv21
// (2026-09-20): at 0 the route passed the first coin 0.67 px outside the bound
// in x and GD credited nothing; at a flat 3 it took that coin and the second,
// which says the allowance is real. What it is NOT is a claim about GD: it is
// this model's own trajectory error at the coin, so it belongs where the error
// shows itself rather than in a constant nobody measured.
inline double g_coinMarginNow = 0.0;
constexpr double kCoinMarginStep = 2.0, kCoinMarginMax = 8.0;
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
// the remainder of that single frame is thousands more ticks. Measured on a custom level
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
// Defined below, next to beginFirstAttempt; spawn()'s first solve needs it first.
inline void addFirstStart(std::vector<std::string>& a);
inline bool g_coldRestarted = false;  // the prefix has been thrown away once
inline int g_escalations = 0;         // how many times the ladder has been given another option

// Nine significant digits, the same precision the dump is written with. Coarser than that and
// the re-anchored x lands on the wrong side of a contact test near x=20,000.
// Defined below with the other reporting helpers; declared here because the
// dpsolve row (which is assembled earlier in the file) fingerprints the tail
// the solver just emitted.
inline std::string planFnv(const std::vector<InputCmd>& p);
inline std::string fileSig(const std::string& path);

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
// ...but not at every lesson: the wall (g_bestDeath) the last reset was made at, and how many
// resets it has had (cfg dplearnresets).
inline long long g_learnResetWall = -1;
inline int g_learnResets = 0;
// Touch boxes the search is no longer required to enter. A box BEHIND the anchor cannot be
// entered by a tail that starts after it, so requiring one empties the frontier before the
// first tick -- `PARTIAL t=0`, which reads from outside as "this level is impassable" and is
// nothing of the kind. The solver reports which boxes it required and which the anchor is
// already past; this is that answer, fed back into the next call.
inline unsigned long long g_needTrigDropped = 0;   // wide: see SolveOutcome::resimTrig
// ...and the ones merely under suspicion. Dropping a box the moment it looks unsatisfiable is
// measured-harmful: the backoff a demanded-but-unreachable box forces is exactly how lv19 ends
// up deep enough to buy the lift ride it needs, and the driver records that dropping on sight
// took lv19 from 16 rounds cleared to 48 and out. So a box is only released when the run has
// stopped getting anywhere without it -- the pressure first, the release later.
inline unsigned long long g_needTrigSuspect = 0;   // wide: see SolveOutcome::resimTrig
inline int g_stallRuns = 0;          // iterations since the run last got deeper
inline bool g_needUnseen = false;    // go and touch doors whose effect has not been seen
inline int g_horizonFull = 0;        // a plan that covers the level
inline int g_horizonNow = 0;         // ...and what is being asked for right now
// cfg dpstephorizon: the plan length the loop returns to after progress, and the
// length it shortens to when stuck. Both are the whole level / kHorizonShort unless the cfg is set.
inline bool g_triedWholeLevel = false;   // this wall's one whole-level escalation (step mode)
// cfg dpfastveto: the plan the last round flew and the tick it died on.
inline std::string g_prevFlownFnv;
// ...and whether the round now being recorded WAS such a repeat (the [veto] line). The fixup
// pass reads it: a model wall (FixupBlockedDyn) only ends the pass on a repeat. SubZero 4001
// cleared with the recorder writing past its walls (t~18,887 and ~18,221, uid 15813 / 14852)
// and got stuck at x=24,777 for 163 rounds when every wall ended the pass; 4002 re-flew one
// plan 25+ times writing past its wall. The repeat is what tells the two apart.
inline bool g_roundRepeated = false;
inline int g_wallRepeats = 0;   // cfg dpadaptivehorizon: stops in a row at one wall
inline long long g_prevFlownDeath = -1;
// EXPERIMENT (cfg dpfastvetoall): every (plan, death tick) this level has flown, not just the last
// round's. lv22 control, 2026-09-19: one SOLVED tail was searched again and died on the same tick
// three times, two rounds apart (a PARTIAL round in between), and the last-round check never saw it.
inline std::set<std::pair<std::string, long long>> g_flownDeaths;

// ---- cfg dpsecauto: the section-solve rung, fired by the loop ----
// What the loop knows about the wall it is stuck at. The wall is the deepest death (the rung's
// prefix is g_best, which dies there). It is the SAME wall until the deepest death has moved
// kAutoCreep ticks past where the counts began, and a death up to kAutoCreep ticks short of it is
// a death at the wall. Both are measured on lv4003 (old model): point fixes creep -- the deepest
// death went 5,366 -> 5,370 -> 5,379 -> 5,388 over twenty rounds, and a count reset by every step
// took 30 rounds to reach 6 -- and a wall's deaths need not sit on its deepest one: after one
// attempt reached t=16,311, the next thirty died at t=16,294, and an 8-tick test never counted
// them, so no signal rose while the loop stood there.
constexpr long long kAutoCreep = 30;
inline float g_bestDeathX = 0.f;         // where the deepest death was
inline long long g_autoWall = -1;        // the deepest death the counts below began at
inline int g_autoWallRounds = 0;         // rounds since then (the dpsecstall signal)
inline solver::RepairProgress g_autoProgress;
inline long long g_autoHead = -1;        // earliest real fixup recorded for a death at the wall
inline int g_autoNoRec = 0;              // deaths at the wall in a row the recorder wrote nothing for
inline int g_autoRecNow = 0;             // real records in the recorder run in progress
inline long long g_autoTriedWall = -1;   // the wall the last rung was fired at
inline int g_autoTries = 0;              // ...and how many it has had (autoWindow's levels)
inline long long g_autoLastT0 = -1;      // ...and where the last of them began
// cfg dpseccaptiers: the cap tier of the last window (its cap is dpseccap << tier), whether that
// window was drawn and fired, whether its search found anything, and whether a larger cap could
// have searched it differently at all (autoRungDone).
inline int g_autoCapTier = 0;
inline bool g_autoLastDrawn = false;
inline bool g_autoLastFound = true;
inline bool g_autoLastCapBound = false;
inline bool g_autoPinOut = false;       // runLadder: every rung past the pin is spent
inline int g_autoChain = 0;              // walls in a row right behind the pin (dpsecchain)
// cfg dpsecsolved: the job about to run records the death's fixups and then hands the wall to a
// section solve instead of laddering (set on the main thread before spawn, read by the job and
// then by poll).
inline bool g_autoRecordThenRung = false;
// cfg dpsecrent: WORK, NOT TIME. A wall clock would make the loop's decisions depend on the
// machine's load -- the same run would fire its rungs at different rounds on a busy machine, and
// the [fp] lines could no longer be compared. Every quantity below is a count the run produces
// the same way every time; the constants only convert counts into one currency (microseconds
// on this machine, measured 2026-09-23 over coin-off SubZero runs), so they decide how good the
// trade is, never whether it is reproducible.
// A DP search: per state it carries forward (cfg dpsecstateprice, 1.75) and per call (loading
// the level, the fixups, the groups; cfg dpseccallprice, 950,000), fitted over arm E's 136 jobs
// as job time - recorder = a*states + b*calls, R^2 0.72. The provisional 0.5 per state with
// nothing per call priced a round at a fifth of its cost -- and fired rungs later: lv4003's
// first wall (t 2,066) got its rung after 5 rounds against 2 with the fit, and the fit's run
// then fired one at every wall to t 2,724 and chained them through the wave from t 9,991, where
// the old pricing's run had needed that one rung for the whole level. Knobs, not constants,
// until that trade is settled.
inline double workDpState() { return g_cfg.dpSecStatePrice; }
inline double workDpCall() { return g_cfg.dpSecCallPrice; }
constexpr double kWorkRecTick = 1500.0;    // per model tick the fixup recorder replays (926-4605)
constexpr double kWorkReplayTick = 80.0;   // per tick GD flies (47-128)
constexpr double kWorkSecStep = 300.0;     // per section-search step on psnap (253-387)
constexpr double kWorkSecStepCp = 1100.0;  // ...and on the checkpoint path (140 s / 129,581)
inline double g_autoWallWork = 0.0;        // the rent paid at the wall
inline double g_autoRoundWork = 0.0;       // the round in progress: its job's searches and
                                           // recorder (worker thread), then its flight (onDeath)
inline double g_autoRungWork = 0.0;        // what this run's rungs have cost
inline int g_autoRungN = 0;
// cfg dpsecwallbudget: the section-solve work of the wall episode in progress, in the same currency.
// An episode starts at the first rung fired at a wall and takes in every rung after it until the loop
// gets past it by itself: the same wall's further windows and cap tiers, the walls a splice leaves
// right behind its pin (fired as pin-out, however far past the pin -- the ladder drops every rung
// before the pin, so the first wall after it goes to a section solve whatever its distance). A rung
// of cfg dpsecsolved is judged like any other: at the episode's wall it joins it, and more than
// kAutoCreep past the wall or pin it starts a new one, since the plan that died there was the loop's
// own flight past the pin. Measured in the bundle cold of 09-30 (seccpfallback=0, before this):
// level 22 off fired 7 rungs at t=2,661 alone, level 21 on 6 rungs past one pin and then a checkpoint
// search of 2,318 s.
inline double g_secEpisodeWork = 0.0;
inline long long g_secEpisodeReach = -1;   // the furthest wall / pin the episode's rungs reached
inline int g_secEpisodeRungs = 0;
inline double autoRungEstimate() {
    return g_autoRungN > 0 ? g_autoRungWork / g_autoRungN : 1e6 * (double)g_cfg.dpSecRungPrior;
}

// cfg dpseccoinrung (with dpsecauto, under coinroute): the COIN WALL -- where the plan the ladder is
// repairing was last cut for a coin, which coin, and that plan -- until an attempt's GD credits the
// coin. While it stands, the rung's wall is this, not the deepest death: the deepest plan and the
// next coin the route owes need not be the same (on SubZero 4002 every
// rung went to the coin-less deepest plan's wall and none to the first coin's hidden passage).
inline long long g_coinWall = -1;
inline int g_coinWallCoin = -1;
inline std::vector<InputCmd> g_coinWallPlan;
inline long long rungWall() {
    return (g_cfg.dpSecCoinRung && g_cfg.coinRoute && g_coinWall >= 0) ? g_coinWall : g_bestDeath;
}

// A new wall (the deepest death moved kAutoCreep past the counts' start) starts the counts again.
// Called by the recorder (worker thread) and by onDeath (main thread); the two never overlap --
// the job is spawned after onDeath and the next death comes after its plan is flown.
// Returns whether it did.
inline bool autoSync() {
    const long long wall = rungWall();
    if (g_autoWall < 0 || wall < g_autoWall || wall >= g_autoWall + kAutoCreep) {
        g_autoWall = wall;
        g_autoWallRounds = 0;
        g_autoWallWork = 0.0;
        g_autoHead = -1;
        g_autoNoRec = 0;
        return true;
    }
    return false;
}

// A rung is back (the splice is installed, or nothing was found): what it cost -- the prefix
// replayed to its head and the search's steps -- and the next round starts from nothing.
// `capBound`: the search's cap dropped something, and it was the cap the rung asked for.
inline void autoRungDone(long long headTick, long long steps, bool onSnap, bool found,
                         bool capBound) {
    const double work = kWorkReplayTick * (double)std::max(0LL, headTick)
                        + (onSnap ? kWorkSecStep : kWorkSecStepCp) * (double)steps;
    g_autoRungWork += work;
    ++g_autoRungN;
    g_secEpisodeWork += work;   // cfg dpsecwallbudget
    ++g_secEpisodeRungs;
    g_autoLastFound = found;
    g_autoLastCapBound = capBound;
    g_autoRoundWork = 0.0;
    // A successful splice earns a new handoff through repairs. A failed entry is retried directly,
    // without replaying the unchanged deepest plan or charging another ordinary repair round.
    g_autoWallWork = 0.0;
    g_autoWallRounds = 0;
    if (found) g_autoProgress.reset();
}

// A rung whose prefix did not reach its head (see g_secRungPrefixFails): counted once per attempt,
// and after kSecRungPrefixTries the rung is given up the way a search that found nothing is -- the
// cfg the handoff overwrote goes back, the deepest plan it installed stays the loop's plan, and the
// next frame tries an earlier head. Its reset drops practice mode and checkpoints (g_forceCleanStart).
// `died` is the tick the prefix ended at. True when this call gave the rung up.
inline bool secRungPrefixFailed(long long died, float x, const char* how) {
    if (!g_secRung || !secsolve::g_on || g_ckpt) return false;
    if (g_secRungFailAttempt == (long long)g_attempt) return false;   // this attempt is counted
    g_secRungFailAttempt = (long long)g_attempt;
    ++g_secRungPrefixFails;
    char b[256];
    snprintf(b, sizeof(b), "secrung: the prefix %s at t=%lld x=%.1f, before the head t=%d (try %d of "
             "%d)", how, died, (double)x, g_cfg.checkpointAt, g_secRungPrefixFails,
             kSecRungPrefixTries);
    writeResult(b);
    if (g_secRungPrefixFails < kSecRungPrefixTries) return false;
    const int head = g_cfg.checkpointAt;
    const bool retryAuto = g_secRungAuto;
    g_secRung = false;
    g_secRungAuto = false;
    if (g_secState) {   // cfg dpsecstate: the size is the rung's, not the next one's
        secsolve::g_forceP1Size = -1;
        secsolve::g_keepP1Size = -1;
        g_secState = 0;
    }
    secsolve::g_on = false;
    secsolve::g_rungCoin = -1;
    g_cfg.practiceAt = g_secSavePractice;
    g_cfg.checkpointAt = g_secSaveCkpt;
    g_cfg.fastloops = g_secSaveFastloops;
    g_cfg.maxAttempts = g_secSaveMaxAttempts;
    secsolve::g_targetDepth = g_secSaveTargetDepth;
    g_plan = g_cfg.inputs;          // the deepest plan the handoff installed (as when nothing is found)
    // Spent: the next rung at this wall goes further back (the search never ran, so not a larger
    // cap on the same window either).
    autoRungDone(std::max(0LL, died), 0, false, true, false);
    g_ckptTick = -1;
    g_headHeld = 0;
    g_practiceOn = false;
    g_stop = false;                 // the loop may spawn solves again
    g_paused = retryAuto;           // hold the failed world until poll chooses the next head
    g_secRetryPending = retryAuto;
    g_forceCleanStart = true;       // drop practice mode and checkpoints at the next reset
    g_hudPhase = "secrung: abandoned - the prefix does not reach the head";
    snprintf(b, sizeof(b), "secrung: abandoned - the prefix did not reach the head t=%d in %d tries; "
             "%s", head, kSecRungPrefixTries,
             retryAuto ? "an earlier window will be tried at the frame boundary"
                       : "the loop carries on from the deepest plan");
    writeResult(b);
    if (!retryAuto) secRenderRelease();
    return true;
}

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
// cfg dpstephorizon: the plan length after progress, and the length a stall or an
// escalation shortens to. Unset, these are exactly the loop's own: the whole level and 3000.
inline int horizonDefault() { return g_cfg.dpStepHorizon > 0 ? g_cfg.dpStepHorizon : g_horizonFull; }
inline int horizonShort() { return g_cfg.dpStepHorizon > 0 ? g_cfg.dpStepHorizon : kHorizonShort; }

// ---- cfg dpcontenthorizon: how far to plan, from what the level is made of ----
//
// The model's fidelity follows the age of the gimmicks it meets. Pre-slope custom levels (game
// versions before 1.8, 75 of them solved to the end, 2026-09-24): 0.41 real divergences per 10,000
// ticks; 1.8 levels about ten times that; the official lv1-15 cube 0.05. So where the loop would
// plan the whole level -- at the start, and after a plan got past its step -- it plans only as far
// as the first object past the anchor that is newer than 1.7, and one step beyond it; a level, or
// the rest of one, with nothing newer still gets the whole level. A deliberate whole-level ask
// (two stops at one wall, the escalation) is left alone: that one is for the lookahead.
//
// "Newer than 1.7", roughly: not a decoration (type 7), id 286 or above (1.8's dual portal and
// everything after it; ids are handed out in release order), and not one of the triggers that
// only change how the level looks. Measured on the official dumps: lv1-14 have none, lv11/12/15
// only colour triggers (899, 915), lv16-22 and SubZero have them from the first few hundred px.
inline std::vector<float> g_newGimmickX;     // sorted x of every such object in the level
inline bool g_horizonFullAsked = false;      // the whole level was asked for on purpose
inline double g_startX = 0.0;                // where the first solve starts
constexpr double kContentBack = 300.0;       // an object this far behind the anchor still counts
constexpr double kContentPxPerTick = 1.0;    // the slowest the player goes (speed 0.7: 1.01 px)
// By what an object does rather than its id alone: most newer ids are re-skinned blocks and
// hazards (the same physics as the old ones -- 1,710 of lv16's 2,353), and colour and camera
// triggers, and those do not count. What counts is a mechanic 1.7 did not have: slopes, the
// newer modes' portals, teleports, the newer orbs and pads, collision/special/area objects, and
// the triggers that move or change the level's geometry. Slopes stay in: 1.8 levels, where they
// and dual arrive, diverge about ten times as often as the pre-slope ones.
inline bool isGeometryTrigger(int id) {
    switch (id) {
        case 901: case 1346: case 1347: case 1814:     // move, rotate, follow, follow player y
        case 1049: case 1268: case 1616:               // toggle, spawn, stop
        case 1815: case 1595: case 1611: case 1811:    // collision, touch, count, instant count
        case 1817: case 1912: case 2066: case 2068:    // pickup, random, gravity, adv. random
        case 2900: case 1935: case 1917: case 1932:    // rotate gameplay, timewarp, reverse,
        case 1934:                                     // player control, song
            return true;
        default:
            return id >= 3006 && id <= 3033;           // area and keyframe triggers
    }
}
inline bool isNewGimmick(int id, int type) {
    if (type == 7 || id < 286) return false;
    switch (type) {
        case 25:                                       // slope
        case 23: case 24: case 26: case 27: case 33: case 41:   // dual/solo, wave, robot,
                                                                 // spider, swing portals
        case 28:                                       // teleport
        case 29: case 32: case 34: case 35: case 36:   // green, drop, red pad, red ring, custom
        case 37: case 38: case 43: case 44: case 46:   // dash, spider orb/pad, teleport orb
        case 39: case 40: case 45:                     // collision, special, area objects
            return true;
        case 20:
            return isGeometryTrigger(id);
        default:
            return false;                              // re-skins, collectibles, the look
    }
}
// The horizon one search from x0 is given.
//
// Mode 2 applies it to the first solve only. Mode 1 everywhere lost on the official levels: lv16-22
// carry newer objects from their first few hundred px, so it planned in steps throughout, and the
// model the official levels have been tuned against is right far past a step there (lv16 38 ->
// 69 rounds, lv21 9 -> 16, against SubZero's 4001 23 -> 19 and 4002's first solve 125 -> 5 s).
inline int horizonFor(double x0) {
    if (g_cfg.dpContentHorizon <= 0 || g_horizonNow != g_horizonFull || g_horizonFullAsked
        || std::isnan(x0))
        return g_horizonNow;
    if (g_cfg.dpContentHorizon == 2 && g_iter > 0) return g_horizonNow;
    auto it = std::lower_bound(g_newGimmickX.begin(), g_newGimmickX.end(),
                               (float)(x0 - kContentBack));
    if (it == g_newGimmickX.end()) return g_horizonFull;
    const double dx = std::max(0.0, (double)*it - x0);
    const long long h = (long long)(dx / kContentPxPerTick) + horizonShort();
    return (int)std::min<long long>(h, g_horizonFull);
}

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
//
// ...BUT ONLY A BAND THE PLAYER HAS BEEN IN SAYS WHERE THE PLAYFIELD IS. Two measured ways the
// game's band is not about the player at all, both on SubZero, and both stopped the level there:
//   - resetLevel does not reset it. Every attempt of 4003 after one that died at x~15,800 starts
//     as a cube at y=105 with pmin/pmax = 1368/1638, the band of the spider section it died in,
//     so t=1 read as 1,263 px off the board, every death was credited at t=1 x=0, and the ladder
//     ran out ("regression to t=1 on a solved branch", then no anchor).
//   - a teleport moves the player a tick before the band follows. 4002 t=16,294: a mini cube goes
//     from y=295 to 1,631 while the band is still 90/430 (0.5 px past 430 + kOffBoard); the next
//     tick the band is 756/1044. That one tick was credited as the death on every attempt, so the
//     loop kept rebuilding the approach to a teleport the run had already passed.
// So the bands count only once the player has been inside one in this attempt, and leaving
// counts when it lasts more than the one tick a teleport's lag lasts. The credit is still the
// first tick outside.
//
// ONCE PER ATTEMPT, NOT PER BAND. Re-arming on every change of the band (d3bf9c8) removed the
// legitimate credits too: lv22's band moves with the camera, the player is outside it when it
// leaves the board, and all five of the credits that run's dump holds were lost -- the level
// then spent 48 rounds under the arc. The inherited band cannot be told by its value either:
// 4003's starts retracting at t=313, a new value every tick, still nothing to do with the
// player. Replayed over the dumps of lv22 (51 attempts), 4003 (106) and 4002 (132), this form
// gives every credit the plain test gives, at the same tick, except the t=1 and the teleport
// ones, and adds none.
constexpr int kOffBoardTicks = 2;
// cfg `offboardtp` (on by default since 2026-10): A TELEPORT THE BAND DOES NOT FOLLOW. The 4002 case above has the band
// catching up one tick later; lv22 with coins has it not catching up for 112 ticks. t=1,818
// (coin-on bundle cold, 2026-09-30, attempts 5/8/10/13): a spider goes from y=224.3 to 3,210.0 in
// one tick. GD's camera goes with it on that tick (camy 27.5 -> 2,449.1), but pmin/pmax stay at the
// portal band 90/414 until t~1,930, when GD re-writes them around the camera (3,084/3,417). The
// test above reads y 3,210 against 414 + kOffBoard and credits the death at t=1,817..1,825 on four
// attempts -- while GD flies on and dies at t=2,006..2,014, killed by its real bound.
// GD's own out-of-bounds kill is not this band (read from the binary, GJBaseGameLayer::
// checkCollisions, 2.2081): 0x213bfe-0x213c22 flags the player when its y is above
// m_maxGameplayY (layer+0x36a8) plus a size term, the flag is kept in player+0xc38 (0x214199), and
// 0x21416e-0x214189 kills (vtable +0x468) only when the previous tick was flagged too -- two ticks
// above maxGameplayY (lv22: 3,675; GD's death here is at y 3,675.14 then 3,678.52). pmin/pmax
// (getMinPortalY / getMaxPortalY, 0x213690 / 0x213770, read further down) bound the flying modes
// only; for a cube or a spider they are the last portal's band, not a playfield.
// So after a teleport the band is no evidence of where the playfield is until the player is in
// it again: a row whose y moved by more than kOffBoardTpDy from the last row read disarms the
// check, and it re-arms the usual way, on the first row back inside the band. No physics step
// comes near that: in the trunk's lv22 dump of 2026-09-30 (333k steps) every per-tick |dy| above
// 12 px is a spider tap (13-190 px, floor to ceiling) or a teleport (the cube pair of 57.5 px at
// t=9,866 / 10,089), and a tap lands inside the band, so it re-arms on the same row. Replayed
// over the three lv22 dumps of 2026-09-30 (123 attempts), the 25 gv18 kits (599) and the 75
// preslope kits (390), it moves exactly the four credits above -- to t=1,972..1,977, 30 ticks
// before GD's own death, where the camera-driven band has fallen behind the rising player -- and
// no other.
constexpr float kOffBoardTpDy = 100.f;
inline long long offBoardTick(long long upTo, float& xAt) {
    bool armed = false;
    long long outT = -1;
    float outX = 0.f;
    bool haveY = false;
    float lastY = 0.f;
    for (long long t = 1; t <= upTo; ++t) {
        const AnchorRow* r = anchors::row(t);
        if (!r) continue;
        if (g_cfg.offBoardTp && haveY && std::fabs(r->y - lastY) > kOffBoardTpDy) {
            armed = false;
            outT = -1;
        }
        lastY = r->y;
        haveY = true;
        if (r->pmax <= r->pmin) { outT = -1; continue; }
        if (!armed) {
            if (r->y < r->pmin || r->y > r->pmax) continue;
            armed = true;
        }
        if (r->y < r->pmin - kOffBoard || r->y > r->pmax + kOffBoard) {
            if (outT < 0) { outT = t; outX = r->x; }
            if (t - outT + 1 >= kOffBoardTicks) {
                xAt = outX;
                return outT;
            }
        } else {
            outT = -1;
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
// The signature of the groups recording last copied beside a died plan (dp_groups_itN), so a
// level's copies are made only when the recording has changed. Reset with the level.
inline std::string g_groupsCopiedSig;
// ...and the recorder's side of the same attempt (keepRecorderInputs): the groups files the
// recorder's replay read, by signature -> the copy already made this level, and whether this
// death's inputs are still to be kept (set before its first pass). Reset with the level.
inline std::map<std::string, std::string> g_fixinGroupsBySig;
inline bool g_fixinPending = false;
// The groups signature the died plan was solved against (the [inputs] line), for the manifest.
inline std::string g_solveGroupsSig;
// cfg groupsretime: the player's x at each tick of the replay that recorded g_groupsPath (NaN
// where that replay held no row), so a later replay can tell whether it is on the same clock.
inline std::vector<float> g_groupsX;
// ...and the same for the two no-death records (finishDeepRecord). Beyond the live record's end
// the model reads them, so one on another clock puts every moving part where another route left
// it: on SubZero 4002's way back the deep record still had the floor at x=30,285 (uid 17148)
// that the live route's own record switched off 1,956 ticks earlier, and the model landed on it.
inline std::vector<float> g_groupsDeepX;
inline std::vector<float> g_groupsBootX;
inline int g_retimeLeftOut = 0;   // the layers addWorldArgs last left out (1 boot, 2 deep), logged on change
constexpr float kRetimeTolX = 2.f;
// The first tick at which two replays' x were more than kRetimeTolX apart where both have a row,
// or -1 (never, or either is empty).
inline long long clockPart(const std::vector<float>& a, const std::vector<float>& b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t t = 0; t < n; ++t)
        if (!std::isnan(a[t]) && !std::isnan(b[t]) && std::fabs(a[t] - b[t]) > kRetimeTolX)
            return (long long)t;
    return -1;
}
// ...and the same thing recorded in one run that could not die (see startDeepRecord). A live
// recording stops where the player died, and the model holds the last sample it saw forever --
// so a platform that was still moving at the wall reads as one that stopped there, and getting
// past the wall is the only way to record more of it. This breaks that circle.
inline std::string g_groupsDeepPath;

// Whether this run asked for the rotation queue. It can only arrive through the
// cfg's dp arguments: no code in src/mod or src/solver APPENDS `--rotqueue` to an
// argv, and the comparison below is the only place the spelling occurs at all --
// so that vector is the whole of the question, and asking it here rather than
// scanning a half-built argv means every caller gets the same answer regardless
// of the order in which its own flags are appended.
//
// This is the gate the queue's two other arguments hang off. Both are appended
// ONLY inside it, which is what lets their inertness be shown by reading rather
// than by running: with the flag absent the loop's argv is unchanged, byte for
// byte, and no extra file is opened.
inline bool rotQueueRequested() {
    for (const std::string& s : g_cfg.dpArgs)
        if (s == "--rotqueue") return true;
    return false;
}

// ============================================================
// ---- the rotation queue's seed, read off the recording (cfg dprotseed) ----
//
// The queue's cursor at an anchor is State::rotSpent / rotChan / rotRev. It used to
// come from a model walk of the plan (--seeddump), which on lv22 dies before the
// anchors that need it and so handed back an empty seed. This reads it off GD's
// own recording instead, with the rule measured offline on two lv22 cold runs:
//
//   CHAIN   every gframe change up to t0 is the active channel's next visible entry
//           (an id-2900 that is not channel-only) within 2 px; entries of other
//           kinds in front of it are passed over; a channel switch writes that
//           channel's reverse bit from the switching 2900's gnddir.
//   STRICT  the seed is only used when nothing the chain cannot see is in doubt:
//             C1  an entry passed over before a visible fire must have been seen
//                 to fire (a 2899: ctrlOff changing as the player crosses it)
//             C2  an entry at or past a channel's cursor that GD's own firing
//                 test says the player has already passed must be accounted for
//                 -- on the active channel it never is; on another channel only
//                 by a witness that it was NOT consumed.
//   LEVEL   which witnesses count, strictest first:
//             A (1)  that entry fires visibly later in the same attempt
//             E (2)  A, or for a 2899: crossed at or before t0 while its channel
//                    was inactive with ctrlOff unchanged; and C1 may be a 2899
//                    seen firing
//             F (3)  E, or an EARLIER attempt of this run showed that entry
//                    surviving an inactive crossing and firing later
//                    (g_rotSurvivors, grown once per finished attempt)
//
// Only an exact seed reaches a solve, and it takes --rotqueue with it; every other
// call gets no queue, so an empty seed never runs one. The queue's order is dp's
// (buildRotQueue), handed back through the solve outcome, not re-derived here.
struct RotQE {
    int ch = 0, uid = 0;
    double px = 0.0, py = 0.0;
    int swarm = 0, swch = -1, chanOnly = 0, gnddir = -1, id = 0;
};
inline std::vector<RotQE> g_rotQOrder;   // this level's queue in dp's order; empty = not read yet
inline std::set<int> g_rotSurvivors;     // level F's witnesses from earlier attempts

inline void rotSeedLevelReset() {
    g_rotQOrder.clear();
    g_rotSurvivors.clear();
}

// The queue's order, from the last solve that loaded it. While dprotseed is on every
// call carries --rotgameplay (addWorldArgs), so a level's first solve fills it.
inline bool rotQOrderReady() {
    if (!g_rotQOrder.empty()) return true;
    std::stringstream ss(dpbridge::outcome().rotQOrder);
    for (std::string item; std::getline(ss, item, ';');) {
        RotQE e;
        if (std::sscanf(item.c_str(), "%d,%d,%lf,%lf,%d,%d,%d,%d,%d", &e.ch, &e.uid, &e.px,
                        &e.py, &e.swarm, &e.swch, &e.chanOnly, &e.gnddir, &e.id) == 9
            && e.ch >= 0 && e.ch <= 15)
            g_rotQOrder.push_back(e);
    }
    return !g_rotQOrder.empty();
}

namespace rotseed {

inline bool visible(const RotQE& e) { return e.id == 2900 && !e.chanOnly; }

// GD's own firing test (checkSpawnObjects): the player's axis, the channel's direction.
inline bool passed(int gframe, double x, double y, const RotQE& e, bool rev) {
    const bool vertical = (gframe & 1) != 0;
    const double ref = vertical ? y : x;
    const double p = vertical ? e.py : e.px;
    return rev ? (ref <= p) : (p <= ref);
}

inline bool near2(const RotQE& e, const AnchorRow& r) {
    return std::min(std::fabs(e.px - (double)r.x), std::fabs(e.py - (double)r.y)) <= 2.0;
}

inline std::vector<std::vector<const RotQE*>> byChannel() {
    std::vector<std::vector<const RotQE*>> v(16);
    for (const RotQE& e : g_rotQOrder) v[(size_t)e.ch].push_back(&e);
    return v;
}

struct Chain {
    std::string fail;          // non-empty: the walk stopped here
    bool underivable = false;  // ...because the recording contradicts the chain
    int chan = 0;
    bool rev[16] = {};
    int ptr[16] = {};
    std::vector<int> spent;
    std::string prov;
    std::vector<std::pair<long long, int>> skipped;   // (tick of the visible fire, uid)
    std::vector<std::pair<long long, int>> timeline;  // (tick, active channel from then on)
};

inline Chain walk(const std::vector<AnchorRow>& v, long long tEnd,
                  const std::vector<std::vector<const RotQE*>>& q) {
    Chain w;
    w.timeline.push_back({0, 0});
    int pg = -1;
    for (long long t = 0; t < (long long)v.size() && t <= tEnd; ++t) {
        const AnchorRow& r = v[(size_t)t];
        if (!r.valid) continue;
        if (pg >= 0 && r.gframe != pg) {
            const std::vector<const RotQE*>& lst = q[(size_t)w.chan];
            int k = w.ptr[w.chan];
            std::vector<int> sk;
            while (k < (int)lst.size() && !visible(*lst[(size_t)k])) {
                if (lst[(size_t)k]->swarm) {
                    w.fail = "channel " + std::to_string(w.chan) + " switched by unseen uid "
                             + std::to_string(lst[(size_t)k]->uid) + " before t="
                             + std::to_string(t);
                    return w;
                }
                sk.push_back(lst[(size_t)k]->uid);
                ++k;
            }
            if (k >= (int)lst.size() || !near2(*lst[(size_t)k], r)) {
                w.fail = "gframe change at t=" + std::to_string(t) + " is not channel "
                         + std::to_string(w.chan) + "'s next visible entry";
                w.underivable = true;
                return w;
            }
            const RotQE& e = *lst[(size_t)k];
            for (int u : sk) {
                w.skipped.push_back({t, u});
                w.spent.push_back(u);
            }
            w.spent.push_back(e.uid);
            w.prov += (w.prov.empty() ? "" : ",") + std::to_string(t) + ":" + std::to_string(e.uid);
            w.ptr[w.chan] = k + 1;
            if (e.swarm && e.swch >= 0 && e.swch <= 15) {
                w.chan = e.swch;
                w.rev[e.swch] = (unsigned)(e.gnddir - 2) < 2u;
                w.timeline.push_back({t, w.chan});
            }
        }
        pg = r.gframe;
    }
    return w;
}

struct Crossing { long long t; int uid; bool ctrlChanged; };

// Every crossing of a 2899's point on the axis of the frame being travelled, with
// whether ctrlOff changed within one tick of it -- (I)'s witness.
inline std::vector<Crossing> crossings2899(const std::vector<AnchorRow>& v) {
    std::vector<long long> chg;
    const AnchorRow* prev = nullptr;
    for (long long t = 0; t < (long long)v.size(); ++t) {
        const AnchorRow& r = v[(size_t)t];
        if (!r.valid) continue;
        if (prev && r.ctrlOff != prev->ctrlOff) chg.push_back(t);
        prev = &r;
    }
    std::vector<Crossing> out;
    prev = nullptr;
    for (long long t = 0; t < (long long)v.size(); ++t) {
        const AnchorRow& r = v[(size_t)t];
        if (!r.valid) continue;
        if (prev) {
            const bool vertical = (prev->gframe & 1) != 0;
            const double c0 = vertical ? prev->y : prev->x;
            const double c1 = vertical ? r.y : r.x;
            for (const RotQE& e : g_rotQOrder) {
                if (e.id != 2899) continue;
                const double p = vertical ? e.py : e.px;
                if (c0 != c1 && (c0 - p) * (c1 - p) <= 0.0) {
                    bool changed = false;
                    for (long long c : chg)
                        if (c >= t - 1 && c <= t + 1) { changed = true; break; }
                    out.push_back({t, e.uid, changed});
                }
            }
        }
        prev = &r;
    }
    return out;
}

struct Seed { std::string cls, why, prov, seed; };

// Whether the travel coordinate turns back between (t-1 -> t) and (t -> t+1).
inline bool reverses(const std::vector<AnchorRow>& v, long long t, int gframe) {
    if (t < 1 || t + 1 >= (long long)v.size()) return false;
    const AnchorRow& a = v[(size_t)(t - 1)];
    const AnchorRow& b = v[(size_t)t];
    const AnchorRow& c = v[(size_t)(t + 1)];
    if (!a.valid || !b.valid || !c.valid) return false;
    const bool vertical = (gframe & 1) != 0;
    const double d0 = vertical ? (double)b.y - (double)a.y : (double)b.x - (double)a.x;
    const double d1 = vertical ? (double)c.y - (double)b.y : (double)c.x - (double)b.x;
    return d0 * d1 < 0.0;
}

// LEVEL S (4): the game's own consumption loop replayed over the recording.
// checkSpawnObjects (0x21a8f0, disassembled 2026-09-01) walks, every tick after
// physics, the ACTIVE channel's bucket from its cursor and consumes each element the
// player has passed -- on the player's axis, in the channel's direction -- until the
// first one not passed; other channels are never looked at. So which elements are
// consumed follows from the recorded positions, with no witnesses. What a consumed
// element DOES is read off the recording: a visible 2900 turns the player (a gframe
// change on that row) or, pointing at the current frame, reverses the travel (lv22
// uid16659). Neither on the row means it was consumed with neither half -- its group
// switched off by a touch Toggle -- and it switches no channel. A turn on a row where
// nothing visible was consumed makes the seed underivable. Replayed over lv22's
// reference recording (py/gdtas/rotseed.py seed_sim, the same rule) every one of its
// turns is reproduced and all 106 section anchors come out exact (A: 58, E: 67); where
// E is exact too the two seeds are identical (67 of 67).
// cfg `rotseedpre` (off): a turn row whose walk fired nothing is walked once more at the position
// BEFORE the tick's button -- the two rows before it carried on one tick. The game walks the queue
// after physics and runs the button after the update, so a press that moves the player on the
// turn's own tick (a spider's teleport) records a row the consumption never saw. lv22 on the
// glitch-avoid route presses at t=1,817: GD consumed uid 1215 at (2143.5, 225) with the spider at
// 224.3 and then teleported it to 316.5, the row the walk reads; underivable from there on, so no
// anchored search of the run had the queue (and the counting Tap on channel 9 never opened in the
// model). Offline, the retry makes that recording exact at every anchor and leaves the normal
// route's provenance unchanged.
inline bool g_preButton = false;
inline Seed seedSim(const std::vector<AnchorRow>& v, long long t0) {
    Seed s;
    const auto q = byChannel();
    int chan = 0;
    bool rev[16] = {};
    int ptr[16] = {};
    std::vector<int> spent;
    int pg = -1;
    for (long long t = 0; t < (long long)v.size() && t <= t0; ++t) {
        const AnchorRow& r = v[(size_t)t];
        if (!r.valid) continue;
        const bool acted = pg >= 0 && (r.gframe != pg || reverses(v, t, r.gframe));
        auto walk = [&](double px, double py) {
            int axis = pg < 0 ? r.gframe : pg;
            int turned = 0;
            for (int guard = 0; guard < 64; ++guard) {
                const std::vector<const RotQE*>& lst = q[(size_t)chan];
                if (ptr[chan] >= (int)lst.size()) break;
                const RotQE& e = *lst[(size_t)ptr[chan]];
                if (!passed(axis, px, py, e, rev[chan])) break;
                ++ptr[chan];
                spent.push_back(e.uid);
                if (visible(e) && !(acted && turned == 0)) continue;   // switched off
                if (e.id == 2900 && e.swarm == 1 && e.swch >= 0 && e.swch <= 15) {
                    chan = e.swch;
                    rev[chan] = (unsigned)(e.gnddir - 2) < 2u;
                }
                if (visible(e)) {
                    ++turned;
                    axis = r.gframe;   // the rest of this tick's walk tests the new axis
                    s.prov += (s.prov.empty() ? "" : ",") + std::to_string(t) + ":"
                              + std::to_string(e.uid);
                }
            }
            return turned;
        };
        int turned = walk(r.x, r.y);
        if (g_preButton && pg >= 0 && r.gframe != pg && turned == 0 && t >= 2
            && v[(size_t)(t - 1)].valid && v[(size_t)(t - 2)].valid) {
            const AnchorRow& a = v[(size_t)(t - 2)];
            const AnchorRow& b = v[(size_t)(t - 1)];
            turned = walk(2.0 * b.x - a.x, 2.0 * b.y - a.y);
            if (turned) s.prov += "(pre)";
        }
        if (pg >= 0 && r.gframe != pg && turned == 0) {
            s.cls = "underivable";
            s.why = "S t=" + std::to_string(t) + " the recording turns and the walk fired nothing";
            return s;
        }
        pg = r.gframe;
    }
    unsigned mask = 0;
    for (int c = 0; c <= 15; ++c) if (rev[c]) mask |= 1u << c;
    char head[32];
    std::snprintf(head, sizeof head, "%d,%x", chan, mask);
    s.seed = head;
    for (int u : spent) s.seed += "," + std::to_string(u);
    s.cls = "exact";
    return s;
}

inline Seed seedFor(const std::vector<AnchorRow>& v, long long t0, int level) {
    Seed s;
    if (!rotQOrderReady()) {
        s.cls = "unavailable";
        s.why = "dp has not handed back the queue order";
        return s;
    }
    if (level >= 4) return seedSim(v, t0);
    const auto q = byChannel();
    const Chain w = walk(v, t0, q);
    s.prov = w.prov;
    if (!w.fail.empty()) {
        s.cls = w.underivable ? "underivable" : "ambiguous";
        s.why = w.fail;
        return s;
    }
    const AnchorRow* last = nullptr;
    for (long long t = std::min<long long>(t0, (long long)v.size() - 1); t >= 0; --t)
        if (v[(size_t)t].valid) { last = &v[(size_t)t]; break; }
    if (!last) {
        s.cls = "underivable";
        s.why = "no recorded row at or before t0";
        return s;
    }
    const std::vector<const RotQE*>& act = q[(size_t)w.chan];
    for (int k = w.ptr[w.chan]; k < (int)act.size(); ++k)
        if (passed(last->gframe, last->x, last->y, *act[(size_t)k], w.rev[w.chan])) {
            s.cls = "ambiguous";
            s.why = "C2 active channel " + std::to_string(w.chan) + " uid "
                    + std::to_string(act[(size_t)k]->uid) + " passed";
            return s;
        }
    const std::vector<Crossing> cr = level >= 2 ? crossings2899(v) : std::vector<Crossing>{};
    auto activeAt = [&](long long t) {
        int c = 0;
        for (const auto& p : w.timeline) if (p.first <= t) c = p.second;
        return c;
    };
    for (const auto& sk : w.skipped) {
        bool seen = false;
        for (const Crossing& c : cr)
            if (c.uid == sk.second && c.t <= sk.first && c.ctrlChanged) { seen = true; break; }
        if (!seen) {
            s.cls = "ambiguous";
            s.why = "C1 uid " + std::to_string(sk.second) + " passed over unseen before t="
                    + std::to_string(sk.first);
            return s;
        }
    }
    for (int c = 0; c <= 15; ++c) {
        if (c == w.chan) continue;
        const std::vector<const RotQE*>& lst = q[(size_t)c];
        for (int k = w.ptr[c]; k < (int)lst.size(); ++k) {
            const RotQE& e = *lst[(size_t)k];
            if (!passed(last->gframe, last->x, last->y, e, w.rev[c])) continue;
            bool ok = false;
            if (visible(e)) {   // A: it fires visibly later in this same attempt
                int pg = -1;
                for (long long t = 0; t < (long long)v.size() && !ok; ++t) {
                    const AnchorRow& r = v[(size_t)t];
                    if (!r.valid) continue;
                    if (t > t0 && pg >= 0 && r.gframe != pg && near2(e, r)) ok = true;
                    pg = r.gframe;
                }
            }
            if (!ok && level >= 2 && e.id == 2899)   // E: crossed while inactive, not consumed
                for (const Crossing& x : cr)
                    if (x.uid == e.uid && x.t <= t0 && !x.ctrlChanged && activeAt(x.t) != c) {
                        ok = true;
                        break;
                    }
            if (!ok && level >= 3 && g_rotSurvivors.count(e.uid)) ok = true;   // F
            if (!ok) {
                s.cls = "ambiguous";
                s.why = "C2 channel " + std::to_string(c) + " uid " + std::to_string(e.uid)
                        + " passed, no witness";
                return s;
            }
        }
    }
    unsigned rev = 0;
    for (int c = 0; c <= 15; ++c) if (w.rev[c]) rev |= 1u << c;
    char head[32];
    std::snprintf(head, sizeof head, "%d,%x", w.chan, rev);
    s.seed = head;
    for (int u : w.spent) s.seed += "," + std::to_string(u);
    s.cls = "exact";
    return s;
}

// Level F's witnesses, grown from one finished attempt: an entry whose firing test held
// while its channel was inactive and which the chain later saw fire visibly.
inline void fold(const std::vector<AnchorRow>& v) {
    if (!rotQOrderReady()) {
        writeResult("dpsolve:   [rotseed] fold skipped - dp has not handed back the queue order");
        return;
    }
    const auto q = byChannel();
    int chan = 0;
    bool rev[16] = {};
    int ptr[16] = {};
    std::map<int, long long> first;
    std::string added;
    int pg = -1;
    for (long long t = 0; t < (long long)v.size(); ++t) {
        const AnchorRow& r = v[(size_t)t];
        if (!r.valid) continue;
        if (pg >= 0 && r.gframe != pg) {
            const std::vector<const RotQE*>& lst = q[(size_t)chan];
            int k = ptr[chan];
            while (k < (int)lst.size() && !visible(*lst[(size_t)k]) && !lst[(size_t)k]->swarm) ++k;
            if (k >= (int)lst.size() || !visible(*lst[(size_t)k]) || !near2(*lst[(size_t)k], r)) break;
            const RotQE& e = *lst[(size_t)k];
            ptr[chan] = k + 1;
            if (first.count(e.uid) && g_rotSurvivors.insert(e.uid).second)
                added += (added.empty() ? "" : ",") + std::to_string(e.uid);
            if (e.swarm && e.swch >= 0 && e.swch <= 15) {
                chan = e.swch;
                rev[chan] = (unsigned)(e.gnddir - 2) < 2u;
            }
        }
        pg = r.gframe;
        for (int c = 0; c <= 15; ++c) {
            if (c == chan) continue;
            const std::vector<const RotQE*>& lst = q[(size_t)c];
            for (int k = ptr[c]; k < (int)lst.size(); ++k)
                if (!first.count(lst[(size_t)k]->uid)
                    && passed(r.gframe, r.x, r.y, *lst[(size_t)k], rev[c]))
                    first[lst[(size_t)k]->uid] = t;
        }
    }
    writeResult("dpsolve:   [rotseed] fold rows=" + std::to_string(v.size()) + " added="
                + (added.empty() ? std::string("-") : added) + " survivors="
                + std::to_string(g_rotSurvivors.size()));
}

}  // namespace rotseed

// One call's queue arguments under cfg dprotseed: logged whatever the class, appended only
// when exact. Returns whether the queue was handed over, so the caller can log dp's read-back.
inline bool rotSeedArgs(long long t0, std::vector<std::string>& a, const char* site) {
    if (g_cfg.dpRotSeed <= 0 || t0 <= 0) return false;
    const std::vector<AnchorRow>& v = *anchors::g_src;
    rotseed::g_preButton = g_cfg.rotSeedPre;
    const rotseed::Seed s = rotseed::seedFor(v, t0, g_cfg.dpRotSeed);
    const AnchorRow* r0 = anchors::row(t0);
    // cfg dprotseedanchor=0: the anchored search is classified and logged like any other
    // call but runs without the queue (Config::dpRotSeedAnchor).
    const bool withheld = std::string(site) == "anchor" && !g_cfg.dpRotSeedAnchor;
    const char* queue = s.cls != "exact" ? "none" : withheld ? "withheld" : "given";
    char head[256];
    std::snprintf(head, sizeof head, "rotseed: site=%s t0=%lld level=%c class=%s rows=%zu x0=%.3f y0=%.3f",
                  site, t0, " AEFS"[g_cfg.dpRotSeed], s.cls.c_str(), v.size(),
                  r0 ? (double)r0->x : -1.0, r0 ? (double)r0->y : -1.0);
    writeResult(std::string(head) + " why=" + (s.why.empty() ? "-" : s.why)
                + " provenance=" + (s.prov.empty() ? "-" : s.prov)
                + " seed=" + (s.seed.empty() ? "-" : s.seed) + " queue=" + queue);
    if (s.cls != "exact" || withheld) return false;
    a.push_back("--rotqueue");
    a.push_back("--startrotq");
    a.push_back(s.seed);
    return true;
}

// ...and dp's own account of that seed, after the call: how many of its uids bound.
inline void logRotSeedRead(long long t0, const char* site) {
    const dpbridge::SolveOutcome o = dpbridge::outcome();
    writeResult(std::string("rotseed-read: site=") + site + " t0=" + std::to_string(t0)
                + " startrotq spent=" + std::to_string(o.startRotHit) + "/"
                + std::to_string(o.startRotGiven)
                + (o.startRotMiss.empty() ? "" : " NOT IN THE QUEUE: " + o.startRotMiss));
}

inline void addWorldArgs(std::vector<std::string>& a);

inline void addWorldArgs(std::vector<std::string>& a) {
    std::error_code ec;
    // ...and where the moving parts of it were. The static table holds their positions at level
    // entry only, so without this a sinking platform is a floor that never sinks and a level
    // built on them is planned in a world that does not exist.
    // Order is override order, weakest first. The no-death recordings are a different worldline
    // from the plan (nothing died in them), so they are only trustworthy where the real replays
    // have not reached; wherever one has, it wins. The input-free bootstrap is the weakest of
    // all -- it is what the level does when nobody plays it.
    // cfg groupsretime: ...and a no-death record whose replay parted from the live record's is on
    // another clock, so it is left out rather than laid under the live one (see g_groupsDeepX).
    long long bootPart = -1, deepPart = -1;
    if (g_cfg.groupsRetime) {
        bootPart = clockPart(g_groupsBootX, g_groupsX);
        deepPart = clockPart(g_groupsDeepX, g_groupsX);
        const int out = (bootPart >= 0 ? 1 : 0) | (deepPart >= 0 ? 2 : 0);
        if (out != g_retimeLeftOut) {
            g_retimeLeftOut = out;
            char b[224];
            snprintf(b, sizeof(b), "dpsolve:   moving geometry: layers on another clock than the "
                     "live record - bootstrap %s (t=%lld), deep %s (t=%lld)",
                     bootPart >= 0 ? "left out" : "kept", bootPart,
                     deepPart >= 0 ? "left out" : "kept", deepPart);
            writeResult(b);
        }
    }
    if (g_cfg.dpGroups && !g_groupsBootPath.empty() && bootPart < 0
        && std::filesystem::exists(g_groupsBootPath, ec)) {
        a.push_back("--groups");
        a.push_back(g_groupsBootPath);
    }
    if (g_cfg.dpGroups && !g_groupsDeepPath.empty() && deepPart < 0
        && std::filesystem::exists(g_groupsDeepPath, ec)) {
        a.push_back("--groups");
        a.push_back(g_groupsDeepPath);
    }
    if (g_cfg.dpGroups && !g_groupsPath.empty()
        && std::filesystem::exists(g_groupsPath, ec)) {
        a.push_back("--groups");
        a.push_back(g_groupsPath);
        // ...and it is the last overlay and ended on a death (harvestGroups writes it only then),
        // so its last row holds a tick.
        a.push_back("--groupholddeath");
    }
    // (A turned object going on turning past the end of those recordings, and a mover's moves
    // going on one by one past them, are no longer asked for: the solver does both whenever it
    // reads a recording since the 0.4.0 clean-up.)
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
        // A COLLAPSED BAND IS WRITTEN DOWN. This used to skip `pmax <= pmin`
        // outright, and since the reader holds the last row until the next one,
        // "the band collapsed here" became "the band is still the last one I
        // saw" -- an old, non-degenerate band handed out for the whole stretch.
        // The four columns are `tick,kind,floor,ceil`; the reader takes the
        // column count as the version, so a file written by an older build
        // still reads.
        // ONLY WHERE THE BAND CLAMPS. The model reads the recorded band in its ship/UFO/swing,
        // ball/spider and wave clamps (dp step.hpp), never for a cube or a robot, and there GD's
        // pmin/pmax clamp nothing -- but they carry the camera's history, so two flights of one
        // plan wrote different files there and the DP's input differed between two runs of the
        // same cold (SubZero 4001, 2026-09-26: the same flight, 1.9 s against 0.9 s of wall time,
        // wrote 47,052 and 47,418 bytes, and the runs parted on it at round 11). A tick is written
        // when it, the tick before or the tick after is in a band mode, either body: the fly floor
        // reads row t-1 and the ceilings row t+1, so a stretch's edge ticks are rows its clamps
        // read. Elsewhere the reader holds the last row written, the last band tick's.
        auto bandMode = [](const AnchorRow* a) {
            auto m = [](int k) { return k == 1 || k == 2 || k == 3 || k == 4 || k == 6 || k == 7; };
            return a && (m(a->mode) || m(a->m2));
        };
        float lf = -1e9f, lc = -1e9f;
        char lk = 0;
        long long rows = 0;
        const long long n = anchors::depth();
        for (long long t = 1; t < n; ++t) {
            const AnchorRow* r = anchors::row(t);
            // No row at all is not the same as a row saying there is no band:
            // this side genuinely has nothing to say about that tick.
            if (!r) continue;
            if (!bandMode(r) && !bandMode(anchors::row(t - 1)) && !bandMode(anchors::row(t + 1)))
                continue;
            const char k = (r->pmax > r->pmin) ? 'I' : 'D';
            // Only changes: the reader holds the last row's value until the
            // next, so flat stretches cost one row instead of thousands.
            if (k == lk && r->pmin == lf && r->pmax == lc) continue;
            lk = k; lf = r->pmin; lc = r->pmax;
            if (k == 'I')
                bf << t << ",I," << r->pmin << ',' << r->pmax << '\n';
            else
                bf << t << ",D," << r->pmin << ',' << r->pmin << '\n';
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
            // cfg `dpbandend=1`: ...and where that recording stops. The reader holds the last
            // row indefinitely, which overrides the band a mode portal past the recording
            // writes (bands.hpp g_bandTrackEnd has the lv10 measurement). n-1 is the last
            // tick the loop above could have written.
            a.push_back("--bandtrackend");
            a.push_back(std::to_string(n - 1));
        }
    }
    // ...and where GD had the controls switched off (--ctrlwin, both ends
    // inclusive). The model cannot derive these itself -- which crossing makes
    // GD raise id 2899 is unsolved, and firing one on a guess does more harm
    // than the real thing (frames.hpp) -- so they are passed only as GD recorded
    // them, from the same anchor source as --start and --bandtrack: for a fixup
    // pass that is the attempt that just died, so the resim replays the very
    // plan the windows were recorded on. A window still open at the last
    // recorded tick closes there; past the recording the model has no windows,
    // which is the hole it always had.
    std::string wins;
    long long t0 = -1, last = -1;
    const long long n = anchors::depth();
    for (long long t = 1; t < n; ++t) {
        const AnchorRow* r = anchors::row(t);
        if (!r) continue;
        last = t;
        if (r->ctrlOff && t0 < 0) t0 = t;
        if (!r->ctrlOff && t0 >= 0) {
            wins += (wins.empty() ? "" : ",") + std::to_string(t0) + ":"
                    + std::to_string(t - 1);
            t0 = -1;
        }
    }
    if (t0 >= 0)
        wins += (wins.empty() ? "" : ",") + std::to_string(t0) + ":"
                + std::to_string(last);
    if (!wins.empty()) {
        a.push_back("--ctrlwin");
        a.push_back(wins);
    }
    // The level's own compatibility flags, written beside objrects when the session opened.
    // This one is NOT under dpWorld: kA39 changes the SHAPE of every circular hazard's test
    // (centre distance instead of the player's rect against the circle -- see hazardHit), which
    // is physics and not world state. The CLI finds the file next to the objrects path on its
    // own; the in-process caller passes the level in memory and argv[1] is a placeholder, so
    // here it has to be named. Levels without the file are unaffected: the loader leaves every
    // flag at 0, which is what 21 of the 22 official levels actually have.
    const std::string lset = std::string(DATA_DIR) + "/levelsettings.txt";
    if (std::filesystem::exists(lset, ec)) {
        a.push_back("--levelsettings"); a.push_back(lset);
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
        // The mask's full width, not 32. This side cannot name dp's kTouchBits
        // (no dp/ header here -- the same reason SolveOutcome carries 64-bit
        // fields), so iterate all 64: g_needTrigDropped is that wide on
        // purpose, and bits past dp's actual width are never set, so this stays
        // correct whatever the constant is. A cap of 32 silently stopped
        // emitting --needtrig-skip for the very boxes a wider mask addresses.
        for (int b = 0; b < 64; ++b)
            if (g_needTrigDropped & (1ull << b)) {
                a.push_back("--needtrig-skip");
                a.push_back(std::to_string(b));
            }
        // cfg routeprereq: the box the current wall waits on (see g_routeNeedUid). Named by uid --
        // the bit is this call's window's -- and dp drops it on its own when the anchor is past it.
        if (g_cfg.routePrereq && g_routeNeedUid >= 0) {
            a.push_back("--needtrig-uid");
            a.push_back(std::to_string(g_routeNeedUid));
        }
    }
    if (std::filesystem::exists(obb, ec)) { a.push_back("--obb"); a.push_back(obb); }
    // Which force blocks share a forceID. GD counts a push once per ID per tick (player+0xb98
    // is a set), so two boxes with the same positive ID are one push and the model was adding
    // both -- lv22 t=20,961 is the case. solver.hpp has written the column since 09-04; the
    // solver has taken the file since e0c2fd2 and treats a level without it exactly as before,
    // which is every official level but lv22. It was measured through the CLI and never handed
    // to the loop, so a cold run kept the doubled push that the replay suite had already lost.
    const std::string fids = std::string(DATA_DIR) + "/forceblocks.txt";
    if (std::filesystem::exists(fids, ec)) { a.push_back("--forceids"); a.push_back(fids); }
    // The 2.2 rotation queue's inputs (solver.hpp writes rotgameplay.txt at session
    // start). UNDER dpWorld, unlike levelsettings above: a queue entry is a trigger
    // sitting at a position, i.e. world state, whereas kA39 changes the shape of a
    // hazard test and is physics -- the distinction :681-683 draws.
    //
    // Named here for the same reason levelsettings is: the CLI finds the file beside
    // the objrects path, and the in-process caller's argv[1] is a placeholder, so
    // rotQPathBeside returns "" (level_loader.hpp:1667). The consequence was not a
    // missing convenience -- the loop could not load the queue AT ALL, so `--rotqueue`
    // in the cfg set g_rotQueue while g_rotQ stayed empty and step.hpp's
    // `g_rotQueue && !g_rotQ.empty()` was false either way. An arm turned on that way
    // measures nothing and reports it as "the queue changes nothing".
    //
    // PAIRED WITH THE FLAG, not passed unconditionally: with no --rotqueue there is
    // nothing to consume the queue, so reading the file would be pure cost, and the
    // loop's argv would change for a run that behaves identically. Gated, the default
    // arm is unchanged byte for byte and that can be shown by grep.
    // cfg dprotseed passes it on EVERY call as well, without --rotqueue: a loaded queue
    // is only consumed when --rotqueue is also given (step.hpp), and loading it is how
    // dp hands back the order rotSeedArgs derives its seeds against.
    if (rotQueueRequested() || g_cfg.dpRotSeed > 0) {
        const std::string rotq = std::string(DATA_DIR) + "/rotgameplay.txt";
        if (std::filesystem::exists(rotq, ec)) {
            a.push_back("--rotgameplay"); a.push_back(rotq);
        }
    }
    // GD's MAX GAMEPLAY Y, read live off the layer (g_maxPlayYLive). Closes the
    // sky-escape phantom: without it the DP plans free climbs into y=3,600+
    // that GD environment-kills (dp/speed.hpp g_maxPlayY has the disassembly).
    if (g_maxPlayYLive > 0.f) {
        a.push_back("--maxplayy");
        a.push_back(num(g_maxPlayYLive));
    }
}


// Say once what the solver is actually being told, and again whenever it changes.
//
// CALLED AT THE HANDOVER, not inside baseArgs. The argv is assembled in two places --
// baseArgs builds the common part and each caller appends its own (--start, --startband,
// --anchor-state, the cfg dpArgs) -- so a line printed inside baseArgs carries the half
// that is the same everywhere and omits the half that distinguishes one solve from the
// next. Measured 2026-09-10: rebuilding three of a cold run's lv22 solves from the logged
// args reproduced none of them (first death 1813 -> 1837, 1898 -> 1836, 6322 -> 6402),
// because the flags that differed were never written down.
// cfg dpsnapshot: a copy of `path` under DATA_DIR/snaps, named by its own size/fnv (the
// same fileSig the call's `input sig` line carries), so identical contents are kept once
// and any copy can be checked against the call that read it. Returns the copy's name
// relative to DATA_DIR, or "" when there is nothing to copy.
inline std::string snapFile(const std::string& path, const std::string& knownSig = "") {
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec)) return "";
    std::string sig = knownSig.empty() ? fileSig(path) : knownSig;
    for (char& c : sig) if (c == '/') c = '_';
    const std::filesystem::path src(path);
    const std::string name = "snaps/" + src.stem().string() + "_" + sig + src.extension().string();
    const std::filesystem::path dst = std::filesystem::path(DATA_DIR) / name;
    std::filesystem::create_directories(dst.parent_path(), ec);
    if (!std::filesystem::exists(dst, ec))
        std::filesystem::copy_file(src, dst, std::filesystem::copy_options::skip_existing, ec);
    return ec ? "" : name;
}

// The flags whose value is a FILE the solver reads. One table, read by both the signature and
// the snapshot below. They used to keep a list each, and --rejoinwatch (the model's trace of the
// plan that died, which --rejoinuse acts on) was on neither: two calls could log the same argv
// and the same `input sig` and still read different inputs (measured
// 2026-09-19 22:42, a call attributed as "same input" was reading dp_rejoin.trace.csv). A flag that
// takes a file goes in here, or both lines stay silent about what it read.
inline constexpr const char* kFileArgs[] = {
    "--groups", "--fixups", "--bandtrack", "--rejoinwatch", "--replay",
    "--triggers", "--objgroups", "--obb", "--levelsettings", "--rotgameplay",
    "--forceids"};
inline bool isFileArg(const std::string& s) {
    for (const char* f : kFileArgs)
        if (s == f) return true;
    return false;
}

// The job writes replay plans and fixups; all other listed inputs are published between jobs.
inline bool immutableInput(const std::string& flag) {
    return isFileArg(flag) && flag != "--fixups" && flag != "--replay";
}

inline solver::FailedPlans g_failedPlans;
inline solver::PlanContext g_candidateContext;
inline solver::PlanEdges g_candidateEdges;
inline bool g_repeatRejected = false;

// Record the complete click sequence, including the verified prefix and an empty plan.
inline solver::PlanEdges planEdges(const std::vector<InputCmd>& plan) {
    solver::PlanEdges edges;
    for (const auto& c : plan) edges.emplace_back(c.step, c.down ? 1 : 0);
    return edges;
}

// Include live settings not held in Config as well as the caller's complete argv.
inline std::string planConfig() {
    return effcfg::modCfg() + " maxplayy=" + num(g_maxPlayYLive)
           + " coinmargin=" + num(g_coinMarginNow);
}

// A rejoin trace chooses a route; it is not physics. All model/world files and settings are.
inline solver::PlanContext planContext(const std::vector<std::string>& args) {
    solver::PlanContext c;
    c.level = dpbridge::inputLevelRevision();
    c.fp = (unsigned)_mm_getcsr() & ~0x3fu;   // control bits, not arithmetic status flags
    c.config = planConfig();
    c.args = args;   // includes runtime vetoes, seed payloads and every search policy
    c.valid = c.level != 0;
    for (size_t i = 0; i + 1 < args.size(); ++i)
        if (isFileArg(args[i]) && args[i] != "--replay" && args[i] != "--rejoinwatch") {
            const auto f = dpbridge::inputFileInfo(args[i + 1], immutableInput(args[i]));
            c.files.emplace_back(args[i] + "=" + args[i + 1], f.revision);
            if (!f.revision) c.valid = false;
        }
    return c;
}

// WHICH WORLD THE CALL JUST PLANNED IN. Written after a solve, once per call,
// from the bridge rather than from dp's stdout -- dp's printf does not reach
// result.txt, so the coverage line and the auto-window's own message are
// invisible in-process. Grepping result.txt for either returns 0 whether they
// fired or not, and that 0 was nearly read as "the gate never fires".
//
// `win=-1` means the call had no touch boxes; it is NOT `win=0`. `map=` is a
// hash over the kept boxes' uids IN BIT ORDER, so the first call whose bit->uid
// mapping differs from another run's is the one whose `map=` differs -- which
// is the join a width or window change has to be traced through.
// cfg coinroute: take the tap-gated coins' shut points from the search call just
// made (dp Outcome::coinGates). Only a SEARCH carries --coins -- the fixup resim
// builds its own argv without it -- so only a search refreshes the table.
inline void adoptCoinGates() {
    if (!g_cfg.coinRoute) return;
    std::vector<solver::CoinGate> v;
    std::stringstream ss(dpbridge::outcome().coinGates);
    for (std::string item; std::getline(ss, item, ';');) {
        solver::CoinGate g;
        int miss = 0;
        if (std::sscanf(item.c_str(), "%d,%d,%lf,%lf,%d,%d,%d,%d,%lf,%lf", &g.uid, &g.chan,
                        &g.x, &g.y, &g.dir, &g.item, &g.need, &miss, &g.mx, &g.my) == 10) {
            g.miss = miss != 0;
            v.push_back(g);
        }
    }
    solver::g_coinGates.swap(v);
    // ...and the coins the search does not call missed when passed (cfg coinmissrev).
    std::vector<int> nm;
    std::stringstream sn(dpbridge::outcome().coinNoPrune);
    for (std::string item; std::getline(sn, item, ';');)
        if (!item.empty()) nm.push_back(std::atoi(item.c_str()));
    solver::g_coinNoMiss.swap(nm);
}

inline void logTrigWindow(const char* tag) {
    const dpbridge::SolveOutcome o = dpbridge::outcome();
    if (o.trigWinTouch < 0 && o.trigTotal == 0) return;   // no boxes: say nothing
    // ...and whether this call's own reading is even about this level. Past
    // maxKeptX the run happened in a world missing droppedRelevant boxes, so a
    // wall there is not evidence of a wall (the only measure of it lived in the
    // offline refaudit until now). A
    // LABEL, not a gate: nothing downstream reads it yet, and turning it into
    // a verdict is a rule change that needs its own evidence.
    // AHEAD ONLY. A windowed anchor drops the boxes BEHIND it deliberately --
    // the world they already moved is in the recording -- so the first version
    // of this label fired on every late anchor and read as "the shaft is
    // uncovered" when it was the window working. The
    // behind count is still printed, under its own name and without a warning.
    char cov[128] = "";
    if (o.trigDroppedAhead > 0 && o.deepX > o.trigMaxKeptX)
        std::snprintf(cov, sizeof cov,
                      "  UNCOVERED past x=%.0f (%lld relevant boxes AHEAD not "
                      "loaded, deepest x=%.0f)",
                      o.trigMaxKeptX, o.trigDroppedAhead, o.deepX);
    char b[448];
    std::snprintf(b, sizeof b,
                  "dpsolve:   [trigwin] %s win=%d total=%lld relevant=%lld "
                  "kept=%lld dropBehind=%lld dropAhead=%lld maxKeptX=%.0f "
                  "map=%016llx%s",
                  tag, o.trigWinTouch, o.trigTotal, o.trigRelevantN, o.trigKept,
                  o.trigDroppedBehind, o.trigDroppedAhead, o.trigMaxKeptX,
                  o.trigMapSig, cov);
    writeResult(b);
}

// cfg dpphaseprof (print only): how long each call into dp took, whole -- `dpcall:` lines -- next to
// dp's own `phaseprof:` (passed --phaseprof), whose prep= is the part before the first layer. A
// ladder rung that comes back doomed at once still pays the call; how much of that is reading and
// building is the question this answers.
// The session's dpplainbeside mode: the cfg key when given, else the settings menu (start()).
inline int g_plainBesideMode = 0;
inline int timedSolve(const std::string& csv, const std::vector<std::string>& a,
                      const char* kind) {
    const auto c0 = std::chrono::steady_clock::now();
    dpbridge::plainBeside(g_plainBesideMode);   // cfg dpplainbeside (config.hpp) or the menu
    const int rc = dpbridge::solveInProcess(csv, a);
    {   // the cap ladder's account of the call, for a change to the ladder to be checked against
        const std::string lad = dpbridge::outcome().ladder;
        if (!lad.empty()) writeResult(std::string("dpsolve:   [ladder] ") + kind + " " + lad);
    }
    if (g_plainBesideMode == 2) {
        const std::string c = dpbridge::besideCheckTaken();
        if (!c.empty()) writeResult(std::string("dpsolve:   [beside] ") + kind + ": " + c);
    }
    if (g_cfg.dpPhaseProf) {
        const std::vector<double> m = dpbridge::prepMarks();
        auto at = [&](size_t k) { return k < m.size() ? m[k] : -1.0; };
        char b[224];
        snprintf(b, sizeof(b), "dpcall: %s %.3f s (prep: args %.3f groups %.3f objpos %.3f "
                 "touch %.3f level %.3f tables %.3f)", kind,
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - c0).count(),
                 at(0), at(1), at(5), at(2), at(3), at(4));
        writeResult(b);
    }
    return rc;
}

inline void logSolverArgs(const std::vector<std::string>& a) {
    std::string line = "dpsolve: solver args:";
    for (const std::string& s : a) line += " " + s;
    if (!g_argsLogged || line != g_argsLast) {
        g_argsLogged = true;
        g_argsLast = line;
        // WHICH ATTEMPT this call's --start was read off. A cold run sends dozens of
        // attempts through the same tick, so a reader joining a call to a per-tick
        // recording (`slprec:`) on the tick alone silently takes whichever attempt was
        // written last -- a different trajectory at the same clock.
        // It goes on its own line, immediately before the argv and never without it, so
        // that the argv line stays exactly the argv: seedcheck replays that line.
        // It is NOT on the `[anchor]` line, which looks like the place for it and is not:
        // measured on one lv16 cold, `[anchor]` covers 104 of the 147 calls, and the
        // anchor this was first wanted for (t=4,353) is in the other 43.
        writeResult("dpsolve:   [call] att=" + std::to_string(g_attempt));
        writeResult(line);
    }
    // ...and the half of the input that is not in the argv at all. cli.hpp
    // prints the same register on its way in, but dp's printf does not reach
    // result.txt (see the [anchor] note below), so the comparison needs a copy
    // written down HERE, on the thread that is about to call the solver.
    // Rounding mode is a runtime property of the process, and the process the
    // mod lives in has had cocos2d, fmod and the graphics driver in it first.
    char fb[64];
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
    std::snprintf(fb, sizeof fb, "dpsolve: fpenv mxcsr=0x%04x",
                  (unsigned)_mm_getcsr());
#elif defined(__aarch64__)
    unsigned long long fpcr = 0;   // AArch64's flush-to-zero and rounding mode
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    std::snprintf(fb, sizeof fb, "dpsolve: fpenv fpcr=0x%08llx", fpcr);
#endif
    writeResult(fb);
    // ...and WHICH BYTES the file arguments held when this call was made.
    //
    // The recordings are rewritten as the loop goes: dp_groups.txt runs from
    // 79 KB at iteration 1 to 23 MB at iteration 41, over 22 distinct contents
    // in one lv22 run ([fp]'s `live=` field). An offline rebuild of call N
    // therefore reads a file call N never saw, and every "the two dp builds
    // disagree" measurement taken that way was comparing two different inputs.
    // That invalidated a whole afternoon's comparison on 2026-09-10; the argv
    // was identical every time, which is exactly what made it convincing.
    //
    // Signing the arguments here makes the mismatch detectable instead of
    // invisible: a rebuild can check the file it is about to read against the
    // signature the call recorded, and refuse rather than produce a number.
    // The line is per-call and unconditional, because the whole point is that
    // it changes between calls whose argv does not.
    std::string sig = "dpsolve: input sig";
    for (size_t i = 0; i + 1 < a.size(); ++i)
        if (isFileArg(a[i]))
            sig += " " + a[i].substr(2) + "="
                   + dpbridge::inputFileInfo(a[i + 1], immutableInput(a[i])).signature;
    writeResult(sig);
    // ...and, under cfg dpsnapshot, the bytes themselves (snapFile), for the same files. The
    // static ones cost one copy per level: a copy is named by its contents and kept once.
    if (g_cfg.dpSnapshot) {
        std::string snap = "dpsolve: input snap";
        for (size_t i = 0; i + 1 < a.size(); ++i)
            if (isFileArg(a[i])) {
                const auto info = dpbridge::inputFileInfo(a[i + 1], immutableInput(a[i]));
                const std::string n = snapFile(a[i + 1], info.signature);
                snap += " " + a[i].substr(2) + "=" + (n.empty() ? "-" : n);
            }
        writeResult(snap);
    }
}

inline long long g_rjAfterTick = -1;   // cfg dprejoinwatch: the death the trace ends in
// cfg dprejoinuse: whether the last tail a rung returned was a join, and -- taken at the
// death -- whether the plan that died was one. A join is not joined again: in the first measured
// run every repeat of lv20's and lv22's early walls was a join onto the plan the previous join had
// made, dying again a few dozen ticks on (lv20 +5 rounds; lv22 t=1941/1941/1828 twice).
inline bool g_rjTailJoined = false;
inline bool g_rjOldIsJoin = false;
// cfg dpsecreuse: the plan being flown is a section solve's splice, which no model has walked.
// Its death keeps the rejoin target it inherited -- the walk of the plan the rung was fired from --
// so the searches after it can come back onto that plan and keep the rest of it.
inline bool g_rjKeepTarget = false;
inline std::string rejoinTracePath() { return std::string(DATA_DIR) + "/dp_rejoin.trace.csv"; }
inline std::vector<std::string> baseArgs(const std::string& out,
                                         double x0 = std::numeric_limits<double>::quiet_NaN()) {
    const bool tiered = g_capTier > 0;
    std::vector<std::string> a{"--out", out,
                               "--horizon", std::to_string(horizonFor(x0)),
                               "--cap", std::to_string(tiered ? kCapTiers[g_capTier - 1] : kCap),
                               "--shipyq", num(tiered ? kTierYq : kYq),
                               "--shipvq", num(tiered ? kTierVq : kVq),
                               "--threads", kThreads};
    if (g_cfg.dpPhaseProf) a.push_back("--phaseprof");   // print only (see timedSolve)
    // --groupfire: searches only, which is what the flag says to the solver -- it changes how a
    // search SHARES a placement between states, which a replay (one state, its own fire ticks)
    // never did. Always passed here since the 0.4.0 clean-up removed cfg dpgroupfire.
    a.push_back("--groupfire");
    // cfg dpcapladder: a small cap first, raised only where the search runs out of states.
    // Not on the tiers: those are the escalation that asked for MORE states on purpose.
    if (g_cfg.dpCapLadder > 0 && !tiered) {
        a.push_back("--capladder");
        a.push_back(std::to_string(g_cfg.dpCapLadder));
        // (The ladder's step-back and its envelope are the solver's fixed behaviour since the
        // 0.4.0 clean-up; nothing is passed for them.)
    }
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
    // ...and the glitch-avoid margins, the user's choice (Config::glitchPortal). Searches only:
    // the fixup resim builds its own argv and must not die where the game does not.
    if (g_cfg.glitchPortal > 0.0) {
        a.push_back("--glitchportal");
        a.push_back(num(g_cfg.glitchPortal));
    }
    if (g_cfg.glitchWave > 0.0) {
        a.push_back("--glitchwave");
        a.push_back(num(g_cfg.glitchWave));
    }
    if (g_cfg.glitchEmbed > 0.0) {
        a.push_back("--glitchembed");
        a.push_back(num(g_cfg.glitchEmbed));
    }
    if (g_cfg.glitchDeco) a.push_back("--glitchdeco");
    addWorldArgs(a);
    // cfg dprejoinwatch: the model's trace of the plan that last died,
    // for dp's --rejoinwatch (refwatch.hpp).
    if (g_rjAfterTick >= 0
        && std::filesystem::exists(rejoinTracePath(), ec)) {
        a.push_back("--rejoinwatch");
        a.push_back(rejoinTracePath());
        a.push_back("--rejoinafter");
        a.push_back(std::to_string(g_rjAfterTick));
        a.push_back("--rejoinuse");
        if (g_cfg.dpRejoinFull) a.push_back("--rejoinfull");
    }
    // EXPERIMENT (cfg dpoffboardkill): the search applies this loop's own playfield bound
    // (offBoardTick / kOffBoard), so it stops offering arcs the loop scores as dead on arrival.
    if (g_cfg.dpOffBoardKill) {
        a.push_back("--offboard");
        a.push_back(num(kOffBoard));
    }
    // cfg coinroute: the goal is the end AND every coin. Only the searches take this (baseArgs
    // is their argv); the fixup resim builds its own and must not have a miss prune in it.
    if (g_cfg.coinRoute) a.push_back("--coins");
    // cfg dpcoinwin: the cap's (y, vy) cells before each coin still lacked (dp thread_pool.hpp
    // g_coinWinY). Searches only, like the prune it keeps branches alive against.
    if (g_cfg.coinRoute && !g_cfg.dpCoinWin.empty()) {
        a.push_back("--coinwin");
        a.push_back(g_cfg.dpCoinWin);
    }
    // (The witness resim inside this search fires the item gates too: the solver's fixed
    // behaviour since the 0.4.0 clean-up removed --walkgates.)
    // ...and the pickups a Count trigger counts (items.txt, written beside the
    // level dump). Passed only with the flag: nothing else reads a counter.
    if (g_cfg.coinRoute) {
        const std::string ip = std::string(DATA_DIR) + "/items.txt";
        if (std::filesystem::exists(ip, ec)) {
            a.push_back("--items");
            a.push_back(ip);
        }
        // ...and how far inside a coin this run has learnt to plan (see
        // g_coinMarginNow). 0 is dp's default, so it is passed only once it has
        // grown -- a run that never has a coin refused looks exactly as before.
        if (g_coinMarginNow > 0.0) {
            a.push_back("--coinmargin");
            a.push_back(num(g_coinMarginNow));
        }
    }
    // cfg dpinputgrid: before dparg, so a dparg=--inputgrid still has the last word.
    if (g_cfg.dpInputGrid > 1) {
        a.push_back("--inputgrid");
        a.push_back(std::to_string(g_cfg.dpInputGrid));
    }
    for (const std::string& s : g_cfg.dpArgs) a.push_back(s);
    // An input grid (cfg dpinputgrid, or dparg=--inputgrid) is a cheap search, not the loop's
    // last word: once the loop has escalated to a capacity tier or thrown the prefix away, the
    // searches run at full timing precision (--gridmap -1e9:1). A wall the grid makes -- a press
    // it cannot place, e.g. the ship portal at x~12,230 of a custom level, which no tier could
    // pass with the grid on -- otherwise survives every escalation. Nothing is added to a run
    // without a grid.
    if (g_capTier > 0 || g_coldRestarted) {
        bool grid = g_cfg.dpInputGrid > 1;
        for (const std::string& s : g_cfg.dpArgs) grid = grid || s == "--inputgrid";
        if (grid) {
            a.push_back("--gridmap");
            a.push_back("-1e9:1");
        }
    }
    // Say once what the solver is actually being told. Everything above is assembled from a
    // dozen switches and files, and when a run behaves unlike another the first question is
    // always which of them differed -- a question that cost a session to answer by inference
    // (2026-08-27: a cfg `dparg` was passed, believed delivered, and the A/B built on that
    // belief was wrong twice). One line, at the first solve of a session.
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
    // cfg `dphoverstrict=1`: NO FLAT RUN AT ALL IS NOT A FULL BUDGET. When vy already differs from
    // the tick before, the loop above stops at once and `left` came out as the whole 67 -- for a
    // robot rising or falling under gravity, which is not hovering. A hover that begins exactly on
    // t0 looks the same backwards, so the tick after decides: flat into t0+1 is a hover that has
    // just started, anything else is not a hover.
    // Measured on lv19 (2026-09-17): the anchor at t0=12,310 (robot, airborne, held, vy=2.014,
    // changed from t0-1) was sent with 67; every fixup after it reads dvy=-0.194 per tick with the
    // model level and GD falling -- the signature of a budget GD did not have -- and the cold run
    // spent two iterations at x=17,600 on it.
    if (j == t0) {
        const AnchorRow* next = anchors::row(t0 + 1);
        const bool startsHere = next && !next->onGround && next->vy == cur->vy;
        return startsHere ? kRobotHoverTicks : 0;
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

// Does a SWING anchored at t0 still owe the flip of a press? The swing's press sets a pending
// bit and the flip lands on the tick after its effect (dp: step.hpp's rHover block), and a
// swing's input latency is 1, so a rising edge pressed at t0-1 has taken effect on t0 and not
// flipped yet. dp's --replay derives this from the plan itself (cli.hpp preRise); a search
// started from --start cannot, because the edge is in the kept prefix, and without the bit it
// plans a swing that never flips.
// Measured on lv22 (2026-09-17): `input=7239,1` / `input=7240,0`, GD's upsideDown 0 through
// t=7240 and 1 from 7241. The anchor at t0=7240 said held=1 flip=0; the tail solved from it kept
// the swing unflipped (y=390.55 at t=7300, clear of spike uid9601), came back SOLVED, and GD
// flipped and died at x=9,890 -- the same plan, three times in one cold run. With the bit, the
// same tail replayed from the same anchor matches GD on every tick 7241..7300 and dies on 9601.
// Gated by cfg `dpswingpending`.
inline int swingPendingAt(const std::vector<InputCmd>& plan, long long t0) {
    int prev = 0, last = 0;
    long long lastStep = -1;
    for (const InputCmd& c : plan) {
        if (c.step >= t0) break;
        prev = last;
        last = c.down ? 1 : 0;
        lastStep = c.step;
    }
    return (lastStep == t0 - 1 && last == 1 && prev == 0) ? 1 : 0;
}

// The first 27 of the 30 fields of `--start`, in the order leveldp reads them
// (28 flipT, 29 armT and 30 rotStep are not written here, so every anchor this
// loop takes reads them as "not said"; history values go through the
// --anchor-state hist payload instead -- see histPayload):
//   t0, x, y, vy, mode, grounded, held, flip, mini, dual, y2, v2, f2, g2, speed,
//   robotHover, dashHeld, dashSlope, snapUid, snapDist, gframe, reversed,
//   rot, rotNeg, boost, mode2, mini2
inline std::string startArg(long long t0, const AnchorRow& r, int held, int swingPending = 0) {
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
    // ...and a swing's pending flip rides the same field (rHover is robot-only in dp and free in
    // mode 7; see swingPendingAt).
    const int hover = r.dashing ? 0
                    : (r.mode == 7 && swingPending) ? 1 : robotHoverLeft(t0);
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
    // cfg dpanchorrotstep: 28th/29th "not said" (-1, dp's own default for both) and the 30th, the
    // size of the step the angle just made, which dp takes as a ball's stake and ignores in every
    // other mode. A ball's stake outlives what wrote it, so without this an anchored ball in the
    // air does not turn until something stakes it again: a custom level, the 09-30 cold run's
    // resim from t=1,767 (the ball at 0.7, GD turning -1.4274 a tick) sat at 175.364 deg while GD
    // turned, and a turned portal's test reads that angle. Read from the rows like the 24th, so an
    // event on t0 that stakes for the next tick only (a ring) is not seen; m_rotationSpeed would
    // see it, but its scale against the angle's step is not measured here.
    if (g_cfg.dpAnchorRotStep || (g_cfg.dpAnchorRot2 && r.dual)) {
        double step = 0.0;
        if (g_cfg.dpAnchorRotStep)
            if (const AnchorRow* prev = anchors::row(t0 - 1))
                step = std::fabs(std::remainder((double)r.rot - (double)prev->rot, 360.0));
        s += ",-1,-1," + num(step);
    }
    // cfg dpanchorrot2: the 31st-33rd, the second body's angle and the sign and size of the step
    // it just made, read from the rows exactly as the 23rd, 24th and 30th are for p1. dp --rot2
    // gives each body its own angle; without these it starts p2 from p1's, the mirror of p2's own
    // for a mirrored UFO pair -- custom level C, a cold run's attempt 16) has p2
    // taking the -54 deg spider portal uid 464 a tick before GD.
    if (g_cfg.dpAnchorRot2 && r.dual) {
        double d2 = 0.0;
        if (const AnchorRow* prev = anchors::row(t0 - 1))
            if (prev->dual) d2 = std::remainder((double)r.rot2 - (double)prev->rot2, 360.0);
        s += "," + num(r.rot2) + "," + std::to_string(d2 < 0.0 ? 1 : 0) + "," + num(std::fabs(d2));
    }
    return s;
}

// The anchor payload (dp's --anchor-state), built from what GD actually did
// rather than from what a recording can be made to say. Named keys, because
// the 26 positional fields above cannot take another without every reader
// changing; trigger UIDS, because dp's bit numbering is a property of its own
// 32-box window and a bit index would mean a different box whenever that
// window moved.
//
// Only activations at or before t0 belong to an anchor at t0 -- the map holds
// the whole attempt, including triggers this anchor has not reached yet.
// It declares the SUBSYSTEM it owns rather than a list of values. `owns=touch`
// means dp takes trig and fireB from here and leaves every other seed to the
// path that already fills it -- the lock keeps its recording-derived seeding,
// because the lock's semantics (which box is locked, how long the window runs,
// when it freezes) live in dp and would have to be re-implemented here to be
// carried. lv19 is the only level with a lock and its recording-derived seed
// already measures zero, so nothing is lost by leaving it there.
//
// Every value is the state AT t0: a value written at t0+1 lands one tick of
// motion further on, which shows up as a first-tick difference of about one dx.
//
// OWNERSHIP IS CLAIMED FROM WHAT WAS CARRIED, NEVER UNCONDITIONALLY. An empty
// payload that still says `owns=touch` tells dp to take trig and fireB from
// here and then hands it nothing, so the recording-derived seeding is
// suppressed on every level and replaced by nothing. That is not hypothetical:
// on lv22 the map and dp's box window name almost disjoint objects (0 of 24
// mapped, see Config::touchPayload), so the always-on version was that empty
// case in practice. The call sites are gated too, but a gate is caller
// discipline -- it protects the two call sites that exist today and not the
// third one. Carrying nothing must be indistinguishable from not being asked.
inline std::string anchorPayload(long long t0) {
    std::string body;
    for (const auto& kv : anchors::seeds().touch) {
        if (kv.second > t0) continue;
        if (!body.empty()) body += ",";
        body += std::to_string(kv.first) + ":" + std::to_string(kv.second);
    }
    if (body.empty()) return std::string();
    return "owns=touch;touch=" + body;
}

// ...and the same for the gravity portals dp has already SPENT by t0. Separate
// from the touch payload rather than folded into it, because the two subsystems
// are in different states: touch is off by default over a population mismatch
// (Config::touchPayload), while the portal map's uids are dp's own portal uids
// and map exactly. Sharing one function would tie the portal seeding to that
// gate for no reason.
//
// Both keys ALWAYS, once ownership is claimed. dp refuses a payload that says
// `owns=portal` and omits either -- a mask nobody sets is the shape of silent
// degradation this whole mechanism exists to stop -- so a level where p2 spent
// nothing carries `portal2=` empty, saying so out loud.
//
// Ownership is still claimed only from what was carried: if neither half has
// spent anything by t0 this returns nothing at all, and dp keeps its own
// (empty, and for this field correct) default.
//
// Uids, not bit indices. The ordinal is dp's, assigned at load over
// L.portals; the uid is the level's. dp prints the map under --slopedbg
// (`gpbit`), which is also how the one portal it does NOT number was found --
// a rotating gravity portal reaches the portal pass from the moving-geometry
// side and gets no bit, so it can never appear here either.
// cfg portalpayloadlt: `< t0`, as spentPadArg and spentOrbArg read the same hook's ticks. The
// activatedByPlayer hook stamps an activation with the tick BEFORE the dump row that shows its
// effect, so `<= t0` also names a portal the attempt only reaches at t0+1. Measured on a custom
// level (the cold run's attempt 22, anchor t0=1,042): the up-gravity portal uid 212 flips GD's
// mini UFO in the row for t=1,043 (upsideDown 0 -> 1, vy 6.648 -> 3.324), the payload named it
// spent at 1,042, and the resim never flipped; with 212 left out it follows GD to GD's own death
// (t=1,442).
inline std::string portalPayload(long long t0) {
    std::string p1, p2;
    const bool lt = g_cfg.portalPayloadLt;
    for (const auto& kv : anchors::seeds().portal)
        if (lt ? kv.second < t0 : kv.second <= t0)
            p1 += (p1.empty() ? "" : ",") + std::to_string(kv.first);
    for (const auto& kv : anchors::seeds().portal2)
        if (lt ? kv.second < t0 : kv.second <= t0)
            p2 += (p2.empty() ? "" : ",") + std::to_string(kv.first);
    if (p1.empty() && p2.empty()) return std::string();
    return "owns=portal;portal=" + p1 + ";portal2=" + p2;
}

// cfg `histpayload=1`: the per-body history values --start does not carry, as
// dp's versioned `hist` value (one transport, never another
// positional field). Version 1 is the press latch; version 2 adds GD's Free Mode
// byte (AnchorRow::freeMode); version 3 the ticks since the spider's teleport
// (AnchorRow::spiderStamp). Empty when the anchor row is missing -- dp then keeps
// its default rather than a guess.
// Version 4 (cfg `histride` only) adds the slope ride's raw facts, and dp maps them
// onto its ride fields under --anchorride (cli.hpp):
//   slopeOn      +0x9b0
//   slopeUnder   +0x9b8, the underside branch (stale once the ride ends: dp reads it
//                only with slopeOn = 1)
//   slopeUid     the ramp at +0x678, -1 for none
//   slopeAge     (+0xaa0 - +0x598) in ticks, rounded, 0..255; -1 off a ramp
//   slopeLanded  1 if a row since the ride's clock was stamped is grounded (groundedOf,
//                the anchor's own test); -1 off a ramp
// The values are GD's, not the model's: which field an age goes into, and how the
// clock's phase lines up with the model's counters, is decided in one place, dp's.
inline std::string histPayload(long long t0) {
    const AnchorRow* r = anchors::row(t0);
    if (!r || !r->valid) return std::string();
    const int ps = (r->b985 && !r->b986) ? 1 : 0;
    const int ps2 = (r->b985_2 > 0 && r->b986_2 == 0) ? 1 : 0;
    // Ticks at 240 per second, rounded: both are the attempt clock's doubles. A stamp ahead of
    // the clock (none written this attempt) says nothing, and -1 leaves dp's default.
    const double since = r->totalTime - r->spiderStamp;
    const long long age = (since >= 0.0) ? std::llround(since * 240.0) : -1;
    std::string s = std::string("owns=hist;hist=") + (g_cfg.histRide ? "4|9" : "3|4")
         + "|pressSpent:" + std::to_string(ps)
         + ",pressSpent2:" + std::to_string(ps2)
         + ",freeMode:" + std::to_string(r->freeMode)
         + ",spiderJumpT:" + std::to_string(std::min<long long>(age, 255));
    if (!g_cfg.histRide) return s;
    long long slopeAge = -1;
    int landed = -1;
    if (r->onSlope) {
        const double rs = r->totalTime - r->slopeStart;
        slopeAge = std::clamp<long long>(std::llround(rs * 240.0), 0, 255);
        // The ride's rows are the ones carrying its stamp: +0x598 is written only on the
        // air -> ramp transition, so it is the same double on every row from the contact on
        // and a different one on the row before it. Counting rows rather than subtracting
        // times keeps this in the recording's own tick frame.
        landed = 0;
        for (long long t = t0; t >= 0; --t) {
            const AnchorRow* q = anchors::row(t);
            if (!q || !q->onSlope || q->slopeStart != r->slopeStart) break;
            if (groundedOf(*q)) { landed = 1; break; }
        }
    }
    return s + ",slopeOn:" + std::to_string(r->onSlope)
         + ",slopeUnder:" + std::to_string(r->slopeUnder)
         + ",slopeUid:" + std::to_string(r->slopeUid)
         + ",slopeAge:" + std::to_string(slopeAge)
         + ",slopeLanded:" + std::to_string(landed);
}

// The payloads joined, so a call site asks once. Any part may be absent; `owns`
// is read key by key and ORed, so the parts can simply be concatenated.
inline std::string anchorPayloadAll(long long t0) {
    std::string out;
    auto add = [&](const std::string& s) {
        if (s.empty()) return;
        out += (out.empty() ? "" : ";") + s;
    };
    add(g_cfg.touchPayload ? anchorPayload(t0) : std::string());
    add(g_cfg.portalPayload ? portalPayload(t0) : std::string());
    add(histPayload(t0));
    return out;
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
    // ...and the rest of what a FAMILY is spelled out of. cause_of builds its
    // signature from the model trace: m<mode> mini<mini> g<grounded> from this
    // row, sp from `dx`, slope from onslope/slopem/slopet, and clamp / uid /
    // orbnear from the NEXT row's clamp, clampuid and nearorb. None of them were
    // parsed here, so a run could not name the family of its own fixups even
    // though its trace on disk carried every column -- the gap that stopped the
    // 2026-09-10 join of census occurrences to real-run families.
    double dx = 0.0, slopem = 0.0, bandf = 0.0, bandc = 0.0;
    int onslope = -1, slopet = -1, nearorb = -1;
    std::string clamp, clampuid;
    // The model's ROTATED-GAMEPLAY FRAME (trace column `frame`), -1 when the
    // trace predates it. Not a cause_of input and deliberately NOT in kClass:
    // adding it there would make an older trace unclassifiable over a column no
    // family is spelled from. It is here because the recorder compares GD's
    // world dy against this row's dy, and frames.hpp:57-59 says the model keeps
    // its state in the CURRENT FRAME's coordinates -- so in a rotated section
    // those two are not the same quantity, and nothing on the line said so.
    int frame = -1;
    // The moving object that kept a MATCHING record from firing on the step into
    // this row (dp's trace column `fxblk`, fixup.hpp g_fxBlockedUid); -1 when none,
    // and when the trace predates the column.
    int fxblk = -1;
    // Whether THIS ROW carried the classification columns. g_traceClassMissing
    // answers it for the header; a row can be short of them while the header
    // has them all, and that row must still be recorded from -- its family is
    // the only thing it cannot supply. Default false so a row built anywhere
    // but the loader (fixupPass synthesises one at the anchor) is never
    // classified out of values it does not have.
    bool classOk = false;
};

// Which classification columns the loaded trace lacked, if any. Empty means the
// family can be spelled; non-empty is reported on the line instead of a family
// built out of defaults.
inline std::string g_traceClassMissing;

// BY NAME, NOT BY POSITION. Three trace schemas exist side by side -- the
// replay trace this reads is 36 columns, the witness resim's is 16, and
// trace.csv is 7 -- and they agree on the first eleven names and then diverge.
// Read positionally, asking a 16-column row for column 18 either throws the row
// away or silently answers with a different column: `dx` would come back as
// whatever sits at 18 in that other file. That is not a hypothetical; on
// 2026-09-10 the two schemas were confused by a reader who had both open.
// Names cost one header parse and cannot do it.
//
// A trace that lacks a required column is REFUSED BY NAME rather than filled
// with -1: "the column is missing" and "the value is -1" are different facts,
// and a family silently spelled from defaults would be wrong in a way nothing
// downstream could detect.
inline bool loadTrace(const std::string& path, std::map<long long, TraceRow>& out) {
    std::ifstream f(path);
    if (!f) return false;
    out.clear();
    auto split = [](const std::string& s) {
        std::vector<std::string> v;
        size_t p = 0;
        while (true) {
            size_t q = s.find(',', p);
            v.push_back(s.substr(p, q == std::string::npos ? q : q - p));
            if (q == std::string::npos) break;
            p = q + 1;
        }
        return v;
    };
    std::string line;
    if (!std::getline(f, line)) return false;
    if (!line.empty() && (unsigned char)line[0] == 0xEF) line.erase(0, 3);
    std::map<std::string, size_t> col;
    {
        const std::vector<std::string> h = split(line);
        for (size_t i = 0; i < h.size(); ++i) col[h[i]] = i;
    }
    // TWO GROUPS, and only one of them may stop the pass.
    //
    // The repair columns are what a fixup record is built from; without them
    // there is nothing to record and refusing is the honest answer. The
    // classification columns only spell the FAMILY -- an observation about the
    // record, not part of it. Requiring them would let a missing diagnostic
    // column cancel a repair, which is a change to what the loop DOES made in
    // the name of watching it. (The first version of this function did exactly
    // that: one list, and a short trace returned false, so fixupPass returned 0
    // and recorded nothing.)
    static const char* kRepair[] = {
        "tick", "x", "y", "vy", "mode", "grounded", "dual", "y2", "vy2", "act",
    };
    static const char* kClass[] = {
        "flip", "mini", "dx", "onslope", "slopem", "slopet", "nearorb",
        "clamp", "clampuid", "bandf", "bandc",
    };
    std::string missing;
    for (const char* n : kRepair)
        if (!col.count(n)) missing += (missing.empty() ? "" : ",") + std::string(n);
    if (!missing.empty()) {
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "dpsolve:   [fixup] REFUSED %s - the trace has %zu columns and is "
                 "missing what a RECORD needs: %s.",
                 path.c_str(), col.size(), missing.c_str());
        writeResult(msg);
        return false;
    }
    g_traceClassMissing.clear();
    for (const char* n : kClass)
        if (!col.count(n))
            g_traceClassMissing += (g_traceClassMissing.empty() ? "" : ",")
                                   + std::string(n);
    if (!g_traceClassMissing.empty()) {
        // Named, and the pass CONTINUES. The line will say the family cannot be
        // spelled rather than spelling one out of defaults -- "the column is
        // missing" and "the value is 0" are different facts.
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "dpsolve:   [fixup] %s cannot be classified - missing: %s. "
                 "Repair continues; the family is reported as unavailable.",
                 path.c_str(), g_traceClassMissing.c_str());
        writeResult(msg);
    }
    // TWO MAXIMA, not one. A row short of the CLASSIFICATION columns still
    // carries everything a record is built from, and dropping it would let a
    // missing diagnostic cancel a repair -- the same mistake as de82e63, one
    // level down: that one cancelled the pass, this one cancelled the row.
    size_t maxRepair = 0, maxClass = 0;
    for (const char* n : kRepair) {
        auto it = col.find(n);
        if (it != col.end()) maxRepair = std::max(maxRepair, it->second);
    }
    for (const char* n : kClass) {
        auto it = col.find(n);
        if (it != col.end()) maxClass = std::max(maxClass, it->second);
    }
    // find(), NOT col[n]. std::map::operator[] INSERTS a missing key with a
    // value-initialised 0 and returns it, so every accessor below answered a
    // missing column with COLUMN 0 -- which is `tick`. The damage was not the
    // diagnostic it looks like: mini and flip are classification columns but
    // they are also part of the RECORD KEY (:1521), and their "the model's row
    // does not carry it, use GD's" sentinel is `>= 0`. A tick number is
    // positive, so the fallback could not fire and the key was written with a
    // tick in the mini field. `mfam=unavailable` guarded the printed family and
    // never touched that.
    //
    // Each accessor now takes the default it should answer with, because the
    // right default is per-column and not zero: -1 for mini/flip is what the
    // key's fallback tests for.
    auto num = [&](const std::vector<std::string>& c, const char* n,
                   double def = 0.0) {
        auto it = col.find(n);
        if (it == col.end() || it->second >= c.size()) return def;
        return std::atof(c[it->second].c_str());
    };
    auto integer = [&](const std::vector<std::string>& c, const char* n,
                       int def = 0) {
        auto it = col.find(n);
        if (it == col.end() || it->second >= c.size()) return def;
        return std::atoi(c[it->second].c_str());
    };
    auto text = [&](const std::vector<std::string>& c, const char* n) {
        auto it = col.find(n);
        if (it == col.end() || it->second >= c.size()) return std::string();
        return c[it->second];
    };
    while (std::getline(f, line)) {
        if (line.empty() || !isdigit((unsigned char)line[0])) continue;
        const std::vector<std::string> c = split(line);
        // Against what a RECORD needs, not against everything that will be read.
        // Guarding on clampuid alone let a row short of any other read fall
        // through to the accessors' 0; guarding on the largest index of BOTH
        // groups went too far the other way and threw the row out entirely for
        // want of a diagnostic. A row is kept when it can be recorded from, and
        // says separately whether it can also be classified.
        if (c.size() <= maxRepair) continue;
        TraceRow r;
        r.valid = true;
        r.classOk = (c.size() > maxClass);
        const long long t = (long long)num(c, "tick");
        r.x = num(c, "x");
        r.y = num(c, "y");
        r.vy = num(c, "vy");
        r.mode = integer(c, "mode");
        r.grounded = integer(c, "grounded");
        r.dual = integer(c, "dual");
        r.y2 = num(c, "y2");
        r.vy2 = num(c, "vy2");
        const std::string a = text(c, "act");
        r.act = (a == "0" || a == "1") ? std::atoi(a.c_str()) : -1;
        // -1, not 0. These two are the only classification columns that are
        // also part of the record key, and -1 is what :1521's fallback tests
        // for -- "the model's row does not carry it, so use GD's". 0 is a
        // legitimate value for both, so it cannot double as "absent".
        r.mini = integer(c, "mini", -1);
        r.flip = integer(c, "flip", -1);
        // ...and the family's remaining inputs.
        r.dx = num(c, "dx");
        r.onslope = integer(c, "onslope");
        r.slopem = num(c, "slopem");
        r.slopet = integer(c, "slopet");
        r.nearorb = integer(c, "nearorb");
        r.clamp = text(c, "clamp");
        r.clampuid = text(c, "clampuid");
        r.frame = integer(c, "frame", -1);
        r.fxblk = integer(c, "fxblk", -1);
        r.bandf = num(c, "bandf");
        r.bandc = num(c, "bandc");
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
    // A record matches this state, but the solver keeps it from firing: it applies no record
    // next to moving geometry (dp fixup.hpp nearDynObject), and says so per tick in the trace
    // column `fxblk`. Not "covered" -- the model goes on getting the transition wrong -- and not
    // "missing" -- writing another record there would be kept back the same way. A model wall.
    // SubZero 4002 t=19,186 read as covered and re-flew one plan 25+ times.
    FixupBlockedDyn,
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
        case FixupBlockedDyn:
            return "a record matches but the solver applies none next to moving geometry";
        default:            return "it could not be written";
    }
}

// Record the transition into tick t: the model's state at t-1 with GD's observed deltas.
// `revive`: the record is the verdict "the model killed a run GD carried on" (fixupPass). The
// model's own transition is GD's then, so the delta can be zero -- and it still has to go on file
// as a real record, because what it carries is the verdict: dp's applyFixup clears `dead` for any
// delta record it matches, zero or not. Filed as a no-op mark it never reached dp, and the mark
// then answered "a record already covers this state" to every later try at the same state.
// SubZero 4002 (2026-09-27): a spider walking into the side of a 3 px slab right after its
// teleport lives in the game (hbox hit=1 and no kill, t=21,290-21,296) and died in the model at
// t=21,291 on identical states; both tries at the verdict came back "already right", then
// "already covered". Only a death inside the recorder's window is revived. Letting a refused
// clear's resim run on to the finish revived two deaths 5,600 ticks past its anchor (t=17,434 and
// 17,946, a wave section, anchor 11,789), and the whole-level searches that pass there then cost
// 56/177/60 s -> 246/220/182 s; whether those two were real is not known.
// `sameDeath`: a kill record for a death the model dies too, a tick later -- see the verdict in
// fixupPass for what that test does and does not compare. Filed exactly as before; it only tells
// the map and the log line which cause it is.
inline int writeFixup(long long t, int kill, const std::map<long long, TraceRow>& m,
                      bool revive = false, bool sameDeath = false) {
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
    const bool noop = (kill == 0 && !revive && std::fabs(eDy) < kNoopEps
                       && std::fabs(eDvy) < kNoopEps
                       && std::fabs(eDy2) < kNoopEps
                       && std::fabs(eDvy2) < kNoopEps);
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
    // ...and "covered" is refined by the solver's own answer, the only side that can see the
    // gate: covered by a record the solver keeps back is a model wall. It REPLACES FixupOnFile
    // and nothing else, so every other outcome -- a refined record written over a disagreeing
    // one above all -- stays exactly what it was (putting the test first cost lv22 one record).
    // ...a revive is not covered by a no-op mark: the mark says "looked at", the revive says
    // "GD lives here", and only the second one reaches dp.
    if (fixupOnFile(g_fixupPath, key, dyG, dvG)
        || (!revive && fixupOnFile(g_fixupNoopPath, key, dyG, dvG)))
        return mCur->second.fxblk >= 0 ? FixupBlockedDyn : FixupOnFile;
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
    // 800, not 600: the line now carries e2 (48) + gd (128) + md (224) of named
    // tails on top of its own text, and snprintf truncates in silence. What it
    // would cut is the END -- the md tail, i.e. exactly the fields a family is
    // spelled from -- so an overflow here would not look like an overflow. It
    // would look like `mfam` quietly going missing on the longest records.
    // NAMES, AND THE CONVENTION THEY CARRY.
    //
    // These two numbers were printed as a bare `err %.3f/%.3f`: the only pair on
    // the line without names, on a line where everything else is name=value, in
    // a loop whose whole diagnostic story this year has been "parse by name".
    // A reader who greps `edy=` -- which is what the RECORD FILE calls them
    // (:1600) -- gets zero hits and concludes the run does not have them.
    // Measured: that happened, and the answer "the value is not in this run" was
    // wrong. So the log now uses the file's names.
    //
    // THE CONVENTION IS PART OF THE NAME. Three different quantities on these
    // lines look alike and are not:
    //   dy= / dvy= here      GD's own delta across the transition   (dyG, dvG)
    //   edy= / edvy= here    dyG MINUS the model's delta -- GD is the baseline,
    //                        so a negative edy means GD moved less than the model
    //   dy= / dvy= on the
    //   `differs (...)` line ACCUMULATED model minus GD (:1908-1909) -- the other
    //                        baseline AND a different quantity (state, not delta)
    // The last one is why the same tick can read +7.481 on one line and -7.481
    // on another: not a sign bug, two instruments. Renaming without saying this
    // would make them look like one.
    char b[800];
    // Both lines carry the second body's error too, for the same reason the test above now
    // consults it: `err 0.000/0.000` next to a record that was decided by a body those two
    // numbers do not describe is how this went unread for a day.
    char e2[48] = "";
    // ...and the second body's pair gets names for the same reason. Leaving
    // `p2 a/b` unnamed beside a named edy=/edvy= would say the two pairs are
    // different in kind, which they are not.
    if (dual) snprintf(e2, sizeof(e2), " edy2=%.3f edvy2=%.3f", eDy2, eDvy2);
    if (noop) {
        ++g_fixupNoop;
        snprintf(b, sizeof(b), "dpsolve:   [fixup] t=%lld already right (edy=%.3f edvy=%.3f%s) "
                 "- marked only (%d)", t, eDy, eDvy, e2, g_fixupNoop);
    } else {
        ++g_fixupCount;
        // ...and on the map (itermap.hpp). A fixup is where the MODEL was wrong, which is the
        // cause the deaths around it are the symptom of -- and the two are often hundreds of
        // pixels apart, which is the single most useful thing the picture says.
        itermap::addFixup(g_iter, t, (float)mPrev->second.x, (float)mPrev->second.y, kill != 0,
                          sameDeath);
        // ...and GD's own half of the transition, as a NAMED TAIL.
        //
        // WHY. A family is cause_of's signature, and that function reads both
        // sides ENTERING the tick plus GD's values on the way out -- the `gdg`
        // and `gdgo` halves of a string like `m2/mini0/g1/gdg1/gdgo0/sp1.1/...`.
        // Everything else it needs is already on this line; GD's four values are
        // not, so a family cannot be rebuilt from a run's own log. That is what
        // stopped the 2026-09-10 join of census occurrences to real-run
        // families: the only family-bearing source was fixups_log_lv*.txt, which
        // nothing has written since 2026-08-22.
        //
        // RAW, and not the finished family. `onGround` goes in untranslated --
        // fixcensus says of the same column "THE COLUMN IS RAW BY DEFAULT", and
        // putting it through grounded_of would make it a different predicate
        // (that one's per-mode gap is in its own docstring), so `gdg1` would
        // stop meaning what a census record means by it. The four inputs are
        // stored rather than the signature because a signature is a proxy for
        // its definition: change cause_of and every stored string is stale,
        // while inputs can be re-signed. Same choice as uid over bit index.
        //
        // A TAIL, not fields in the middle: two readers key on adjacency
        // (archive_dy_census.py:22, fixup_bias_census.py:44) and an insertion
        // would drop their rows in silence. dp made the same choice for
        // `,dual2=` and gives the reason at cli.hpp:549.
        // ...and WHAT GD WAS RESTING ON, which is the half a family cannot see.
        //
        // A family is spelled from the MODEL's row: `clamp` and `clampuid` below
        // come from the model's own trace, and GD contributes only onGround and
        // mode. So a transition where GD was held against something and the
        // model thought it was in free air is spelled `/air` -- the family names
        // the model's story about the tick and has no room for the game's. That
        // makes "no clamp-bearing family in this run" ambiguous between "no such
        // divergence happened" and "the contact was on GD's side", which is the
        // half the family cannot express. Measured 2026-09-11: 8 of the run's 69
        // records carry a model-side clamp, and nothing in the log could say how
        // many carried a game-side one.
        //
        // NOT NAMED `gdclamp`. GD has no clamp: it has `m_objectSnappedTo` and
        // `m_snapDistance` (recorded raw at :183), which answer "the game placed
        // the player against object U, by D px". Calling that a clamp would put
        // the model's concept in the game's mouth and the name would then be
        // read as agreement between two things that were never compared.
        //
        // From gCur, not gPrev: the snap happens inside the step, the same
        // reason `clamp` below is read from the next row. -1 = no row (a kill)
        // or nothing snapped, and those two are already distinguished by gdgo.
        //
        // ★AND IT IS NOT A PER-TICK CONTACT PREDICATE. m_objectSnappedTo is
        // STICKY: it keeps the last object it snapped to, and GD does not clear
        // it on leaving. Measured on gdref/lv22.csv (21,140 ticks): snapuid >= 0
        // on 98% of all ticks and on ★99% of the AIRBORNE ones, with a single
        // value persisting up to 3,818 ticks. So `gdsnap >= 0` says "the game
        // has snapped to U at or before this tick", NOT "the game is holding
        // the player here", and a count of records with a non-negative gdsnap
        // is not a count of contacts. This was written down after taking that
        // count -- 60 of 69 records, which read as a discovery and is an
        // artefact of the field's persistence.
        //
        // What that leaves: the pair still says WHICH object and how far, which
        // no other field on this line carries, and it is exact when read at a
        // tick where the snap CHANGES. What it does not do is close the gap
        // this tail was added for. The recording's only per-tick contact facts
        // are onGround/onGround2, already here as gdg/gdgo, so "GD held the
        // player while the model flew" cannot be answered from what is recorded
        // today; answering it needs a new recorded field, not a new print.
        //
        // gdsnapd is not gated by gdsnap either: a record here carried
        // gdsnap=-1 with gdsnapd=27.79. The uid is the gate; the distance
        // beside a -1 is the previous snap's, left over.
        //
        // PRINTED LINE ONLY. The record written to g_fixupPath keeps its format:
        // two readers key on adjacency there (archive_dy_census.py:22,
        // fixup_bias_census.py:44) and dp/fixup.hpp parses it, while this line's
        // readers all match by name.
        // ...and BOTH FRAMES, side by side, because the deltas above are only
        // comparable when they agree.
        //
        // eDy is `dyG - (model's dy)`. GD's y is world (anchors::row fills it
        // from PlayerObject, and nothing here maps it into a frame), while the
        // model's is already in the current
        // frame's coordinates (frames.hpp:57-59). In a rotated section those
        // are different axes and the subtraction is meaningless -- yet nothing
        // on the line said which frame either side was in, so a record made
        // there looked exactly like one made anywhere else.
        //
        // This does NOT fix the comparison; it makes the precondition visible.
        // Whether a given family came from a frame mismatch is then a question
        // the log can answer instead of one needing a trace that no longer
        // exists. -1 = the trace has no `frame` column.
        // gdslp: GD's slope ride, before and after the step, and the ramp it
        // was on after it (AnchorRow::onSlope / slopeUid, player+0x9b0 and +0x678).
        // checkCollisions clears +0x9b0 at the top of every tick and the ride sets
        // it again, and the row is taken after the tick, so `after` reads "GD rode
        // a slope on this tick" -- measured visible there on lv16's ramps (31 of
        // 201 ticks at t=8,600..8,800). Beside mslope it names the records where
        // only one side was on a slope. -1 = no row after (a kill).
        char gd[192] = "";
        snprintf(gd, sizeof(gd), " gdg=%d gdm=%d gdgo=%d gdmo=%d"
                 " gdsnap=%d gdsnapd=%.2f mframe=%d gdframe=%d gdslp=%d/%d/%d",
                 gPrev->onGround, gPrev->mode,
                 gCur ? gCur->onGround : -1, gCur ? gCur->mode : -1,
                 gCur ? gCur->snapUid : -1, gCur ? gCur->snapDist : 0.f,
                 mPrev->second.frame, gPrev->gframe,
                 gPrev->onSlope, gCur ? gCur->onSlope : -1, gCur ? gCur->slopeUid : -1);
        // ...and the MODEL's half, which is the rest of what a family is spelled
        // out of. GD's four above are only one side of cause_of; without these a
        // family still cannot be named, and the trace they come from is deleted
        // before the next pass (:1569 -- deliberately, so one pass cannot read
        // the previous pass's file), so they cannot be recovered afterwards.
        // Written here, while the row is in hand.
        //
        // `clamp` and `nearorb` are read from the NEXT row, not this one: both
        // happen inside that step, so neither is on the row the transition
        // leaves from. cause_of does the same and says so.
        char md[224] = "";
        const bool rowsClassifiable =
            mPrev->second.classOk && (mCur == m.end() || mCur->second.classOk);
        if (!g_traceClassMissing.empty() || !rowsClassifiable) {
            // Say WHY there is no family, and which columns are absent. A tail
            // of defaults would be a family that is wrong with nothing to show
            // for it.
            //
            // TWO REASONS, and they are different facts: the header lacked the
            // columns, or THESE ROWS were short of them while the header had
            // them. The second is why `classOk` exists -- such a row is still
            // recorded from, and only its family is withheld.
            snprintf(md, sizeof(md), " mfam=unavailable(%s)",
                     !g_traceClassMissing.empty() ? g_traceClassMissing.c_str()
                                                  : "row short of them");
        } else {
            const TraceRow& p = mPrev->second;
            const TraceRow* nx = (mCur != m.end()) ? &mCur->second : nullptr;
            // g and y are cause_of's too -- `g<n>` comes from the model's own
            // grounded, and y is compared against the band edges for
            // nearceil/nearfloor. They were already parsed and still missing
            // from the line: having a value in memory is not having it on file.
            // mband at the SAME precision as y, because y is what it is
            // compared against. cause_of's test is `|y - bandc| < 20.0`, so an
            // error of e in the band edge moves that comparison by e: the band's
            // quantum has to be no coarser than y's or the answer can change on
            // the way through the line. y is %.3f, so these are %.3f. That is
            // the derivation, not a preference -- picking a "nicer" width would
            // make the threshold an artefact of the format.
            //
            // At %.1f it could and did: constructed on real columns, true
            // bandc 540.06 with y 520.07 gives |diff| 19.99 and spells
            // `nearceil`, while the printed 540.1 gives 20.03 and drops it.
            // Not witnessed in 399 real rows -- unwitnessed, which is not the
            // same as impossible, and the mechanism is shown.
            snprintf(md, sizeof(md),
                     " mg=%d my=%.3f mmini=%d mdx=%.4f mslope=%d/%.4f/%d"
                     " mclamp=%s mcuid=%s morb=%d mband=%.3f/%.3f",
                     p.grounded, p.y, p.mini, p.dx, p.onslope, p.slopem, p.slopet,
                     nx && !nx->clamp.empty() ? nx->clamp.c_str() : "-",
                     nx && !nx->clampuid.empty() ? nx->clampuid.c_str() : "-",
                     nx ? nx->nearorb : -1, p.bandf, p.bandc);
        }
        snprintf(b, sizeof(b), "dpsolve:   [fixup] t=%lld x=%.1f mode=%d in=%d "
                 "dy=%.3f dvy=%.3f kill=%d edy=%.3f edvy=%.3f%s%s%s%s%s (%d total)",
                 t, mPrev->second.x, mPrev->second.mode, act, dyG, dvG, kill,
                 eDy, eDvy, e2, gd, md, revive ? " revive" : "",
                 sameDeath ? " same-death" : "", g_fixupCount);
    }
    writeResult(b);
    // KILL-ONLY: the second veto hit (see g_killVetoIter). The p2 halves are
    // consulted whatever cfg dpFixP2 says -- unlike the noop test above, being
    // strict here only ever withholds credit.
    const bool physAgrees =
        std::fabs(eDy) < kNoopEps && std::fabs(eDvy) < kNoopEps
        && (!dual || (std::fabs(eDy2) < kNoopEps && std::fabs(eDvy2) < kNoopEps));
    // !g_ckInner: a checkpoint death is not a death of the loop's plan, and the veto credit below
    // is booked against the loop's own last death (g_lastDeathX).
    if (!noop && kill != 0 && physAgrees && g_lastTailSolved && !g_phantomLifted
        && g_killVetoIter != g_iter && !g_ckInner) {
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

// cfg dpsecauto: a real record went on file at tick t, for a death at deathTick. Counted for the
// recorder run in progress, and -- when the death is at the deepest wall -- the earliest such tick
// is where the model starts to part from GD there: the entry of the wall's section solve.
inline void autoNoteRecord(long long t, long long deathTick) {
    ++g_autoRecNow;
    if (rungWall() >= 0 && deathTick >= rungWall() - kAutoCreep
        && (g_autoHead < 0 || t < g_autoHead))
        g_autoHead = t;
}

// One pass: re-simulate the plan from the anchor and record the first divergence found.
// Returns how many records went on file.
// THE RECORDER'S OWN INPUTS, KEPT WHERE IT READS THEM. dp_band_itN / dp_groups_itN are what the
// died plan was SOLVED against; the recorder's replay of that plan against GD reads a band that
// addWorldArgs rewrites for its own call (from the attempt that just died) and whichever groups
// files are current when it runs. So the solve-side copies cannot rebuild the recorder's call:
// on lv21 its band was 81 B while dp_band_it20 was 49 B -- the next plan's copy happened to
// match, which is a coincidence and not a record. This keeps what the recorder's argv names,
// once per death, and writes the attempt's manifest beside the died plan:
//   dp_fixin_band_itN_tT.txt     the recorder's --bandtrack file
//   dp_fixin_groups_itN_tT_K.txt each --groups file, copied once per signature per level (a
//                                repeat is named in the manifest instead of copied again)
//   dp_fixin_fixups_itN_tT.txt   the --fixups records as they were for that call (a rebuild of
//                                lv21's without them left the recorder 200 ticks early)
//   dp_attempt_itN_tT.txt        plan, solve-side and recorder-side signatures, iteration,
//                                death tick, the recorder's anchor and coverage
inline void keepRecorderInputs(const std::vector<std::string>& a, long long t0, long long deathTick) {
    std::error_code ec;
    const std::string dir = std::string(DATA_DIR) + "/";
    std::string planSig = "-", bandSig = "-", bandCopy = "-", groupsLine;
    std::string fixSig = "-", fixCopy = "-";
    int k = 0;
    for (size_t i = 0; i + 1 < a.size(); ++i) {
        if (a[i] == "--replay") {
            planSig = fileSig(a[i + 1]);
        } else if (a[i] == "--fixups") {
            // ...and the records the recorder replays with: the file grows every pass, and a
            // rebuild without the ones it had parts from the recorder long before the death.
            fixSig = fileSig(a[i + 1]);
            char fp[256];
            snprintf(fp, sizeof(fp), "dp_fixin_fixups_it%d_t%lld.txt", g_iter, deathTick);
            std::filesystem::copy_file(a[i + 1], dir + fp,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            fixCopy = ec ? std::string("copy-failed") : std::string(fp);
        } else if (a[i] == "--bandtrack") {
            bandSig = fileSig(a[i + 1]);
            char bp[256];
            snprintf(bp, sizeof(bp), "dp_fixin_band_it%d_t%lld.txt", g_iter, deathTick);
            std::filesystem::copy_file(a[i + 1], dir + bp,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            bandCopy = ec ? std::string("copy-failed") : std::string(bp);
        } else if (a[i] == "--groups") {
            const std::string gs = fileSig(a[i + 1]);
            std::string copy;
            auto it = g_fixinGroupsBySig.find(gs);
            if (it != g_fixinGroupsBySig.end()) {
                copy = it->second;
            } else {
                char gp[256];
                snprintf(gp, sizeof(gp), "dp_fixin_groups_it%d_t%lld_%d.txt", g_iter, deathTick, k);
                std::filesystem::copy_file(a[i + 1], dir + gp,
                                           std::filesystem::copy_options::overwrite_existing, ec);
                copy = ec ? std::string("copy-failed") : std::string(gp);
                if (!ec) g_fixinGroupsBySig[gs] = copy;
            }
            groupsLine += (groupsLine.empty() ? "" : ";") + gs + "=" + copy;
            ++k;
        }
    }
    char mp[256];
    snprintf(mp, sizeof(mp), "dp_attempt_it%d_t%lld.txt", g_iter, deathTick);
    char solveBand[256];
    snprintf(solveBand, sizeof(solveBand), "%sdp_band_it%d_t%lld.txt", dir.c_str(), g_iter, deathTick);
    {
        std::ofstream mf(dir + mp, std::ios::trunc);
        mf << "iteration " << g_iter << "\n"
           << "death_tick " << deathTick << "\n"
           << "plan " << planSig << "\n"
           << "solve_band " << fileSig(solveBand) << "\n"
           << "solve_groups " << (g_solveGroupsSig.empty() ? "-" : g_solveGroupsSig) << "\n"
           << "fixup_band " << bandSig << " " << bandCopy << "\n"
           << "fixup_groups " << (groupsLine.empty() ? "-" : groupsLine) << "\n"
           << "fixup_records " << fixSig << " " << fixCopy << "\n"
           << "coverage " << t0 << " " << deathTick << "\n";
    }
    char b[384];
    snprintf(b, sizeof(b), "dpsolve:   [fixin] it=%d band=%s groups=%s fixups=%s coverage=%lld..%lld",
             g_iter, bandSig.c_str(), groupsLine.empty() ? "-" : groupsLine.c_str(),
             fixSig.c_str(), t0, deathTick);
    writeResult(b);
}

inline int fixupPass(long long t0, const std::string& startArgStr, const std::string& band,
                     long long deathTick) {
    const std::string base = std::string(DATA_DIR) + "/dp_fixup";
    std::vector<std::string> a{"--replay", g_planPath, "--start", startArgStr,
                               "--out", base,
                               "--cap", std::to_string(kCap),
                               "--shipyq", num(kYq), "--shipvq", num(kVq),
                               "--threads", kThreads};
    if (!band.empty()) { a.push_back("--startband"); a.push_back(band); }
    {   // ...and the touch triggers GD had already set off by t0, and the
        // gravity portals it had already spent (cfg touchpayload, off by default,
        // and portalpayload, on -- see Config for each). Both anchor paths take the
        // SAME payload, or the resim
        // and the solve would be anchored into different worlds, which is the
        // failure this seeding exists to avoid.
        const std::string ap = anchorPayloadAll(t0);
        if (!ap.empty()) { a.push_back("--anchor-state"); a.push_back(ap); }
        // cfg histride: the ride's facts in that payload are seeded (dp --anchorride); without
        // it the consumer is turned off too, so the payload and the consumer always agree.
        a.push_back(g_cfg.histRide ? "--anchorride" : "--no-anchorride");
    }
    bool rotQ = false;
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
        // ...and, under cfg dprotseed, the queue itself -- ONLY when the recording
        // fixes its cursor exactly (rotSeedArgs). --spentrot above seeds the
        // pre-queue selection's one-shots and stays on every call.
        rotQ = rotSeedArgs(t0, a, "resim");
        // ...and the queue's toggle rule with the touch Toggles this attempt had already
        // entered (see touchSeedArg). --rotqtoggle names this call as an anchored one, which is
        // where the rule applies; always passed here since the 0.4.0 clean-up removed cfg
        // dprotqtoggle.
        {
            a.push_back("--rotqtoggle");
            const std::string ts = touchSeedArg(t0);
            if (!ts.empty()) { a.push_back("--touchseed"); a.push_back(ts); }
        }
        // ...and which touch boxes the attempt had reached.
        {
            a.push_back("--touchentered");
            a.push_back(touchEnteredArg(t0));
        }
        // ...and, under cfg dpspentpad, the pads it had already fired (spentPadArg).
        if (g_cfg.dpSpentPad) {
            const std::string sp = spentPadArg(t0);
            if (!sp.empty()) { a.push_back("--spentpad"); a.push_back(sp); }
        }
        // ...and the rings (spentOrbArg).
        {
            const std::string so = spentOrbArg(t0);
            if (!so.empty()) { a.push_back("--spentorb"); a.push_back(so); }
        }
        // ...and, under cfg dpxtrack, GD's x up to t0 (xtrackArg).
        if (g_cfg.dpXTrack) {
            const std::string xt = xtrackArg(t0);
            if (!xt.empty()) { a.push_back("--xtrack"); a.push_back(xt); }
        }
    }
    std::error_code ec;
    if (std::filesystem::exists(g_fixupPath, ec)) {
        a.push_back("--fixups");
        a.push_back(g_fixupPath);
    }
    addWorldArgs(a);
    // This replay fires the item gates the search does, so it needs the gate tables (--coins) and
    // the same item givers (--items). The replay has no coin prune (that is the search's), so
    // --coins adds nothing else here.
    if (g_cfg.coinRoute) {
        a.push_back("--coins");
        const std::string ip = std::string(DATA_DIR) + "/items.txt";
        if (std::filesystem::exists(ip, ec)) {
            a.push_back("--items");
            a.push_back(ip);
        }
    }
    for (const std::string& s : g_cfg.dpArgs) a.push_back(s);
    std::filesystem::remove(base + ".trace.csv", ec);
    if (g_fixinPending) {   // the first pass for this death: keep what it reads
        g_fixinPending = false;
        keepRecorderInputs(a, t0, deathTick);
    }
    logSolverArgs(a);
    timedSolve(g_csv, a, "resim");
    logTrigWindow("resim");
    if (rotQ) logRotSeedRead(t0, "resim");
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
        // A model wall: a record matches and the solver will not apply it here. On a round that
        // REPEATED the last one (same plan, same death tick), stop, as a record does: "looking
        // further" then wrote a fresh record one tick later every round (4002: t=19,264,
        // 19,265, ...) and the plan came back unchanged. On any other round keep looking, as
        // before -- the records written past a wall are what carried 4001 to its clear, and
        // ending every pass at the wall held it at x=24,777 (see g_roundRepeated).
        if (w == FixupBlockedDyn && g_roundRepeated) {
            char b[360];
            snprintf(b, sizeof(b), "dpsolve:   [fixup] t=%lld x=%.1f y=%.1f differs (dy=%.3f "
                     "dvy=%.3f), a record matches, but the solver applies none next to moving "
                     "uid %d - a model wall, not a record; nothing past it is recorded (plan %s)",
                     t, it->second.x, it->second.y, dy, dv, it->second.fxblk,
                     g_prevFlownFnv.empty() ? "-" : g_prevFlownFnv.c_str());
            writeResult(b);
            break;
        }
        if (w == FixupReal || w == FixupMark) {
            ++made;
            if (w == FixupReal) ++real, autoNoteRecord(t, deathTick);
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
            verdictWhy = writeFixup(modelDied, 0, m, /*revive=*/true);
            if (verdictWhy == FixupReal) ++real, ++made, autoNoteRecord(modelDied, deathTick);
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
            // ...unless the run was ended BY US at a missed coin (cfg
            // coinroute). Then "GD ended the run here" is this loop's own
            // doing, not the level's, and the record would teach the model a
            // kill that does not exist. The per-tick divergence records above
            // are still wanted: the trajectories really did part, and that
            // parting is why the coin was missed.
            // ...and when the model dies on the very next tick, it is THE SAME DEATH: GD's
            // `death:` row is destroyPlayer's tick and the model's death is reported one tick
            // later for the same event (its dump shows the dead row there too), so the model was
            // right and the round died because the search sent a plan that dies. The record is
            // filed all the same -- it is what stops the search sending that state again -- and
            // marked, so the map and the log count it apart from the fixups where the model was
            // wrong. Measured on official lv11 with coins: all four kill records of the run were
            // this, the model on GD's object with GD's y and vy (the plans' own `[SOLVED]` lines
            // had already reported them dying there, `resimdie=`).
            // WHAT THE MARK TESTS is this branch's gate plus the tick: agreement at deathTick-1
            // (y and vy within kDivergeEps, both bodies) and the model dying at deathTick+1. The
            // object is NOT compared -- the replay's killer never reaches the loop. Measured on
            // one official run without coins (58 marks): of the 38 where GD named its killer, 35
            // were the model's object, 2 the neighbouring spike of the same row, which the
            // player's box overlaps as well at that tick (lv8 t=8,407, lv20 t=15,833), and 1 a
            // moving pair left unresolved (lv20 t=21,967); GD named none for 19 (a solid
            // collision) and the model's cause was solid-side or slope-inside for all 19.
            // KEEPING THE MARKED RECORD OFF FILE WAS MEASURED AND REJECTED (2026-09-28, official
            // 22 with and without coins, one worker, the arms interleaved): lv1-19 unchanged, but
            // lv22 with coins died nine rounds more at t=8,217, where the delta records the
            // recorder kept writing at t=8,212 never reached the death and the kill record ends
            // it in one round, and lv20 without coins took three more (98 -> 101 and 95 -> 102
            // rounds in all).
            if (!g_lastDeathCoinMiss) {
                verdictWhy = writeFixup(deathTick, 1, m, /*revive=*/false,
                                        /*sameDeath=*/modelDied == deathTick + 1);
                if (verdictWhy == FixupReal) ++real, ++made, autoNoteRecord(deathTick, deathTick);
            }
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
    // A section solve's splice is pinned (secrung): the ladder anchors nowhere before g_secPin,
    // and the model is known to part from GD inside it -- that is why the rung was fired.
    // Records there cannot help a search that starts past the pin, and they spend the passes
    // the death's own divergence needs. Measured (lv4003, old model): after a splice over
    // t=7700-7946, all 76 records of the next 20 deaths landed at t=7800-7893, and the run
    // kept dying at t=8087.
    if (g_secPin > 0 && g_secPin < deathTick && t0 < g_secPin) t0 = g_secPin;
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
    const std::string arg = startArg(t0, *r, heldBefore(g_plan, t0),
                                     swingPendingAt(g_plan, t0));
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
    // cfg dpsecauto: the counts belong to one wall; a deeper one starts them again.
    autoSync();
    g_autoRecNow = 0;
    int passes = 0;
    // cfg `capture`: only arm keepRecorderInputs (the dp_fixin_*/dp_attempt_itN research copies)
    // when the harness asked for them -- see Config::researchCapture.
    g_fixinPending = g_cfg.researchCapture;
    for (int pass = 1; pass <= kFixupPasses; ++pass) {
        // A divergence spanning several ticks needs one delta each, and the next one only
        // becomes visible once the previous is being applied -- hence the passes.
        ++passes;
        if (fixupPass(t0, arg, band, deathTick) == 0) break;
    }
    if (rungWall() >= 0 && deathTick >= rungWall() - kAutoCreep)
        g_autoNoRec = (g_autoRecNow == 0) ? g_autoNoRec + 1 : 0;
    // cfg dpsecrent: the recorder's work, counted -- each pass replays the plan from t0 to the death.
    g_autoRoundWork += kWorkRecTick * (double)passes * (double)std::max(0LL, deathTick - t0);
    {
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - fxT0).count();
        char b[160];
        snprintf(b, sizeof(b), "dpsolve:   [fixup] recorder: %d pass(es) in %.0f ms "
                 "(anchor t=%lld, death t=%lld)", passes, ms, t0, deathTick);
        writeResult(b);
    }
}

// ---- cfg dpsecauto: the loop fires the section-solve rung itself ----
//
// The window (four walls of lv4003 under an older model):
// the ENTRY is where the model starts to part from GD, not where it dies -- the head of the fixups
// recorded at this wall, 20 ticks early, and never later than 200 ticks before the death (a window
// starting 100 ticks before one death was exhausted at depth 104: too late to change anything).
// A stretch already pinned is kept rather than searched again, as long as that still leaves 100
// ticks before the death. The EXIT is past the death: a window that stopped where the divergence
// stopped was solved in 5 s and its splice died on the wall itself, because the model could not
// see what killed the run there and planned nothing in the 23 ticks between.
// The exit is a DEPTH -- alive dpsecmargin ticks past the death's tick -- not an x. The level
// scrolls by itself, so outliving the death is the whole condition, and a depth holds where an x
// cannot: an x past the death was already behind a head in a section that runs left (a "solve" at
// depth 1, lv4003 t=11,008), and in rotated gameplay x does not advance at all.
struct AutoWindow {
    long long t0 = -1;
    long long depth = 0;
    long long horizon = 0;
    std::string from;
    bool pastMax = false;   // cfg dpsecmaxback: the next window would begin further back than that
};
//
// A WALL GETS AS MANY AS IT TAKES, each reaching further back: 200, 400 and 800 ticks before the
// death (or the fixups' head, if that is earlier), then 800 more each time, until a window that
// begins at the level's first tick has failed too. Only the first keeps a pinned stretch.
// Measured (4002, current model): the one window at the wall t=22,914 began 200 ticks before it
// and every branch was dead at depth 200 -- on the death's own tick; the fixups' head sat there
// too, so the model does not see the kill at all -- and with one rung per wall the loop then
// stood there without one. The control run crossed that wall only after its ladder had reached
// 600-1,500 ticks back: the decision that matters is further back than the first window looks.
// Three tries (800 ticks at most) were not enough either: official lv16 with coins, run with the
// cap ladder and the input grid off, died 60 times at its dual ball section (x~19,437, p2 killed
// with no object) after all three windows at its wall came back empty. A section solve searches
// the game itself, so a stretch it cannot cross from an entry is not one the model will plan
// across from there: it is the loop's last line, and it keeps going back. Linearly past 800
// ticks, since a window's search grows with its length.
// Keep the window progression in the game-independent repair policy for boundary tests.
inline long long autoBack(int level) {
    return solver::RepairBacktrack::back(level);
}
inline bool autoWindow(AutoWindow& w, int level) {
    const long long wall = rungWall();
    if (wall < 0 || level < 0) return false;
    long long t0 = wall - autoBack(level);
    w.from = std::to_string(autoBack(level)) + " ticks before the death";
    if (g_autoHead > 0 && g_autoHead - 20 < t0) {
        t0 = g_autoHead - 20;
        w.from = "the head of the fixups";
    }
    // Every try at a wall starts further back than the one before it, even where the fixups'
    // head had already put that one further back than its own step.
    if (level > 0 && g_autoLastT0 > 0 && t0 >= g_autoLastT0) {
        t0 = g_autoLastT0 - (autoBack(level) - autoBack(level - 1));
        w.from = std::to_string(autoBack(level) - autoBack(level - 1))
                 + " ticks before the last window";
    }
    if (level == 0) {
        if (t0 < wall - kFixupWindow) t0 = wall - kFixupWindow;
        if (g_secPin > t0 && g_secPin <= wall - 100) {
            t0 = g_secPin;
            w.from = "the pin";
        } else if (g_cfg.dpSecPinBack >= 0 && g_secPin > 0 && g_secPin < wall
                   && g_secPin - g_cfg.dpSecPinBack > t0) {
            // cfg dpsecpinback: a wall right behind the last splice starts its window a little
            // before the pin instead of 200 ticks before the death, most of which that splice has
            // already solved in the game (see g_cfg.dpSecPinBack).
            t0 = g_secPin - g_cfg.dpSecPinBack;
            w.from = "just before the pin";
        }
    }
    if (t0 < 1) {
        t0 = 1;
        w.from = "the start of the level";
    }
    // cfg dpsecmaxback: no window begins further back than this from the wall. Over the spinoff
    // cold logs (2026-09-26) all 56 windows that solved began at most 2,400 ticks back, and the 23
    // that began further back all failed, for 2,986 s -- the search dying on its own before the
    // wall (a custom level: from 1,600 ticks back on, every window's frontier died near t=10,084, a stretch
    // the loop's own plans fly through). Past that the section solve is not the tool.
    if (g_cfg.dpSecMaxBack > 0 && wall - t0 > g_cfg.dpSecMaxBack) {
        w.pastMax = true;
        return false;
    }
    if (t0 >= wall - 50) return false;
    w.t0 = t0;
    long long margin = g_cfg.dpSecMargin;
    if (g_cfg.dpSecChain && level == 0 && g_autoChain > 0) margin <<= std::min(g_autoChain, 3);
    w.depth = (wall - t0) + margin;
    w.horizon = w.depth + 50;
    return true;
}

// cfg dpseccaptiers=N (0 = off, the default): A WINDOW WHOSE SEARCH FOUND NOTHING IS SEARCHED AGAIN
// AT TWICE THE CAP, up to N times (dpseccap << N), before the next window reaches further back. Most
// walls are crossed at the base cap and go exactly as before; a cap only grows at a window that has
// failed at the one below it. Measured on a custom level (2026-09-28): the one route over its last
// wall skips an optional size portal, and from the only head that still reaches back past that
// portal the search (seccover, secsizefam, secp2key) came back empty at cap 100 and solved at 200 and
// at 400, 183 s and 308 s against about 100 s -- while the windows the ladder draws after it, each
// further back, are searches the base cap has less and less chance in, not more. Kept apart from
// the window ladder so that a wall the base cap crosses at any window is untouched.
// NOT WHEN THE CAP NEVER BOUND. A search whose cap dropped nothing -- or whose cap was the
// checkpoint path's clamp rather than the one asked for -- is the same search at any larger request,
// node for node: on that level the window 229 ticks back found nothing at cap 200 with nothing
// capped, and cap 400 then ran it again to the same 11,612 nodes and 35,742 steps. Such a window goes
// straight on to the next one back.
inline AutoWindow g_autoLastWin;          // the last window drawn (a cap retry searches it again)
inline bool autoCapRetry() {
    const bool sameWall = g_autoTriedWall >= 0 && rungWall() >= g_autoTriedWall
                          && rungWall() < g_autoTriedWall + kAutoCreep;
    return g_cfg.dpSecCapTiers > 0 && sameWall && g_autoLastDrawn && !g_autoLastFound
           && g_autoLastCapBound && g_autoCapTier < g_cfg.dpSecCapTiers;
}

// Which try the next rung at the deepest wall would be (0 = the first), or -1 when this wall has
// had them all: its last window began at the level's first tick. A cap retry (autoCapRetry) is the
// last window's own try again.
inline int autoLevel() {
    const bool sameWall = g_autoTriedWall >= 0 && rungWall() >= g_autoTriedWall
                          && rungWall() < g_autoTriedWall + kAutoCreep;
    if (!sameWall) return 0;
    if (autoCapRetry()) return g_autoTries - 1;
    return (g_autoLastT0 >= 0 && g_autoLastT0 <= 1) ? -1 : g_autoTries;
}

// Only an already-started sequence at this wall may outlive the repair budget.
inline bool autoCanContinue() {
    const bool sameWall = g_autoTriedWall >= 0 && rungWall() >= g_autoTriedWall
                          && rungWall() < g_autoTriedWall + kAutoCreep;
    return sameWall && g_autoTries > 0 && autoLevel() >= 0;
}
inline bool autoTried() { return autoLevel() < 0; }

// ---- cfg dpsecstate: THE STATE A WALL MAY NEED ----
//
// A plan that took an optional size portal can reach a wall only the other size gets over, and
// every window the ladder draws after that portal searches the size the plan already has. Measured
// on a custom level (2026-09-29): player 1 turned mini at t=24,779 and died at t=27,528; the loop's
// windows from 228 to 2,396 ticks before the wall came back empty, 14 searches and about 600 s,
// and the first to begin before the portal (3,196 ticks back) crossed only at cap 400, after 100
// and 200 had failed (176 s more).
//
// So when the wall's first window and its cap tiers have found nothing, and the plan's size last
// changed within kStateReach ticks before that window, the game is asked first: the same window,
// searched with the size undone at its head (secforcesize -- a state no route reaches, so the
// answer is a verdict and nothing is spliced). On that level it crossed in 7 s at normal size, and
// failed at the plan's own size exactly as the loop's window had.
//
// If it crosses, only the DECISION is searched in the game: a window from kStatePad ticks before
// the size change to kStatePast ticks after it, dropping every branch that changes the size
// (seckeepsize), at the base cap and then at each cap tier. Its splice is pinned to its own end,
// not past the wall, so the model plans the rest of the way to the wall at the kept size and the
// ordinary windows take whatever stretch of it the model gets wrong. (A kept window reaching all the
// way past the wall -- from 100 ticks before that level's portal, 2,902 ticks -- crossed only at
// cap 400, 150 s, having died at cap 100 and 200 in a UFO stretch far from the decision; the loop
// cleared in 31 rounds and 435 s against 44 and 1,496 s without dpsecstate.) If the probe does not
// cross, or the kept windows find nothing, the ladder carries on where it was (its second window),
// having spent those searches.
constexpr long long kStateReach = 4000;   // how far before the first window a size change counts
constexpr long long kStatePad = 100;      // a kept window begins this far before the change
constexpr long long kStatePast = 200;     // ...and ends this far after it
inline long long g_swWall = -1;   // the wall the fields below belong to (g_autoTriedWall)
inline int g_swPhase = 0;         // 0 not asked, 1 probe in flight, 2 kept windows, 3 done
inline long long g_swEvent = -1;  // the tick player 1's size last changed before the first window
inline int g_swSize = -1;         // the size the probe forces and the windows keep (0/1)
inline int g_swTier = 0;          // kept windows fired
inline AutoWindow g_swWin;        // the wall's first window, which the probe searches again
// THE REQUIREMENT OUTLIVES THE WINDOW. Once a kept window is spliced, the size it kept is what the
// run needs at that wall, and every wall the new way meets before it is judged under that need.
// Asked wall by wall, a wall in between reversed it: on the custom level the kept splice's way
// died at t=25,001 on a spike hung under a block in the dual section, the probe there crossed at
// mini size, a window keeping mini was spliced -- and the run was back at the 99% wall at mini
// size, with the decision now an unavoidable portal. So until the run is past the wall the probe
// asked about, the probe does not question the size, and every ordinary window that begins at or
// after the kept window's head searches with the size kept (a rung of kind 3).
inline int g_reqSize = -1;          // the size the run keeps (0/1), -1 = none
inline long long g_reqFrom = -1;    // ...from the kept window's head
inline long long g_reqUntil = -1;   // ...until it is past this tick (the wall, plus the margin)

inline void stateQueue(const AutoWindow& w, int cap, int kind, int size) {
    g_secReqStart = w.t0;
    g_secReqTarget = 0.0;
    g_secReqDepth = w.depth;
    g_secReqHorizon = w.horizon;
    g_secReqCap = cap;
    g_secReqRung = true;
    g_secReqCoin = -1;
    g_secReqState = kind;
    g_secReqSize = size;
    g_secReqPending = true;
    g_paused = true;
    if (secsolve::g_snapMode == 0 && secsolve::g_verifyEvery == 0) {   // as autoFire
        secsolve::g_snapMode = 2;
        secsolve::g_verifyEvery = 20;
    }
}

// Called by autoFire when the ladder is about to draw its second window at this wall (the first
// window and its cap tiers are spent). True = a rung was queued.
// The requirement is spent once the deepest death is past its wall.
inline bool stateReqActive() {
    if (g_reqSize < 0) return false;
    if (g_bestDeath > g_reqUntil) {
        char b[160];
        snprintf(b, sizeof(b), "dpsolve:   [secstate] past t=%lld - player 1's size is free again",
                 g_reqUntil);
        writeResult(b);
        g_reqSize = -1;
        g_reqFrom = g_reqUntil = -1;
        return false;
    }
    return true;
}

inline bool stateFire(const char* why) {
    if (g_swWall != g_autoTriedWall) {
        g_swWall = g_autoTriedWall;
        g_swPhase = 0;
        g_swTier = 0;
        g_swEvent = -1;
        g_swSize = -1;
    }
    const long long wall = rungWall();
    char b[320];
    if (g_swPhase == 0) {
        g_swPhase = 3;   // unless a size change turns up below
        if (stateReqActive()) {
            snprintf(b, sizeof(b), "dpsolve:   [secstate] the run keeps player 1 %s until past t=%lld "
                     "- this wall's windows keep it too, and the size is not questioned",
                     g_reqSize ? "mini" : "normal", g_reqUntil);
            writeResult(b);
            return false;
        }
        g_swWin = g_autoLastWin;
        const std::vector<AnchorRow>& v = anchors::g_deepest;
        auto at = [&](long long t) -> const AnchorRow* {
            return (t >= 0 && (size_t)t < v.size() && v[(size_t)t].valid) ? &v[(size_t)t] : nullptr;
        };
        const AnchorRow* head = at(g_swWin.t0);
        if (!head) return false;
        const long long lo = std::max(1LL, g_swWin.t0 - kStateReach);
        for (long long t = g_swWin.t0; t > lo; --t) {
            const AnchorRow* r = at(t);
            const AnchorRow* p = at(t - 1);
            if (r && p && r->mini != p->mini) { g_swEvent = t; break; }
        }
        if (g_swEvent < 0) return false;
        g_swSize = head->mini ? 0 : 1;
        g_swPhase = 1;
        stateQueue(g_swWin, g_cfg.dpSecCap, 1, g_swSize);
        snprintf(b, sizeof(b), "dpsolve:   [secstate] %s at the wall t=%lld: player 1 turned %s at "
                 "t=%lld, %lld ticks before the first window - that window again from t=%lld at %s "
                 "size, to ask whether the size is what stops it (a probe; nothing is spliced)",
                 why, wall, head->mini ? "mini" : "normal", g_swEvent, g_swWin.t0 - g_swEvent,
                 g_swWin.t0, g_swSize ? "mini" : "normal");
        writeResult(b);
        return true;
    }
    if (g_swPhase == 2) {
        if (g_swTier > g_cfg.dpSecCapTiers) {
            g_swPhase = 3;
            writeResult("dpsolve:   [secstate] the windows that keep the size found nothing - the "
                        "ladder carries on");
            return false;
        }
        AutoWindow w;
        w.t0 = std::max(1LL, g_swEvent - kStatePad);
        w.depth = std::min(g_swEvent + kStatePast, wall + g_cfg.dpSecMargin) - w.t0;
        w.horizon = w.depth + 50;
        w.from = "before the size change";
        const int cap = g_cfg.dpSecCap << g_swTier;
        ++g_swTier;
        stateQueue(w, cap, 2, g_swSize);
        snprintf(b, sizeof(b), "dpsolve:   [secstate] %s at the wall t=%lld - a section solve from "
                 "t=%lld (%lld ticks before player 1's size changed at t=%lld) that keeps it %s, "
                 "alive to t=%lld (depth %lld), cap %d; the model plans on from there",
                 why, wall, w.t0, g_swEvent - w.t0, g_swEvent, g_swSize ? "mini" : "normal",
                 w.t0 + w.depth, w.depth, cap);
        writeResult(b);
        return true;
    }
    return false;
}

// A kept window's splice is the run's way on. The deepest plan kept the size the probe showed the
// wall will not let through, so it is no longer the plan to go back to: the splice replaces it, and
// its end becomes the depth to beat -- the next flight that gets further is an ordinary improvement
// (onDeath banks its rows as the deepest), and the ladder and the windows measure their walls on
// the new way, not at the old wall with the old plan.
inline void stateAdopt(const std::vector<InputCmd>& plan, long long end, float x) {
    char b[200];
    snprintf(b, sizeof(b), "dpsolve:   [secstate] the kept window's splice replaces the deepest plan: "
             "the depth to beat goes %lld -> %lld", g_bestDeath, end);
    writeResult(b);
    g_best = plan;
    g_bestDeath = end;
    g_bestDeathX = x;
    g_spentAnchors.clear();
    // ...and the size is kept until the run is past the wall the probe asked about.
    g_reqSize = g_swSize;
    g_reqFrom = std::max(1LL, g_swEvent - kStatePad);
    g_reqUntil = g_swWall + g_cfg.dpSecMargin;
}

// A rung of cfg dpsecstate is back.
inline void stateRungDone(int kind, bool found) {
    if (kind == 1) {
        g_swPhase = found ? 2 : 3;
        writeResult(found ? "dpsolve:   [secstate] the probe crossed the wall at the other size - "
                            "the next windows begin before the size change and keep it"
                          : "dpsolve:   [secstate] the probe did not cross at the other size "
                            "either - the ladder carries on");
    } else if (kind == 2 && found) {
        g_swPhase = 3;   // spliced; a wall past it starts over
    }
}

// Queue the rung the way the `secrung` command does; poll() hands over at the next frame boundary
// with no job in flight. Returns whether it was queued.
inline bool autoFire(const char* why) {
    const bool coinRung = g_cfg.dpSecCoinRung && g_cfg.coinRoute && g_coinWall >= 0
                          && !g_coinWallPlan.empty();
    // "No verified plan yet" is g_bestDeath < 0, not an empty g_best: a plan that never presses
    // is a verified plan once GD has flown it (the first solve's acceptance in spawn()).
    if (!g_cfg.dpSecAuto || g_secReqPending || (g_bestDeath < 0 && !coinRung) || rungWall() < 0)
        return false;
    // cfg dpsecwallbudget: which wall episode this rung belongs to (see g_secEpisodeWork), and whether
    // that episode has any section-solve work left. A wall the loop reached by itself -- past the
    // episode's furthest wall or pin by more than kAutoCreep, and not fired as pin-out -- starts a new
    // one. Checked before anything below changes the ladder's state.
    {
        const bool pinOut = std::strcmp(why, "every rung past the pin is spent") == 0;
        const long long reach = std::max(g_secEpisodeReach, g_secPin);
        if (g_secEpisodeReach < 0 || (!pinOut && rungWall() > reach + kAutoCreep)) {
            g_secEpisodeWork = 0.0;
            g_secEpisodeRungs = 0;
        }
        g_secEpisodeReach = std::max(reach, rungWall());
        if (g_cfg.dpSecWallBudget > 0.0 && g_secEpisodeWork >= g_cfg.dpSecWallBudget * 1e6) {
            char eb[256];
            snprintf(eb, sizeof(eb), "dpsolve:   [secauto] the section-solve budget of this wall is spent "
                     "(%.1f of %.1f s-equiv over %d rungs since t=%lld) - the fixup loop carries on "
                     "(cfg dpsecwallbudget)", g_secEpisodeWork / 1e6, g_cfg.dpSecWallBudget,
                     g_secEpisodeRungs, g_secEpisodeReach);
            writeResult(eb);
            return false;
        }
    }
    const bool retry = autoCapRetry();   // cfg dpseccaptiers: the last window again, larger
    const int level = autoLevel();
    if (level < 0) return false;
    // cfg dpsecstate: before the wall's second window, whether player 1's size is what stops it.
    if (g_cfg.dpSecState && !retry && level >= 1 && !coinRung && stateFire(why)) return true;
    if (!retry && g_cfg.dpSecCapTiers > 0 && level > 0 && g_autoLastDrawn && !g_autoLastFound
        && !g_autoLastCapBound && g_autoCapTier < g_cfg.dpSecCapTiers)
        writeResult("dpsolve:   [secauto] the last window's cap never bound (nothing capped, or "
                    "the checkpoint path's clamp) - a larger cap would search it the same way; "
                    "the next window instead (cfg dpseccaptiers)");
    // cfg dpsecchain: a new wall right behind the pin (within dpsecchainspan ticks of it) extends
    // the chain, any other ends it.
    if (level == 0 && !retry)
        g_autoChain = (g_secPin > 0 && rungWall() > g_secPin
                       && rungWall() - g_secPin <= (long long)g_cfg.dpSecChainSpan)
                          ? g_autoChain + 1 : 0;
    if (level == 0 && !retry) {
        g_autoTriedWall = rungWall();
        g_autoLastT0 = -1;
    }
    g_autoTries = level + 1;   // spent whether or not a window can be drawn
    g_autoCapTier = retry ? g_autoCapTier + 1 : 0;
    AutoWindow w;
    char b[320];
    if (retry) {
        w = g_autoLastWin;
    } else if (!autoWindow(w, level)) {
        g_autoLastDrawn = false;
        // Nothing fits even reaching past the level's first tick: there is no further back to go.
        // (cfg dpsecmaxback: nor past the furthest a window may begin -- the wall is done too.)
        if (rungWall() - autoBack(level) < 1 || w.pastMax) g_autoLastT0 = 1;
        if (w.pastMax)
            snprintf(b, sizeof(b), "dpsolve:   [secauto] %s at t=%lld, but the next window would "
                     "begin more than %d ticks back (cfg dpsecmaxback) - this wall's windows are "
                     "spent", why, rungWall(), g_cfg.dpSecMaxBack);
        else
            snprintf(b, sizeof(b), "dpsolve:   [secauto] %s at t=%lld, but no window fits before "
                     "the death - the loop carries on", why, g_bestDeath);
        writeResult(b);
        return false;
    }
    g_autoLastT0 = w.t0;
    g_autoLastWin = w;
    g_autoLastDrawn = true;
    const int cap = g_cfg.dpSecCap << g_autoCapTier;
    g_secReqStart = w.t0;
    g_secReqTarget = 0.0;            // no x condition (see AutoWindow)
    g_secReqDepth = w.depth;
    g_secReqHorizon = w.horizon;
    g_secReqCap = cap;
    g_secReqRung = true;
    g_secReqAuto = true;
    g_secReqCoin = coinRung ? g_coinWallCoin : -1;
    // cfg dpsecstate: a window under the run's size requirement keeps the size (a rung of kind 3).
    const bool keepReq = g_cfg.dpSecState && !coinRung && stateReqActive() && w.t0 >= g_reqFrom;
    if (keepReq) {
        g_secReqState = 3;
        g_secReqSize = g_reqSize;
    }
    // The spine flies the deepest plan (the prefix), which the game flew to g_bestDeath: a spine
    // that dies before it caught the snapshot lying (secsolve::g_spineUntil). Not for a coin wall's
    // plan, nor for a window that drops the size the plan has.
    g_secReqSpineUntil = (!coinRung && !keepReq) ? g_bestDeath : -1;
    g_secReqPending = true;
    g_paused = true;   // hold the level still until the handoff takes it
    // The search's branching primitive, unless the session chose one. The player snapshot with a
    // cross-check against the checkpoint every 20 layers solved the same four windows as the
    // checkpoint path, at the same depths, 22-28x faster (140 -> 4.9 s, 20 -> 1.1 s,
    // 139 -> 6.3 s, 94 -> 4.1 s; lv4003, old model), and every splice held in the game. Its one
    // error, a missed death, is what the cross-check and the leaf replay are there to catch; a
    // section with moving portals still falls back to the checkpoint inside the search.
    if (secsolve::g_snapMode == 0 && secsolve::g_verifyEvery == 0) {
        secsolve::g_snapMode = 2;
        secsolve::g_verifyEvery = 20;
    }
    snprintf(b, sizeof(b), "dpsolve:   [secauto] %s at the wall t=%lld x=%.1f (rounds %d, "
             "no-record %d, head %lld, try %d) - a section solve from t=%lld (%s), alive to "
             "t=%lld (depth %lld), horizon %lld cap %d", why, rungWall(),
             (double)(rungWall() == g_bestDeath ? g_bestDeathX : 0.f),
             g_autoWallRounds, g_autoNoRec, g_autoHead, level + 1, w.t0, w.from.c_str(),
             w.t0 + w.depth, w.depth, w.horizon, cap);
    writeResult(b);
    if (retry) {
        snprintf(b, sizeof(b), "dpsolve:   [secauto] the same window again (cfg dpseccaptiers): at "
                 "cap %d it found nothing", g_cfg.dpSecCap << (g_autoCapTier - 1));
        writeResult(b);
    }
    if (keepReq) {
        snprintf(b, sizeof(b), "dpsolve:   [secstate] ...keeping player 1 %s (the run needs it until "
                 "past t=%lld)", g_reqSize ? "mini" : "normal", g_reqUntil);
        writeResult(b);
    }
    // Under coinroute the deepest plan and the plan the ladder is repairing need not be the same
    // one: the coins each had taken by its death, as GD credited them, and
    // the first coin the ladder's plan still lacks.
    if (g_cfg.coinRoute) {
        auto coinsAt = [](const std::vector<AnchorRow>& v, long long t) -> int {
            for (long long k = std::min<long long>(t, (long long)v.size() - 1); k >= 0; --k)
                if (v[(size_t)k].valid) return v[(size_t)k].coins;
            return -1;
        };
        const int bestCoins = coinsAt(anchors::g_deepest, g_bestDeath - 1);
        const int ladCoins = coinsAt(anchors::g_dead, g_lastDeathPhys - 1);
        int next = -1;
        for (size_t i = 0; i < solver::g_coins.size() && i < 8; ++i)
            if (ladCoins < 0 || !((ladCoins >> i) & 1)) { next = (int)i; break; }
        snprintf(b, sizeof(b), "dpsolve:   [secauto] coins: the rung's plan (deepest, died t=%lld) "
                 "had 0x%x; the ladder's plan died t=%lld (ranked t=%lld) with 0x%x; first coin it "
                 "lacks: %d", g_bestDeath, bestCoins < 0 ? 0 : bestCoins, g_lastDeathPhys,
                 g_lastDeath, ladCoins < 0 ? 0 : ladCoins, next);
        writeResult(b);
    }
    if (g_cfg.dpSecChain && g_autoChain > 0)
        writeResult("dpsolve:   [secauto] wall " + std::to_string(g_autoChain)
                    + " in a row right behind the pin - margin "
                    + std::to_string((w.t0 + w.depth) - rungWall()) + " ticks");
    return true;
}

// ---- learning from a refuted checkpoint (worker thread) ----
//
// Called by the job after a search came back cancelled. Runs the ordinary fixup recorder on the
// flight the game refuted -- its plan and its recorded rows, swapped in for the loop's own for the
// length of the call and swapped back after -- and says whether the model learnt anything. The
// caller then solves the same question again; the death tick is noted, so a flight that dies on it
// again is passed instead of cancelling forever.
//
// Returns false only when there was no refuted checkpoint to learn from, i.e. the cancel came from
// something else (the session ending). The caller treats that as the end of the job.
inline bool ckLearn(int& learnt) {
    learnt = 0;
    CkDeath d;
    {
        std::lock_guard<std::mutex> g(g_ckMx);
        if (!g_ckDeath.ready) return false;
        d = std::move(g_ckDeath);
        g_ckDeath = CkDeath{};
    }
    // Nothing can be flying now: the search that published the checkpoint has returned, and the
    // next one has not started. So nothing can raise a cancel that this would wipe.
    dpbridge::cancelSearch(false);
    const int before = g_fixupCount;
    // The recorder replays g_planPath, reads heldBefore(g_plan, ...) and reads the dead attempt's
    // rows. All three are the loop's own for the rest of the job, so they are lent, not replaced.
    writeInputsFile(g_planPath, d.plan);
    std::vector<InputCmd> ownPlan;
    ownPlan.swap(g_plan);
    g_plan = d.plan;
    anchors::g_dead.swap(d.rows);
    g_ckInner = true;
    recordFixups(d.deathT);
    g_ckInner = false;
    anchors::g_dead.swap(d.rows);
    g_plan.swap(ownPlan);
    learnt = g_fixupCount - before;
    char b[256];
    snprintf(b, sizeof(b), "dpsolve:   [check] checkpoint #%zu (t0=%lld..%lld) died at t=%lld "
             "x=%.1f: %d new record(s) - solving it again; this death is passed from now on",
             d.index, d.t0, d.end, d.deathT, (double)d.deathX, learnt);
    writeResult(b);
    // ONE CANCEL PER DEATH TICK PER JOB, whether or not the recorder wrote anything. Measured on
    // lv22 (2026-09-17, first run with this on): the rung at t0=230 was cancelled 33 times for
    // the same death at t=662. The recorder wrote 3-4 new records every time, and every time the
    // re-solved search put its first state on a lineage that died at t=662 again -- records that
    // are real and still do not move the frontier's first choice. The loop itself learns once
    // from a death and then re-anchors onto a different question; this does the same.
    {
        std::lock_guard<std::mutex> g(g_ckMx);
        g_ckDeaf.insert(d.deathT);
    }
    return true;
}

// How many times one rung may be cancelled and solved again. Each retry needs a new fixup record
// or a newly passed death tick, so this is a backstop against a bug, not a budget.
constexpr int kCkRetriesPerRung = 64;

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
    // After a section solve (secrung) the plan ends where the search reached its target, and
    // what follows was flown with no input at all -- planned by nobody. The splice's end is the
    // last state known to be good, so it is a rung of its own, in its place among the others;
    // the geometric steps jump over it. Measured (lv4003, old model): a splice ending at t=5443
    // flew off the top of the level by t=6044 and died at t=6243, the rungs 6, 24, 96 and 384
    // ticks before that death all started inside the flight, and the next one was behind the pin.
    if (g_secPin > 0 && dt - g_secPin >= 2) {
        const int pb = (int)(dt - g_secPin);
        auto at = std::lower_bound(rungs.begin() + (std::ptrdiff_t)nForced, rungs.end(), pb);
        if (at == rungs.end() || *at != pb) rungs.insert(at, pb);
    }

    std::vector<InputCmd> tail, chosenPlan;
    size_t nPrefix = 0;
    long long chosenT = -1;
    int chosenBackoff = 0;
    bool solvedTail = false;
    bool tailClaimsGoal = false;   // ...and SOLVED by reaching the end, not by outliving the horizon
    size_t ckRung = (size_t)-1;   // the rung the checkpoint retries below are counting
    int ckRetries = 0;
    int fineTries = 0;            // cfg dpfineretry: fine retries asked, this ladder
    int pinReleases = 0;          // cfg needpinrelease: boxes released behind a pin, this ladder
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
        // A section solve's answer lives in the plan from g_secPin backwards (secrung).
        // An anchor before it hands that stretch back to the model that could not do it:
        // measured on a second wall, the ladder re-anchored across a spliced segment and
        // the loop then died 37 times at the same tick, re-writing the same fixups.
        // Not a hard floor -- if every rung is pinned out the pin is dropped rather than
        // leaving the ladder with nothing to try.
        if (g_secPin > 0 && t0 < g_secPin) {
            writeResult("dpsolve:   anchor t=" + std::to_string(t0)
                        + " is before the section solve pinned at t="
                        + std::to_string(g_secPin) + " - skipping this rung");
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
        const std::string arg = startArg(t0, *r, held,
                                         swingPendingAt(g_plan, t0));
        std::vector<std::string> a = baseArgs(g_tailPath, (double)r->x);
        // The forcing slit rides ONLY the forced rung (the hint's re-anchor); every other
        // rung and every later solve is free to answer differently.
        if (rungIdx < nForced && !forceBand.empty()) {
            a.push_back("--deadband");
            a.push_back(forceBand);
        }
        bool rotQ = false;
        {   // 2900s the recorded run consumed before this anchor (see spentRotArg)
            const std::string sr = spentRotArg(t0);
            if (!sr.empty()) { a.push_back("--spentrot"); a.push_back(sr); }
            // ...and the recording's trigger ticks in rotated territory
            // (--trigraw; same wiring as the fixup resim above)
            if (!g_rotObjs.empty()) a.push_back("--trigraw");
            // ...and, under cfg dprotseed, the queue, exactly as the fixup resim does.
            rotQ = rotSeedArgs(t0, a, "anchor");
            // ...and the queue's toggle rule, seeded with the touch Toggles this attempt had
            // already entered (touchSeedArg), as the resim does (always, as there).
            {
                a.push_back("--rotqtoggle");
                const std::string ts = touchSeedArg(t0);
                if (!ts.empty()) { a.push_back("--touchseed"); a.push_back(ts); }
            }
            // ...and which touch boxes the attempt had reached.
            {
                a.push_back("--touchentered");
                a.push_back(touchEnteredArg(t0));
            }
            // ...and, under cfg dpspentpad, the pads it had already fired, as the resim does.
            if (g_cfg.dpSpentPad) {
                const std::string sp = spentPadArg(t0);
                if (!sp.empty()) { a.push_back("--spentpad"); a.push_back(sp); }
            }
            // ...and the rings.
            {
                const std::string so = spentOrbArg(t0);
                if (!so.empty()) { a.push_back("--spentorb"); a.push_back(so); }
            }
            // ...and, under cfg dpxtrack, GD's x up to t0, as the resim does.
            if (g_cfg.dpXTrack) {
                const std::string xt = xtrackArg(t0);
                if (!xt.empty()) { a.push_back("--xtrack"); a.push_back(xt); }
            }
        }
        a.push_back("--start");
        a.push_back(arg);
        // ...and the coins GD had already credited by t0 (AnchorRow::coins).
        const std::string coinMask = g_cfg.coinRoute ? std::to_string(r->coins) : "";
        if (!coinMask.empty()) { a.push_back("--coinmask"); a.push_back(coinMask); }
        // ...and GD's item counters there (AnchorRow::item1). The pickups behind
        // the anchor are not in the window the model's own mask numbers, so the
        // history has to arrive as the number the Count trigger reads.
        std::string itemBase;
        if (g_cfg.coinRoute && r->item1 >= 0) {
            itemBase = std::to_string(r->item1) + ":" + std::to_string(r->cnt1);
            if (r->item2 >= 0)
                itemBase += "," + std::to_string(r->item2) + ":"
                          + std::to_string(r->cnt2);
            a.push_back("--itembase");
            a.push_back(itemBase);
        }
        // ...and the touch triggers GD had already set off by t0 (see
        // anchorPayload). The solve gets the same seeding the fixup resim does,
        // or the two would be anchored into different worlds.
        const std::string ap = anchorPayloadAll(t0);
        if (!ap.empty()) { a.push_back("--anchor-state"); a.push_back(ap); }
        a.push_back(g_cfg.histRide ? "--anchorride" : "--no-anchorride");   // as in fixupPass
        std::string band;
        if (r->pmax > r->pmin) {
            band = num(r->pmin) + "," + num(r->pmax);
            a.push_back("--startband");
            a.push_back(band);
        }
        // The payload goes on this line too, and not only for reading: it is the
        // ONLY place it is ever written down. The argv is built in-process and
        // handed to cliMain, dp's own printf does not reach result.txt, and
        // `solver args:` prints once per session (g_argsLogged) -- at the FIRST
        // solve, which has no anchor. So every instrument pointed at this file
        // reported "no payload" for a run in which the payload fired at every
        // anchor. Logged here, seedcheck can also replay a real payload offline
        // instead of a hand-made one.
        // The attempt this row came from is on the `[call] att=` line that logSolverArgs
        // writes just below, which covers every call rather than only the anchored ones.
        writeResult("dpsolve:   [anchor] att=" + std::to_string(g_attempt)
                    + " --start " + arg
                    + (band.empty() ? "  (band guessed from x)" : "  --startband " + band)
                    + (ap.empty() ? "" : "  --anchor-state " + ap)
                    + (coinMask.empty() ? "" : "  --coinmask " + coinMask)
                    + (itemBase.empty() ? "" : "  --itembase " + itemBase));
        std::error_code ec;
        std::filesystem::remove(g_tailPath, ec);   // a stale tail must not read as this call's
        logSolverArgs(a);
        const int rc = timedSolve(g_csv, a, "anchor");
        logTrigWindow("anchor");
        adoptCoinGates();
        if (rotQ) logRotSeedRead(t0, "anchor");
        if (g_cfg.dpSnapshot) {   // the plan this anchored search emitted (see snapFile)
            const std::string n = snapFile(g_tailPath);
            writeResult("dpsolve: output snap t0=" + std::to_string(t0) + " tail="
                        + (n.empty() ? std::string("-") : n + " " + fileSig(g_tailPath)));
        }
        dpbridge::SolveOutcome o = dpbridge::outcome();   // the fine retry below may replace it
        // cfg dpsecrent: the round's work, counted (see workDpState). Its own line rather than
        // a field on the verdict line below, whose tail is optional text.
        g_autoRoundWork += workDpCall() + workDpState() * (double)o.workStates;
        if (g_cfg.dpSecRent)
            writeResult("dpsolve:   [work] t0=" + std::to_string(t0) + " states="
                        + std::to_string(o.workStates));
        // A CHECKPOINT OF THIS RUNG WAS REFUTED (cfg `dpcheck`). The game killed a lineage the
        // model believed in, so the model is wrong somewhere on it: learn from that death and ask
        // this same rung again, with everything else about the ladder exactly as it was. The
        // rung's verdict comes from the search that finally runs to the end, so the splice below
        // and every reader of it see an ordinary answer.
        if (o.verdict == dpbridge::OutcomeCancelled) {
            int learnt = 0;
            if (!ckLearn(learnt)) {
                writeResult("dpsolve:   [check] the search was cancelled with no refuted "
                            "checkpoint waiting (the session is ending) - leaving the job");
                return false;
            }
            if (ckRung != rungIdx) { ckRung = rungIdx; ckRetries = 0; }
            if (++ckRetries <= kCkRetriesPerRung) {
                --rungIdx;     // size_t: wraps at 0 and the ++ in the for brings it back
                continue;
            }
            writeResult("dpsolve:   [check] this rung was cancelled "
                        + std::to_string(kCkRetriesPerRung)
                        + " times - treating it as doomed and backing off");
            continue;
        }
        // cfg dprejoinuse: a joined tail is flown as it is. Two refusal rules were
        // measured and dropped (2026-09-19, lv16/20/22): refusing a join whose own walk dies, and
        // refusing one whose walk stops retracing the old plan's. Both refused most of lv22's
        // joins -- the old trace was walked under an older model, so the new walk departs from it
        // as a matter of course -- and each refusal paid a second search: lv22 went 249 s -> 462 s
        // and 526 s. What is kept is only the note, and which tail is a join (see g_rjOldIsJoin).
        g_rjTailJoined = o.rejoinT >= 0;
        if (o.rejoinT >= 0) {
            char jb[224];
            if (o.rejoinBadT >= 0)
                snprintf(jb, sizeof(jb), "dpsolve:   [rejoin] joined the plan that last died at "
                         "t=%lld (its walk leaves the old one at t=%lld: %s)", o.rejoinT,
                         o.rejoinBadT, o.rejoinBadWhy ? o.rejoinBadWhy : "?");
            else
                snprintf(jb, sizeof(jb), "dpsolve:   [rejoin] joined the plan that last died at "
                         "t=%lld", o.rejoinT);
            writeResult(jb);
        }        std::vector<InputCmd> cand;
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
        bool usable = haveFile   // the fine retry below may replace it
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
        // 280 before resimdie was added, 352 before the killer was added to it;
        // snprintf truncates rather than overflows, but a truncated line would
        // silently drop the "- doomed" tail, which is the half a reader acts on.
        char b[448];
        char deep[64] = "";
        if (o.deepT >= 0) snprintf(deep, sizeof(deep), " t=%lld x=%.0f", o.deepT, o.deepX);
        // capHits is reported even though nothing acts on it: it is the one number that says
        // whether the search ran out of capacity or out of physics, and the rejected tier
        // ladder above is the reason that distinction has to stay visible
        // ...and so is resimdie, for the same reason and a sharper one: the
        // verdict on the left of this line is printed by the search, and the
        // plan on the right is then walked once more without anyone reading
        // whether it survived. A SOLVED whose own walk dies is an iteration
        // this loop is about to spend on GD to be told something dp already
        // knew. Nothing acts on it yet -- whether a fired tick invalidates a
        // plan depends on how often the model over-kills, and the whole-run
        // corpus turns out to have almost no power to measure that (19 of 22
        // levels never die at all). Counted first, decided later.
        char rd[128] = "";
        if (o.resimDead > 0)
            // the cause travels with it: "dp said it dies and GD killed it" is
            // agreement only if they are the same death. Two deaths in
            // different places read as a model that knew something it did not.
            //
            // ...and so does the KILLER, because the cause is a category. Every
            // hazard on the level answers `cube/hazard`, so on the cause alone
            // "the same object, moved" and "a different object entirely" are
            // the same row. That is exactly the open question on lv22: the
            // loop's walk dies at 1813 and an offline rebuild of the same solve
            // dies at 1837, and until the rows name an object the two cannot be
            // compared at all. stdout says it as `resimwho:`, which the mod
            // cannot read (dp_bridge.hpp:57).
            //
            // The position is read in whatever FRAME the walk was in, so the
            // frame travels with it. Without it, two rows reporting different
            // coordinates cannot be told from two rows reporting the same
            // place seen from different frames -- on lv22 the loop's killer and
            // an offline rebuild's turn out to differ by exactly a quarter
            // turn, and that was read off the numbers rather than measured.
            snprintf(rd, sizeof(rd),
                     " resimdie=%lld@%lld/%s who=%d@(%.1f,%.1f)f%d/0x%08llx",
                     o.resimDead, o.resimFirst, o.resimWhy ? o.resimWhy : "?",
                     o.resimUid, o.resimObjX, o.resimObjY, o.resimFrame,
                     o.resimTrig);
        else if (o.resimDead == 0)
            // ...and a clean walk says so. Printing nothing here would make
            // "the plan survived itself" and "this field is not wired on this
            // path" the same picture, and a whole cold run of blank lines would
            // then read as "no plan ever dies" with no way to tell it from a
            // dead field. The first run of this counter printed nothing on 192
            // dpsolve lines for exactly that reason.
            snprintf(rd, sizeof(rd), " resimdie=0");
        else
            snprintf(rd, sizeof(rd), " resimdie=?");
        // THE PLAN ITSELF, as a fingerprint. CMakeLists.txt:29-35 calls the two
        // dp builds behaving identically "an acceptance criterion, not an
        // aspiration", and the thing that has to match is the emitted plan --
        // not a number derived from it. `resimdie`'s first tick was used for
        // that on 2026-09-10 and it does not survive the job: raising --cap
        // alone moved it 1837 -> 1918 -> 1858 on one unchanged argv, so an
        // agreement there can be coincidence and a disagreement can be noise.
        //
        // `cand` is the tail this call just emitted, read back from g_tailPath,
        // so this is the bytes. planFnv is already the loop's own plan
        // fingerprint (the [fp] line), which means the offline side has one
        // function to reimplement and no format to guess.
        const std::string tailFp = cand.empty() ? std::string("0/-")
                                                : planFnv(cand);
        snprintf(b, sizeof(b),
                 "dpsolve:   [%s%s] rc=%d inputs=%zu capHits=%lld tail=%s%s%s",
                 o.verdict == dpbridge::OutcomeSolved ? "SOLVED"
                     : (o.verdict == dpbridge::OutcomePartial ? "PARTIAL" : "FAILED"),
                 deep, rc, cand.size(), o.capHits, tailFp.c_str(), rd,
                 usable ? "" : (!haveFile ? " - no tail written"
                              : (restartScale ? " - doomed, and this far back only a complete "
                                                "route is worth the prefix"
                                              : " - doomed, backing off")));
        writeResult(b);
        // The frontier was empty before a single tick ran, and the call required a box the
        // anchor is already past. That is not the level being impassable, it is a requirement
        // that cannot be met from here -- drop the box and let the ladder ask again.
        const unsigned long long stuckBoxes =
            o.needTrigMask & o.needTrigPassed & ~g_needTrigDropped;
        // cfg needpinrelease: ...at once, when a section-solve pin stands behind the anchor. The
        // release below waits for "the run stops getting anywhere" so that the empty frontier can
        // push the ladder back to an anchor before the box -- but the ladder never anchors before
        // a pin, so behind one that push has nowhere to go, and dpsecauto hands the wall to a
        // section solve before escalate() ever releases the box. Measured on lv22 with coins
        // (Steam, 2026-09-29): box uid 17770 (x=8,955) emptied every rung from t=9,571, the loop
        // re-flew one plan into t=9,674 for ten rounds between section solves that found nothing,
        // and the search with the box dropped went on to t=11,871 -- a plan the game then flew
        // to t=12,000. Retries the rung, a bounded number of times per ladder.
        if (g_cfg.needPinRelease && !usable && stuckBoxes && o.deepT <= 0 && g_secPin > 0
            && g_secPin < t0 && pinReleases < 8) {
            ++pinReleases;
            g_needTrigDropped |= stuckBoxes;
            snprintf(b, sizeof(b), "dpsolve:   the frontier was empty before tick 1 on a box this "
                     "anchor has already passed (mask 0x%llx), and the pin at t=%lld keeps every "
                     "rung past it - released, asking this rung again", stuckBoxes,
                     (long long)g_secPin);
            writeResult(b);
            --rungIdx;     // size_t: wraps at 0 and the ++ in the for brings it back
            continue;
        }
        if (!usable && stuckBoxes && o.deepT <= 0 && !(g_needTrigSuspect & stuckBoxes)) {
            g_needTrigSuspect |= stuckBoxes;
            snprintf(b, sizeof(b), "dpsolve:   the frontier was empty before tick 1: the search "
                     "was told to enter a box this anchor has already passed (mask 0x%llx) - "
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
        const unsigned long long aheadBoxes =
            o.needTrigMask & ~o.needTrigPassed & ~g_needTrigDropped & ~g_needTrigSuspect;
        if (!usable && aheadBoxes && o.deepT > 0) {
            g_needTrigSuspect |= aheadBoxes;
            snprintf(b, sizeof(b), "dpsolve:   a doomed rung was still being asked for boxes "
                     "ahead of it (mask 0x%llx) - noted, released only if the run stops getting "
                     "anywhere", aheadBoxes);
            writeResult(b);
        }
        // cfg dpfineretry: THE CORRIDOR NARROWER THAN A BIN. A doomed PARTIAL with no cap hit says
        // the search kept every cell it made and still lost the route -- and in ship / UFO a cell is
        // the flying modes' coarse dedupe bin (--shipyq / --shipvq), so two states a bin apart are one
        // cell and the keeper may be the one that dies. Asked once more over a short window with fine
        // bins (the cube's), no input grid and a big cap, from the same anchor; the answer replaces
        // this rung's and the usual adoption below takes it or backs off.
        // Measured on custom level I t~290, a UFO corridor ~5 px wide at x 327-355: every rung
        // came back PARTIAL t=295 capHits=0 for 7 rounds (309 s of a 617 s clear) while the model
        // replays GD's passing plan through it. --refwatch on that plan: LOST t=250 gate=dedupe, the
        // reference (y 211.77) merged into a kept state 1.69 px away that dies at 277. From t0=55
        // with --shipyq 2 --shipvq 10 --inputgrid 1 --cap 50000 over 400 ticks the frontier lives to
        // the horizon in 12.5 s; with the fine bins but the grid kept it dies at 311 (the route needs
        // a one-tick press on an odd tick), and with the grid lifted but coarse bins at 277.
        if (!usable && g_cfg.dpFineRetry && haveFile && o.verdict == dpbridge::OutcomePartial
            && o.capHits == 0 && o.deepT > t0 && fineTries < kFineTriesPerLadder) {
            const AnchorRow* dr = anchors::row(dt);
            if (!dr) dr = anchors::row(dt - 1);
            const bool flying = dr && (dr->mode == 1 || dr->mode == 3);   // ship, UFO (modeIdx)
            if (flying) {
                ++fineTries;
                const long long h = (dt - t0) + kFineLead;
                std::vector<std::string> fa = a;
                for (const std::string& s : {std::string("--cap"), std::to_string(kFineCap),
                                             std::string("--shipyq"), num(kFineYq),
                                             std::string("--shipvq"), num(kFineVq),
                                             std::string("--inputgrid"), std::string("1"),
                                             std::string("--capladder"), std::string("0"),
                                             std::string("--horizon"), std::to_string(h)})
                    fa.push_back(s);   // dp reads the last occurrence of each
                std::filesystem::remove(g_tailPath, ec);
                const int frc = timedSolve(g_csv, fa, "fine");
                const dpbridge::SolveOutcome of = dpbridge::outcome();
                std::vector<InputCmd> fc;
                loadInputsFile(g_tailPath, fc);
                const bool fHave = std::filesystem::exists(g_tailPath, ec);
                // Past the wall by a margin, or alive at the window's end (a horizon-cut SOLVED,
                // which the adoption below already tells from a goal-reaching one by horizonCut).
                const bool fUsable = fHave
                    && (of.verdict == dpbridge::OutcomeSolved
                        || (of.verdict == dpbridge::OutcomePartial && of.deepT > dt + 20));
                char fb[256];
                snprintf(fb, sizeof(fb),
                         "dpsolve:   [fine] t0=%lld horizon=%lld rc=%d %s t=%lld x=%.0f "
                         "capHits=%lld inputs=%zu%s", t0, h, frc,
                         of.verdict == dpbridge::OutcomeSolved
                             ? (of.horizonCut ? "SOLVED(horizon)" : "SOLVED")
                             : (of.verdict == dpbridge::OutcomePartial ? "PARTIAL" : "FAILED"),
                         of.deepT, of.deepX, of.capHits, fc.size(),
                         fUsable ? " - taken" : " - doomed too");
                writeResult(fb);
                if (fUsable) {
                    o = of;
                    cand = std::move(fc);
                    usable = true;
                }
            }
        }
        if (!usable) continue;
        // Test the FULL splice: the same tail on a different prefix is a different question.
        std::vector<InputCmd> next;
        for (const auto& c : g_plan) {
            if (c.step >= t0) break;
            next.push_back(c);
        }
        const size_t prefix = next.size();
        next.insert(next.end(), cand.begin(), cand.end());
        const auto context = planContext(a);
        const auto edges = planEdges(next);
        const long long failed = g_failedPlans.failedAt(context, edges);
        if (failed >= 0) {
            writeResult("dpsolve:   [repeat] identical full plan already died at t="
                        + std::to_string(failed) + " under unchanged inputs - backing off");
            g_repeatRejected = true;
            continue;
        }
        chosenPlan = std::move(next);
        nPrefix = prefix;
        g_candidateContext = context;
        g_candidateEdges = edges;
        tail = std::move(cand);
        chosenT = t0;
        chosenBackoff = bo;
        // KEEP THE TAIL, next to the splice that is about to consume it. Once it
        // is merged into the plan its own ticks are gone, and the next question
        // about a doomed plan -- whether the tail solver's anchor->global tick
        // mapping is off by one -- cannot be asked from the spliced plan alone.
        // Named by the iteration and the anchor so it pairs with the dp_died_*
        // file the death writes.
        {
            char tp[512];
            snprintf(tp, sizeof(tp), "%s/dp_tail_it%d_a%lld.txt", DATA_DIR,
                     g_iter, (long long)t0);
            writeInputsFile(tp, tail);
        }
        solvedTail = (o.verdict == dpbridge::OutcomeSolved);
        tailClaimsGoal = solvedTail && !o.horizonCut;
        // The forced (portal) rung's tail gets a follow grace -- see the branch at the
        // rewind decision. Set on the choice, not the splice, so only this rung grants it.
        if (rungIdx < nForced) g_followForced = 6;
        break;
    }
    if (chosenT < 0) {
        // Every rung refused. If the section-solve pin is what closed them, drop it rather
        // than leave the loop with no anchor at all: keeping a stretch is worth less than
        // being able to repair.
        //
        // cfg dpsecauto: not when the splice HELD. If the run now dies past the pin, the rungs
        // past it are spent on the NEXT wall, and dropping the pin hands the stretch behind it
        // back to the model that could not do it: measured (lv4003, old model), the ladder's
        // growing backoff dropped a good pin twice, and 3 of the next 15 rounds re-planned the
        // solved stretch and died on its wall again. That next wall is a section solve's.
        if (g_secPin > 0) {
            if (g_cfg.dpSecAuto && dt > g_secPin && !autoTried()) {
                writeResult("dpsolve:   every rung past the pin at t=" + std::to_string(g_secPin)
                            + " is spent - the pin stays, this wall goes to a section solve");
                g_autoPinOut = true;
            } else {
                writeResult("dpsolve:   every rung is before the pin at t="
                            + std::to_string(g_secPin) + " - dropping the pin");
                g_secPin = -1;
            }
        }
        return false;
    }

    // Splice. The prefix is cut at `< t0`, the same window heldBefore uses -- cutting one tick
    // earlier drops an input the tail was told is still held.
    //
    // Nothing is inserted at the seam. Releasing the button there (which an earlier version of
    // the driver did) breaks a tail that was solved on the assumption the button is down, and
    // the fixed position it used was one tick early for every mode with input latency 1. The
    // tail emits its own release at whatever tick its mode calls for.
    std::vector<InputCmd> next = std::move(chosenPlan);
    g_plan.swap(next);
    g_curBackoff = chosenBackoff;
    g_lastTailSolved = solvedTail;
    // ...and whether the WHOLE installed plan now claims the goal, which under
    // cfg coinroute means "every coin as well". Only such a plan can have a coin
    // refused by the game; a partial one misses coins by construction and must
    // be allowed to fly on (see the miss request in hooks_gamelayer.cpp).
    // A tail SOLVED by outliving its horizon claims neither the end nor a coin. It was
    // read as claiming both, and on SubZero 4002 (coin on, 2026-09-23) 55 of the 57
    // attempts the game ended at the second coin followed such a tail: the loop
    // anchored 6 ticks before the coin, the step-sized tail came back SOLVED, the game
    // cut the flight where the player passed the coin the plan never meant to take,
    // and the loop booked it as a refused coin and anchored there again.
    g_planClaimsGoal = tailClaimsGoal;
    g_anchorT = chosenT;
    g_anchorX = anchors::row(chosenT) ? anchors::row(chosenT)->x : 0.f;
    char b[224];
    snprintf(b, sizeof(b), "dpsolve:   re-anchored at t=%lld (backoff %d): %zu kept + %zu new "
             "= %zu inputs", chosenT, chosenBackoff, nPrefix, tail.size(), g_plan.size());
    writeResult(b);
    return true;
}

// The fingerprint of a plan, in ONE place. The [fp] line signs the plan GD flew
// with it and the [resim] line signs the plan the fixup recorder is about to
// replay; two fingerprints computed by two copies of this arithmetic could drift
// apart, and then a comparison between them would prove nothing.
inline std::string planFnv(const std::vector<InputCmd>& p) {
    uint64_t h = 1469598103934665603ULL;
    for (const InputCmd& c : p) {
        const uint64_t v = (uint64_t)c.step * 2u + (uint64_t)(c.down ? 1 : 0);
        for (int i = 0; i < 8; ++i) {
            h ^= (uint8_t)(v >> (i * 8));
            h *= 1099511628211ULL;
        }
    }
    char out[48];
    snprintf(out, sizeof(out), "%zu/%08x", p.size(),
             (unsigned)(h & 0xffffffffULL));
    return out;
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
    // Checkpoint flights belong to one job. Cleared HERE, on the main thread, before the worker
    // exists: `cancel` is set by the main thread and read by the worker, so clearing it inside the
    // worker could wipe a cancel the main thread had just raised.
    g_ckFlying = false;
    g_ckInstalled = false;
    g_ckCall = 0;
    g_ckIndex = 0;
    g_ckT0 = -1;
    g_ckEnd = -1;
    g_ckPlan.clear();
    g_ckBase = g_plan;
    g_ckFlights = g_ckPassed = g_ckDeaths = 0;
    g_ckObs = CkObs{};
    g_ckObsSkipped = 0;
    {
        std::lock_guard<std::mutex> g(g_ckMx);
        g_ckDeath = CkDeath{};
        g_ckDeaf.clear();
    }
    dpbridge::cancelSearch(false);    g_t0 = std::chrono::steady_clock::now();
    g_dpSolving = true;    // the badge and the session HUD say SOLVING from here
    // Hold the level still while the solver works. Without this the player runs into the first
    // hazard over and over for the length of the solve, which is exactly what it looks like
    // when the mod is doing nothing at all -- the one impression this must not give
    g_paused = true;
    g_hudPhase = phase;
    const int gen = g_generation.load();   // this session's generation, read on the main thread
    std::thread([kind, arg, gen]() {
        bool ok = false;
        g_candidateContext = solver::PlanContext{};
        g_candidateEdges.clear();
        g_repeatRejected = false;
        try {
            dpbridge::beginInputJob((unsigned long long)gen, g_csv);
            struct InputJob {
                // Even an exception must release the job's immutable-file pins.
                ~InputJob() { dpbridge::endInputJob(); }
            } inputJob;
            if (kind == JobFirstSolve) {
                // A refuted checkpoint (cfg `dpcheck`) cancels this search; learn from it and
                // solve again, as a ladder rung does. The argv is rebuilt each time: the first
                // record creates the fixups file, and baseArgs only passes a file that exists.
                bool cancelledOut = false;
                bool fromFlight = false;
                for (int tries = 0;; ++tries) {
                    std::error_code ec;
                    std::filesystem::remove(g_planPath, ec);
                    // From the start of the level (cfg dpcontenthorizon reads where that is).
                    std::vector<std::string> a0 = baseArgs(g_planPath, g_startX);
                    addFirstStart(a0);   // ...or from GD's own tick 1 (prepareFirstStart)
                    logSolverArgs(a0);
                    g_rc = timedSolve(g_csv, a0, "first");
                    g_autoRoundWork += workDpCall()
                                       + workDpState() * (double)dpbridge::outcome().workStates;
                    logTrigWindow("head");
                    adoptCoinGates();
                    if (dpbridge::outcome().verdict != dpbridge::OutcomeCancelled) {
                        g_candidateContext = planContext(a0);
                        break;
                    }
                    // cfg dpcheckfirst: the flight that stopped this solve IS the first plan.
                    if (g_cfg.dpCheckFirst) {
                        CkDeath d;
                        {
                            std::lock_guard<std::mutex> g(g_ckMx);
                            if (g_ckDeath.ready) {
                                d = std::move(g_ckDeath);
                                g_ckDeath = CkDeath{};
                            }
                        }
                        dpbridge::cancelSearch(false);
                        if (d.ready) {
                            g_plan = d.plan;
                            writeInputsFile(g_planPath, g_plan);
                            fromFlight = true;
                            char fb[240];
                            snprintf(fb, sizeof(fb), "dpsolve:   [checkfirst] the first solve "
                                     "stopped at its flight's death t=%lld (checkpoint #%zu, "
                                     "t0=%lld..%lld) - that flight is the first plan", d.deathT,
                                     d.index, d.t0, d.end);
                            writeResult(fb);
                            break;
                        }
                    }
                    int learnt = 0;
                    if (!ckLearn(learnt) || tries + 1 >= kCkRetriesPerRung) {
                        writeResult("dpsolve:   [check] the first solve was cancelled and is not "
                                    "being retried");
                        cancelledOut = true;
                        break;
                    }
                }
                // A plan with no inputs is a plan: dp writes the file only when it has one (a
                // FAILED search returns without writing, and the file was removed above), so the
                // file's EXISTENCE is the test -- the same one the ladder's tail uses.
                // loadInputsFile answers whether it parsed any line, and reading that as "no
                // plan" ended the run at round 0 on levels whose first stretch needs no press:
                // a custom level (and four of the divdb's 2.2 levels) were solved by the model
                // with zero inputs, reported "(NO PLAN)", and gave up without GD ever flying it.
                if (!fromFlight && !cancelledOut) {
                    loadInputsFile(g_planPath, g_plan);
                    std::error_code fe;
                    ok = std::filesystem::exists(g_planPath, fe);
                    if (ok) g_candidateEdges = planEdges(g_plan);
                } else {
                    ok = fromFlight;
                }
                // Whether this first plan claims the goal (see g_planClaimsGoal). A flight's
                // lineage stops at its checkpoint and claims nothing.
                g_planClaimsGoal = !fromFlight && ok
                                   && dpbridge::outcome().verdict == dpbridge::OutcomeSolved
                                   && !dpbridge::outcome().horizonCut;
            } else if (kind == JobSeedPlan) {
                g_rc = 0;
                ok = loadInputsFile(g_cfg.dpSeedPlan, g_plan);
                // A seeded plan is somebody else's claim; the game is about to
                // test it, so treat it as claiming the goal.
                g_planClaimsGoal = ok;
            } else {
                // Put the plan GD just replayed on disk. The fixup resim runs it through the
                // model with `--replay`, and the file is the only way to hand it over -- while
                // the plan itself lives in memory and is respliced every iteration. Without
                // this the resim replays whatever the last solver call happened to write, so
                // the model is running one plan and GD ran another, and EVERY tick of the
                // comparison is a divergence. Measured on lv18: it cleared in one round without
                // fixups and ground out all 41 with them.
                // **The plan GD flew**, not whatever is installed now: the rewind
                // in onDeath (`g_plan = g_best`) runs between the death and this
                // job, and the resim below replays this file against GD's dump of
                // the attempt that died. Before the first death there is no flown
                // plan, and g_plan is the same thing anyway. (Asked by the death,
                // not by emptiness: a plan that never presses is flown too.)
                const bool flown = g_lastDeath >= 0;
                writeInputsFile(g_planPath, flown ? g_flownPlan : g_plan);
                // ...and SAY WHICH PLAN THAT IS. The rewind above (g_plan =
                // g_best, in onDeath below logFingerprint) can have replaced it
                // since GD flew, in which case the recorder compares the model's
                // replay of one plan against GD's dump of another and every
                // record it writes is a difference between two plans. The [fp]
                // line already signs the flown plan; this signs the replayed one,
                // so the two can simply be read off against each other instead of
                // inferred from whether a rewind was logged.
                {
                    char fb[128];
                    snprintf(fb, sizeof(fb), "dpsolve:   [resim] it=%d plan=%s",
                             g_iter,
                             planFnv(flown ? g_flownPlan : g_plan).c_str());
                    writeResult(fb);
                }
                // Learn first, then search. The recorder reads the trajectory GD just flew, so
                // it has to run before anything resets the level -- and the ladder's first call
                // should already have the benefit of it.
                const int fixBefore = g_fixupCount;
                // A coin miss is recorded too. The pass compares the model's
                // replay with GD's dump, and a coin missed by half a pixel IS a
                // trajectory disagreement worth learning -- what must not be
                // written is the KILL record, which the pass itself withholds
                // (see the g_lastDeathCoinMiss gate in fixupPass).
                // The PHYSICAL death: under cfg coinmisspost a death can be ranked at a coin it
                // passed (g_lastDeath), but what GD and the model disagree about is where it died.
                if (!g_lastDeathNoCollision)
                    recordFixups(g_lastDeathPhys >= 0 ? g_lastDeathPhys : g_lastDeath);
                else
                    writeResult("dpsolve:   attempt end without collision evidence - no synthetic collision fixups recorded");
                // ...and what it learns there reopens the ladder only when the death IS where the
                // ladder climbs from. A death cfg coinmisspost ranked at a missed coin was learnt
                // from past that coin, which says nothing new about the anchors before it -- and
                // resetting to the shallowest rung on every such death (every one of them is past
                // the coin and teaches something) kept the ladder from ever backing off past the
                // switch the coin needs (SubZero 4003: 24 -> 96 -> 384 -> back to 24, forty rounds).
                const bool rankedAway = g_lastDeathPhys >= 0 && g_lastDeathPhys != g_lastDeath;
                if (g_fixupCount > fixBefore && rankedAway)
                    writeResult("dpsolve:   the model learnt something past the coin this death was "
                                "ranked at - the ladder keeps its rung");
                // ...and only so often at one wall. A lesson that is the same disagreement one
                // tick further on each round resets the ladder each round, so it never climbs
                // back to where the route is decided. On a custom level, a ship ring GD fires a tick
                // before the model was fired AGAIN in the model after each re-anchor, one record
                // per round at t=20,473, 74, 75 ... -- 13 resets in a row at x=29,779, the backoff
                // never past 96, while the anchors that still had a route were 105-119 ticks back.
                // The records are kept either way; what the cap withholds is the rewind.
                const bool wallCapped = g_cfg.dpLearnResets >= 0
                                        && g_learnResetWall == g_bestDeath
                                        && g_learnResets >= g_cfg.dpLearnResets;
                if (g_fixupCount > fixBefore && !rankedAway && wallCapped) {
                    char lb[200];
                    snprintf(lb, sizeof(lb), "dpsolve:   the model learnt something again at the "
                             "wall t=%lld (%d reset%s there already) - the ladder keeps its rung",
                             g_bestDeath, g_learnResets, g_learnResets == 1 ? "" : "s");
                    writeResult(lb);
                }
                if (g_fixupCount > fixBefore && !rankedAway && !wallCapped) {
                    // The model is not what it was. Anchors that were doomed under the old one
                    // are open questions again, and the shallow rungs -- the ones that keep the
                    // most of the verified prefix -- deserve the first look.
                    if (g_learnResetWall != g_bestDeath) g_learnResets = 0;
                    g_learnResetWall = g_bestDeath;
                    ++g_learnResets;
                    g_spentAnchors.clear();
                    g_curBackoff = kBackOff;
                    writeResult("dpsolve:   the model learnt something - every anchor is worth "
                                "another look, starting from the shallowest");
                }
                // cfg dpsecsolved: this death goes to a section solve (onDeath). No ladder --
                // poll() queues the rung when the job comes back empty -- unless no window fits,
                // and then the round is an ordinary one.
                AutoWindow aw;
                if (g_autoRecordThenRung && autoWindow(aw, autoLevel())) {
                    ok = false;
                } else {
                    g_autoRecordThenRung = false;
                    ok = runLadder(arg);
                }
            }
        } catch (...) {
            ok = false;    // never let an exception cross back into the game's frame
        }
        const auto cache = dpbridge::inputJobStats();
        writeResult("dpsolve:   [inputcache] reads=" + std::to_string(cache.reads)
                    + " hits=" + std::to_string(cache.hits)
                    + " levelhits=" + std::to_string(cache.levelHits));
        g_haveNewPlan = ok;
        g_resultGeneration = gen;   // set before g_finished, same ordering convention as g_rc
        g_finished = true;
    }).detach();
}

// What begins the run: a real search, or a seeded plan waiting to be verified. Both call sites
// that used to spawn(JobFirstSolve, ...) directly go through this instead (see JobSeedPlan).
// ---- where the first solve starts ----
//
// Is GD's tick-1 row the model's own default start -- a normal-size, normal-speed cube on the
// floor at (0, 105) in the unrotated frame? vy and the contact flags are not compared: they are
// what that position already implies, and a level that starts elsewhere differs in the fields
// that are. Every official level is the default, so they take none of what follows.
inline bool spawnIsModelDefault(const AnchorRow& r) {
    return std::fabs(r.x) < 0.01f && std::fabs(r.y - 105.f) < 0.01f && r.mode == 0
        && r.flip == 0 && r.mini == 0 && r.dual == 0 && r.gframe == 0
        && std::fabs(r.speed - 0.9f) < 1e-3f;   // 0.9 = GD's 1x
}

// The first solve's --start / --startband, built on the main thread before the job exists (the
// job only reads them). Empty = the model's default start, which is the argv every level solved
// before 2026-09-25 got.
//
// The row is GD's, so this is the same anchor the ladder takes (startArg), only at t0 = 1: that
// tick is where the recorder's rows begin, and GD's tick 1 does not move x (x = 0 there, the
// model's x(1) = 0 as well), so nothing is lost but a press ON tick 1.
//
// Known only when an attempt has run a tick before the first solve -- on a level with moving
// parts, which records itself first (startBootstrapRecord). A level with none still starts from
// the default; its first flight dies wherever GD really is, and the ladder anchors on GD from
// then on (and an empty first plan is flown, not refused -- see spawn()).
inline std::string g_firstStart, g_firstBand;

inline void prepareFirstStart() {
    g_firstStart.clear();
    g_firstBand.clear();
    const AnchorRow& r = g_spawnRow;
    if (!r.valid || spawnIsModelDefault(r)) return;
    g_firstStart = startArg(1, r, 0);
    if (r.pmax > r.pmin) g_firstBand = num(r.pmin) + "," + num(r.pmax);
    char b[400];
    snprintf(b, sizeof(b), "dpsolve:   [spawn] GD starts the player at x=%.3f y=%.3f mode=%d "
             "mini=%d flip=%d dual=%d speed=%.3f frame=%d, not where the model does (a 1x cube "
             "at 0,105) - the first solve starts from GD's tick 1: --start %s%s%s",
             (double)r.x, (double)r.y, r.mode, r.mini, r.flip, r.dual, (double)r.speed, r.gframe,
             g_firstStart.c_str(), g_firstBand.empty() ? "" : " --startband ",
             g_firstBand.c_str());
    writeResult(b);
}

inline void addFirstStart(std::vector<std::string>& a) {
    if (g_firstStart.empty()) return;
    a.push_back("--start");
    a.push_back(g_firstStart);
    if (!g_firstBand.empty()) {
        a.push_back("--startband");
        a.push_back(g_firstBand);
    }
}

inline void beginFirstAttempt() {
    prepareFirstStart();
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

// The keys of `now` whose value is not the one in `defs` (both effcfg::modCfg lines), as
// `key=value(default)`; "-" when none differ.
inline std::string cfgDiffLine(const std::string& defs, const std::string& now) {
    auto split = [](const std::string& s) {
        std::vector<std::pair<std::string, std::string>> v;
        std::istringstream in(s);
        for (std::string tok; in >> tok;) {
            const size_t eq = tok.find('=');
            v.emplace_back(tok.substr(0, eq), eq == std::string::npos ? "" : tok.substr(eq + 1));
        }
        return v;
    };
    std::map<std::string, std::string> d;
    for (auto& kv : split(defs)) d.insert(std::move(kv));
    std::string out;
    for (const auto& [k, v] : split(now)) {
        const auto it = d.find(k);
        if (it != d.end() && it->second == v) continue;
        if (!out.empty()) out += ' ';
        out += k + "=" + v + "(" + (it == d.end() ? std::string("?") : it->second) + ")";
    }
    return out.empty() ? "-" : out;
}

inline void start(GJBaseGameLayer* l) {
    if (!l) return;
    if (refuseIfPlatformer(l)) return;
    // autorun.cfg named a key the flag clean-up removed (session.hpp kRemovedCfg, which
    // printed each one): refuse to solve rather than run a loop that ignores what was asked.
    if (!g_cfgRemoved.empty()) {
        for (const auto& m : g_cfgRemoved) writeResult("cfg: " + m);
        writeResult("dpsolve: refused - autorun.cfg names " + std::to_string(g_cfgRemoved.size())
                    + " removed key(s)");
        g_paused = true;
        endSession("removed_cfg");
        return;
    }
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
    if (g_running.load()) {
        // ...and the level is held still until it runs. Left running, the new level's first
        // attempt dies within a frame's batch, onDeath takes it as a round of a session that has
        // not started, spawns a job for it -- and the slot is never free at a frame boundary
        // again, so the queued start never runs. Measured with checkpoint flights on (2026-09-19,
        // lv4 -> lv5 in one game): a flight cleared lv4 while its search was still out, and lv5
        // was then "solved" with lv4's plan and csv for 17 rounds.
        g_pendingLayer = l;
        g_paused = true;
        writeResult("dpsolve: start queued behind a job still running");
        return;
    }
    g_pendingLayer = nullptr;
    // A heavy level is solved on a slice of itself (level_slice.hpp): the level is swapped for
    // the copy, and this runs again on it.
    if (levelslice::maybeSlice(l)) return;
    std::vector<InputCmd> carried;
    // A slice's wall check -- the copy's deepest plan flown on the level, or the return to the
    // same copy afterwards -- is a flight, not a new run: nothing of the loop is touched (its
    // recordings and anchors are the copy's, and the copy comes back from the same string).
    if (levelslice::takeLight(carried)) {
        std::sort(carried.begin(), carried.end(),
                  [](const InputCmd& a, const InputCmd& c) { return a.step < c.step; });
        g_plan = std::move(carried);
        g_cfg.inputs = g_plan;
        g_paused = false;
        writeResult("dpsolve: " + std::string(levelslice::g_phase == levelslice::Verifying
                                                  ? "check flight on the level itself"
                                                  : levelslice::g_phase == levelslice::RefFlight
                                                        ? "first-attempt flight on a fresh copy"
                                                        : "back on the copy, where the loop was")
                    + " (" + std::to_string(g_plan.size()) + " inputs, round "
                    + std::to_string(g_iter) + ")");
        return;
    }
    // ...and on the level a swap brought in, the plan the last one handed over is flown first,
    // with what the run has learnt so far ("what carries across a swap", level_slice.hpp).
    const bool carry = levelslice::takeCarry(carried);
    // The section search's settings, put back at session end -- taken when the run starts, not
    // on a level a slice swap brought in: the run goes on there, with whatever its rungs set.
    if (!carry) captureSecsolveSettings();
    std::ostringstream oss;
    solver::writeObjRects(oss, l);
    g_csv = oss.str();
    // A level the solver cannot represent is refused HERE, before any search, the way a
    // platformer is. It used to be found inside the first solve, where the loader called
    // std::exit on the solver's detached thread -- which ended the game itself (0xC0000409 in
    // abort) with nothing in result.txt after the `input sig` line. dp now reports it as
    // Level::unsupported and the CLI exits 2; here the session ends normally and says why.
    {
        const dpbridge::LevelStats st = dpbridge::statsFromCsv(g_csv);
        if (!st.unsupported.empty()) {
            writeResult("dpsolve: unsupported level - " + st.unsupported
                        + ". Nothing was searched and no plan was produced.");
            notify::show("gdsolver: unsupported level - " + st.unsupported,
                         NotificationIcon::Error, 6.f);
            g_paused = true;
            endSession("unsupported_level");
            return;
        }
    }
    loadModePortals(g_csv);    // where the mode portals are; see missedPortalTick
    loadRotObjs(g_csv);        // ...and the 2900s, for --spentrot (see spentRotArg)
    rotSeedLevelReset();       // ...and cfg dprotseed's queue order and witnesses
    if (g_cfg.dpRotSeed > 0 && rotQueueRequested()) {
        writeResult("dpsolve: cfg dprotseed REFUSED - cfg dparg=--rotqueue already runs the "
                    "queue on every call; dprotseed is off for this session");
        g_cfg.dpRotSeed = 0;
    }
    g_triedMissedPortal = false;
    g_forceAnchorT = -1;
    // Subscribe (or not) for the whole session. With no subscriber the solver publishes no
    // checkpoint and waits for nothing, so a session with `dpcheck` off runs the search it has
    // always run. Set here, before any solve is spawned: dp reads it once per call.
    dpbridge::checkSubscribe(g_cfg.dpCheck);
    dpbridge::cancelSearch(false);
    // cfg dpplainbeside when it is given, else the settings menu's "Solve faster" (the gamble).
    // Read here, on the game's thread, once for the session.
    const int menuMode = Mod::get()->getSettingValue<bool>("solve-gamble") ? 3 : 0;
    g_plainBesideMode = g_cfg.dpPlainBeside >= 0 ? g_cfg.dpPlainBeside : menuMode;
    if (g_plainBesideMode == 3)
        writeResult("dpsolve: solving under the gamble (dpplainbeside=3): the plain search is "
                    "taken as soon as it will do, so this run is not reproducible");
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
    // ...and so are the per-iteration plan and tail files this run is about to
    // write. They are named by ITERATION, so a shorter run leaves a previous
    // run's higher numbers in place and a reader globbing `dp_died_it5_t*`
    // silently gets whichever came first -- which is how the check of this very
    // instrument first came back MISMATCH on a fix that was working
    // (2026-09-02). The launcher empties the data dir for a cold run, but a
    // session started any other way does not, so the writer clears its own.
    // (A carried run keeps them: it is the same run, and its rounds go on counting.)
    if (!carry) {
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator(DATA_DIR, ec)) {
            const std::string n = e.path().filename().string();
            if (n.rfind("dp_died_it", 0) == 0 || n.rfind("dp_tail_it", 0) == 0
                || n.rfind("dp_band_it", 0) == 0 || n.rfind("dp_groups_it", 0) == 0
                || n.rfind("dp_fixin_", 0) == 0 || n.rfind("dp_attempt_it", 0) == 0)
                std::filesystem::remove(e.path(), ec);
        }
    }
    {
        std::error_code ec;
        // A carried run keeps its fixups: they are keyed by the player's state, not by an object.
        if (!carry) {
            std::filesystem::remove(g_fixupPath, ec);
            std::filesystem::remove(g_fixupNoopPath, ec);
        }
        // The recordings are keyed by uid, which the new level numbers differently, so they go
        // even when the run carries on.
        std::filesystem::remove(g_groupsPath, ec);   // the recordings are this run's, too
        std::filesystem::remove(g_groupsDeepPath, ec);
        std::filesystem::remove(g_groupsBootPath, ec);
        // ...and the one they are all copied FROM. It is written by the recorder, not by this
        // loop, so it was not in this list -- and a level that records nothing (no grouped
        // objects at all) leaves the previous level's file sitting there under the name every
        // harvest reads. The gate on that read is grouptrace::g_lastRoll, which now says rows=0
        // for such a level; deleting the file as well makes "no recording from another run is
        // reachable" true by construction rather than by one correct comparison.
        std::filesystem::remove(std::string(DATA_DIR) + "/grouptrace_last.txt", ec);
    }
    if (!carry) {
        g_fixupCount = 0;
        g_fixupNoop = 0;
    }
    g_groupsDepth = -1;
    g_groupsCopiedSig.clear();
    g_fixinGroupsBySig.clear();
    g_fixinPending = false;
    g_solveGroupsSig.clear();
    g_groupsX.clear();
    g_coinWall = -1;
    g_coinWallCoin = -1;
    g_coinWallPlan.clear();
    g_groupsDeepX.clear();
    g_groupsBootX.clear();
    g_retimeLeftOut = 0;
    g_deepActive = false;
    g_recordAttempt = false;
    g_deepDoneAt = -1;
    g_recordKind = RecNone;
    g_recordRequest = false;
    g_coldRestarted = false;
    g_escalations = 0;
    g_capTier = 0;
    g_topTierIter = 0;
    g_needTrigDropped = 0;
    g_needTrigSuspect = 0;
    g_spentAnchors.clear();
    g_learnResetWall = -1;
    g_learnResets = 0;
    // The vetoes are THIS run's observations, the same as the fixups above: a box the game
    // refuted on one level says nothing about the next, and carrying one into a second solve in
    // the same GD session would close ground that was never tested. (A carried run is the same
    // run on the same level with objects put back, so its vetoes stay.)
    if (!carry) {
        g_phantomHits.clear();
        g_phantomScale.clear();
        g_phantomBands.clear();
        g_phantomLifted = false;
    }
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
    // Adaptive mode opens with the whole level: nothing is known yet about where the model is wrong.
    g_horizonNow = g_horizonFull;
    // cfg dpcontenthorizon: ...unless the level says where it is likely to be (horizonFor).
    g_horizonFullAsked = false;
    g_newGimmickX.clear();
    g_startX = 0.0;
    if (l && l->m_player1) g_startX = l->m_player1->getPositionX();
    if (l && l->m_objects) {
        for (unsigned i = 0; i < l->m_objects->count(); ++i) {
            auto* o = static_cast<GameObject*>(l->m_objects->objectAtIndex(i));
            if (o && isNewGimmick(o->m_objectID, (int)o->m_objectType))
                g_newGimmickX.push_back((float)o->m_positionX);
        }
        std::sort(g_newGimmickX.begin(), g_newGimmickX.end());
    }
    if (g_cfg.dpContentHorizon) {
        char hb[200];
        snprintf(hb, sizeof(hb), "dpsolve:   [content] %zu objects newer than 1.7 (first at x=%.0f) "
                 "- the first solve plans %d ticks of %d", g_newGimmickX.size(),
                 g_newGimmickX.empty() ? -1.0 : (double)g_newGimmickX.front(),
                 horizonFor(g_startX), g_horizonFull);
        writeResult(hb);
    }
    g_wallRepeats = 0;
    g_triedWholeLevel = false;
    g_prevFlownFnv.clear();
    g_prevFlownDeath = -1;
    g_roundRepeated = false;
    g_flownDeaths.clear();
    g_rjAfterTick = -1;
    g_rjTailJoined = false;
    g_rjOldIsJoin = false;
    g_rjKeepTarget = false;
    {
        std::error_code ec;
        std::filesystem::remove(rejoinTracePath(), ec);
    }
    g_stallRuns = 0;
    g_needUnseen = false;
    if (!carry) g_iter = 0;
    g_plan.clear();
    // The deepest plan is not carried as the deepest: its depth was measured on the level the
    // run has just left. The carried plan is flown first and measured again here.
    g_best.clear();
    g_flownPlan.clear();
    g_bestDeath = -1;
    g_bestDeathX = 0.f;
    // The section-solve rung's state is this run's too. The pin in particular: carried into the
    // next level of a one-session run it would close every anchor before a tick of the LAST
    // level, and the recorder would start there.
    g_secPin = -1;
    g_secPinWall = -1;
    g_secRung = false;
    g_secRungAuto = false;
    g_secRetryPending = false;
    g_autoWall = -1;
    g_autoWallRounds = 0;
    g_autoProgress.reset();
    g_failedPlans.clear();
    g_candidateContext = solver::PlanContext{};
    g_candidateEdges.clear();
    g_repeatRejected = false;
    g_autoWallWork = 0.0;
    g_autoRoundWork = 0.0;
    g_secEpisodeWork = 0.0;
    g_secEpisodeReach = -1;
    g_secEpisodeRungs = 0;
    g_autoRungWork = 0.0;
    g_autoRungN = 0;
    g_autoHead = -1;
    g_autoNoRec = 0;
    g_autoRecNow = 0;
    g_autoTriedWall = -1;
    g_autoTries = 0;
    g_autoLastT0 = -1;
    g_autoCapTier = 0;
    g_autoLastDrawn = false;
    g_autoLastFound = true;
    g_autoLastCapBound = false;
    g_autoPinOut = false;
    g_autoChain = 0;
    g_autoRecordThenRung = false;
    g_swWall = -1;           // cfg dpsecstate
    g_swPhase = 0;
    g_swEvent = -1;
    g_swSize = -1;
    g_swTier = 0;
    g_swWin = AutoWindow{};
    g_reqSize = -1;
    g_reqFrom = -1;
    g_reqUntil = -1;
    g_lastDeath = -1;
    g_lastDeathNoCollision = false;
    g_coinMissPending = false;
    g_lastDeathCoinMiss = false;
    g_coinMissPostPending = false;
    g_postMissCoin = -1;
    g_postCoins = 0;
    for (long long& t : g_postCoinT) t = -1;
    g_routeNeedUid = -1;     // cfg routeprereq: a run's walls are its own
    g_routeNeedDeath = -1;
    g_routeEngaged = 0;
    for (int& n : g_routeRounds) n = 0;
    route::g_onTickDeepest.assign(route::g_prereq.size(), -1);
    g_lastDeathPhys = -1;
    if (!carry) g_coinMarginNow = 0.0;   // what the game refused so far (a carried run keeps it)
    g_planClaimsGoal = false;
    g_deathRuns.clear();
    g_curBackoff = kBackOff;
    g_lastTailSolved = false;
    g_followSolved = 0;
    g_followPeak = -1;
    g_followForced = 0;
    g_anchorT = -1;
    g_lastDeathX = 0.f;   // inert while g_lastDeath gates its only reader, but it is the same
                          // per-run pair and the two must not drift apart
    g_stop = false;
    g_showRequest = false;
    g_dpShowSolution = false;
    g_psnapPlayerWritten = false;   // tidyPlayerModes: per solve
    // GD's MAX GAMEPLAY Y is read off the LAYER, so it belongs to the level that was standing
    // when it was read. Nothing lowered it again: after a level whose bootstrap pass had already
    // run a tick, the next level's FIRST search -- the one made before any tick of it exists --
    // was handed the previous level's ceiling as `--maxplayy`. 0 withholds the flag, which is
    // what a freshly started game passes and what the cold logs of every level show.
    g_maxPlayYLive = 0.f;
    g_argsLogged = false;
    g_argsLast.clear();   // ...or level N+1's first solve compares against level N's
    anchors::reset();
    cpflight::reset();          // ...and the checkpoints kept with them (cfg cpflight)
    if (!carry) cpflight::resetCounts();   // the session's tally goes on across a carried run
    g_cpFlightSummary = &cpflight::summary;   // with the flag off too: its counts are then 0
    g_spawnRow = AnchorRow{};   // ...and so is the spawn: level N's must not start level N+1
    writeResult("dpsolve: " + std::string(carry ? "resume" : "start") + " level="
                + std::to_string(g_cfg.levelId)
                + " csv=" + std::to_string(g_csv.size()) + " bytes horizon="
                + std::to_string(g_horizon));
    // What this session runs with -- every cfg key, the ones that differ from the defaults, and
    // the core's built-in defaults -- for the run's manifest (py/cold_manifest.py). A default is
    // visible neither in the cfg a driver wrote nor in the solver's argv. No job is running here
    // (a start behind one is queued above), so the core reset defaultsProfile makes touches
    // nothing in flight.
    {
        const std::string eff = effcfg::modCfg();
        writeResult("cfgeff: " + eff);
        writeResult("cfgdiff: " + (g_cfgDefaults.empty()
                                       ? std::string("(no autorun.cfg was read, no defaults to compare)")
                                       : cfgDiffLine(g_cfgDefaults, eff)));
        writeResult("dpdefaults: " + dpbridge::defaultsProfile());
    }
    // A carried plan is flown as it is, on the attempt this reset is starting: the game is the
    // one to say how far it gets here, and the loop goes on from that death as from any other.
    if (carry) {
        std::sort(carried.begin(), carried.end(),
                  [](const InputCmd& a, const InputCmd& c) { return a.step < c.step; });
        g_plan = std::move(carried);
        g_cfg.inputs = g_plan;
        g_paused = false;
        writeResult("dpsolve: resume - flying the carried plan (" + std::to_string(g_plan.size())
                    + " inputs) after " + std::to_string(g_iter) + " rounds, "
                    + std::to_string(g_fixupCount) + " fixups kept");
        return;
    }
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
    // The plan is in memory, so it is summed the same way over its own edges --
    // through planFnv, the one copy of that arithmetic (see its note).
    const std::string ph = planFnv(g_plan);
    // The band the ATTEMPT started in, not the one it died in (see the note above).
    const std::vector<AnchorRow>* savedSrc = anchors::g_src;
    anchors::ladderOn(false);
    const AnchorRow* r1 = anchors::row(1);
    anchors::g_src = savedSrc;
    char band[48] = "-";
    if (r1) snprintf(band, sizeof(band), "%.1f,%.1f", (double)r1->pmin, (double)r1->pmax);
    char b[384];
    snprintf(b, sizeof(b),
             "dpsolve:   [fp] it=%d plan=%s att=%d t=%lld x=%.3f band1=%s "
             "fix=%d/%s horizon=%d backoff=%d live=%s deep=%s",
             g_iter, ph.c_str(), g_attempt,
             dt, deathX, band, g_fixupCount, fileSig(g_fixupPath).c_str(),
             g_horizonNow, g_curBackoff, fileSig(g_groupsPath).c_str(),
             fileSig(g_groupsDeepPath).c_str());
    writeResult(b);
    // KEEP THE PLAN THAT DIED -- **from memory, not from the file**. `g_plan` at
    // this point is the plan GD just replayed; the rewind that can replace it
    // (`g_plan = g_best`) is further down this same function, and the file only
    // catches up in the NEXT spawn(JobLadder). Copying dp_plan.txt here therefore
    // saved the PREVIOUS iteration's plan under this iteration's name, and a
    // reader comparing it against this attempt's dump compares two different
    // plans -- which on 2026-09-02 produced four withdrawn "model defects" in one
    // day, this instrument's own first version included.
    // One file per frontier report, named by the iteration and the death tick so
    // it lines up with the [fp] line above. ~15 KB each, in a data dir the
    // launcher empties before every cold run.
    {
        char pp[512];
        snprintf(pp, sizeof(pp), "%s/dp_died_it%d_t%lld.txt", DATA_DIR,
                 g_iter, dt);
        writeInputsFile(pp, g_plan);
    }
    // ...AND THE RECORDINGS THAT PLAN WAS SOLVED AGAINST, under the same name. dp_band.txt is
    // rewritten for every call from that call's anchor source and dp_groups.txt is replaced
    // whenever a deeper replay is harvested, so by the time anyone rebuilds this attempt the files
    // in the data dir belong to a later call -- and a rebuild with another call's band or moving
    // geometry cannot say whether a model rule or the missing input made the difference. The band
    // is small and copied every time; the groups recording runs to tens of MB on a level with
    // many movers, so it is copied only when its signature has changed since the last copy.
    {
        // cfg `capture`: the two copies below are the research trail (see Config::researchCapture)
        // -- off by default so a player's Solve does not fill the data folder with one dp_band_itN
        // / dp_groups_itN pair per died attempt. The [inputs] line itself is always written, with
        // capture=on|off appended, so a session always says which.
        std::error_code ec;
        const std::string bandSrc = std::string(DATA_DIR) + "/dp_band.txt";
        if (g_cfg.researchCapture) {
            char bp[512];
            snprintf(bp, sizeof(bp), "%s/dp_band_it%d_t%lld.txt", DATA_DIR, g_iter, dt);
            if (std::filesystem::exists(bandSrc, ec))
                std::filesystem::copy_file(bandSrc, bp,
                                           std::filesystem::copy_options::overwrite_existing, ec);
        }
        const std::string gs = fileSig(g_groupsPath);
        g_solveGroupsSig = gs;   // for the attempt's manifest (keepRecorderInputs)
        if (g_cfg.researchCapture && !g_groupsPath.empty()
            && std::filesystem::exists(g_groupsPath, ec) && gs != g_groupsCopiedSig) {
            char gp[512];
            snprintf(gp, sizeof(gp), "%s/dp_groups_it%d_t%lld.txt", DATA_DIR, g_iter, dt);
            std::filesystem::copy_file(g_groupsPath, gp,
                                       std::filesystem::copy_options::overwrite_existing, ec);
            if (!ec) g_groupsCopiedSig = gs;
        }
        char b2[256];
        snprintf(b2, sizeof(b2), "dpsolve:   [inputs] it=%d band=%s groups=%s capture=%s", g_iter,
                 fileSig(bandSrc).c_str(), gs.c_str(), g_cfg.researchCapture ? "on" : "off");
        writeResult(b2);
    }
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
    // Giving up on a slice's copy is the copy's verdict, not the level's (level_slice.hpp).
    if (levelslice::onGiveUp(reason)) return;
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

// The normal repair budget never restarts; a final backtrack only drains its existing windows.
inline void stopIterationBudget() {
    writeResult("dpsolve: iteration budget exhausted (" + std::to_string(g_cfg.dpMaxIters)
                + ") - no eligible backtrack window, stopping");
    g_stop = true;
    g_hudPhase = "gave up: out of iterations";
    giveUp("it ran out of repair rounds", "dpsolve_budget");
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
//
// ...unless the deepest record is on another clock (cfg groupsretime). The record is indexed by
// tick, and "deepest" assumes every replay reaches a given x at the same tick -- true while the
// routes differ only in y, false once one skips a speed portal. When this replay's x and the
// record's own replay's x part before this replay died, the record is out of phase from that
// tick on, so this replay's record replaces it even though it is shallower. `fresh` is false when
// the attempt was void and anchors::g_dead is still the previous one.
inline void harvestGroups(bool fresh = true) {
    if (!grouptrace::g_on || g_groupsPath.empty()) return;
    const grouptrace::Roll r = grouptrace::g_lastRoll;
    if (r.rows <= 0) return;
    const std::vector<AnchorRow>& v = anchors::g_dead;
    long long partT = -1;
    float xKept = 0.f, xNow = 0.f;
    if (r.depth <= g_groupsDepth) {
        if (!g_cfg.groupsRetime || !fresh || g_groupsX.empty()) return;
        const size_t n = std::min({v.size(), g_groupsX.size(), (size_t)std::max(0LL, r.depth)});
        for (size_t t = 0; t < n && partT < 0; ++t)
            if (v[t].valid && !std::isnan(g_groupsX[t])
                && std::fabs(v[t].x - g_groupsX[t]) > kRetimeTolX) {
                partT = (long long)t;
                xKept = g_groupsX[t];
                xNow = v[t].x;
            }
        if (partT < 0) return;
    }
    std::error_code ec;
    std::filesystem::copy_file(std::string(DATA_DIR) + "/grouptrace_last.txt", g_groupsPath,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return;
    g_groupsDepth = r.depth;
    if (g_cfg.groupsRetime) {
        g_groupsX.assign(v.size(), std::numeric_limits<float>::quiet_NaN());
        if (fresh)
            for (size_t t = 0; t < v.size(); ++t)
                if (v[t].valid) g_groupsX[t] = v[t].x;
    }
    char b[256];
    if (partT >= 0)
        snprintf(b, sizeof(b), "dpsolve:   moving geometry: kept this run's record (%lld rows, "
                 "depth t=%lld) over a deeper one on another clock - they part at t=%lld "
                 "(x %.1f there, %.1f here)", r.rows, r.depth, partT, xKept, xNow);
    else
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

// ---- checkpoint flights (cfg `dpcheck`, default off) ------------------------------------------
//
// THE PROBLEM. A whole-level search costs 45-60 s on lv22, and the game then kills the plan
// 100-1500 ticks past the anchor. On the 2026-09-17 blessed cold run, 33 of lv22's 44 rounds
// taught the model something (a new fixup record), and the 523 s of search before those rounds
// was spent almost entirely past the point where the game would have shown the model wrong.
//
// WHAT THIS DOES. While a search runs, the game flies the search's checkpoints (dp/progress.hpp):
// at fixed layers past the anchor, the lineage of the frontier's first state. A checkpoint that
// survives to its last tick is passed. One the game kills strictly inside is a disagreement with
// the model -- every frontier state is alive in the model through its layer -- so the search is
// cancelled, the fixup recorder runs on that flight, and the same question is solved again
// (ckLearn, runLadder). The loop's own rounds, verdicts and bookkeeping are untouched: they only
// ever see the search that ran to the end.
//
// WHY THE OUTCOME DOES NOT DEPEND ON TIMING. Checkpoints are layers, flights are judged in order,
// and dp holds its answer until every published checkpoint is judged -- so the sequence of flights,
// deaths and records is a function of the model and the game alone. The first version of this
// flew the frontier's common prefix and ended the round on a death; it was measured and not kept
// (lv22, 2026-09-17: the frontier stayed forked for thousands of ticks past the deaths, and a
// cancelled rung had no verdict for the ladder to book).
//
// A flight that dies AT or PAST its last tick, or on a tick this job has already learnt from, is
// passed: the first is the plan running out, the second would cancel again and again.

// End of a physics tick. A flight whose inputs have run out is held right there and passed:
// left to run on, it either dies a death that means nothing or wanders into the stall guard.
inline void ckTick(long long t) {
    if (!g_ckFlying) return;
    if (dpbridge::checkCall() != g_ckCall) {
        // Cannot happen while dp waits for its judgements; said out loud if it ever does.
        g_ckFlying = false;
        g_paused = true;
        writeResult("dpsolve:   [check] the call that published this checkpoint has returned - "
                    "dropping the flight");
        return;
    }
    if (t < g_ckEnd) return;
    g_ckFlying = false;
    g_paused = true;
    ++g_ckPassed;
    if (!dpbridge::passCheckpoint(g_ckCall, g_ckIndex))
        writeResult("dpsolve:   [check] a passed checkpoint was refused by the search");
}

// A death while a job is out. Only a flight's death means anything; everything else is ignored,
// as it always was.
inline void ckOnDeath(long long dt, float deathX, bool noCollision = false) {
    if (!g_cfg.dpCheck || !g_ckFlying) return;
    g_ckFlying = false;
    g_paused = true;
    char b[256];
    if (dpbridge::checkCall() != g_ckCall) {
        writeResult("dpsolve:   [check] died on a checkpoint whose call has returned - ignored");
        return;
    }
    if (noCollision) {
        ++g_ckObsSkipped;
        writeResult("dpsolve:   [check] attempt end without collision evidence - checkpoint skipped, not a collision refutation");
        if (!dpbridge::passCheckpoint(g_ckCall, g_ckIndex))
            writeResult("dpsolve:   [check] a skipped checkpoint was refused by the search");
        return;
    }
    bool deaf = false;
    {
        std::lock_guard<std::mutex> g(g_ckMx);
        deaf = g_ckDeaf.count(dt) != 0;
    }
    if (dt >= g_ckEnd || deaf) {
        snprintf(b, sizeof(b), "dpsolve:   [check] checkpoint #%zu (t0=%lld..%lld) died at t=%lld "
                 "- passed (%s)", g_ckIndex, g_ckT0, g_ckEnd, dt,
                 deaf ? "this job already learnt from this death" : "its inputs had run out");
        writeResult(b);
        ++g_ckPassed;
        if (!dpbridge::passCheckpoint(g_ckCall, g_ckIndex))
            writeResult("dpsolve:   [check] a passed checkpoint was refused by the search");
        return;
    }
    ++g_ckDeaths;
    // cfg dpcheckobs: keep the first death of the job, and let the search go on as if the flight
    // had passed. Nothing is recorded and nothing is cancelled.
    if (g_cfg.dpCheckObs) {
        if (!g_ckObs.have) {
            g_ckObs.have = true;
            g_ckObs.index = g_ckIndex;
            g_ckObs.t0 = g_ckT0;
            g_ckObs.end = g_ckEnd;
            g_ckObs.deathT = dt;
            g_ckObs.plan = g_ckPlan;
            g_ckObs.atSec = std::chrono::duration<double>(
                std::chrono::steady_clock::now() - g_t0).count();
            snprintf(b, sizeof(b), "dpsolve:   [checkobs] checkpoint #%zu (t0=%lld..%lld) died at "
                     "t=%lld x=%.1f, %.1f s into the job - kept; the search goes on", g_ckIndex,
                     g_ckT0, g_ckEnd, dt, (double)deathX, g_ckObs.atSec);
            writeResult(b);
        }
        if (!dpbridge::passCheckpoint(g_ckCall, g_ckIndex))
            writeResult("dpsolve:   [check] a passed checkpoint was refused by the search");
        return;
    }
    {
        // Hand the attempt over NOW: GD restarts the level about a second after a death and the
        // restart empties anchors::g_live.
        std::lock_guard<std::mutex> g(g_ckMx);
        g_ckDeath = CkDeath{};
        g_ckDeath.ready = true;
        g_ckDeath.call = g_ckCall;
        g_ckDeath.index = g_ckIndex;
        g_ckDeath.t0 = g_ckT0;
        g_ckDeath.end = g_ckEnd;
        g_ckDeath.deathT = dt;
        g_ckDeath.deathX = deathX;
        g_ckDeath.plan = g_ckPlan;
        g_ckDeath.rows.swap(anchors::g_live);
    }
    dpbridge::cancelSearch(true);
}

// A flight reached the end of the level. That is a real clear of the plan GD just flew -- the
// ordinary completion path files it as the solution -- so the job in flight is abandoned: its
// search is cancelled with no refuted checkpoint waiting, which ends it, and the collect in
// poll() discards whatever it returns. Returns false when no flight is in the air, in which case
// a completion during a job is not the mod's to take (and cannot happen: the level is held still).
inline bool g_ckAbandonJob = false;
inline bool ckClearedDuringJob() {
    if (!g_cfg.dpCheck || !g_ckFlying) return false;
    g_ckFlying = false;
    g_ckAbandonJob = true;
    char b[192];
    snprintf(b, sizeof(b), "dpsolve:   [check] checkpoint #%zu (t0=%lld..%lld) cleared the level - "
             "abandoning the search in flight", g_ckIndex, g_ckT0, g_ckEnd);
    writeResult(b);
    dpbridge::cancelSearch(true);
    return true;
}

// Once per frame while a job is out: fly the next checkpoint if one is waiting and nothing is in
// the air. Every checkpoint gets its own flight, in order -- a backlog is never skipped, because
// skipping on a backlog is how the clock would get back into the outcome.
inline void ckConsider() {
    if (!g_cfg.dpCheck || g_ckFlying) return;
    if (!g_running.load() || g_finished.load()) return;
    if (g_stop || g_sessionOver || !g_started || g_deepActive || g_dpShowSolution) return;
    auto* pl = PlayLayer::get();
    if (!pl) return;
    dpbridge::SolveCheckpoint cp;
    // cfg dpcheckobs: once the job has its death, every later checkpoint is passed unflown -- the
    // search's lineage runs through that death, and only the first one is anyone's to act on.
    if (g_cfg.dpCheckObs && g_ckObs.have) {
        while (dpbridge::nextCheckpoint(cp)) {
            ++g_ckObsSkipped;
            if (!dpbridge::passCheckpoint(cp.call, cp.index))
                writeResult("dpsolve:   [check] a passed checkpoint was refused by the search");
        }
        return;
    }
    if (!dpbridge::nextCheckpoint(cp)) return;
    // Spliced exactly as the ladder splices a tail: the installed plan up to (not including) the
    // anchor, then the checkpoint's own edges. A lineage's first edge can be pressed up to `lat`
    // ticks before its effect, so the seam can run backwards by a tick -- hence the sort.
    std::vector<InputCmd> plan;
    for (const InputCmd& c : g_ckBase) {
        if (c.step >= cp.t0) break;
        plan.push_back(c);
    }
    for (const std::pair<long long, int>& e : cp.edges)
        plan.push_back(InputCmd{(int)e.first, e.second != 0});
    std::sort(plan.begin(), plan.end(),
              [](const InputCmd& a, const InputCmd& c) { return a.step < c.step; });
    g_ckCall = cp.call;
    g_ckIndex = cp.index;
    g_ckT0 = cp.t0;
    g_ckEnd = cp.tick;
    g_ckPlan = plan;
    g_ckFlying = true;
    g_ckInstalled = true;
    ++g_ckFlights;
    g_cfg.inputs = plan;
    g_paused = false;
    pl->resetLevel();
}

// cfg dpcheckobs: the plan the job installed, against the flight its search lost first. Inputs
// compared by edge, from tick 0 through the death tick (and through the tick before it: an edge
// on the death tick itself cannot have caused it). Print only.
inline void ckObsCompare(double jobSec) {
    char b[360];
    if (!g_ckObs.have) {
        snprintf(b, sizeof(b), "dpsolve:   [checkobs] job %.1f s: no flight died (%zu flown)",
                 jobSec, g_ckFlights);
        writeResult(b);
        return;
    }
    const long long T = g_ckObs.deathT;
    auto upTo = [](const std::vector<InputCmd>& p, long long last) {
        std::vector<std::pair<int, int>> v;
        for (const InputCmd& c : p)
            if (c.step <= last) v.push_back({c.step, c.down ? 1 : 0});
        std::sort(v.begin(), v.end());
        return v;
    };
    const auto fl = upTo(g_ckObs.plan, T), pl = upTo(g_plan, T);
    const bool sameLe = fl == pl;
    const bool sameLt = upTo(g_ckObs.plan, T - 1) == upTo(g_plan, T - 1);
    long long firstDiff = -1;
    if (!sameLe) {
        size_t i = 0;
        while (i < fl.size() && i < pl.size() && fl[i] == pl[i]) ++i;
        const long long a = i < fl.size() ? fl[i].first : LLONG_MAX;
        const long long c = i < pl.size() ? pl[i].first : LLONG_MAX;
        firstDiff = std::min(a, c);
    }
    g_ckObs.compared = true;
    g_ckObs.same = sameLt;
    snprintf(b, sizeof(b), "dpsolve:   [checkobs] job %.1f s; flight #%zu (t0=%lld) died %.1f s in at "
             "t=%lld; the job's plan %s the flight's inputs up to it (through t-1: %s, first "
             "difference t=%lld; %zu flown, %zu passed unflown)", jobSec, g_ckObs.index,
             g_ckObs.t0, g_ckObs.atSec, T, sameLe ? "shares" : "does not share",
             sameLt ? "same" : "different", firstDiff, g_ckFlights, g_ckObsSkipped);
    writeResult(b);
}

// ---- the map's tails (itermap.hpp) ----
//
// One round's tail: what the attempt recorded in `rows` flew, from the tick it was spliced at to
// the tick it ended on. Recording only, like everything else the map is given.
//
// THE STRIDE GROWS WITH THE TAIL rather than the tail being cut at the point budget. A fixed
// stride under a fixed cap drew the first 9,600 ticks of a tail and nothing after them, and a tail
// is that long whenever the splice is early -- the first round's starts at 0. Measured on lv14
// (2026-09-19): round 2 flew t=5,531..19,758 and its line stopped at x=19,644, 6,000 px short of
// where it died. The last row is always added, so a line ends where its round ended rather than
// up to a stride before it.
inline void mapTail(const std::vector<AnchorRow>& rows, int iter, int kind, long long p0,
                    long long p1) {
    if (p0 < 0) p0 = 0;
    if (p1 > 400000) p1 = 400000;
    if (p1 >= (long long)rows.size()) p1 = (long long)rows.size() - 1;
    if (p1 <= p0) return;
    // Two short of the budget: the stride can land one point per `room`, plus the start, plus the
    // last row -- so the cap in addPathPoint never drops the end.
    const long long room = (long long)itermap::kPathMaxPts - 2;
    const long long step = std::max<long long>(itermap::kPathStep, (p1 - p0 + room - 1) / room);
    itermap::beginPath(iter, kind);
    long long last = -1;
    for (long long k = p0; k <= p1; k += step)
        if (rows[(size_t)k].valid) {
            itermap::addPathPoint(rows[(size_t)k].x, rows[(size_t)k].y);
            last = k;
        }
    for (long long k = p1; k > last; --k)
        if (rows[(size_t)k].valid) {
            itermap::addPathPoint(rows[(size_t)k].x, rows[(size_t)k].y);
            break;
        }
    itermap::endPath();
}

// The round that CLEARED. Every other round is put on the map by onDeath, and this one never
// dies -- so the map drew every tail but the one that got through, and the trajectory stopped at
// the last death as if the level ended there (reported 2026-09-19 on lv1/14/18; lv1's line ended
// at x=9,413, its only death). Called from levelComplete on the loop's own clear, before the map
// is saved. The recording is still the live one: only a death banks an attempt.
//
// Scored `deeper`, which is what it is, so the colour key needs nothing new; it is the one tail
// with no death mark at its end. A checkpoint flight (cfg `dpcheck`) that clears was spliced at
// its checkpoint's t0, not at the loop's anchor -- `byFlight` says which.
//
// A clear on the FIRST plan too, where this tail is the whole map: from tick 0 to the finish,
// with no death or fixup beside it (itermap::emptyLocked counts it, so the map is saved).
inline void mapClear(long long t, bool byFlight) {
    mapTail(anchors::g_live, g_iter + 1, itermap::KindDeeper, byFlight ? g_ckT0 : g_anchorT, t);
}

// cfg `coinmisspost` (config.hpp). Where a recorded attempt came closest to coin `ci`: the
// Chebyshev distance in world coordinates, which reads the same in any gameplay frame, over the
// rows before `upto`, and the latest such tick, so a route that passes the coin twice is taken at
// its last pass. False when no row qualifies.
//
// Measured against where the coin WAS at each tick (solver::g_coinPosLog, this attempt's own track)
// and skipping the ticks it was off, when there is a track; against its load position otherwise.
// SubZero 4002's third coin drops 600 px at x=31,899 and is taken on the way back: measured at its
// load position the "closest approach" was a pass on the tier above (t=23,791, y=1,768), and every
// repair went there, 250 px over the coin. `track` is the coins' track of the attempt `rows`
// came from: solver::g_coinPosLog for the one that just ended, anchors::g_deepestCoinPos for the
// deepest trajectory.
inline bool coinApproach(const std::vector<AnchorRow>& rows,
                         const std::vector<std::vector<solver::CoinPos>>& track, size_t ci,
                         long long upto, long long& t, float& x, double& d) {
    const auto& c = solver::g_coins[ci];
    const std::vector<solver::CoinPos>* lg =
        (ci < track.size() && !track[ci].empty()) ? &track[ci] : nullptr;
    t = -1;
    d = 1e18;
    // cfg coinapproachoff: the closest approach over the ticks the coin's group was switched off
    // too, kept apart from the one over the ticks it was on.
    long long tOff = -1;
    float xOff = 0.f;
    double dOff = 1e18;
    // cfg coinapproachreach: the off ticks again, per PASS within pickup reach (the coin's half
    // plus 15, the largest player half but a mini's), keeping each pass's closest tick.
    const double reach = (c.hw > 0.f ? (double)c.hw : 20.0) + 15.0;
    long long tReach = -1, lastIn = -2;
    float xReach = 0.f;
    double dReach = 1e18;
    size_t k = 0;   // the track row in force at tick i
    for (size_t i = 0; i < rows.size() && (long long)i < upto; ++i) {
        double cx = c.x, cy = c.y;
        bool off = false;
        if (lg) {
            while (k + 1 < lg->size() && (*lg)[k + 1].t <= (long long)i) ++k;
            if ((*lg)[k].t > (long long)i) continue;
            off = !(*lg)[k].on;
            if (off && !g_cfg.coinApproachOff) continue;
            cx = (*lg)[k].x;
            cy = (*lg)[k].y;
        }
        const AnchorRow& r = rows[i];
        if (!r.valid) continue;
        const double e = std::max(std::fabs((double)r.x - cx), std::fabs((double)r.y - cy));
        if (off) {
            if (e <= dOff) { dOff = e; tOff = (long long)i; xOff = r.x; }
            if (g_cfg.coinApproachReach && e <= reach) {
                if ((long long)i > lastIn + 1 || e <= dReach) {   // a new pass, or closer in this one
                    dReach = e; tReach = (long long)i; xReach = r.x;
                }
                lastIn = (long long)i;
            }
        } else if (e <= d) {
            d = e; t = (long long)i; x = r.x;
        }
    }
    // ...and the LATER of the two is the one ranked. A group switched off is not a coin gone: a
    // route can switch it back on (SubZero 4003's third coin is off from t~10,274 on every
    // attempt, and GD credited it at t=13,707 to the route that did), while the ladder only ever
    // backs away from the tick it is given -- a late rank still reaches the early chance, an early
    // one never sees the late. A coin that is off until it appears keeps its on-rank, which is the
    // later one there.
    if (g_cfg.coinApproachOff && tOff > t) { t = tOff; x = xOff; d = dOff; }
    // cfg coinapproachreach: ...and among the off passes the coin could have been taken on had it
    // been on, the LAST, not the closest -- the same late-over-early argument. lv22's third coin
    // (glitch-avoid route, coins on) is off until six presses after a Tap; the route passes it at
    // t=12,055 at 1.9 px, long before the presses can happen, and again at t=14,960 at 23.6 px
    // where the normal route takes it. Ranked at the first pass, every repair of the run went to
    // t=12,055 and none of them could take the coin there.
    if (g_cfg.coinApproachReach && tReach > t) { t = tReach; x = xReach; d = dReach; }
    return t > 0;
}

// cfg routeprereq: coinApproach, except that a coin whose prerequisite (solver/route.hpp) this
// attempt had not switched on by the time it came closest to the coin is measured at the attempt's
// closest approach to the prerequisite's BOX -- the first thing the route has to change is there,
// not at the coin. The distance to a box is from its rect, on the larger axis, the way a coin's is
// from its centre. `onTick` is the attempt's route::g_onTick. `need` is the box's uid when the rank
// moved there, else -1.
inline bool coinApproachRoute(const std::vector<AnchorRow>& rows,
                              const std::vector<std::vector<solver::CoinPos>>& track,
                              const std::vector<long long>& onTick, size_t ci, long long upto,
                              long long& t, float& x, double& d, int& need) {
    need = -1;
    const bool ok = coinApproach(rows, track, ci, upto, t, x, d);
    if (!g_cfg.routePrereq || !route::g_built || !routeEngaged(ci)) return ok;
    const std::vector<size_t> miss = route::unmet((int)ci, onTick, ok ? t : -1);
    if (miss.empty()) return ok;
    long long bt = -1;
    float bx = 0.f;
    double bd = 1e18;
    int bu = -1;
    for (const size_t k : miss) {
        const route::Prereq& p = route::g_prereq[k];
        long long kt = -1;
        float kx = 0.f;
        double kd = 1e18;
        for (size_t i = 0; i < rows.size() && (long long)i < upto; ++i) {
            const AnchorRow& r = rows[i];
            if (!r.valid) continue;
            const double ex = std::max(std::fabs((double)r.x - p.bx) - p.bhw, 0.0);
            const double ey = std::max(std::fabs((double)r.y - p.by) - p.bhh, 0.0);
            const double e = std::max(ex, ey);
            if (e < kd) { kd = e; kt = (long long)i; kx = r.x; }
        }
        // The earliest box the route has not opened is the first thing to change.
        if (kt > 0 && (bt < 0 || kt < bt)) { bt = kt; bx = kx; bd = kd; bu = p.boxUid; }
    }
    if (bt <= 0) return ok;
    t = bt;
    x = bx;
    d = bd;
    need = bu;
    return true;
}

// cfg routeprereqafter: start ranking coin `ci` by its prerequisites. The deepest plan is re-ranked
// the same way at once (as fileCoinMissPost does when it files), so the wall the ladder goes back to
// is the box when the deepest plan never opened it. True when that moved the wall.
inline bool routeEngage(size_t ci, const char* why) {
    if (ci >= 32 || routeEngaged(ci)) return false;
    g_routeEngaged |= 1u << ci;
    writeResult("dpsolve:   [route] coin " + std::to_string(ci) + ": " + why
                + " - its prerequisites rank from here (cfg routeprereqafter)");
    if (g_bestDeath < 0) return false;
    int had = 0;
    for (auto it = anchors::g_deepest.rbegin(); it != anchors::g_deepest.rend(); ++it)
        if (it->valid) { had = it->coins; break; }
    if (ci < 8 && (had & (1 << ci))) return false;
    long long t0;
    float x0 = 0.f;
    double d0;
    int need = -1;
    if (!coinApproachRoute(anchors::g_deepest, anchors::g_deepestCoinPos, route::g_onTickDeepest, ci,
                           g_bestDeath + 1, t0, x0, d0, need)
        || need < 0 || t0 >= g_bestDeath)
        return false;
    char b[224];
    snprintf(b, sizeof(b), "dpsolve:   [route] the deepest plan never opened box uid %d - its wall "
             "moves to the box: best %lld -> %lld", need, g_bestDeath, t0);
    writeResult(b);
    g_bestDeath = t0;
    g_bestDeathX = x0;
    g_routeNeedUid = need;
    return true;
}

// A clear refused for a missing coin, moved back to this attempt's closest approach to the first
// coin it missed. The coin is certainly missed -- the level is over -- but WHERE the attempt lost
// it is a guess: the closest approach to the coin's load position is where a repair is most
// likely to help, not a proof of the last chance (a coin that moves, or is off for a while, or a
// route that passes it twice at the same distance can each put that elsewhere). The coin then
// joins g_postCoins, and from here on every attempt is ranked the same way (scoreByPostCoins).
// Returns false, changing nothing, when the flag is off or no coin is missing.
//
// ...and the incumbent is re-ranked at ITS closest approach when its own trajectory never had the
// coin either. Its depth past the coin is depth on a route that is not the one asked for, and left
// where it was it would win every comparison in onDeath -- measured on 4003 (2026-09-24): the
// clear filed at t=8,457 lost to the deepest plan's t=17,951 and the loop rewound onto it.
inline bool fileCoinMissPost(long long& dt, float& deathX) {
    if (!g_cfg.coinMissPost) return false;
    long long bestT = -1;
    float bestX = 0.f;
    double bestD = 0.0;
    size_t bestC = 0;
    int bestNeed = -1;
    for (size_t i = 0; i < solver::g_coins.size() && i < solver::g_coinGdTick.size(); ++i) {
        if (solver::g_coinGdTick[i] >= 0) continue;
        long long ct;
        float cx = 0.f;
        double cd;
        int need = -1;
        if (coinApproachRoute(anchors::g_live, solver::g_coinPosLog, route::g_onTick, i, dt, ct, cx,
                              cd, need)
            && (bestT < 0 || ct < bestT)) {
            bestT = ct;
            bestX = cx;
            bestD = cd;
            bestC = i;
            bestNeed = need;
        }
    }
    if (bestT <= 0) return false;
    g_routeNeedDeath = bestNeed;
    g_postCoins |= 1 << bestC;
    if (bestC < 8 && (g_postCoinT[bestC] < 0 || bestT < g_postCoinT[bestC]))
        g_postCoinT[bestC] = bestT;
    const long long wasBest = g_bestDeath;
    if (g_bestDeath >= 0) {
        int had = 0;
        for (auto it = anchors::g_deepest.rbegin(); it != anchors::g_deepest.rend(); ++it)
            if (it->valid) { had = it->coins; break; }
        long long it0;
        float ix = 0.f;
        double id;
        int ineed = -1;
        if (!(had & (1 << bestC))
            && coinApproachRoute(anchors::g_deepest, anchors::g_deepestCoinPos, route::g_onTickDeepest,
                                 bestC, g_bestDeath + 1, it0, ix, id, ineed)
            && it0 < g_bestDeath) {
            g_bestDeath = it0;
            // ...and when that rank is a box, the wall the ladder goes back to is the box.
            if (ineed >= 0) g_routeNeedUid = ineed;
        }
    }
    char b[288];
    if (bestNeed >= 0)
        snprintf(b, sizeof(b), "dpsolve:   [route] coinmisspost: coin %zu (uid %d) missed, and the box "
                 "that has to open it first (uid %d) was not - filed at the closest approach to the "
                 "box t=%lld x=%.0f, %.1f px away, not at t=%lld (cfg routeprereq)", bestC,
                 solver::g_coins[bestC].uid, bestNeed, bestT, (double)bestX, bestD, dt);
    else
        snprintf(b, sizeof(b), "dpsolve:   [coin] coinmisspost: coin %zu (uid %d) missed - closest "
                 "approach t=%lld x=%.0f, %.1f px away; filed there, not at t=%lld", bestC,
                 solver::g_coins[bestC].uid, bestT, (double)bestX, bestD, dt);
    writeResult(b);
    if (g_bestDeath != wasBest) {
        snprintf(b, sizeof(b), "dpsolve:   [coin] the deepest plan never had it either - ranked at "
                 "its own closest approach: best %lld -> %lld", wasBest, g_bestDeath);
        writeResult(b);
    }
    dt = bestT;
    deathX = bestX;
    g_coinMissPostPending = true;
    g_postMissCoin = (int)bestC;
    g_postMissCrossT = bestC < solver::g_coinPosLog.size()
                           ? firstFinalCrossing(anchors::g_live, solver::g_coinPosLog[bestC], dt) : -1;
    return true;
}

// ...and every later death of an attempt without such a coin is ranked at that attempt's closest
// approach to it, so a route that skipped the coin cannot outrank one that went for it by dying
// further on (4003, 2026-09-24: after the first filing the loop settled on coin-less deaths at
// t=11,120 and followed those instead). Not a claim that the coin is gone -- in a level that turns
// the attempt might have come back for it -- only the ranking: a route to an all-coins clear has
// to pass that coin, and this one had not. True when the death was moved.
inline bool scoreByPostCoins(long long& dt, float& deathX) {
    if (!g_cfg.coinMissPost || !g_postCoins) return false;
    long long bestT = -1;
    float bestX = 0.f;
    size_t bestC = 0;
    int bestNeed = -1;
    for (size_t i = 0; i < solver::g_coins.size() && i < solver::g_coinGdTick.size(); ++i) {
        if (!(g_postCoins & (1 << i)) || solver::g_coinGdTick[i] >= 0) continue;
        if (i < 8 && dt <= g_postCoinT[i]) continue;   // has not reached the coin yet
        long long ct;
        float cx = 0.f;
        double cd;
        int need = -1;
        if (coinApproachRoute(anchors::g_live, solver::g_coinPosLog, route::g_onTick, i, dt, ct, cx,
                              cd, need)
            && (bestT < 0 || ct < bestT)) {
            bestT = ct;
            bestX = cx;
            bestC = i;
            bestNeed = need;
        }
    }
    if (bestT <= 0 || bestT >= dt) return false;
    g_routeNeedDeath = bestNeed;
    char b[256];
    if (bestNeed >= 0)
        snprintf(b, sizeof(b), "dpsolve:   [route] died at t=%lld without coin %zu, and without the "
                 "box that opens it (uid %d) - ranked at its closest approach to the box t=%lld "
                 "x=%.0f (cfg routeprereq)", dt, bestC, bestNeed, bestT, (double)bestX);
    else
        snprintf(b, sizeof(b), "dpsolve:   [coin] died at t=%lld without coin %zu (a clear has missed "
                 "it) - ranked at its closest approach t=%lld x=%.0f", dt, bestC, bestT,
                 (double)bestX);
    writeResult(b);
    dt = bestT;
    deathX = bestX;
    return true;
}

// cfg coinmissearly: a coin this attempt can no longer get, filed without waiting for a clear. The
// coin needs a box entered first (route::g_prereq) and the attempt went past that box's far edge
// without entering it, so the coin never came on and no later move of this attempt brings it back.
// It joins g_postCoins as a refused clear's missed coin would (fileCoinMissPost), from the tick the
// box was passed, and scoreByPostCoins then ranks this death -- and every later one without the
// coin -- at the closest approach to that box; the deepest plan is re-ranked the same way when it
// never had the coin either. fileCoinMissPost alone waits for a clear, and a route that cannot
// clear never gets one: official lv20 with coins (RC7 and a ladderback arm, 2026-10-01) passed
// coin 0's box at x 7,003 unopened about 440 ticks behind the clearing routes (both speed
// portals before it missed), and that delay made its last wall at x ~28,612 impassable -- 100+
// rounds of deaths ranked where they happened, every section solve EXHAUSTED in the game, and
// the box 15,000 ticks behind the wall. Not on a level that turns the gameplay frame: past in x is
// not past there.
inline void fileCoinMissEarly(long long dt) {
    if (!g_cfg.coinMissEarly || !g_cfg.coinMissPost || !g_cfg.routePrereq || !route::g_built
        || solver::g_hasRotGameplay)
        return;
    for (size_t i = 0; i < solver::g_coins.size() && i < solver::g_coinGdTick.size() && i < 8; ++i) {
        if (solver::g_coinGdTick[i] >= 0 || (g_postCoins & (1 << i))) continue;
        long long passT = -1;
        int boxUid = -1;
        for (const size_t k : route::unmet((int)i, route::g_onTick, -1)) {
            const route::Prereq& p = route::g_prereq[k];
            const float edge = p.bx + p.bhw + 15.f;   // the player's largest half, as the cut uses
            for (size_t r = 0; r < anchors::g_live.size() && (long long)r < dt; ++r) {
                const AnchorRow& row = anchors::g_live[r];
                if (row.valid && row.x > edge) {
                    if (passT < 0 || (long long)r < passT) { passT = (long long)r; boxUid = p.boxUid; }
                    break;
                }
            }
        }
        if (passT < 0) continue;
        g_postCoins |= 1 << i;
        if (g_postCoinT[i] < 0 || passT < g_postCoinT[i]) g_postCoinT[i] = passT;
        char b[224];
        snprintf(b, sizeof(b), "dpsolve:   [route] coinmissearly: coin %zu (uid %d) cannot come on any "
                 "more - its box (uid %d) was passed unopened at t=%lld; ranked as a missed coin from "
                 "here (cfg coinmissearly)", i, solver::g_coins[i].uid, boxUid, passT);
        writeResult(b);
        routeEngage(i, "its box was passed unopened");
        if (g_bestDeath < 0) continue;
        int had = 0;
        for (auto it = anchors::g_deepest.rbegin(); it != anchors::g_deepest.rend(); ++it)
            if (it->valid) { had = it->coins; break; }
        if (had & (1 << i)) continue;
        long long it0;
        float ix = 0.f;
        double id;
        int ineed = -1;
        if (coinApproachRoute(anchors::g_deepest, anchors::g_deepestCoinPos, route::g_onTickDeepest, i,
                              g_bestDeath + 1, it0, ix, id, ineed)
            && it0 < g_bestDeath) {
            snprintf(b, sizeof(b), "dpsolve:   [coin] the deepest plan never had coin %zu either - "
                     "ranked at its own closest approach: best %lld -> %lld", i, g_bestDeath, it0);
            writeResult(b);
            g_bestDeath = it0;
            if (ineed >= 0) g_routeNeedUid = ineed;
        }
    }
}

inline void onDeath(long long dt, float deathX,
                    solver::AttemptEndKind kind = solver::AttemptEndKind::Collision) {
    const auto endPolicy = solver::attemptEndPolicy(kind);
    const bool noCollision = !endPolicy.learnCollision;
    // Consumed first, whichever way this returns, so a coin miss cannot leak into a later death.
    const bool coinMissPost = g_coinMissPostPending;
    g_coinMissPostPending = false;
    const bool coinMiss = g_coinMissPending || coinMissPost;
    g_coinMissPending = false;
    // Whether the plan that just died claimed to reach the end (cfg dpsecsolved). Read before
    // anything below can install another plan.
    const bool flownClaimedGoal = g_planClaimsGoal;
    // Once the solve is over, a death is just a death: the showing of the solution is a plain
    // replay and must not restart the search behind it
    if (!g_cfg.dpSolve || g_dpShowSolution || g_stop) return;
    // A session whose start is still queued (see start()) has no rounds yet.
    if (g_pendingLayer) return;
    // cfg cpflightprobe: the deaths of the probe and its control are kept and compared, never booked.
    // Held still after each: the control starts at the next frame boundary, the solve's plan after it.
    if (cpflight::g_probeFlying) {
        writeResult(cpflight::probeCapture(dt, deathX, "died"));
        g_paused = true;
        return;
    }
    if (cpflight::g_controlFlying) {
        writeResult(cpflight::controlEnd(dt, deathX, "died"));
        g_paused = true;
        return;
    }
    // A death while a job is in flight is never a death of the loop's plan -- the level is held
    // still. The one that can happen is a checkpoint flight's (cfg `dpcheck`), and ckOnDeath
    // decides what that one means.
    if (g_running.load()) {
        ckOnDeath(dt, deathX, noCollision);
        return;
    }
    // The plan that cleared a slice, dying on the level itself: the slice decides whether the
    // run goes back to a copy with objects put back (level_slice.hpp). Asked before the attempt's
    // rows are banked, since where it parted from the copy is read from them.
    if (levelslice::onVerifyDeath(dt, deathX)) return;
    // ...and a wall the copy has held for a while is checked on the level itself, once.
    if (levelslice::maybeCheckWall()) return;
    // cfg dpcheckobs: where the job's plan died, next to where its search's flight had.
    if (g_cfg.dpCheckObs && g_ckObs.compared) {
        g_ckObs.compared = false;
        char ob[200];
        snprintf(ob, sizeof(ob), "dpsolve:   [checkobs] the job's plan died at t=%lld; its flight "
                 "at t=%lld - %s (inputs through t-1 %s)", dt, g_ckObs.deathT,
                 dt == g_ckObs.deathT ? "the same tick" : "a different tick",
                 g_ckObs.same ? "same" : "different");
        writeResult(ob);
    }
    // cfg coinmisspost: an attempt without a coin a clear has already missed is ranked where it
    // passed that coin. ONLY the ranking and the ladder move: the death itself was real, so the
    // kill fixup and the phantom veto still see it where it happened (physDt / physX).
    const long long physDt = dt;
    const float physX = deathX;
    // cfg routeprereq: the box this death was ranked at, if its coin's prerequisite was not met --
    // filed with the refused clear (fileCoinMissPost), or by scoreByPostCoins just below.
    const int filedNeed = g_routeNeedDeath;
    g_routeNeedDeath = -1;
    // cfg routeprereqafter: a round for every filed coin GD has still not credited; at the count,
    // that coin's prerequisites start to rank (from this death's scoring on).
    if (g_cfg.routePrereq && g_cfg.routePrereqAfter > 0 && route::g_built)
        for (size_t i = 0; i < solver::g_coins.size() && i < 8; ++i) {
            if (!(g_postCoins & (1 << i)) || routeEngaged(i)) continue;
            if (i < solver::g_coinGdTick.size() && solver::g_coinGdTick[i] >= 0) continue;
            if (++g_routeRounds[i] < g_cfg.routePrereqAfter) continue;
            bool has = false;
            for (const route::Prereq& p : route::g_prereq) has = has || p.coin == (int)i;
            if (!has) continue;
            routeEngage(i, ("still missing " + std::to_string(g_routeRounds[i])
                            + " rounds after its filing").c_str());
        }
    if (!coinMiss) fileCoinMissEarly(dt);
    const bool postScored = !coinMiss && scoreByPostCoins(dt, deathX);
    const int deathNeed = coinMissPost ? filedNeed : (postScored ? g_routeNeedDeath : -1);
    g_routeNeedDeath = -1;
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
        cpflight::g_live.reset();
        dt = g_lastDeath;
        deathX = g_lastDeathX;
    } else {
        anchors::bank();
        // cfg cpflight: this attempt's checkpoints and books go with its rows (before any fold).
        if (cpflight::on()) {
            cpflight::bank(physDt, physX,
                           grouptrace::g_lastRoll.rows > 0 && grouptrace::g_lastRoll.depth >= 0);
            cpflight::queueProbe();   // cfg cpflightprobe: flown while the solve this death starts runs
        }
        if (g_cfg.dpRotSeed == 3) rotseed::fold(anchors::g_dead);
    }
    harvestGroups(!voidAttempt);   // rollGroupTrace has already committed this run's recording
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
    bool wasWedged = !endPolicy.creditProgress;
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
                    || std::fabs(rk->y - rd->y) > 0.5f
                    || rk->dual != rd->dual
                    || (rd->dual && std::fabs(rk->y2 - rd->y2) > 0.5f))
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
    // Only a guard-forced end is wedge-class; a reset-confirmed p2 failure may credit progress.
    // Long custom levels can also have genuine deaths beyond 30k ticks.
    // The HUD's iteration block is fed by the external driver through hud.txt. There is no
    // driver here, so the loop fills the same fields itself -- otherwise the panel's Solve mode
    // sits on "iter 0 starting" for the whole run
    g_hudIter = g_iter;
    if (deathX > g_hudVerifiedX) g_hudVerifiedX = deathX;
    char b[256];
    snprintf(b, sizeof(b), "dpsolve: iter %d: death t=%lld x=%.1f (best t=%lld)",
             g_iter, dt, (double)deathX, g_bestDeath);
    writeResult(b);
    // THE PLAN GD JUST FLEW, kept before anything can replace it. The rewind
    // below sets `g_plan = g_best`, and the fixup recorder's resim reads the file
    // written AFTER that -- so without this the model side of every fixup a
    // rewound iteration records is a different plan from the GD side, and the
    // record is a difference between two plans rather than between the model and
    // the game. Measured on the lv22 run of 2026-09-02: 35 of 73 iterations
    // rewound, and 80 of the 126 fixups were recorded in them.
    // Taken here rather than inside logFingerprint because that one returns early
    // when fingerprinting is off, and this is not a diagnostic.
    g_flownPlan = g_plan;
    // Timeout, wedge and coin cuts are not proof that these input edges collide.
    if (!noCollision && !wasWedged && !voidAttempt && !coinMiss && !postScored
        && g_candidateContext.config == planConfig()
        && g_candidateEdges == planEdges(g_flownPlan))
        g_failedPlans.remember(g_candidateContext, g_candidateEdges, physDt);
    g_candidateContext.valid = false;
    logFingerprint(dt, deathX);
    // cfg dprejoinwatch: keep the model's trace of the plan that just died -- the
    // newer of the first solve's and the last tail's -- before the next search overwrites it.
    std::error_code ec;
    const std::string pt = g_planPath + ".trace.csv", tt = g_tailPath + ".trace.csv";
    const bool hp = std::filesystem::exists(pt, ec), ht = std::filesystem::exists(tt, ec);
    std::string src;
    if (hp && ht)
        src = std::filesystem::last_write_time(tt, ec) > std::filesystem::last_write_time(pt, ec)
              ? tt : pt;
    else if (hp) src = pt;
    else if (ht) src = tt;
    // cfg dpsecreuse: a splice has no walk of its own; the target it inherited stays.
    const bool keepTarget = src.empty() && g_rjKeepTarget && g_rjAfterTick >= 0;
    g_rjKeepTarget = false;
    if (keepTarget) {
        // the file and the tick it ends in are the ones the rung was fired with
    } else if (!src.empty()
        && std::filesystem::copy_file(src, rejoinTracePath(),
                                      std::filesystem::copy_options::overwrite_existing, ec))
        g_rjAfterTick = dt;
    else
        g_rjAfterTick = -1;
    // ...but not after a death cfg coinmisspost placed at a missed coin (a refused clear, or a
    // later death ranked there). The old path past that point is exactly the route that missed
    // the coin, so a search that rejoins it -- measured on SubZero 4003, one tick past the coin,
    // after taking the coin on another branch -- flies the same coin-less plan again.
    if (coinMissPost || postScored) {
        g_rjAfterTick = -1;
        writeResult("dpsolve:   [rejoin] off for this death - it is ranked at a missed coin, and "
                    "the old path past it is the one that missed it");
    }
    // Which trace it was, and its bytes, so a later call's `rejoinwatch=` signature can be
    // traced back to the plan it came from.
    writeResult("dpsolve:   [rejoin] trace <- "
                + std::string(keepTarget ? "kept (the splice's source plan)"
                              : src.empty() ? "none" : (src == tt ? "tail" : "plan"))
                + " " + (g_rjAfterTick >= 0 ? fileSig(rejoinTracePath()) : std::string("-")));
    if (!keepTarget)
        g_rjOldIsJoin = g_rjTailJoined;   // the plan that just died: was its tail a join?
    if (g_rjOldIsJoin)
        writeResult("dpsolve:   [rejoin] the plan that died was itself a join - the next "
                    "searches do not join it");
    // Another death in the same tick bucket; enough of them are veto credit
    // on their own (the note at g_deathRuns).
    const int sameDeaths = ++g_deathRuns[dt / 4];
    // cfg dpfastveto: the plan the round before flew, flown again and killed on the
    // same tick. The search is deterministic and the model did not move, so waiting for more hits
    // at this site is waiting for the same answer: measured on lv22 (step horizon, 2026-09-19),
    // three sites each repeated one plan 3-4 times, about 23 s of search a round, before the
    // fourth hit dropped the box that got the run past them.
    bool repeatSame = false;
    {
        const std::string fnv = planFnv(g_plan);
        repeatSame = fnv == g_prevFlownFnv && dt == g_prevFlownDeath;
        g_prevFlownFnv = fnv;
        g_prevFlownDeath = dt;
        if (g_cfg.dpFastVetoAll && !repeatSame && !g_flownDeaths.emplace(fnv, dt).second) {
            repeatSame = true;
            writeResult("dpsolve:   [veto] this plan already died on this tick in an earlier round - "
                        "a deterministic repeat, not new evidence");
        } else if (repeatSame) {
            writeResult("dpsolve:   [veto] the same plan died on the same tick as the round before - "
                        "a deterministic repeat, not new evidence");
        }
        g_roundRepeated = repeatSame;
    }
    if (coinMiss) {
        // ...and ask the next plans to be that much further inside the coin. The
        // model believed it collected this one; the game says it was outside, so
        // the difference is the model's own error here and the plan has to leave
        // room for it. Grows per miss, capped, and never shrinks within a run.
        // Not for a miss filed after a clear (coinMissPost): that plan may never have claimed it.
        if (!coinMissPost && g_coinMarginNow < kCoinMarginMax) {
            g_coinMarginNow = std::min(kCoinMarginMax,
                                       g_coinMarginNow + kCoinMarginStep);
            char cb[176];
            snprintf(cb, sizeof(cb), "dpsolve:   [coin] the game refused a coin the plan claimed "
                     "- asking for %.0f px inside it from here on", g_coinMarginNow);
            writeResult(cb);
        }
        writeResult(coinMissPost
                        ? "dpsolve:   [coin] a clear short of a coin, filed at the closest approach"
                          " to it - a death for the ladder, not for the kill fixups or the veto"
                        : "dpsolve:   [coin] ended at a missed coin - a death for the ladder and the "
                          "fixups, but not for the phantom veto");
    }
    else
        checkPhantom(postScored ? physDt : dt, postScored ? (double)physX : (double)deathX,
                     wasWedged || sameDeaths >= 8,
                     sameDeaths >= 8 || repeatSame);
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
    // Rank this last verification before deciding whether its existing backtrack has more heads.
    // In particular, a splice that reached a new wall must not open another sequence over budget.
    const bool outOfIterations = g_iter > g_cfg.dpMaxIters;
    // cfg coinoverdepth: this attempt has every coin the deepest plan had and took, as GD credited
    // it, a coin that plan never had (the one coinmisspost ranked it at). Progress whatever the
    // tick: the deepest plan's rank is a tick on its own route and this death one on another.
    int coinOver = -1;
    if (g_cfg.coinOverDepth && g_cfg.coinMissPost && g_postCoins && g_bestDeath >= 0
        && !wasWedged && !voidAttempt && dt <= g_bestDeath) {
        int had = 0;
        for (auto it = anchors::g_deepest.rbegin(); it != anchors::g_deepest.rend(); ++it)
            if (it->valid) { had = it->coins; break; }
        int got = 0;
        for (size_t i = 0; i < solver::g_coins.size() && i < solver::g_coinGdTick.size() && i < 8; ++i)
            if (solver::g_coinGdTick[i] >= 0) got |= 1 << i;
        const int more = got & ~had & g_postCoins;
        if ((got & had) == had && more)
            for (int i = 0; i < 8; ++i)
                if ((more >> i) & 1) { coinOver = i; break; }
        if (coinOver >= 0) {
            char cb[224];
            snprintf(cb, sizeof(cb), "dpsolve:   [coin] took coin %d (GD t=%lld), which the deepest "
                     "plan (0x%x, ranked t=%lld) never had - this plan (0x%x, died t=%lld) replaces it "
                     "(cfg coinoverdepth)", coinOver, (long long)solver::g_coinGdTick[(size_t)coinOver],
                     had, g_bestDeath, got, dt);
            writeResult(cb);
        }
    }
    const bool deeper = (dt > g_bestDeath || coinOver >= 0) && !wasWedged;
    // cfg dptopstop: the same stop, reached by the search sitting at its largest capacity without
    // getting deeper. Only going deeper takes the ladder back down (the first branch below), so a
    // round that is about to do that is let through.
    if (!outOfIterations && g_cfg.dpTopStop > 0
        && g_capTier == (int)(sizeof(kCapTiers) / sizeof(kCapTiers[0]))
        && !deeper && g_iter - g_topTierIter >= g_cfg.dpTopStop) {
        // HEURISTIC-STALLED, NOT UNSOLVABLE: this is an empirical cut of the search, not a proof
        // that the level has no solution from here, so it says so in its own words -- a verdict
        // or a baseline must not be taken from a run stopped this way.
        writeResult("dpsolve: " + std::to_string(g_iter - g_topTierIter) + " rounds at "
                    + std::to_string(kCapTiers[g_capTier - 1]) + " states without getting deeper"
                    + " (best " + std::to_string(g_bestDeath) + ") - stopping (heuristic)");
        g_stop = true;
        g_hudPhase = "heuristic-stalled: no progress at the largest search";
        giveUp("heuristic-stalled: no deeper at the largest search (dptopstop)",
               "dpsolve_topstop");
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
    if (deeper) {
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
        if (g_horizonNow != horizonDefault()) {
            if (horizonDefault() == g_horizonFull)
                snprintf(b, sizeof(b), "dpsolve:   deeper at t=%lld - back to planning the whole "
                         "level (%d ticks)", dt, g_horizonFull);
            else
                snprintf(b, sizeof(b), "dpsolve:   deeper at t=%lld - back to planning %d ticks "
                         "at a time", dt, horizonDefault());
            writeResult(b);
            g_horizonNow = horizonDefault();
        }
        g_triedWholeLevel = false;   // a new wall gets its own whole-level try (step mode)
        g_bestDeath = dt;
        g_bestDeathX = deathX;
        // cfg routeprereq: the new wall's box, or none (a wall that is the coin's own, or no coin's).
        if (g_cfg.routePrereq && g_routeNeedUid != deathNeed) {
            snprintf(b, sizeof(b), deathNeed >= 0
                         ? "dpsolve:   [route] the wall is the box uid %d - searches that start before "
                           "it have to enter it"
                         : "dpsolve:   [route] the wall no longer waits on a box (was uid %d)",
                     deathNeed >= 0 ? deathNeed : g_routeNeedUid);
            writeResult(b);
        }
        g_routeNeedUid = deathNeed;
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
        cpflight::keepAsDeepest();  // ...and its checkpoints (cfg cpflight)
        anchors::ladderOn(false);
        g_curBackoff = kBackOff;
        g_followSolved = 0;
        g_followPeak = -1;     // a new wall: nothing has been followed on it yet
        g_followForced = 0;    // the route made progress; the normal flow owns it now
        outcome = itermap::KindDeeper;
    } else if (g_lastTailSolved
               && (dt > std::max(g_lastDeath, g_followPeak) || g_followSolved < 4)) {
        // THE GRACE IS PER WALL. Progress along a solved branch keeps the grace whole, but
        // progress means reaching further than the branch has reached on this wall -- not
        // further than the round before. Measured on lv22 (2026-09-17, a probe arm that booked
        // its cancelled rungs as SOLVED): the branch settled into two deaths, t=2,164 and
        // t=3,014, and every rise from the shallow one back to the deep one read as progress
        // and reset the count, so the 4-round grace never ran out -- 26 rounds in the same
        // pair before the run was stopped. Nothing about that shape needs the probe: any two
        // solved tails that alternate would do it. Against the peak, the second visit to
        // t=3,014 is a repeat, the count reaches 4 after four such rounds, and the loop
        // rewinds as it does for any stalled branch.
        //
        // On the 22 blessed cold logs (2026-09-17 run) this rule decides no round differently:
        // follow rounds exist only on lv16 (1), lv20 (6) and lv22 (6), and none of them rises
        // to a death at or below an earlier one on the same wall.
        const long long ref = std::max(g_lastDeath, g_followPeak);
        outcome = itermap::KindFollow;
        if (dt > g_lastDeath && dt <= ref) {
            snprintf(b, sizeof(b), "dpsolve:   t=%lld is above the last death (t=%lld) but not "
                     "above this wall's follow peak (t=%lld) - a repeat, not progress", dt,
                     g_lastDeath, ref);
            writeResult(b);
        }
        g_followSolved = (dt > ref) ? 0 : g_followSolved + 1;
        g_followPeak = std::max(g_followPeak, dt);
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
        if (g_bestDeath >= 0) g_plan = g_best;   // an empty best is still the best (autoFire)
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
        if (g_stallRuns >= kStallToShorten && g_horizonNow == g_horizonFull
            && horizonShort() != g_horizonFull) {
            // Ask for less, and find out sooner whether it is true. Planning the whole level
            // each time is only worth it while the model is right about the whole level; once
            // it is demonstrably wrong somewhere, a shorter plan gets checked in the game
            // sooner and the loop learns from the game instead of from the model.
            g_horizonNow = horizonShort();
            snprintf(b, sizeof(b), "dpsolve:   %d rounds without progress - planning %d ticks "
                     "at a time so the game checks each step", g_stallRuns, horizonShort());
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
        mapTail(*anchors::g_src, g_iter, outcome, g_anchorT, dt);
        anchors::g_src = savedSrcIm;
        itermap::addDeath(g_iter, dt, deathX, dy, outcome, g_anchorT, g_anchorX, g_curBackoff);
    }
    // cfg dpadaptivehorizon: the next plan's length, from how far past its anchor the
    // game let this one get. Measured over 22 levels (2026-09-19): planning the step everywhere
    // halved the search where the model is often wrong (lv16, lv22) and cost a round every 3,000
    // ticks where it is right (4-6 extra rounds on every easy level). This asks the run itself
    // which of the two it is in, here, rather than a table of levels.
    if (g_cfg.dpStepHorizon > 0) {
        const long long ran = dt - std::max<long long>(0, g_anchorT);
        // ...and a wall the run keeps stopping at. On lv21 (2026-09-19) the step plans died 6-98
        // ticks past their anchors at t=11,270 six rounds running: close to the anchor, so the rule
        // above kept planning short, while the way past it was a lane only the whole-level lookahead
        // picked. The second stop in a row at the same wall asks for the whole level.
        const bool sameWall = (outcome != itermap::KindDeeper) && g_lastDeath >= 0
                              && std::llabs(dt - g_lastDeath) <= 8;
        g_wallRepeats = sameWall ? g_wallRepeats + 1 : 0;
        const int want = (ran < g_cfg.dpStepHorizon && g_wallRepeats < 2) ? g_cfg.dpStepHorizon
                                                                            : g_horizonFull;
        // cfg dpcontenthorizon: the second stop at one wall asks for the whole level on purpose.
        g_horizonFullAsked = g_wallRepeats >= 2;
        if (want != g_horizonNow) {
            snprintf(b, sizeof(b), "dpsolve:   the game ended the plan %lld ticks past its anchor "
                     "- planning %d ticks next", ran, want);
            writeResult(b);
            g_horizonNow = want;
        }
    }
    g_lastDeath = dt;
    g_lastDeathNoCollision = noCollision;
    g_lastDeathX = deathX;
    // Only a ranked death differs; every other keeps whatever dt became above (the off-board and
    // wedge credits move it too, and the recorder has always read the moved one).
    g_lastDeathPhys = postScored ? physDt : dt;
    g_lastDeathCoinMiss = coinMiss;
    // cfg dpseccoinrung: the coin wall (see g_coinWall). Cleared by an attempt GD credited that coin
    // in; set by a cut for a coin, at the cut's own tick, with the plan that was cut.
    if (g_cfg.dpSecCoinRung && g_cfg.coinRoute) {
        if (g_coinWallCoin >= 0 && (size_t)g_coinWallCoin < solver::g_coinGdTick.size()
            && solver::g_coinGdTick[(size_t)g_coinWallCoin] >= 0) {
            writeResult("dpsolve:   [secauto] coin " + std::to_string(g_coinWallCoin)
                        + " taken - its wall (t=" + std::to_string(g_coinWall) + ") is down");
            g_coinWall = -1;
            g_coinWallCoin = -1;
            g_coinWallPlan.clear();
        }
        // ...a cut names its coin; a clear refused for one (coinmisspost) names it too, at its
        // closest approach -- the only way a coin the cut spares is ever missed (SubZero 4002's
        // third: two runs stood 38 px from it for 40 rounds and no rung went there).
        const int wallCoin = solver::g_coinMissIdx >= 0 ? solver::g_coinMissIdx
                             : (coinMissPost ? g_postMissCoin : -1);
        // A refused clear's wall is at the first crossing of the coin's final position when there
        // is one (g_postMissCrossT), not at the closest approach it was ranked at.
        const long long wallT = (solver::g_coinMissIdx < 0 && coinMissPost && g_postMissCrossT > 0)
                                    ? g_postMissCrossT : physDt;
        // cfg routeprereq: not when the death was ranked at a box -- the coin cannot be had in a
        // window at the box, and a rung that has to take it there answers nothing.
        if (coinMiss && wallCoin >= 0 && deathNeed < 0) {
            if (g_coinWallCoin != wallCoin || std::llabs(g_coinWall - wallT) > kAutoCreep)
                writeResult("dpsolve:   [secauto] coin wall: coin " + std::to_string(wallCoin)
                            + (solver::g_coinMissIdx >= 0 ? " cut" : " missed (clear refused)")
                            + " at t=" + std::to_string(wallT)
                            + (wallT != physDt ? " (its first crossing; ranked at t="
                                                     + std::to_string(physDt) + ")" : ""));
            g_coinWall = wallT;
            g_coinWallCoin = wallCoin;
            g_coinWallPlan = g_plan;
        } else if (coinMiss && wallCoin >= 0 && deathNeed >= 0 && g_coinWall >= 0) {
            writeResult("dpsolve:   [route] coin wall (coin " + std::to_string(g_coinWallCoin)
                        + " at t=" + std::to_string(g_coinWall) + ") set aside - the wall is the box"
                        " uid " + std::to_string(deathNeed));
            g_coinWall = -1;
            g_coinWallCoin = -1;
            g_coinWallPlan.clear();
        }
    }
    if (outOfIterations) {
        if (autoCanContinue()
            && autoFire("finishing the existing backtrack after the repair budget")) return;
        stopIterationBudget();
        return;
    }
    // cfg dpsecauto: point fixes are not getting the deepest wall across -- either the recorder
    // cannot express what is wrong there (it wrote nothing, death after death), or it can and the
    // loop is still not moving. Either way the wall goes to a section solve instead of another
    // round; the handoff happens at the next frame boundary, with no job in flight.
    if (g_cfg.dpSecAuto) {
        // The round that just ended: the job after the last death (its searches and recorder,
        // added on the worker thread) and the flight that ended here.
        const double roundWork = g_autoRoundWork + kWorkReplayTick * (double)std::max(0LL, dt);
        g_autoRoundWork = 0.0;
        // The round that FOUND the wall is not rent paid at it -- it got there.
        if (!autoSync()) g_autoWallWork += roundWork;
        ++g_autoWallRounds;   // a round that did not move the wall (autoSync restarts the count)
        // Keep this streak across autoSync's small cumulative wall shifts. Point fixes that
        // advance a few ticks each round must not postpone the section search indefinitely.
        g_autoProgress.observe(rungWall(), (!wasWedged && !voidAttempt && !postScored)
                                           ? dt : -1);
        char why[128] = "";
        if (g_cfg.dpSecNoRec > 0 && g_autoNoRec >= g_cfg.dpSecNoRec)
            snprintf(why, sizeof(why), "the recorder wrote nothing for %d deaths", g_autoNoRec);
        else if (g_cfg.dpSecStall > 0 && g_autoWallRounds >= g_cfg.dpSecStall)
            snprintf(why, sizeof(why), "%d rounds", g_autoWallRounds);
        else if (g_cfg.dpSecRent && g_autoProgress.shouldSearch())
            snprintf(why, sizeof(why), "%d consecutive rounds advancing less than %lld ticks",
                     g_autoProgress.rounds, solver::RepairProgress::kMinAdvance);
        else if (g_cfg.dpSecRent && g_autoWallWork >= autoRungEstimate())
            snprintf(why, sizeof(why), "%.1f s-equiv of work spent (a rung is ~%.1f, %d measured)",
                     g_autoWallWork / 1e6, autoRungEstimate() / 1e6, g_autoRungN);
        if (why[0] && autoFire(why)) return;
        // cfg dpsecsolved: the plan reached the end in the model and died at the deepest wall.
        // Its recorder runs as usual -- the records say where the divergence starts, which is
        // the window's entry -- and the job then hands the wall over instead of laddering.
        if (g_cfg.dpSecSolved && flownClaimedGoal && !coinMiss && !g_secReqPending
            && dt >= g_bestDeath - kAutoCreep && !autoTried()) {
            g_autoRecordThenRung = true;
            writeResult("dpsolve:   [secauto] a plan that reached the end died at t="
                        + std::to_string(dt) + " - recording, then a section solve");
        }
    }
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
    if (g_bestDeath < 0) return false;   // an empty best still has a trajectory to record
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
        // cfg groupsretime: this pass's own clock, from the rows the anchor recorder took while it
        // ran (nothing banks them: a no-death pass never reaches onDeath).
        if (!ec && g_cfg.groupsRetime) {
            std::vector<float>& x = (kind == RecBootstrap) ? g_groupsBootX : g_groupsDeepX;
            x.assign(anchors::g_live.size(), std::numeric_limits<float>::quiet_NaN());
            for (size_t t = 0; t < anchors::g_live.size(); ++t)
                if (anchors::g_live[t].valid) x[t] = anchors::g_live[t].x;
        }
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
    // cfg routeprereqafter: a filed coin the loop has not got, whose prerequisite the deepest plan
    // never opened -- the wall goes to that box before anything else is turned on. Rig route2: every
    // anchor past the second key had no state, and the loop gave up with the coin 5,600 px behind
    // its first key.
    bool routeMoved = false;
    if (g_cfg.routePrereq && route::g_built)
        for (size_t i = 0; i < solver::g_coins.size() && i < 8 && !routeMoved; ++i) {
            if (!(g_postCoins & (1 << i)) || routeEngaged(i)) continue;
            if (i < solver::g_coinGdTick.size() && solver::g_coinGdTick[i] >= 0) continue;
            routeMoved = routeEngage(i, "the ladder found no anchor");
        }
    if (routeMoved) {
        // (the wall moved to the box; asked again below)
    } else if (g_cfg.dpWorld && !g_needUnseen) {
        g_needUnseen = true;
        writeResult("dpsolve:   no anchor - trying again with the doors the search has not "
                    "entered");
    } else if (g_cfg.dpStepHorizon <= 0 && g_horizonNow == g_horizonFull) {
        g_horizonNow = kHorizonShort;
        char b[192];
        snprintf(b, sizeof(b), "dpsolve:   no anchor - trying again in %d-tick steps, so the "
                 "game checks each one", kHorizonShort);
        writeResult(b);
    } else if (g_cfg.dpStepHorizon > 0 && g_horizonNow != g_horizonFull && !g_triedWholeLevel) {
        // cfg dpstephorizon: the inverse of the step above. The run plans in steps by
        // default, so when no anchor works the thing to try is the whole-level lookahead, which
        // picks the branch that can still reach the end. Once per wall.
        g_triedWholeLevel = true;
        g_horizonNow = g_horizonFull;
        g_horizonFullAsked = true;   // (cfg dpcontenthorizon: on purpose)
        char b[192];
        snprintf(b, sizeof(b), "dpsolve:   no anchor - trying again planning the whole level "
                 "(%d ticks)", g_horizonFull);
        writeResult(b);
    } else if (g_needTrigSuspect & ~g_needTrigDropped) {
        // Now. The run has stopped getting anywhere, so the pressure those boxes were applying
        // has had its chance and is only emptying frontiers.
        const unsigned long long rel = g_needTrigSuspect & ~g_needTrigDropped;
        g_needTrigDropped |= rel;
        char b[192];
        snprintf(b, sizeof(b), "dpsolve:   no anchor - releasing the doors it cannot reach "
                 "from here (mask 0x%llx)", rel);
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
    } else if (g_cfg.dpSecTierFirst > 0 && g_capTier < (int)(sizeof(kCapTiers) / sizeof(kCapTiers[0]))
               && autoFire("the ladder found no anchor")) {
        // cfg dpsectierfirst: a section solve before more states. autoFire refuses once this
        // wall's windows reach the level's start (or a solve is already queued), and the tier
        // below comes next. A wall takes as many windows as it takes, so they are not counted
        // against the escalation backstop.
        --g_escalations;
        return true;
    } else if (g_cfg.dpCapTiers > 0
               && g_capTier < (int)(sizeof(kCapTiers) / sizeof(kCapTiers[0]))) {
        // (cfg dpcaptiers: off, this is skipped and the restart below runs at the base setting)
        ++g_capTier;
        if (g_capTier == (int)(sizeof(kCapTiers) / sizeof(kCapTiers[0]))) g_topTierIter = g_iter;
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
        if (g_cfg.dpCapTiers <= 0)
            writeResult("dpsolve:   (the capacity tiers were skipped - cfg dpcaptiers=0)");
        writeResult("dpsolve:   no anchor - solving the level again from the start, with what "
                    "this run has learnt");
        g_horizonNow = g_horizonFull;   // a whole-level attempt, not a step
        g_horizonFullAsked = true;      // (cfg dpcontenthorizon: on purpose)
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
// ...or the clear is a slice's (level_slice.hpp): its plan goes on to the level itself, and the
// copy never ends as a level.
inline bool takesOverClear() {
    // cfg cpflight: a clear from a checkpoint is flown again from the head before anything files
    // it, and the probe's is only compared (hooks_playlayer, levelComplete).
    if (g_cfg.dpSolve && !g_dpShowSolution
        && (cpflight::g_from >= 0 || cpflight::g_probeFlying || cpflight::g_controlFlying))
        return true;
    return g_recordAttempt || g_deepActive || wantsShow()
           || (g_cfg.dpSolve && !g_dpShowSolution
               && (levelslice::g_phase == levelslice::Sliced
                   || levelslice::g_phase == levelslice::RefFlight));
}

// Before the showing of a solution: a section search's snapshots have written the players' mode
// flags back without the toggles that draw a mode (g_psnapPlayerWritten), and the showing's reset
// toggles only the flags it changes, so the replay ran with a cube, a spider and a robot drawn at
// once (reported 2026-10-01). Each mode is turned off by GD's own toggle -- its flag raised first,
// so the toggle's off path runs whatever the flag held -- which hides that mode's parts and leaves
// a consistent cube for the reset to start the level's own mode from. Only after such a write:
// a solve without one shows its solution as it always has.
inline void tidyPlayerModes() {
    if (!g_psnapPlayerWritten) return;
    auto* pl = PlayLayer::get();
    if (!pl) return;
    for (PlayerObject* p : {pl->m_player1, pl->m_player2}) {
        if (!p) continue;
        p->m_isShip = true;   p->toggleFlyMode(false, true);
        p->m_isBird = true;   p->toggleBirdMode(false, true);
        p->m_isBall = true;   p->toggleRollMode(false, true);
        p->m_isDart = true;   p->toggleDartMode(false, true);
        p->m_isRobot = true;  p->toggleRobotMode(false, true);
        p->m_isSpider = true; p->toggleSpiderMode(false, true);
        p->m_isSwing = true;  p->toggleSwingMode(false, true);
    }
    writeResult("dpsolve: the players' modes tidied before the showing (a snapshot had written "
                "their flags)");
}

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
        else
            writeResult(std::string("dpsolve: a queued start was dropped (started=")
                        + (g_started ? "1" : "0") + " over=" + (g_sessionOver ? "1" : "0")
                        + " solve=" + (g_cfg.dpSolve ? "1" : "0") + " sameLayer="
                        + (PlayLayer::get() == l ? "1" : "0") + ")");
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
        tidyPlayerModes();
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
    if (g_secRetryPending && (!g_started || g_sessionOver || !g_cfg.dpSolve || g_dpShowSolution))
        g_secRetryPending = false;
    // A failed section has no new plan to verify. Queue its next earlier head directly, while
    // retaining the render hold until the next handoff has reset the search's dirty world.
    // The coroutine has finished and released its checkpoints before poll reaches this block.
    if (g_secRetryPending && !g_running.load() && !g_deepActive) {
        g_secRetryPending = false;
        g_stallResetPending = false;  // this decision performs the clean reset itself
        if (!g_secReqPending
            && !(autoCanContinue()
                 && autoFire("the previous section failed - trying an earlier entry"))) {
            writeResult(g_iter >= g_cfg.dpMaxIters
                            ? "secrung: no earlier window remains and the repair budget is spent"
                            : "secrung: no earlier window remains - returning to ordinary repairs");
            g_paused = false;
            if (auto* pl = PlayLayer::get()) pl->resetLevel();
            secRenderRelease();
            // At the budget there is no reason to fly the same failed deepest plan once more.
            if (g_iter >= g_cfg.dpMaxIters) stopIterationBudget();
            return;
        }
    }
    // Stop the loop from starting anything new, FIRST, and take over once the solver thread in
    // flight has finished. Waiting for an idle thread without this never fires: one frame of a
    // fast loop installs a plan, replays the whole attempt, books the death and spawns the next
    // solve, so g_running is true at every poll() a caller could observe. Measured on a custom
    // level (2026-08-27): the command was accepted and the handoff never came. g_stop closes
    // onDeath's spawn, so the flag goes false and STAYS false.
    if (g_secReqPending && g_cfg.dpSolve && !g_dpShowSolution && !g_stop) {
        g_stop = true;
        writeResult("secsolve handoff: the repair loop stops after the solve in flight");
    }
    if (g_secReqPending && g_cfg.dpSolve && !g_dpShowSolution
        && !g_running.load() && !g_deepActive) {
        g_secReqPending = false;
        if (g_bestDeath >= 0 && g_secReqStart >= g_bestDeath) {
            writeResult("secsolve handoff: refused - start t=" + std::to_string(g_secReqStart)
                        + " is past the verified depth t=" + std::to_string(g_bestDeath));
            return;
        }
        g_stop = true;               // no more spawns; onDeath's guard makes deaths inert
        if (g_bestDeath < 0)
            writeResult("secsolve handoff: no verified plan yet - the prefix run is inputless");
        // cfg dpseccoinrung: a rung fired at the coin wall starts from the plan that was cut there,
        // not from the deepest one.
        const bool coinRung = g_secReqRung && g_secReqCoin >= 0 && !g_coinWallPlan.empty();
        std::vector<InputCmd>& rungSrc = coinRung ? g_coinWallPlan : g_best;
        std::sort(rungSrc.begin(), rungSrc.end(),
                  [](const InputCmd& a, const InputCmd& c) { return a.step < c.step; });
        g_cfg.inputs = rungSrc;
        // Write down the prefix this hands to the search. Without it the section can only ever
        // be run again through the handoff: the served path (py/secsolve_run.py) takes a plan
        // FILE, and the plan here exists only in memory, so "does the same section behave the
        // same outside a solve session" -- the first question to ask when a handoff's search
        // returns something a replay will not reproduce -- could not be asked at all.
        {
            const std::string pp = std::string(DATA_DIR) + "/secsolve_prefix.txt";
            if (writeInputsFile(pp, rungSrc))
                writeResult("secsolve handoff: prefix written to " + pp);
        }
        // A RUNG puts all of this back when the search is done (secsolve's end in
        // hooks_gamelayer), so what it overwrites is written down first.
        g_secRung = g_secReqRung;
        g_secRungAuto = g_secReqAuto;
        g_secReqRung = false;
        g_secReqAuto = false;
        // The wall this rung is being fired at: the deepest death the loop has reached.
        // The pin the resume installs has to clear it (see g_secPinWall).
        g_secPinWall = coinRung ? g_coinWall : g_bestDeath;
        // ...and the coin the search has to take (hooks_gamelayer, cfg seccoins).
        secsolve::g_rungCoin = coinRung ? g_secReqCoin : -1;
        g_secReqCoin = -1;
        // ...and, for a rung of cfg dpsecstate, the size it forces (the probe) or keeps.
        g_secState = g_secRung ? g_secReqState : 0;
        if (g_secState == 1) secsolve::g_forceP1Size = g_secReqSize;
        if (g_secState == 2 || g_secState == 3) secsolve::g_keepP1Size = g_secReqSize;
        // A kept window searches the decision only, and the model plans the way to the wall from
        // its end: its pin goes no further than its own splice (see stateFire).
        if (g_secState == 2) g_secPinWall = -1;
        g_secReqState = 0;
        g_secReqSize = -1;
        g_secSavePractice = g_cfg.practiceAt;
        g_secSaveCkpt = g_cfg.checkpointAt;
        g_secSaveFastloops = g_cfg.fastloops;
        g_secSaveMaxAttempts = g_cfg.maxAttempts;
        g_cfg.practiceAt = (int)std::max(1LL, g_secReqStart - 100);
        g_cfg.checkpointAt = (int)g_secReqStart;
        // The checkpoint is placed at a frame boundary, so boundaries must be tick-fine --
        // the proven cfg of the served path. The search itself steps with secdt directly and
        // does not care what this is.
        g_cfg.fastloops = 1;
        // If the prefix somehow keeps dying before the section head, let the attempt cap end
        // the session instead of replaying forever (the loop is stopped and will not repair).
        // A rung keeps its own budget: the loop it resumes into needs attempts of its own.
        if (!g_secRung) g_cfg.maxAttempts = g_finishedAttempts + 5;
        // ...and a rung is abandoned instead (secRungAbandon, kSecRungPrefixTries).
        g_secRungPrefixFails = 0;
        g_secRungFailAttempt = -1;
        secsolve::g_on = true;
        secsolve::g_done = false;
        secsolve::g_startTick = g_secReqStart;
        secsolve::g_targetX = g_secReqTarget;
        // The depth target is put back when a rung ends (hooks_gamelayer), so the session's own
        // sectargetdepth is what the next command-fired rung sees.
        g_secSaveTargetDepth = secsolve::g_targetDepth;
        if (g_secReqDepth > 0) secsolve::g_targetDepth = (int)g_secReqDepth;
        if (g_secReqHorizon > 0) secsolve::g_horizon = (int)g_secReqHorizon;
        if (g_secReqCap > 0) secsolve::g_cap = (size_t)g_secReqCap;
        // The window as handed over, to ask it again on checkpoints if the snapshot is caught
        // lying (hooks_gamelayer), and how long the spine should live (secsolve::g_spineUntil).
        g_secLastReq = {g_secReqStart, g_secReqDepth, g_secReqHorizon, g_secReqCap, g_secReqTarget,
                        secsolve::g_rungCoin};
        secsolve::g_spineUntil = g_secRung ? g_secReqSpineUntil : -1;
        g_secReqSpineUntil = -1;
        g_secCpRedo = g_secRung && g_secReqForceCp;
        if (g_secReqForceCp) {
            g_secReqForceCp = false;
            secsolve::g_snapMode = 0;      // checkpoints; the next ordinary rung puts psnap back
            secsolve::g_verifyEvery = 0;   // (autoFire, when both are 0)
            writeResult("secrung: this window again on checkpoints - the player snapshot parted "
                        "from the game in it");
        }
        secsolve::g_log = true;      // without seclayer: lines, a capped frontier and a real
                                     // wall cannot be told apart
        char hb[256];
        snprintf(hb, sizeof(hb),
                 "%s handoff: loop %s after iter %d - replaying %zu inputs to "
                 "t=%lld, then solving to x=%.1f depth=%d (horizon=%d cap=%zu)",
                 g_secRung ? "secrung" : "secsolve",
                 g_secRung ? "suspended" : "stopped",
                 g_iter, g_cfg.inputs.size(), (long long)g_secReqStart,
                 secsolve::g_targetX, secsolve::g_targetDepth, secsolve::g_horizon,
                 secsolve::g_cap);
        writeResult(hb);
        g_hudPhase = "secsolve: replaying to the section head";
        g_paused = false;
        secRenderHold();   // the screen stays off until the rung ends (session_hotkeys.hpp)
        if (auto* pl = PlayLayer::get()) pl->resetLevel();
        return;
    }
    // While a job is out, the level does not have to sit still: fly the search's checkpoints
    // (ckConsider above). Below this line the job has finished.
    ckConsider();
    // cfg cpflightprobe: the death that started this job, flown again from its own checkpoint while
    // the job runs (the layer starts it when poll returns). No job, no probe.
    if (cpflight::g_probeQueued.rec) {
        if (g_running.load() && !g_finished.load() && !g_cfg.dpCheck && !g_stop && !g_sessionOver
            && g_started && !g_deepActive && !g_dpShowSolution && !cpflight::g_probeFlying) {
            cpflight::g_pending = std::move(cpflight::g_probeQueued);
            g_paused = false;
        }
        cpflight::g_probeQueued = cpflight::Choice{};
    }
    if (!g_running.load() || !g_finished.load()) return;
    // ...and the plan is not installed under a probe still in the air: it ends at the head flight's
    // death or kProbeOverrun past it (hooks_gamelayer).
    if (cpflight::g_probeFlying || cpflight::g_pending.probe || cpflight::g_controlPending
        || cpflight::g_controlFlying)
        return;
    g_running = false;
    g_finished = false;
    // Whatever the job concluded, no flight outlives it. If one had replaced the installed plan in
    // g_cfg.inputs, every path below either installs a plan of its own or wants the loop's own
    // plan back -- a checkpoint's lineage must not be left behind as "the plan".
    const bool hadFlight = g_ckInstalled;
    g_ckFlying = false;
    g_ckInstalled = false;
    if (g_cfg.dpCheck) {
        char cb[160];
        snprintf(cb, sizeof(cb), "dpsolve:   [check] this job: %zu flight(s), %zu passed, "
                 "%zu refuted", g_ckFlights, g_ckPassed, g_ckDeaths);
        writeResult(cb);
        // cfg dpcheckfirst: the first solve was the one to watch; the rest run unwatched. dp reads
        // the subscription once per call, and no job is out now.
        if (g_cfg.dpCheckFirst) {
            g_cfg.dpCheck = false;
            dpbridge::checkSubscribe(false);
            writeResult("dpsolve:   [checkfirst] the first solve is over - no more checkpoint "
                        "flights this run");
        }
    }
    // A flight cleared the level and the completion path has taken it (ckClearedDuringJob); what
    // the abandoned search returned is not an answer to anything any more.
    if (g_ckAbandonJob) {
        g_ckAbandonJob = false;
        writeResult("dpsolve:   [check] the job was abandoned for a checkpoint clear - its result "
                    "is discarded");
        return;
    }
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
             sec, g_plan.size(), rcs,
             !g_haveNewPlan ? " (NO PLAN)"
                            : g_plan.empty() ? " (a plan that never presses - flying it)" : "");
    writeResult(b);
    g_dpSolving = false;
    // Only a missing plan stops here. An EMPTY one is an answer ("never press"), and the game
    // is the one to say whether it is right -- see the first solve's acceptance in spawn().
    if (!g_haveNewPlan) {
        if (hadFlight) g_cfg.inputs = g_plan;   // never leave a flight installed as the plan
        g_paused = false;      // never leave the game frozen because the solve failed
        // A repeated, game-refuted answer needs another strategy, not another identical flight.
        if (g_repeatRejected && g_cfg.dpSecAuto && !g_autoPinOut && !g_autoRecordThenRung
            && autoFire("the ladder exhausted after rejecting game-refuted repeats")) return;
        // cfg dpsecauto: the ladder stopped at a pin that held (runLadder) -- the section solve
        // is the next rung. If none can be queued, the pin goes as it always did.
        if (g_autoPinOut) {
            g_autoPinOut = false;
            if (autoFire("every rung past the pin is spent")) return;
            writeResult("dpsolve:   no section solve for this wall - dropping the pin at t="
                        + std::to_string(g_secPin));
            g_secPin = -1;
        }
        // cfg dpsecsolved: the job recorded the death and left the wall to a section solve.
        if (g_autoRecordThenRung) {
            g_autoRecordThenRung = false;
            if (autoFire("a plan that reached the end died")) return;
            if (g_secReqPending) return;   // a command's section solve is queued and takes over
        }
        // Before conceding: everything that changes how the search is set up gets a turn.
        if (g_iter > 0 && escalate()) return;
        // ...and so does the section solve, while this wall has a window left: it searches the game,
        // not the model, so "no anchor can be solved" is not its verdict. Measured on official level
        // 22 with coins on (solved on its slice): the run ended here after the one window 200 ticks
        // before its wall (t=2,938) had found nothing, on the snapshot and again on checkpoints --
        // while the same stretch on checkpoints from t=2,212 crossed the wall.
        if (g_iter > 0 && g_cfg.dpSecAuto && autoFire("no anchor on this prefix can be solved"))
            return;
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
    if (g_candidateContext.valid) g_candidateEdges = planEdges(g_plan);
    if (g_cfg.dpCheck && g_cfg.dpCheckObs) ckObsCompare(sec);
    g_paused = false;
    // The screen stays off here. These replays are the loop TESTING a candidate, not showing a
    // result -- most of them die -- and each one drawn at 1x costs the length of the song. The
    // run comes back to the screen once, when a candidate has actually cleared (showSolution).
    g_hudPhase = g_iter == 0 ? "verifying the first plan in the game"
                             : "verifying the repaired plan in the game";
    g_hudAnchorT = g_anchorT;
    g_hudAnchorX = g_anchorX;
    // cfg cpflight: from a checkpoint of an earlier attempt whose inputs agree up to it, if one
    // does (the layer restores it when poll returns); from the head otherwise.
    if (auto* pl = PlayLayer::get())
        if (!cpflight::requestStart(g_cfg.inputs)) pl->resetLevel();
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
