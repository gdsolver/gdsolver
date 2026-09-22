#pragma once
#include "dp/stairs.hpp"

namespace dp {

// --groups <file>: the MOD's `grouptrace=1` output, `tick,uid,cx,cy,w,h`.
// Keyed by uid because that is the only identifier both dumps share.
// --groups can be given more than once, and a later file OVERRIDES an earlier
// one for as far as it reaches. That is not a convenience -- it is the only way
// the timeline can be right.
//
// The cheap way to record it is one bootstrap pass with an empty plan and
// `nodeath=1`, which reaches the level end and fires everything that fires off
// the screen. But not every trigger does: lv19's gate at x=27,705 is opened by
// the player getting there, and in the bootstrap pass (where the player is dead
// and falling) it never opens at all. Measured on the two recordings of the
// same level: the bootstrap has ONE sample for those two blocks, the real
// replay has 99 each, sliding the 60 px gap open from t=19,606 to t=19,704 --
// exactly when the player arrives.
//
// So the driver records again on every GD replay of its own plan, and that
// recording wins wherever it reaches (its last tick). Beyond that the bootstrap
// fills in, and beyond THAT every object holds its last known rect.
using GroupTimeline = std::unordered_map<int, std::vector<DynSample>>;

// The uid the level loader gives an object's hazard twin (DynSample::env; the base is in
// object.hpp, where the kill counter reads it).
inline int envTwinUid(int uid) { return kEnvTwinUidBase + uid; }
inline GroupTimeline loadGroupTimeline(const std::string& path) {
    GroupTimeline g;
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "groups: cannot open %s\n", path.c_str());
        return g;
    }
    std::string line;
    std::getline(in, line);   // header
    long long rows = 0;
    while (std::getline(in, line)) {
        int t = 0, uid = 0, on = 1, env = 0;
        float cx = 0, cy = 0, w = 0, h = 0, rot = 0;
        // a run that is cut off mid-write leaves one short line; skip it.
        // `on` and `rot` are optional so a trace recorded before those columns
        // existed still loads (as "always there, never turned", which is what it
        // used to mean). `env` is written only on the rows that have it (the box
        // GD's random numbers can put the object in, DynSample::env), always
        // after a `rot`.
        const int n = std::sscanf(line.c_str(), "%d,%d,%f,%f,%f,%f,%d,%f,%d", &t,
                                  &uid, &cx, &cy, &w, &h, &on, &rot, &env);
        if (n < 6) continue;
        g[uid].push_back({t, cx, cy, w * 0.5f, h * 0.5f,
                          (uint8_t)(on ? 1 : 0), rot, (uint8_t)(env ? 1 : 0)});
        ++rows;
    }
    for (auto& kv : g)
        std::sort(kv.second.begin(), kv.second.end(),
                  [](const DynSample& a, const DynSample& b) { return a.t < b.t; });
    std::printf("groups: %lld samples for %zu objects (%s)\n", rows, g.size(),
                path.c_str());
    return g;
}

// --groupholdend <n>: the bootstrap takes over only n ticks AFTER the overriding
// recording ends, so its last row holds for those ticks. 0 (the default) is the
// old behaviour. Why n > 0 is needed: the live recording is the GD replay of the
// plan that just DIED, and it stops on the tick of the death -- while the kill is
// decided on the model's NEXT row. That row therefore always comes from the
// bootstrap, where the killer can be somewhere else entirely or switched off.
// Measured on lv20 (census v0.1.4, iterations 6-9, one plan sent four times):
// moving spike uid13068 is at x 24,350.46 with on=1 in the live recording's last
// row (t=16,252); the bootstrap has it at x 26,376 with on=0 on t=16,253, so the
// model's hazard never saw it and GD killed the same plan four times.
inline int g_groupHoldEnd = 0;

