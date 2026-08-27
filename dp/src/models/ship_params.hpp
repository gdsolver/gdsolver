// Measured Ship constants.
//
// Every number here was recovered from GD state dumps by
// src/tools/calibrate.cpp; none of it is guessed. See docs/model-spec.md for
// the derivation and docs/findings.md for the measurement log.
//
// Update rule (player frame, "up" positive; the world frame is obtained by
// multiplying by -1 when gravity is flipped):
//
//     s       = gravityFlipped ? -accelSwitchVy : +accelSwitchVy
//     a       = held ? (vy <= s ? holdStrong : holdWeak)
//                    : (vy >  s ? releaseStrong : releaseWeak)
//     vy[t+1] = clamp(vy[t] + a, vyMinPlayerFrame, vyMaxPlayerFrame)
//     y [t+1] = y[t] + yScale * vy[t+1]          // note: the NEW velocity
//     x [t+1] = x[t] + dxPerTick
//
// The sign flip on the switch threshold is NOT a player-frame mirror: under
// flipped gravity GD compares against the same *world*-frame velocity, so the
// branch boundary lands on the opposite side in the player frame. This is
// measured, not assumed -- see docs/findings.md "gravity-flipped asymmetry".
#pragma once

#include "models/speed.hpp"

namespace gdapprox {

struct ShipParams {
    // Player-frame accelerations, per 1/240 s tick.
    double holdStrong = 0.108;
    double holdWeak = 0.086;
    double releaseStrong = -0.103;
    double releaseWeak = -0.069;

    // Velocity above which the "weak" branch is taken. The observed data
    // brackets this to [1.916, 1.917]; the midpoint is used.
    double accelSwitchVy = 1.9165;
    // [2026-08-21] THE FLIPPED-SIDE THRESHOLD IS NOT NECESSARILY -accelSwitchVy.
    // As the header note says, it is "not a player-frame mirror", so it is kept
    // as a separately measured value. The default stays -accelSwitchVy as
    // before; the per-speed table is `accelSwitchVyFlipForSpeed`.
    double accelSwitchVyFlip = -1.9165;

    double vyMaxPlayerFrame = 8.0;
    double vyMinPlayerFrame = -6.4;

    // y[t+1] = y[t] + yScale * vy[t+1]. Exact to 5 decimals over a 32-tick
    // free-fall baseline; consistent with PlayerObject::update receiving
    // dt = 0.25 (60 fps relative) and GD integrating position after velocity.
    double yScale = 0.225;

    // Horizontal advance per tick. This is the ONLY quantity that depends on
    // m_playerSpeed -- see dxPerTickForSpeed() and docs/findings.md finding 19.
    double dxPerTick = 1.29825;

    static ShipParams normal() { return ShipParams{}; }

    // Mini ship. Velocity caps are exactly the normal caps / 0.85; the
    // accelerations are close to that but are stored to 3 decimals and do not
    // all round consistently, so they are kept as separately measured values.
    static ShipParams mini() {
        ShipParams p;
        p.holdStrong = 0.127;
        p.holdWeak = 0.101;
        p.releaseStrong = -0.122;
        p.releaseWeak = -0.081;
        // Measured as printed 3-decimal constants, NOT as exact 1/0.85 values.
        // 8.0/0.85 = 9.411765 would print as "9.41176" at the dump's 6
        // significant digits; the dump prints "9.412". Same for -7.529 vs
        // -7.52941. GD evidently stores the mini constants pre-rounded, which
        // is consistent with the accelerations also being 3-decimal.
        p.vyMaxPlayerFrame = 9.412;
        p.vyMinPlayerFrame = -7.529;
        // Measured independently: the mini bracket is (1.905, 1.924], which
        // contains the normal-size bracket. The switch velocity is therefore
        // NOT scaled by the mini factor -- the same absolute value is used.
        p.accelSwitchVy = 1.9165;
        return p;
    }

    static ShipParams forSize(bool mini_) { return mini_ ? mini() : normal(); }

