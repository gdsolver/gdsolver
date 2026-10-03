#pragma once
#include "dp/stairs.hpp"
#include "dp/input_files.hpp"

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
// `endOut`, when the file carries the trailer the recorder writes (`end,<tick>`),
// comes back as the tick the RUN reached. That is what the overlay below has to
// use as the newer recording's reach: its last ROW stops as soon as nothing
// moves, and the ticks between that and the end of the run are covered by the
// run just as much (SubZero 4002, measured 2026-09-23). -1 = an older
// recording without the trailer, where the last row is all there is.
// The recordings' t=0 on/off (their `init,` lines), by uid, for the objects an activator
// switches (--activators; the loader reads it). Every recording of a level carries the same
// snapshot -- it is taken right after the game's reset, before anything the run did -- so the
// first one that has it is enough. Empty when none does.
inline std::unordered_map<int, uint8_t> g_groupInit;

// sscanf's %d and %f, one field at a time, on a recording held in memory. The reader was
// getline + sscanf("%d,%d,%f,%f,%f,%f,%d,%f,%d"), and on a custom level's 1.5M-row recording that
// was over two seconds of every call that found a recording changed (MOAI, measured 2026-09-30).
// These read the same text the same way: blanks before a number are skipped, the comma after it
// must follow directly, and the first field that does not convert ends the line with the count so
// far -- so a line cut off mid-write still loads or is skipped exactly as before.
namespace groupscan {

inline bool blank(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}
inline bool digit(char c) { return c >= '0' && c <= '9'; }

inline bool scanInt(const char*& p, const char* e, int& out) {
    const char* q = p;
    while (q < e && blank(*q)) ++q;
    bool neg = false;
    if (q < e && (*q == '+' || *q == '-')) neg = *q++ == '-';
    if (q >= e || !digit(*q)) return false;
    long long v = 0;
    for (; q < e && digit(*q); ++q)
        if (v < 100000000000LL) v = v * 10 + (*q - '0');
    out = (int)(neg ? -v : v);
    p = q;
    return true;
}

// A plain decimal (the recorder writes %.3f) is m / 10^k with m below 2^53: the quotient of two
// exact doubles is the correctly rounded double, and with at most 8 fractional digits a value that
// is not a float's rounding midpoint cannot be within half a double ulp of one, so narrowing it
// gives the correctly rounded float -- the value %f produces. Anything else (an exponent, inf, nan,
// hex, more digits) goes to strtof, the C library's own reading of the same grammar.
inline bool scanFloat(const char*& p, const char* e, float& out) {
    static const double kPow10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8};
    const char* q = p;
    while (q < e && blank(*q)) ++q;
    const char* s = q;
    bool neg = false;
    if (q < e && (*q == '+' || *q == '-')) neg = *q++ == '-';
    unsigned long long m = 0;
    int nInt = 0, nFrac = 0;
    for (; q < e && digit(*q); ++q, ++nInt)
        if (nInt < 18) m = m * 10 + (unsigned long long)(*q - '0');
    if (q < e && *q == '.') {
        ++q;
        for (; q < e && digit(*q); ++q, ++nFrac)
            if (nInt + nFrac < 18) m = m * 10 + (unsigned long long)(*q - '0');
    }
    const bool letterNext = q < e && ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z'));
    if (nInt + nFrac > 0 && nInt + nFrac <= 15 && nFrac <= 8 && !letterNext) {
        const double d = (double)m / kPow10[nFrac];
        out = (float)(neg ? -d : d);
        p = q;
        return true;
    }
    // strtof skips blanks itself, and from the line's end that would read the next line's number.
    if (s >= e) return false;
    char* end = nullptr;
    const float v = std::strtof(s, &end);
    if (end == s) return false;
    out = v;
    p = end;
    return true;
}

// A literal in the format: must be the very next character.
inline bool scanChar(const char*& p, const char* e, char c) {
    if (p >= e || *p != c) return false;
    ++p;
    return true;
}

}  // namespace groupscan