// Bootstrap re-timing: an object that is still MOVING where the live recording ends
// continues on the bootstrap's rows, and those are on the bootstrap run's
// timeline, not this one's. The bootstrap (empty plan, nodeath) reaches each
// trigger's x a little later than a real run, and the lag grows along the level.
// Measured on lv21 (2026-09-22, cold it 2, live recording ending at t=11,033):
// aligning the two recordings object by object gives a shift of 1-3 ticks up to
// t=3,000, 5-6 over 3,000-8,000 and 8 over 10,000-11,033 (35 of 35 moving
// objects, residual < 0.05 px). For the rotating bar at x 14,355 (group 97, a
// rotate with no move, which rotsplit leaves on its recording) the seam puts the
// bar 8 ticks back on t=11,034: the model flies through a gap that GD has
// already closed, and GD kills the same plan four times (uid 14623 at t=11,033).
// Shifting the bootstrap's rows by those 8 ticks makes the same call end at
// t=11,033 as GD does.
//
// So: for an object that moves in the last kBootAlignWin ticks of the live
// recording, find the shift at which the bootstrap's rows reproduce the live
// ones there, and join the bootstrap on at that shift. An object that is at rest
// at the seam is left as it was -- its next motion starts on a trigger, and the
// trigger's anchor already re-times that (AutoTrig).
//
// On by default since the 22-level colds with it (2026-09-22): coin off changed
// lv21 alone (12 -> 8 iterations, the other 21 levels' [fp] identical), and coin
// on likewise (lv21 14 -> 5, every level 3/3). Always on since the 0.2.0 flag
// clean-up (it was --bootretime).
constexpr int kBootAlignWin = 60;      // ticks before the seam that are compared
constexpr int kBootAlignMaxShift = 240;
constexpr double kBootAlignMinTravel = 3.0;   // px inside the window
constexpr double kBootAlignTol = 0.05;        // px, mean |dcx| + |dcy|

// The shift s (bootstrap tick = live tick + s) at which `boot` reproduces the
// last kBootAlignWin ticks of `live`, or 0 when there is none or it is not clear.
// For the report only: *moving says the object was scored at all, *bestErr is
// the best mean error found and *nFit how many shifts came under the tolerance
// (the nearest of them is the one taken; more than one would be a periodic
// motion matching itself, which is not yet a reason to refuse -- AUD-26).
inline int bootAlignShift(const std::vector<DynSample>& live,
                          const std::vector<DynSample>& boot, int cover,
                          bool* moving = nullptr, double* bestErrOut = nullptr,
                          int* nFit = nullptr) {
    std::vector<const DynSample*> win;
    for (const DynSample& s : live)
        if (s.t > cover - kBootAlignWin && s.t <= cover) win.push_back(&s);
    if (win.size() < 10 || boot.empty()) return 0;
    double travel = 0.0;
    for (const DynSample* s : win)
        travel = std::max(travel, std::fabs((double)s->cx - win.front()->cx)
                                      + std::fabs((double)s->cy - win.front()->cy));
    if (travel < kBootAlignMinTravel) return 0;
    if (moving) *moving = true;
    auto at = [&](int t) -> const DynSample* {
        const auto it = std::lower_bound(
            boot.begin(), boot.end(), t,
            [](const DynSample& a, int v) { return a.t < v; });
        return (it != boot.end() && it->t == t) ? &*it : nullptr;
    };
    int best = 0;
    double bestErr = 1e18;
    // Nearest shift first, so a periodic motion (a two-turn rotate passes every
    // point twice) keeps the smallest shift that fits.
    for (int k = 0; k <= 2 * kBootAlignMaxShift; ++k) {
        const int s = (k & 1) ? (k + 1) / 2 : -(k / 2);
        double err = 0.0;
        size_t n = 0;
        for (const DynSample* w : win) {
            const DynSample* b = at(w->t + s);
            if (!b) continue;
            err += std::fabs((double)b->cx - w->cx) + std::fabs((double)b->cy - w->cy);
            ++n;
        }
        if (n * 5 < win.size() * 4) continue;    // at least 80% of the window
        err /= (double)n;
        if (err < kBootAlignTol && nFit) ++*nFit;
        if (err < bestErr - 1e-9) { bestErr = err; best = s; }
    }
    if (bestErrOut) *bestErrOut = bestErr;
    return bestErr < kBootAlignTol ? best : 0;
}

