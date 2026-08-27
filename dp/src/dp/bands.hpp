#pragma once
#include "dp/progress.hpp"   // which includes constants.hpp -- the chain's next link

namespace dp {

// ...and the whole thing turned out to be ONE function in GD, read straight out
// of the exe (docs/findings-ceiling.md). `GJBaseGameLayer::checkCollisions`
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
// with the ceilprobe build's pmin/pmax columns (docs/findings-ceiling.md):
//   cube in dual  [360,630] = H 270   ship in dual [330,630] = H 300
//   ufo  in dual  [390,690] = H 300   ball in dual [390,660] = H 270
inline double bandHeightDual(int mode) {
    return (mode == 1 || mode == 3 || mode == 4) ? kBandFly : kBandOther;
}
// --startband <floor>,<ceil>: the anchor tick's band, read out of GD's dump
// (pmin/pmax = getMinPortalY / getMaxPortalY) instead of guessed from x.
inline bool g_startBandSet = false;
inline double g_startBandFloor = 0.0, g_startBandCeil = 1e9;
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
struct BandTrackRow { int t; float fl, ce; };
inline std::vector<BandTrackRow> g_bandTrack;
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
    if (g_bandTrack.empty()) return false;
    size_t lo = 0, hi = g_bandTrack.size();
    while (lo + 1 < hi) {
        const size_t m = (lo + hi) / 2;
        if (g_bandTrack[m].t <= t) lo = m; else hi = m;
    }
    if ((long long)g_bandTrack[lo].t > t) return false;
    fl = (double)g_bandTrack[lo].fl;
    ce = (double)g_bandTrack[lo].ce;
    return true;
}
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
inline int g_bandTrackCam = -1;
inline bool bandTrackIsCamera() {
    if (g_bandTrackCam < 0) {
        if (g_bandTrack.size() < 8) {
            g_bandTrackCam = 0;          // too short to judge; as before
        } else {
            size_t off = 0;
            for (const BandTrackRow& r : g_bandTrack) {
                const double c = (double)r.ce;
                if (std::fabs(c - std::round(c / 30.0) * 30.0) >= 0.01) ++off;
            }
            g_bandTrackCam = (off * 2 > g_bandTrack.size()) ? 1 : 0;
            std::printf("bandtrack: %zu rows, %zu off-grid -> %s\n",
                        g_bandTrack.size(), off,
                        g_bandTrackCam ? "CAMERA-driven (no physical ceiling)"
                                       : "portal-set (the ceiling is a wall)");
        }
    }
    return g_bandTrackCam != 0;
}
// Calibration factor for the band height (height asked of GD via --startband / the
// value computed from the zoom). 0 = uncalibrated. Fixed once, at the first tick
// where the band is evaluated (= the anchor).
inline double g_bandK = 0.0;
inline bool g_slopeDbg = false;  // --slopedbg: one line per ramp acquisition
inline bool g_bandDbg = false;   // --banddbg: one line per wave tick near the band ceiling
inline bool g_shipCeilSet = false;   // --shipceil given: pin the band by hand
// --rotport: treat a portal that a trigger ROTATES as a turned object even
// while its angle still reads 0, so it is tested with the two-box SAT against
// the player's turned box. Measured right (see Dynamics::turnedBox) but OFF by
// default: on, it cost lv22's cold run 41% -> 15%. Declared here rather than
// with the other knobs in thread_pool.hpp because dynamics.hpp reads it and
// comes earlier in the header chain.
inline bool g_rotPort = false;
// --rotlast: adopt the LAST 2900 that matches on a tick rather than the nearest
// one on the perpendicular axis. That is the pre-0a1e8b6 rule; with
// --rotperp 30 it restores that function exactly, which is how the 84% cold run
// of 08-24 is reproduced on a current build.
inline bool g_rotLast = false;
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
// the knob carries an optional x window: `--ceil 420@13100:13600`, repeatable,
// and a bare `--ceil 330` still means "everywhere" (lv9).
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