// `initOut`, when given, receives the `init,<uid>,<on>` lines: each tracked object's on/off as
// the game's reset left it, before the first update (the mod's grouptrace::snapshotInit) -- the
// t=0 an object's switches (OnEvent) are applied to. A recording made before those lines existed
// leaves it empty. Without `initOut` the lines are skipped, as they always were.
// `bytes` is the whole file; `path` only names it in the line this prints.
inline GroupTimeline parseGroupTimeline(const std::string& bytes, const std::string& path,
                                        long long* endOut = nullptr,
                                        std::unordered_map<int, uint8_t>* initOut = nullptr) {
    using namespace groupscan;
    if (endOut) *endOut = -1;
    if (initOut) initOut->clear();
    GroupTimeline g;
    const char* p = bytes.data();
    const char* const end = p + bytes.size();
    // Lines as getline on a text-mode stream gives them: split at '\n', a '\r' before it dropped.
    auto nextLine = [&](const char*& b, const char*& e) {
        if (p >= end) return false;
        b = p;
        const char* nl = static_cast<const char*>(std::memchr(p, '\n', (size_t)(end - p)));
        e = nl ? nl : end;
        p = nl ? nl + 1 : end;
        if (nl && e > b && e[-1] == '\r') --e;
        return true;
    };
    const char* b = nullptr;
    const char* e = nullptr;
    nextLine(b, e);   // header
    long long rows = 0;
    while (nextLine(b, e)) {
        int t = 0, uid = 0, on = 1, env = 0;
        float cx = 0, cy = 0, w = 0, h = 0, rot = 0;
        // a run that is cut off mid-write leaves one short line; skip it.
        // `on` and `rot` are optional so a trace recorded before those columns
        // existed still loads (as "always there, never turned", which is what it
        // used to mean). `env` is written only on the rows that have it (the box
        // GD's random numbers can put the object in, DynSample::env), always
        // after a `rot`.
        const size_t len = (size_t)(e - b);
        if (len >= 4 && !std::memcmp(b, "end,", 4)) {
            if (endOut) *endOut = std::atoll(std::string(b + 4, e).c_str());
            continue;
        }
        if (len >= 5 && !std::memcmp(b, "init,", 5)) {
            const char* q = b + 5;
            int u = 0, o = 1;
            if (initOut && scanInt(q, e, u) && scanChar(q, e, ',') && scanInt(q, e, o))
                (*initOut)[u] = o ? 1 : 0;
            continue;
        }
        const char* q = b;
        int n = 0;
        if (scanInt(q, e, t) && ++n && scanChar(q, e, ',') && scanInt(q, e, uid) && ++n
            && scanChar(q, e, ',') && scanFloat(q, e, cx) && ++n
            && scanChar(q, e, ',') && scanFloat(q, e, cy) && ++n
            && scanChar(q, e, ',') && scanFloat(q, e, w) && ++n
            && scanChar(q, e, ',') && scanFloat(q, e, h) && ++n
            && scanChar(q, e, ',') && scanInt(q, e, on) && ++n
            && scanChar(q, e, ',') && scanFloat(q, e, rot) && ++n
            && scanChar(q, e, ',') && scanInt(q, e, env))
            ++n;
        if (n < 6) continue;
        g[uid].push_back({t, cx, cy, w * 0.5f, h * 0.5f,
                          (uint8_t)(on ? 1 : 0), rot, (uint8_t)(env ? 1 : 0)});
        ++rows;
    }
    for (auto& kv : g)
        std::sort(kv.second.begin(), kv.second.end(),
                  [](const DynSample& a, const DynSample& b2) { return a.t < b2.t; });
    std::printf("groups: %lld samples for %zu objects (%s)\n", rows, g.size(),
                path.c_str());
    return g;
}

inline GroupTimeline loadGroupTimeline(const std::string& path,
                                       long long* endOut = nullptr,
                                       std::unordered_map<int, uint8_t>* initOut = nullptr) {
    std::string bytes;
    if (!readFileBytes(path, bytes)) {
        if (endOut) *endOut = -1;
        if (initOut) initOut->clear();
        std::fprintf(stderr, "groups: cannot open %s\n", path.c_str());
        return GroupTimeline{};
    }
    return parseGroupTimeline(bytes, path, endOut, initOut);
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
// The last tick the --groups recordings cover (the largest `end,<tick>` trailer among the
// files; -1 = no trailer, or no recording). Read by --ridebox to tell a ride the recording
// saw end from one it was still inside when it stopped.
inline long long g_groupsEndT = -1;

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
// motion matching itself, which is not yet a reason to refuse).
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
// file is always the last overlay. The hold was asked to be tied
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
// says `over` ended on a death (--groupholddeath). `report`, when given, receives the lines this
// prints instead of stdout (cli.hpp's layer cache replays them on a hit).
inline void overlayGroupTimeline(GroupTimeline& base, const GroupTimeline& over,
                                 bool deathEnd = false, long long overEnd = -1,
                                 std::string* report = nullptr) {
    // How far the overriding recording actually reaches. One number for the
    // whole file, not per object: an object that simply did not move inside the
    // covered window has one sample and must NOT be treated as uncovered.
    // `overEnd` (the recorder's own trailer) is the tick its RUN reached, which
    // is past the last row whenever the level went quiet before the run ended --
    // and those quiet ticks are covered too. Without it the older recording's
    // rows for them win (4002 t=22,677, measured 2026-09-23).
    int cover = -1;
    for (const auto& kv : over)
        if (!kv.second.empty()) cover = std::max(cover, kv.second.back().t);
    if (cover < 0) return;
    if (overEnd > cover) cover = (int)overEnd;
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
    char b[512];
    std::string lines;
    std::snprintf(b, sizeof b, "groups: overlay covers t<=%d\n", cover);
    lines += b;
    std::snprintf(b, sizeof b, "groups: the bootstrap joined %d moving object(s) at a shift "
                  "of %d..%d ticks; %d moving at the seam, %d with no fitting "
                  "shift, worst fitting error %.4f px, %d with more than one "
                  "fitting shift (at most %d)\n", nRetimed, sLo, sHi, nMoving,
                  nNoFit, worstJoin, nMulti, mostFits);
    lines += b;
    if (deathEnd) {
        std::snprintf(b, sizeof b, "groups: --groupholddeath: the last recording ended on a "
                      "death at t=%d; %d object(s) hold its last row one tick\n", cover, nHeld);
        lines += b;
    }
    if (report) *report += lines;
    else std::fputs(lines.c_str(), stdout);
}

}  // namespace dp