// --groupholddeath: the LAST --groups file ended on a death, so its last row
// holds for one tick before the recordings under it take over. The producer
// says so, not dp: the mod appends the flag only when it passes the live
// recording, which it writes only after a replay died (harvestGroups), and that
// file is always the last overlay. AUD-20260919-05 asked for the hold to be tied
// to a death-terminated recording rather than applied to every overlay, which is
// what --groupholdend does.
//
// Why a tick: the live recording stops on the tick GD killed the player, and dp
// decides that kill on its NEXT row, which otherwise comes from the recording
// underneath. Measured on lv20 (2026-09-22, cold it 12, anchor t=16,771): spike
// uid13068 is at x 24,356.64 with on=1 on the live recording's last row
// (t=16,777) and at x 27,399.96 with on=0 in the bootstrap, so the model plans
// through, and GD kills that plan at t=16,777. Holding the last row one tick,
// the same call plans a different branch, and that branch runs past 16,777 in
// GD (to 17,075, the next place on the list). An object the bootstrap re-timing
// has joined on is not held: its next row is the bootstrap's own continuation.
inline bool g_groupHoldDeath = false;

// `over` wins for every tick it covers; `base` supplies the rest. `deathEnd`
// says `over` ended on a death (--groupholddeath).
inline void overlayGroupTimeline(GroupTimeline& base, const GroupTimeline& over,
                                 bool deathEnd = false) {
    // How far the overriding recording actually reaches. One number for the
    // whole file, not per object: an object that simply did not move inside the
    // covered window has one sample and must NOT be treated as uncovered.
    int cover = -1;
    for (const auto& kv : over)
        if (!kv.second.empty()) cover = std::max(cover, kv.second.back().t);
    if (cover < 0) return;
    int nRetimed = 0, sLo = 0, sHi = 0;
    int nMoving = 0, nNoFit = 0, nMulti = 0, mostFits = 0, nHeld = 0;
    double worstJoin = 0.0;
    for (const auto& kv : over) {
        std::vector<DynSample> merged = kv.second;
        auto it = base.find(kv.first);
        if (it != base.end()) {
            bool moving = false;
            double bestErr = 0.0;
            int nFit = 0;
            const int s = bootAlignShift(kv.second, it->second, cover, &moving, &bestErr,
                                         &nFit);
            if (moving) {
                ++nMoving;
                if (nFit == 0) ++nNoFit;
                else worstJoin = std::max(worstJoin, bestErr);
                if (nFit > 1) ++nMulti;
                mostFits = std::max(mostFits, nFit);
            }
            if (s != 0) {
                sLo = nRetimed ? std::min(sLo, s) : s;
                sHi = nRetimed ? std::max(sHi, s) : s;
                ++nRetimed;
            }
            const int hold = (deathEnd && s == 0) ? std::max(1, g_groupHoldEnd)
                                                  : g_groupHoldEnd;
            if (hold > g_groupHoldEnd) ++nHeld;
            for (const DynSample& b : it->second) {
                DynSample c = b;
                c.t -= s;
                if (c.t > cover + hold) merged.push_back(c);
            }
        }
        std::sort(merged.begin(), merged.end(),
                  [](const DynSample& a, const DynSample& b) { return a.t < b.t; });
        base[kv.first] = std::move(merged);
    }
    std::printf("groups: overlay covers t<=%d\n", cover);
    std::printf("groups: the bootstrap joined %d moving object(s) at a shift "
                "of %d..%d ticks; %d moving at the seam, %d with no fitting "
                "shift, worst fitting error %.4f px, %d with more than one "
                "fitting shift (at most %d)\n", nRetimed, sLo, sHi, nMoving,
                nNoFit, worstJoin, nMulti, mostFits);
    if (deathEnd)
        std::printf("groups: --groupholddeath: the last recording ended on a death at "
                    "t=%d; %d object(s) hold its last row one tick\n", cover, nHeld);
}

}  // namespace dp
