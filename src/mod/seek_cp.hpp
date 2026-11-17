#pragma once
// ============================================================
// Checkpoints for the seek bar (cfg `seekcp=S`, S ticks apart; 0 = off; S = 600 by default since
// 2026-10).
//
// A seek backwards restarts the level and fast-forwards to the target, so its cost grows with how
// far into the level the target is. On a long, heavy level that is seconds per seek even with the
// physics as cheap as it gets. With the flag on, a replay takes a checkpoint every S ticks as it
// plays -- watched at 1x or flown through by a seek -- and a seek starts from the latest one at or
// before its target, or jumps ahead to one past the player when the run has been there before.
//
// The checkpoint is the loop's strict one (cp_flight.hpp: GD's checkpoint plus the player's bytes
// and containers, the hold, the seeds, the camera, the area state), taken and restored by the same
// code the loop's flights and the human takeover use (hooks_gamelayer: cpCaptureState / cpMark /
// cpRestoreState). Only in a plain replay: not a solve, a served worker, a section run or a takeover.
//
// What says it is right: the x of every tick a head run (one from tick 0) passed is kept, and a run
// restored from a checkpoint is compared with it tick by tick. The first tick that differs drops
// every checkpoint and stops taking them for the session (`seekcp: parted` line); a seek from then
// on restarts the level as before.
// ============================================================
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace p1::seekcp {

inline long long& g_every = itermap::g_seekCpEvery;   // cfg seekcp (read by session.hpp, before this file)
constexpr size_t kMax = 64;                   // more than this: every other one is dropped, S doubles
inline long long g_step = 0;                  // the spacing in force (g_every, doubled by thinning)

inline std::map<long long, std::shared_ptr<cpflight::Rec>> g_cps;   // by tick
// cfg groupcheck (print only): each object's (disabled, in-section) at the take, m_objects order, bit 0 /
// bit 1 -- what hooks_gamelayer's groupToggleDiff compares a restore with
inline std::map<const cpflight::Rec*, std::vector<uint8_t>> g_toggles;
inline const void* g_layer = nullptr;         // the layer and plan they belong to
inline size_t g_objects = 0;
inline std::vector<InputCmd> g_plan;
inline std::vector<float> g_headX, g_headY;   // the player at the end of each tick of a head run (NaN: not seen)
inline long long g_from = -1;                 // this run was restored at this tick (-1: from the head)
inline bool g_restoring = false;              // the restore's resetLevel is running
inline bool g_off = false;                    // a restored run parted from the head run: off for good
inline bool g_partSaid = false;               // cfg seekcpkeep: this restored run's first difference is printed
// cfg headcheck (print only): later head runs are compared with the first instead of replacing it,
// and each attempt's inheritance (cpflight::carrySig, taken before its reset) is printed
inline long long g_headRun = 0, g_headChecked = 0;
inline cpflight::CarrySig g_lineSig;            // what the first head run inherited
// counts for the session's line
inline long long g_taken = 0, g_restores = 0, g_thinned = 0, g_checked = 0;
inline double g_takeMs = 0.0, g_restoreMs = 0.0;
inline long long g_memBefore = -1, g_memAfter = -1;   // private bytes around the takes, MB

inline void clear() {
    g_cps.clear();
    g_toggles.clear();
    g_layer = nullptr;
    g_objects = 0;
    g_plan.clear();
    g_headX.clear();
    g_headY.clear();
    g_from = -1;
    g_step = g_every;
}

inline void resetSession() {
    clear();
    g_off = false;
    g_restoring = false;
    g_taken = g_restores = g_thinned = g_checked = 0;
    g_headRun = g_headChecked = 0;
    g_lineSig = cpflight::CarrySig{};
    g_takeMs = g_restoreMs = 0.0;
    g_memBefore = g_memAfter = -1;
}

inline bool samePlan(const std::vector<InputCmd>& a, const std::vector<InputCmd>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].step != b[i].step || a[i].down != b[i].down || a[i].p2 != b[i].p2) return false;
    return true;
}

// The checkpoints are for this layer, its object set and this plan; anything else drops them.
inline void validate(const void* layer, size_t objects) {
    if (g_layer == layer && g_objects == objects && samePlan(g_plan, g_cfg.inputs)) return;
    if (!g_cps.empty() || !g_headX.empty())
        writeResult("seekcp: the layer or the plan changed - " + std::to_string(g_cps.size())
                    + " checkpoint(s) dropped");
    clear();
    g_layer = layer;
    g_objects = objects;
    g_plan = g_cfg.inputs;
}

// A checkpoint is due at T: none held yet in T's slot.
inline bool due(long long t) {
    if (g_step <= 0 || t < 1) return false;
    if (t < itermap::g_seekCpFrom || (itermap::g_seekCpTo >= 0 && t > itermap::g_seekCpTo)) return false;
    const long long slot = t / g_step;
    auto it = g_cps.lower_bound(slot * g_step);
    return it == g_cps.end() || it->first >= (slot + 1) * g_step;
}

inline void add(std::shared_ptr<cpflight::Rec> r) {
    g_cps[r->tick] = std::move(r);
    if (g_cps.size() <= kMax) return;
    // Twice the spacing, and the first checkpoint of each new slot kept.
    g_step *= 2;
    long long lastSlot = -1;
    for (auto it = g_cps.begin(); it != g_cps.end();) {
        const long long s = it->first / g_step;
        if (s == lastSlot) { ++g_thinned; it = g_cps.erase(it); }
        else { lastSlot = s; ++it; }
    }
}

