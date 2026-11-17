#pragma once
#include "dp/progress.hpp"   // which includes constants.hpp -- the chain's next link

namespace dp {

// ...and the whole thing turned out to be ONE function in GD, read straight out
// of the exe. `GJBaseGameLayer::checkCollisions`
// clamps to getMinPortalY()/getMaxPortalY() +- 15*vsize, and the band those
// return is written by `animateInDualGroundNew` when a mode portal is passed:
//
//   H      = getGroundHeightForMode(portal type)   (its jump table, read directly)
//   floor  = max(90, floor30(cy - H/2))
//   ceil   = floor + H
//
// H is 300 for ship/UFO/wave/swing, 240 for ball, 270 for everything else --
// and a CUBE or ROBOT portal does not update the band at all (updateDualGround
// skips the call when H == 270 and the mode is not spider or dual), so the band
// survives across a cube section unchanged.
// Verified against 18 portals on lv1/9/12/14 and against every knob that had
// been placed by hand: lv9's `--ceil 330`, lv14's `420@13100:13600`, lv15's
// `330@16300:17800`, lv10's 700, lv13's 500, lv16/lv18's 1050. The old comment
// here said the ship bound was "a search bound, not physics" -- the physics was
// one level down, and 375 was right on lv1 only because that section's true
// ceiling really is 390 (= 375 + the player's half).
constexpr double kBandFly = 300.0;    // ship / UFO / wave / swing
constexpr double kBandBall = 240.0;
constexpr double kBandOther = 270.0;  // spider updates with this; cube/robot do not
// 0 = this portal does not touch the band
inline double bandHeightFor(int portalType) {
    switch (portalType) {
        case 5: case 19: case 26: case 41: return kBandFly;
        case 16: return kBandBall;
        case 33: return kBandOther;       // spider
        // 6 cube / 27 robot leave the band alone. 23/24 are handled separately
        // (bandHeightDual): a dual portal DOES rewrite the band -- the old
        // "leave it alone" here is what stalled every lv16 cold run at x=13,295.
        default: return 0.0;
    }
}
// ...and the same while the player is a dual. updateDualGround only skips the
// call when H == 270 AND the mode is neither spider nor dual, so in a dual even
// a CUBE updates the band, and the ball's 240 reads as 270. Measured on lv16
// with the ceilprobe build's pmin/pmax columns:
//   cube in dual  [360,630] = H 270   ship in dual [330,630] = H 300
//   ufo  in dual  [390,690] = H 300   ball in dual [390,660] = H 270
inline double bandHeightDual(int mode) {
    return (mode == 1 || mode == 3 || mode == 4) ? kBandFly : kBandOther;
}
// WHAT A BAND READING MEANS, as a type rather than as a pair of numbers. The
// recording carried three different facts in the same two floats, and the
// parser used to drop everything that was not an interval -- so "the band
// collapsed here", "there is no band here" and "the writer could not read it"
// all arrived at the readers as "no row", which the lookup answers with the
// last interval it saw. Separating them does not decide what a reader should
// do; it lets the reader say which case it is answering.
enum class BandKind : uint8_t {
    Interval,       // fl < ce: a real band
    Degenerate,     // fl == ce: the band collapsed (NOT "no band")
    NoBand,         // the writer knows there is none here
    Unknown,        // the writer could not read it (a missing or zero field)
};
// --startband <floor>,<ceil>: the anchor tick's band, read out of GD's dump
// (pmin/pmax = getMinPortalY / getMaxPortalY) instead of guessed from x.
// The kind is the gate. `g_startBandCeil` used to default to 1e9 and three
// call sites then asked `< 1e8` to find out whether a band had actually been
// given -- a sentinel standing in for a missing value, with a fourth site
// asking a different question (`g_startBandSet`) for the same fact. Both are
// replaced by the kind, so "no band was given" is a value rather than a number
// large enough to notice.
inline BandKind g_startBandKind = BandKind::Unknown;
inline double g_startBandFloor = 0.0, g_startBandCeil = 0.0;
inline bool startBandUsable() {
    return g_startBandKind == BandKind::Interval;
}
// --bandtrack <file>: GD's flying band from the recorded run (the dump's
// tick,pmin,pmax with only the rows where it changes). A band frozen at the anchor
// becomes a lie: GD's band moves every tick with the camera + zoom, and over lv22's
// ship stairs .. robot section it travels more than 260px. A DP on a frozen band
// cannot hold a climbing route even in principle, and kept planning into the crush
// at x=20,175 while breeding `clamp:fly/bandceil` fixups (the t=16,669 wall). Ticks
// that have a record take the record as truth; beyond the record (untrodden
// section) the last row is held = the frontier's neighbourhood is always fresh.
// In a rotated frame (frame != 0) it is as before (the band is a world-Y quantity
// and the frame mapping is a separate matter; the maze passes as it is, so it is
// left alone).
// src: WHICH band GD's getMinPortalY / getMaxPortalY returned (the MOD's fifth column):
// 1 = branch A, read off the ground layers and moving with the camera ([layer+0x23c] != 0 and
// [layer+0x2a0] == 0), 0 = branch B, the portal's band. -1 = the file has no fifth column (a
// recording from before it was written), and every reader then behaves as it did without one.
struct BandTrackRow { int t; BandKind kind; float fl, ce; int8_t src = -1; };
inline std::vector<BandTrackRow> g_bandTrack;
// The Interval rows' indices, in the same order. The lookup below walks THIS,
// so a run that sees no D/N/U row behaves exactly as it did before the kinds
// existed; the readers that want to see the other kinds ask bandTrackKindAt.
inline std::vector<size_t> g_bandTrackIntervals;
// The file's `t,S,src` rows (band_rows.hpp): the band's source changed at t while its kind and values did
// not. Kept apart from g_bandTrack so that every reader of the rows -- the camera verdict, the band-seen
// test, the dual walk -- sees exactly the rows it saw before the S rows existed; only the source lookup
// (bandTrackKindAt's `src`) reads them. In tick order.
struct BandSrcRow { int t; int8_t src; };
inline std::vector<BandSrcRow> g_bandSrcRows;
// --bandtrackend <t>: the last tick the recording behind --bandtrack reached. -1 =
// not given, and the last row is held indefinitely as before.
// Holding the last row keeps the frontier's neighbourhood fresh while the band is the
// CAMERA's, but a mode portal past the recording sets a new band the recording
// cannot know about, and the held row overrides the one the portal writes into the
// State. Measured on lv10 (a cold run, both kill-only iterations of it): the
// solve for it3 read a track ending at t=14,410 whose last row was 180..420; the
// ship portal at t=17,707 (cy 301) sets 150..450, GD descends to y=173.7 and dies on
// a spike at 154, the model rides the stale floor at y=195 and calls the plan SOLVED.
// Re-walked with the recorder's track the model follows GD to 0.025 px and dies too.
// it2 is the same shape (ball portal at t=14,150, stale ceiling 390, model 375 vs GD
// 397). Past the end the lookups answer "no row", so the State's portal band decides.
inline long long g_bandTrackEnd = -1;
inline thread_local bool g_bandHoldNow = false;
constexpr uint8_t kBandPastEnd = 32;
inline bool bandTrackPastEnd(long long t) {
    return g_bandTrackEnd >= 0 && t > g_bandTrackEnd && !g_bandHoldNow;
}
// [2026-08-22 r107] **Look up the row at t+1.** The 36 fixups at lv22's vertical
// entrance (x 20,000..20,090, the section where the band rises at +1.4px/tick)
// named it: GD's seat is
//   y_gd = pmin(t+1) + 12.15
// while the model's old implementation (row t) was
//   y_model = pmin(t) + 12.16
// -- the same seat amount, just one row stale. The decisive sign was dy growing
// with exactly the same slope as one step of the band (1.3966->1.4268) (under a
// constant-offset explanation dy would be constant regardless of the band speed).
// It never showed in the census because in sections where the band moves under
// 0.05px/tick it hides below eps (same shape as the 0.02px family in
// [[gd-velocity-001-grid]]).
// The raw row lookup (no phase shift): the recorded row at or before `t`, held
// until the next. Callers pick the phase -- see bandTrackAt and the fly floor.
inline bool bandTrackRowAt(long long t, double& fl, double& ce) {
    if (g_bandTrackIntervals.empty()) return false;
    if (bandTrackPastEnd(t)) return false;
    size_t lo = 0, hi = g_bandTrackIntervals.size();
    while (lo + 1 < hi) {
        const size_t m = (lo + hi) / 2;
        if (g_bandTrack[g_bandTrackIntervals[m]].t <= t) lo = m; else hi = m;
    }
    const BandTrackRow& r = g_bandTrack[g_bandTrackIntervals[lo]];
    if ((long long)r.t > t) return false;
    fl = (double)r.fl;
    ce = (double)r.ce;
    return true;
}
// The same search over EVERY row, answering with the kind and with how stale
// the answer is. `age` is t minus the row's tick, so a caller can tell "the
// band is this" from "the band was this, `age` ticks ago". No age limit is
// imposed here: which staleness is acceptable is the caller's question, and
// today every caller accepts any.
inline bool bandTrackKindAt(long long t, BandKind& kind, double& fl, double& ce,
                            long long& age, int* src = nullptr) {
    if (g_bandTrack.empty()) return false;
    if (bandTrackPastEnd(t)) return false;
    size_t lo = 0, hi = g_bandTrack.size();
    while (lo + 1 < hi) {
        const size_t m = (lo + hi) / 2;
        if (g_bandTrack[m].t <= t) lo = m; else hi = m;
    }
    if ((long long)g_bandTrack[lo].t > t) return false;
    kind = g_bandTrack[lo].kind;
    fl = (double)g_bandTrack[lo].fl;
    ce = (double)g_bandTrack[lo].ce;
    age = t - (long long)g_bandTrack[lo].t;
    if (src) {
        *src = g_bandTrack[lo].src;
        // ...unless the source changed since that row (an S row at or before t, after the row).
        size_t a = 0, b = g_bandSrcRows.size();
        while (a < b) {
            const size_t m = (a + b) / 2;
            if ((long long)g_bandSrcRows[m].t <= t) a = m + 1; else b = m;
        }
        if (a > 0 && g_bandSrcRows[a - 1].t >= g_bandTrack[lo].t) *src = g_bandSrcRows[a - 1].src;
    }
    return true;
}
// [2026-10-04] --bandowntrack: State::bandBranch bit 4 (kBandOwn) says this worldline's band
// was written by a portal the recording behind --bandtrack never saw fire. The recording is one
// route's camera; up to --bandtrackend it is read as truth for every route, so a route that
// leaves the recorded one and takes a portal the recording passed below or above keeps the
// recording's last band. Measured on a custom level: the recorded run stayed on the floor past a
// ship portal at cy 1005; the solve that took that portal kept the earlier ship section's band
// 630..930 (GD: 840..1140), rode its ceiling at y=915, and was called SOLVED, while GD sank onto
// a spike at y=862. The anchor at t=18,000 carried GD's band and agreed with GD to the tick, so
// the replay instruments never saw it. What "seen" means is bandTrackSawBand (below); an anchor
// whose --startband the recording does not hold at t0 starts with the bit (cli.hpp). On by default
// (2026-10-18; --no-bandowntrack is the off arm, where the bit is never set).
constexpr uint8_t kBandOwn = 16;
constexpr int kBandOwnNear = 3;   // GD's band changes on the portal's own tick
inline bool g_bandOwnTrack = true;
inline bool bandTrackAt(long long t, double& fl, double& ce) {
    return bandTrackRowAt(t + 1, fl, ce);
}
// [2026-08-24] The FLY FLOOR reads the row at t-1 -- the camera pans AFTER the
// clamp, so the dump's pmin on row t is post-pan and the value the clamp itself
// used is the previous row's. Measured at the same shaft entrance as r107, on a
// SHIP this time, against GD's whole ride (t=16,408-16,428, pan +1.44/tick,
// dy checked per tick): y_gd(T) = pmin(T-1) + pHalf reproduces the catch tick
// (T=16,409: 565.2+15=580.2 vs GD 580.25, y rising against vy<0 -- the floor
// caught it) and every seated tick after it, wobble +-0.06 = the pan's easing.
// r107's representation (row t+1 with an effective offset 12.15) is the same
// line under a constant 1.44 rise -- 12.15 + 2*1.44 = 15.03 = pHalf -- so both
// fits agree everywhere the pan speed is constant, and this one keeps pHalf.
inline bool bandTrackFloorAt(long long t, double& fl, double& ce) {
    return bandTrackRowAt(t - 1, fl, ce);
}
// [2026-08-25] IS THE RECORDED BAND THE CAMERA'S, OR A PORTAL'S? Only a
// PORTAL-set limit is a physical ceiling; the camera's is not, and the two are
// told apart by the shape of the whole recording rather than by any one value.
// Measured, GD's own dumps:
//   lv9  -- the band changes FOUR times in the level (t=1 90..90 uninitialised,
//           t=6,961 90..330, t=10,359 90..390, t=13,964 90..330), every value a
//           multiple of 30, each held for thousands of ticks. At t=7,312 the
//           flipped ball charging in at terminal +15.000 STOPS DEAD at y=315
//           (top = 330 = pmax) and rides it 232 px over the spiked wall.
//   lv22 -- the band changes 1,213 times in 3,000 ticks and 1,204 of those
//           values are off the 30 grid (364.554, 423.732, 416.058 ...): it is
//           the camera. GD's ball at t=2,296 flies straight through pmax=360
//           at +15.000 and keeps going to y=1,003.
// The old proxy was "the value is a multiple of 30" (r81 note below), and lv22
// falsifies it: the camera rests with pmin pinned at the floor and pmax at
// 90+270=360 for 164 ticks around t=2,296 -- on the grid, and not a wall. The
// separation by track shape is total (0% off-grid vs 99%), and a run with no
// recording keeps the old behaviour exactly.
// Cached: this is read once per child per tick and the track runs to thousands
// of rows. -1 = not computed yet; reset.hpp clears it with the track (a stale
// verdict would otherwise cross into the next in-process solve).
// The first use comes from the search's pool threads, so several can find it
// unset at once. Each computes the same verdict from the read-only track; only
// the one whose compare-exchange stores it prints the line (it used to print
// once per thread that got in, a schedule-dependent count).
inline std::atomic<int> g_bandTrackCam{-1};
inline bool bandTrackIsCamera() {
    const int cam = g_bandTrackCam.load(std::memory_order_relaxed);
    if (cam >= 0) return cam != 0;
    // Only Interval rows carry a ceiling to be on or off the grid, so they
    // are the population -- both the numerator and the count this is
    // compared against. A collapsed or absent band is not evidence either
    // way about which kind of band the recording holds.
    const size_t n = g_bandTrackIntervals.size();
    size_t off = 0;
    int verdict = 0;                     // too short to judge (n < 8); as before
    if (n >= 8) {
        for (const size_t i : g_bandTrackIntervals) {
            const double c = (double)g_bandTrack[i].ce;
            if (std::fabs(c - std::round(c / 30.0) * 30.0) >= 0.01) ++off;
        }
        verdict = (off * 2 > n) ? 1 : 0;
    }
    int unset = -1;
    if (g_bandTrackCam.compare_exchange_strong(unset, verdict, std::memory_order_relaxed)
        && n >= 8)
        std::printf("bandtrack: %zu interval rows, %zu off-grid -> %s\n",
                    n, off,
                    verdict ? "CAMERA-driven (no physical ceiling)"
                            : "portal-set (the ceiling is a wall)");
    return verdict != 0;
}
// --bandtrack <file>: read the recorded band (history at g_bandTrack's declaration). Appends to the
// track, as every --bandtrack on a command line always has.
//
// NO ROW IS DROPPED. The old reader kept only `ce > fl` and said nothing about the rest, so a
// collapsed band reached the lookup as "no row" and the lookup answered with the last interval it
// had -- an old, wrong, non-degenerate band. Each row now arrives with its kind and is counted; what
// a reader does with a kind is the reader's decision, not the parser's.
//
// The version is the column count: three fields is the original `t,floor,ceil`, four is
// `t,kind,a,b`, five adds the source. No header line, because a reader of the older format would
// take one for a row. An S row (band_rows.hpp) goes to g_bandSrcRows, never to the track.
inline bool loadBandTrack(const char* path) {
    std::FILE* bf = std::fopen(path, "r");
    if (!bf) {
        std::fprintf(stderr, "bandtrack: cannot open %s\n", path);
        return false;
    }
    size_t kn[4] = {0, 0, 0, 0};
    size_t skipped = 0, nSrc = 0;
    char line[256];
    while (std::fgets(line, sizeof line, bf)) {
        BandTrackRow r{};
        char k = 0;
        float a = 0, b = 0;
        // a fifth field is the band's source (BandTrackRow::src); absent = -1
        int src = -1;
        const int nf = std::sscanf(line, "%d,%c,%f,%f,%d", &r.t, &k, &a, &b, &src);
        if (nf >= 3 && k == 'S') {
            const int s = (int)a;
            g_bandSrcRows.push_back({r.t, (int8_t)((s == 0 || s == 1) ? s : -1)});
            ++nSrc;
            continue;
        }
        if (nf >= 2
            && (k == 'I' || k == 'D' || k == 'N' || k == 'U')) {
            r.src = (int8_t)((nf >= 5 && (src == 0 || src == 1)) ? src : -1);
            r.fl = a;
            // A D row carries the collapsed value in both columns, so this does not depend on the
            // fourth field being present -- but the writers fill it in anyway, so a future reader
            // may tighten the field count.
            r.ce = (k == 'D') ? a : b;
            r.kind = k == 'I' ? BandKind::Interval
                   : k == 'D' ? BandKind::Degenerate
                   : k == 'N' ? BandKind::NoBand
                              : BandKind::Unknown;
            // An interval that is not one is not silently kept: the invariant is part of the type.
            if (r.kind == BandKind::Interval && !(r.ce > r.fl))
                r.kind = BandKind::Unknown;
        } else if (std::sscanf(line, "%d,%f,%f", &r.t, &r.fl, &r.ce) == 3) {
            r.kind = r.ce > r.fl  ? BandKind::Interval
                   : r.ce == r.fl ? BandKind::Degenerate
                                  : BandKind::Unknown;
            if (r.ce < r.fl)
                std::fprintf(stderr, "bandtrack: t=%d has ceil %g below floor %g\n", r.t,
                             (double)r.ce, (double)r.fl);
        } else {
            // A line that matches neither shape. It is still not dropped in silence: the count is
            // printed with the rest, because "nothing here" and "I could not read this" are the two
            // the whole change exists to keep apart.
            ++skipped;
            continue;
        }
        if (r.kind == BandKind::Interval)
            g_bandTrackIntervals.push_back(g_bandTrack.size());
        ++kn[(size_t)r.kind];
        g_bandTrack.push_back(r);
    }
    std::fclose(bf);
    g_bandTrackCam = -1;   // the verdict belongs to THIS track
    if (nSrc)
        std::printf("bandtrack: %zu rows (I %zu D %zu N %zu U %zu) unreadable %zu, source changes %zu (%s)\n",
                    g_bandTrack.size(), kn[0], kn[1], kn[2], kn[3], skipped, nSrc, path);
    else
        std::printf("bandtrack: %zu rows (I %zu D %zu N %zu U %zu) unreadable %zu (%s)\n",
                    g_bandTrack.size(), kn[0], kn[1], kn[2], kn[3], skipped, path);
    return true;
}
// --bandowntrack: did the recording see THIS band written near tick t? On a portal-set track a
// row within kBandOwnNear ticks must also carry the band itself, or it is another portal's -- a
// second lane's portal firing within the window, which the presence test alone would take as
// this one. The track holds CHANGES only, so a portal that rewrote the band the recording already
// held leaves no row: a band equal to the row in force at t is seen too (a custom level's UFO
// portal rewrote [300, 600] over [300, 600], and the old test made the band the state's own and
// dropped the recording's for the rest of the track).
// A camera-driven track is always "seen": its rows are the camera's pans, not portal writes, so
// their presence near t says nothing about which portal fired (a custom level's anchor re-fired
// the ship portal GD had just taken, on a tick the camera did not move, and the bit threw away
// the camera ceiling GD stopped the ship on).
inline bool bandTrackSawBand(long long t, double fl, double ce) {
    if (g_bandTrack.empty()) return false;
    if (bandTrackIsCamera()) return true;
    size_t lo = 0, hi = g_bandTrack.size();
    while (lo < hi) {
        const size_t m = (lo + hi) / 2;
        if ((long long)g_bandTrack[m].t < t - kBandOwnNear) lo = m + 1; else hi = m;
    }
    for (size_t i = lo; i < g_bandTrack.size() && (long long)g_bandTrack[i].t <= t + kBandOwnNear;
         ++i) {
        const BandTrackRow& r = g_bandTrack[i];
        if (r.kind == BandKind::Interval && std::fabs((double)r.fl - fl) <= 1.0
            && std::fabs((double)r.ce - ce) <= 1.0)
            return true;
    }
    double f0 = 0.0, c0 = 0.0;
    return bandTrackRowAt(t, f0, c0) && std::fabs(f0 - fl) <= 1.0 && std::fabs(c0 - ce) <= 1.0;
}
// [2026-10-05] --portalstarty: a mode portal's band is centred on where the portal was PLACED,
// not where a group has since moved it. animateInDualGroundNew (0x140213370) reads the portal's
// y through vtable slot 0x670, GameObject::getStartPos (0x140137ca0), which copies
// m_startPosition (+0x3c8); the current position (m_positionX/Y, +0x3b8/+0x3c0) is
// getRealPosition, slot 0x4a8. Measured on a custom level: a UFO portal placed at y=450 and moved
// to 510 at t=3 left GD's band at [300, 600] = bandFor(450) for the next 300 ticks, while the
// model's bandFor(510) put the floor at 360 and stopped the UFO on it as GD fell past. A static
// portal has never moved, so its cy is its start. On by default since 2026-10-05 (also five moved
// mode portals crossed in GD, every band at the placed position; a kitref of 0 fixed and 0 broken in
// 1,773 episodes on one exe); --no-portalstarty is the off arm, the portal's current cy (step.hpp
// portalBandY).
inline bool g_portalStartY = true;
// Calibration factor for the band height (height asked of GD via --startband / the
// value computed from the zoom). 0 = uncalibrated. Fixed once, at the first tick
// where the band is evaluated (= the anchor).
inline double g_bandK = 0.0;
inline bool g_slopeDbg = false;  // --slopedbg: one line per ramp acquisition
// --slopereldbg: the downhill-RELEASE branch only (step.hpp). Separate from
// --slopedbg because that one turns on forty-two sites and buries a cold run.
inline bool g_slopeRelDbg = false;
// (frame, rev) censuses. Declared here rather than in fixup.hpp because step.hpp
// is upstream of that header and does the counting.
//
// SPLIT BY REVERSED, because frame and rev are two names for one physical
// situation. The mod rewrites GD's gframe 2 as (frame 0, rev 1)
// (repair.hpp:970), so a section GD records as frame 2 reaches the model either
// as frame 2 -- when the model rotated into it through a trigger, and lv22 has
// two at rot -180 -- or as frame 0 with rev set. A frame-only count cannot tell
// those apart, so "frame 2 was never seen" and "frame 2 was seen wearing frame
// 0's name" look identical. That is exactly the ambiguity an earlier f2 = 0
// reading could not resolve, and why Reach (every step) is kept beside Call
// (every fixup lookup): the first is the denominator of the second.
//
// ONE ROW PER THREAD, summed where it is read (frameRevReach). Every thread of
// the search's pool steps states, and a single shared array was a cache line
// they all wrote on every step -- and, a plain `long long`, one that lost
// increments to the race. `g_threadSlot` is the row: 0 for whoever calls
// cliMain, 1.. for the pool's workers (ThreadPool sets it as each one starts).
// A row is exactly one cache line. Past kThreadSlots threads rows are shared
// again, which is where the old array always was.
inline thread_local int g_threadSlot = 0;
inline constexpr int kThreadSlots = 64;
struct alignas(64) FrameRevRow { long long n[4][2]; };
inline FrameRevRow g_frameRevReach[kThreadSlots] = {};
inline long long frameRevReach(int f, int r) {
    long long sum = 0;
    for (const FrameRevRow& row : g_frameRevReach) sum += row.n[f][r];
    return sum;
}
// --fxwatch <t>: why a fixup did or did not fire on the replayed state at one
// step (fixup.hpp fxDescribe fills it, the replay's per-tick line prints it).
// Declared here rather than in fixup.hpp for the same reason as the counters
// above: reset.hpp has to see it.
inline long long g_fxWatchT = -1;
inline char g_fxWhy[1600] = "";
// (the fixup lookups and hits by the same (frame, rev) pair are counted in
// fixup.hpp, g_fixupCensus)
// --dcydbg: one line every time a SURFACE'S OWN SPEED is stamped into vy as
// `dcy / 0.25`, tagged with which of the four sites did it. Separate from
// --slopedbg only because that one prints per ramp tick and buries these.
// This measures REACH -- how many ticks in the corpus each site actually
// decides -- which is the number that says whether a rule about one of them is
// worth anything. Counting it off whole-run traces does not work: past a
// level's first divergence the model is on its own timeline.
//
// `dcy` and the stamped value print at FULL precision (%.17g), deliberately and
// not for tidiness. Whether dcy is a round 0.25 -- and so whether dividing by
// 0.25 lands on a round 1.0 -- is itself the claim being argued at the lv19
// 21,4xx sites, and at 4 decimals both -0.250040 and -0.249996 print as
// `-0.2500`, manufacturing the very roundness that was supposed to be the
// finding. Do not narrow these back.
inline bool g_dcyDbg = false;
// --supdbg: every solid the cube's support scan looks at, with the gap and the x
// test that decide it. Written for 4002 t=22,768, where the player stood on a
// platform GD has at y=1,648.5 and the model had at 2,248.5: printing the scan is
// what showed the object was in the wrong PLACE rather than the rule being wrong.
inline bool g_supDbg = false;
inline bool g_bandDbg = false;   // --banddbg: one line per wave tick near the band ceiling
// --spddbg: one line per SPEED PORTAL CANDIDATE per tick, naming the gate that
// rejected it. The three gates (x overlap / oriented hit / y gap) each `continue`
// on their own, so a portal that never fires says nothing about WHY -- and on
// lv21 the arithmetic says all three should have passed 48 px before the model
// actually fired. This prints what each gate computed, so the answer is read
// rather than inferred.
inline bool g_spdDbg = false;
// Was "--shipceil given: pin the band by hand". The flag is gone (2026-08-31),
// so this is false for the whole run and every reader of g_shipCeil is dead.
inline bool g_shipCeilSet = false;
// --rotport: treat a portal that a trigger ROTATES as a turned object even
// while its angle still reads 0, so it is tested with the two-box SAT against
// the player's turned box. Measured right (see Dynamics::turnedBox) but OFF by
// default: on, it cost lv22's cold run 41% -> 15%. Declared here rather than
// with the other knobs in thread_pool.hpp because dynamics.hpp reads it and
// comes earlier in the header chain.
// On by default since 2026-10 (was opt-in).
// (The switch is gone since the 0.4.0 clean-up; its on behaviour is fixed.)
// (--rotlast, the A/B switch that adopted the LAST matching 2900 on a tick instead of the nearest one
// on the perpendicular axis -- the pre-0a1e8b6 rule -- is gone.)
// [2026-09-02] THE BAND'S HEIGHT IS THE ONE THE PORTAL WROTE, and a Free Mode
// portal writes nothing. Read out of the exe and checked against GD's own
// 21,140-tick dump of lv22 (notes: band-numerator-writers, 2026-09-02):
//
//   updateDualGround:
//     if (!freeMode && (H != 270 || spider || dual)) animateInDualGroundNew(H)
//     else                                           retract the band
//
// The second half of that gate is already here -- bandHeightFor returns 0 for
// cube and robot, which is the same statement -- so the only missing piece is
// Free Mode. Reproduced against GD's ground-sprite column for ALL 17 of lv22's
// mode changes (2 animate-in, 15 retract); the note's own 11/11 is this same
// check with the cube/robot rows left out.
//
// WHY THE TWO HALVES CANNOT LAND SEPARATELY. The obvious half of the fix --
// "use the band's own height instead of the 270 constant in the zoom rewrite"
// -- makes the model WORSE on its own. lv22's ball portal at x=7,635 carries
// Free Mode, so GD never writes the ball's 240 and the band keeps the 270 that
// the spider at x=975 put there. The model, which applies every portal, has
// 240 stored, and 240/0.9091 = 264 against GD's 270/0.9091 = 297: 33 px of
// headroom taken away, on a section that is right today only because the 270
// constant happened to agree. So both halves hang off this one flag, and a
// dump that predates the column keeps the old behaviour exactly.
// (Set from the objrects HEADER, by name -- see loadLevelFrom.)
inline bool g_freeModeCol = false;
// [2026-09-22] ...AND FREE MODE IS WHAT DECIDES WHETHER THE BAND IS A WALL.
// checkCollisions (0x2139b8) tests [layer+0x311] before anything else: set, and
// no mode is clamped to the band at all; clear, and the six flying modes are
// clamped to getMin/MaxPortalY whatever branch those took (the only other gate
// is the teleport byte, State::tpSkip). [layer+0x311] is the Free Mode byte
// playerWillSwitchMode (0x212ef0) copies from EVERY mode portal it takes --
// lv22's seventeen mode changes all leave GD's byte equal to their portal's
// `free` column, cube and robot portals included.
// The band being "on the 30 grid" was a stand-in for this, and a leaky one: a
// zoom divides a portal band's height too (branch B is centre -+ 0.5*H/zoom),
// so SubZero 4003's spider band sits at 1354.5/1651.5 = 270/0.9091 high, off the
// grid, and GD lands the spider on its floor while the grid test dropped it.
// Measured on GD's replays with the dump's `freemode` column (27046c9): flying
// ticks outside the band -- lv9 0 of 11,299 with the byte clear, lv22 0 of 2,052
// clear and 570 of 5,575 set, 4003 0 of 5,732 clear.
// Carried in State::bandBranch bits 2-3. Bit 2 says the value is KNOWN: set at
// the level's start (the byte is clear after resetLevel) and by every mode
// portal, or handed over by an anchor's hist payload (version 2). An anchor
// that does not carry it leaves it unknown, and then the grid test stands --
// the old behaviour exactly, which is what the anchored suites were built on.
constexpr uint8_t kBandFreeKnown = 4;
constexpr uint8_t kBandFree = 8;
// (--bandowntrack's bit is kBandOwn, declared with the track above.)
inline bool bandIsWall(uint8_t branch, bool legacyGridTest) {
    if (branch & kBandFreeKnown) return (branch & kBandFree) == 0;
    return legacyGridTest;
}
// --flyfree: ...and the flying modes ask it too. The cube family's band faces went through
// bandIsWall from the start; the ship / UFO / swing branch and the wave's own branch clamped to
// the band whatever the byte said. Measured on a custom level whose first ship portal carries Free
// Mode (band [7804, 8126] in GD's own pmin/pmax): GD's ship falls straight past 7819 = floor + 15
// at -6.4 a tick and dies on a hazard strip at 7800 that rides along beneath it, which is where
// that level's runs keep dying; the model sat on 7819 (fly/bandfloor) and lived thousands of ticks
// longer. Known and set only -- an unknown byte keeps the old clamp. On by default since 2026-10-06;
// --no-flyfree is the off arm.
inline bool g_flyFree = true;
// --groundbandzoom: ...and a single ball's or spider's (outside a dual) the same way, branch B only. A custom level's
// spider under a zoom of 0.8: GD's pmin/pmax read 116.25 / 453.75 = 285 -+ 135 / 0.8, and at a block's end
// (t=14,721) it walked off and fell past 163.5 (the zoom-1 band's seat, floor 150); the model stood it on 150 and
// ran on along it.
inline bool g_groundBandZoom = true;
// --camfree: a Camera Mode trigger (id 2925) on the crossing queue writes the Free Mode byte too, from its
// m_cameraIsFreeMode (the objrects `free` column), on the tick its x is crossed -- after that tick's
// collisions, like every crossing trigger, so the band it frees is the next tick's (step.hpp, at the step's
// end). A custom level's ball, mode unchanged: GD's dump turns `freemode` 0 -> 1 on the tick it crosses a
// 2925 at x 6,180 with free=1, and from there the ball rises past its band's ceiling 1,110 (pmax still
// reads 1,110), where the model stood it on that ceiling (ground/flipceil). On by default; --no-camfree is
// the off arm.
inline bool g_camFree = true;
struct CamFree {
    double cx;
    uint8_t free;
    int uid;
};
inline std::vector<CamFree> g_camFrees;   // sorted by cx at load
inline bool flyBandFree(uint8_t branch) {
    return g_flyFree && (branch & kBandFreeKnown) && (branch & kBandFree);
}
// The same for the trigger queue's admission gate: PlayLayer::addObject only
// enqueues a trigger when it is neither touch- nor spawn-triggered, so those
// never fire on an x crossing. Two of lv22's twenty zoom triggers are in that
// state and the model was firing both. `touch`/`spawn` columns, again by name.
inline bool g_trigGateCol = false;
// STATIC CAMERA (id 1914). It is what opens branch A of getMin/MaxPortalY,
// where the band stops being "the portal's H about a centre" and becomes the
// two ground SPRITES read back through the zoom -- and those carry the band
// animation, so the height there is 322 - (322 - H) * p rather than H.
// updateStaticCameraPos writes staticY (0x23c) and clears bandFromMode
// (0x2a0); property 110 (Exit Static) takes it back off. Property 101 is the
// axis: 0 both / 1 X only / 2 Y only, and only the two that include Y touch
// this band at all.
// Unlike a portal this fires on the player's x crossing alone -- no overlap,
// no height -- so it is a LEVEL property, resolved from x like every other
// autonomous trigger (see the ZoomTrig block, and the same reasoning in
// dynamics.hpp's autotrigger note).
// The columns (`axis`, `exstat`) are not in any dump yet; without them this
// list is empty and every level keeps branch B, which is the behaviour on
// record.
struct StaticCam { double cx; uint8_t exit_; };
inline std::vector<StaticCam> g_staticCams;   // sorted by cx at load
inline bool g_staticCamCol = false;
// The band animation, packed into State::bandAnim: bit 7 = target (1 = in),
// bits 0-6 = ticks elapsed, 127 = settled. Measured on lv22's 21,140 ticks
// against GD's own sprite column: 119 ticks in on ease-in-out rate 2, 95 out
// on rate 1.5, starting on the portal's own tick.
constexpr int kBandAnimIn = 119, kBandAnimOut = 95;
constexpr double kBandRetracted = 322.0;   // winSize.height + 2
inline uint8_t bandAnimStart(bool animateIn) {
    return (uint8_t)(animateIn ? 0x80 : 0x00);
}
inline uint8_t bandAnimStep(uint8_t a) {
    const int e = a & 0x7f;
    return (uint8_t)((a & 0x80) | (e >= 127 ? 127 : e + 1));
}
// --dualzoomband: in a dual animateInDualGroundNew (0x213370) tweens the band in over 0.4 s, not 0.5
// (its duration is 0.4 when the dual byte +0x422 or +0x310 is set): 96 ticks.
constexpr int kBandAnimInDual = 96;
inline double bandProgress(uint8_t a, int inTicks = kBandAnimIn) {
    const bool in = (a & 0x80) != 0;
    const int e = a & 0x7f;
    if (e >= (in ? inTicks : kBandAnimOut)) return in ? 1.0 : 0.0;
    // ease-in-out, the exponent GD's tween carries
    const double u = (double)e / (in ? inTicks : kBandAnimOut);
    const double rate = in ? 2.0 : 1.5;
    const double t = 2.0 * u;
    const double e01 = t < 1.0 ? 0.5 * std::pow(t, rate)
                               : 1.0 - 0.5 * std::pow(2.0 - t, rate);
    return in ? e01 : 1.0 - e01;
}
struct FlyBand { double floorY = 0.0, ceilY = 1e9; };
inline FlyBand bandFor(double cy, double H) {
    FlyBand b;
    b.floorY = std::max(90.0, std::floor((cy - H * 0.5) / 30.0) * 30.0);
    b.ceilY = b.floorY + H;
    return b;
}
// The invisible ceiling is NOT ship-only. Measured on lv9: a flipped ball taps
// off the ground at x=9,212, rises, and at x=9,354 stops dead at y=315.00 with
// onGround=1 -- its top is 330 -- then rides that for 460 px straight over the
// spiked wall at x=9,480 that has no other way past. Only TWO 30 px blocks
// exist in that stretch, so it is not riding geometry.
// Same story as the ship's: per level, not derivable from the geometry. Default
// is "none"; `--ceil` sets it for levels that need it (lv9 = 330).
//
// It is not one number per level either. lv14 is built high (its ship corridors
// are at y=735 and its geometry reaches 1065), and its ball section at
// x=13,100..13,600 has a ceiling at 420: a flipped mini ball stops dead at
// y=411.000 with onGround=1 and rides it, and a ball injected ABOVE it at
// y=500 is put straight back to 411. A single global 420 would nail the cube to
// the floor of the pad corridor at x=9,165, which GD plays at y=700..735. So
// the knob carried an optional x window: `--ceil 420@13100:13600`, repeatable,
// and a bare `--ceil 330` meant "everywhere" (lv9).
//
// [2026-08-31] THE FLAG IS GONE AND THIS LIST IS ALWAYS EMPTY, so playerCeilAt
// returns 1e9 at every x and the two sites in step.hpp that call it are asking
// about a ceiling nobody set. Nothing in the pipeline had been passing --ceil,
// so lv9 and lv14 have been solving WITHOUT the numbers above for as long as
// anyone has measured -- the readings are real, but the paragraph reads as
// though they were in force, and they were not. Kept, unchanged, as the record
// of two measurements and as dead code that is easy to remove on purpose.
struct CeilBand {
    double y;
    double x0, x1;   // inclusive; -inf..+inf for a bare value
};
inline std::vector<CeilBand> g_playerCeils;
inline double playerCeilAt(double x) {
    double best = 1e9;
    for (const CeilBand& b : g_playerCeils)
        if (x >= b.x0 && x <= b.x1 && b.y < best) best = b.y;
    return best;
}
// Support persistence past a block's edge. This is just the hitbox: the cube
// stays grounded while its 30-wide box still overlaps the block, i.e. while
// |x - cx| <= hw + 15.0. The old 13.0 was a deliberate under-estimate chosen
// when the measurements looked contradictory, at a documented cost of "up to
// ~2 ticks" of fall-off error -- and that error is exactly what kept flipping
// lv4/lv5 onto the wrong side of a ledge.
//
// Measured directly by hooking GD (data/cmp_lv5.gdsnap, block cx=10995):
//   t=8481  x=11025.0000  sd=30.0000  <- STILL in contact
// so the comparison is INCLUSIVE at hw + 15.0, not strict. cocos2d's rect
// intersection treats touching edges as intersecting, which matches.
//
// The reach IS the player's half width, so the two tests read it from pHalf and
// this constant now only records the normal-size value the measurement pinned.
// Confirmed for mini on lv11: standing on the block that ends at x=5010, GD is
// still grounded at t=3864 (x=5018.33, 8.33 px past the edge) and starts
// applying gravity at t=3865 (x=5019.63, 9.63 px past). The boundary lies in
// (8.33, 9.63] and the mini half is 9.0 -- same rule, not a new constant.
// With 15 the model stood ~5 px of x too long and then "jumped" off a ledge GD
// had already dropped it from, which is why its trace and GD's split there.
constexpr double kSupportReach = 15.0;
// TRIED AND REVERTED (2026-07-31): 0.01 here, to absorb the ~0.003 px by which
// the model's float x misses GD's. The motivating case was real -- lv5 t=8482
// sits at |x - cx| = 30.0000 exactly, GD stays grounded, and a 0.003 px
// overshoot let go one tick early. But widening EVERY contact test by 0.01 px
// broke all five levels at once (lv1/2/3 went from cleared to stuck). Ledge
// boundaries are dense enough that a uniform bias walks the cube one tick too
// far again and again. Fix the x reproduction instead of padding the test.
constexpr double kContactEps = 0.0;

}  // namespace dp