    // Everything except dxPerTick and accelSwitchVy is speed-invariant.
    //
    // accelSwitchVy is NOT speed-invariant, which is what the old comment here
    // assumed. Measured on lv18 with 1-tick injections (y held fixed, only vy
    // replaced, read the next tick's dvy: 0.127 = strong, 0.101 = weak), mini
    // ship both times:
    //   speed 1.1, t=14,520, upright,  onGround 0: 1.91 strong / 1.92 weak
    //   speed 0.7, t=15,287, upright,  onGround 1: 1.88 strong / 1.89 weak
    //   speed 0.7, t=15,350, FLIPPED,  onGround 0: 1.90 strong / 1.88 weak
    // The two 0.7 points agree (T in (1.880, 1.890)) across flip AND onGround,
    // so neither of those is the axis -- the speed is. 1.1 keeps the long-
    // standing 1.9165 (its bracket contains it).
    //
    // Kept as a per-speed lookup rather than a formula on purpose: GD's other
    // speed-dependent constants are lookups with non-monotonic values too (see
    // leveldp's cubePhysFor -- jump 11.180 / 11.420 / 11.230 for 0.9 / 1.1 /
    // 1.3), so a fitted curve through two points would be an invention. 1.3 and
    // 1.6 are UNMEASURED and keep the old value.
    // [2026-08-21] 1.1 WAS NOT 1.9165. Sorting the ship ticks of all 22 gdref
    // levels by "the previous tick's vy" against "that tick's dvy" makes the
    // strong/weak boundary come out clearly per speed (mini, upright):
    //   sp0.7  weak 1.915 / 1.917 / 1.921 / 1.924  -> T < 1.915 (1.885 left as is)
    //   sp0.9  strong 1.909 / 1.916   weak 1.919 / 1.922 -> T in (1.916, 1.919)
    //   sp1.1  strong 1.905 / 1.912   weak 1.915 / 1.922 -> T in (1.912, 1.915)
    // On the normal-size side, sp0.9 gives strong 1.916 / weak 1.917 =
    // (1.916, 1.917) (as the note above says), and sp1.1 is the loose
    // (1.899, 1.917), which contains 1.9135.
    // Intersecting with the injection above (lv18 t=14,520, sp1.1: "1.91 strong
    // / 1.92 weak") makes sp1.1 (1.912, 1.915). Take the midpoint 1.9135.
    // Real damage: at lv15 t=6,238 (mini sp1.1, vy=1.915) only the model took
    // strong, and 0.026/tick accumulated over 17 ticks into census
    // `m1/mini1/.../clamp:fly/ceilride/uid3211` edy +0.432.
    static double accelSwitchVyForSpeed(double playerSpeed) {
        if (playerSpeed < 0.8) return 1.885;    // 0.7  injection on lv18
        if (playerSpeed < 1.0) return 1.9165;   // 0.9  (1.916, 1.917)
        if (playerSpeed < 1.2) return 1.9135;   // 1.1  (1.912, 1.915)
        return 1.9165;                          // 1.3 / 1.6 UNMEASURED
    }

    // [2026-08-21] The flipped-side threshold (this includes p1 of a dual).
    // Measured by sorting gdref's mini ships by "player-frame vp" against "that
    // tick's dvy":
    //   sp0.7 (dual, upward p1)
    //       release: strong >= -1.840 / weak <= -1.881 -> T in (-1.881, -1.840)
    //       hold   : strong <= -1.892 / weak >= -1.878 -> T in (-1.892, -1.878)
    //       THE ONLY OVERLAP OF THE TWO INTERVALS IS (-1.881, -1.878) -> take -1.8795.
    //       (-1.885 is outside the release-side interval. Setting -1.86 first put
    //        the hold side outside and drifted lv16 t=19,7xx by 0.026/tick --
    //        look at BOTH)
    //   sp0.9 (flipped, non-dual)  strong >= -1.830 / weak <= -1.918 -> contains -1.9165
    //   sp1.1 (flipped, non-dual)  strong >= -1.870 / weak <= -1.992 -> contains -1.9165
    // Real damage: at lv16 t=18,363 (dual mini ship sp0.7, vp=-1.881) only the
    // model took releaseStrong, and 0.041/tick accumulated into census
    // `m1/mini1/.../sp0.7/clamp:fly/land/uid9416/air` edvy -5.387.
    static double accelSwitchVyFlipForSpeed(double playerSpeed) {
        if (playerSpeed < 0.8) return -1.8795;  // 0.7  (-1.881, -1.878)
        return -accelSwitchVyForSpeed(playerSpeed);
    }

    ShipParams withSpeed(double playerSpeed) const {
        ShipParams p = *this;
        p.dxPerTick = dxPerTickForSpeed(playerSpeed);
        p.accelSwitchVy = accelSwitchVyForSpeed(playerSpeed);
        p.accelSwitchVyFlip = accelSwitchVyFlipForSpeed(playerSpeed);
        return p;
    }
};

// A plan command at tick k first affects the velocity transition observed at
// dump tick k+2. Measured: 0 mismatches in 7,485 labelled ship ticks at this
// offset, 36+ at any other. See docs/findings.md "input latency".
inline constexpr int kInputLatencyTicks = 2;

}  // namespace gdapprox