// The latest checkpoint at or before `target`.
inline std::shared_ptr<cpflight::Rec> atOrBefore(long long target) {
    auto it = g_cps.upper_bound(target);
    if (it == g_cps.begin()) return nullptr;
    return std::prev(it)->second;
}

// A checkpoint worth jumping ahead to: past `now` by more than a spacing, at or before `target`.
inline bool aheadOf(long long now, long long target) {
    if (g_off || g_cps.empty()) return false;
    auto r = atOrBefore(target);
    return r && r->tick > now + std::max<long long>(g_step, 240);
}

// The end of tick t: kept from a head run, compared on a restored one (x and y, bit for bit).
inline void noteX(long long t, float x, float y) {
    if ((g_every <= 0 && !itermap::g_headCheck) || g_off || t < 0 || t > 400000) return;
    const size_t i = (size_t)t;
    auto keep = [&] {
        if (i >= g_headX.size()) {
            g_headX.resize(i + 1, std::numeric_limits<float>::quiet_NaN());
            g_headY.resize(i + 1, std::numeric_limits<float>::quiet_NaN());
        }
        g_headX[i] = x;
        g_headY[i] = y;
    };
    if (g_from < 0) {
        // cfg headcheck: a later head run against the first, which stays the reference
        if (itermap::g_headCheck && g_headRun >= 2 && i < g_headX.size() && !std::isnan(g_headX[i])) {
            ++g_headChecked;
            if ((g_headX[i] != x || g_headY[i] != y) && !g_partSaid) {
                g_partSaid = true;
                char b[240];
                snprintf(b, sizeof(b), "headcheck: head run %lld is at (%.4f, %.4f) at t=%lld where head run 1 was at "
                         "(%.4f, %.4f)", g_headRun, (double)x, (double)y, t, (double)g_headX[i], (double)g_headY[i]);
                writeResult(b);
            }
            return;
        }
        keep();
        return;
    }
    if (t <= g_from) return;
    // Past what a head run has seen: this run has matched up to here, so it extends the record.
    if (i >= g_headX.size() || std::isnan(g_headX[i])) { keep(); return; }
    ++g_checked;
    if (g_headX[i] == x && g_headY[i] == y) return;
    if (itermap::g_seekCpKeep && g_partSaid) return;
    char b[280];
    snprintf(b, sizeof(b), "seekcp: parted - the run restored at t=%lld is at (%.4f, %.4f) at t=%lld where "
             "the head run was at (%.4f, %.4f); %zu checkpoint(s) dropped, seeks restart the level from now on",
             g_from, (double)x, (double)y, t, (double)g_headX[i], (double)g_headY[i], g_cps.size());
    writeResult(b);
    if (itermap::g_seekCpKeep) { g_partSaid = true; return; }   // print only: keep going
    g_cps.clear();
    g_off = true;
    // What is on screen is now a run the head run never was. Take the viewer back to the same
    // place the way a seek did before checkpoints: from the head of the level.
    const long long target = itermap::g_seekTick >= 0 ? itermap::g_seekTick : t;
    itermap::seekToTick(target);
    itermap::g_seekRestart = true;
}

// An attempt begins (hooks_playlayer, before PlayLayer::resetLevel): a head run, or a seek's restore.
inline void onAttemptBegin(GJBaseGameLayer* l) {
    g_from = -1;
    g_partSaid = false;
    if (!g_restoring) ++g_headRun;
    if (!itermap::g_headCheck || !l) return;
    const cpflight::CarrySig s = cpflight::carrySig(l);
    if (!g_restoring && g_headRun == 1) g_lineSig = s;
    const int d = cpflight::sigDiff(g_lineSig, s);
    char b[320];
    snprintf(b, sizeof(b), "headcheck: %s %lld inherits band=%08x,%08x latch=%016llx area=%016llx players=%016llx "
             "nodes=%zu/%016llx - parts that differ from head run 1's: %d (1 band, 2 latch, 4 area, 8 players, 16 nodes)",
             g_restoring ? "restore after head run" : "head run", g_headRun, s.band.centre, s.band.height,
             (unsigned long long)s.latch, (unsigned long long)s.area, (unsigned long long)s.player, s.nodeSet.size(),
             (unsigned long long)s.nodes, d);
    writeResult(b);
    if (d & 16) writeResult("headcheck: nodes " + cpflight::nodesDiff(g_lineSig, s));
}

inline std::string summary() {
    char b[320];
    snprintf(b, sizeof(b), "seekcp: every %lld ticks (now %lld) | taken %lld (%.2f ms each), held %zu, "
             "thinned %lld | restores %lld (%.1f ms each) | ticks checked after a restore %lld%s | "
             "private MB %lld -> %lld", g_every, g_step, g_taken, g_taken ? g_takeMs / g_taken : 0.0,
             g_cps.size(), g_thinned, g_restores, g_restores ? g_restoreMs / g_restores : 0.0, g_checked,
             g_off ? " | PARTED (off)" : "", g_memBefore, g_memAfter);
    return b;
}

}  // namespace p1::seekcp
