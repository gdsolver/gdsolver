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

    // Velocity above which the "weak" branch is taken: 2 x GD's gravity for the
    // speed (accelSwitchVyForSpeed). These two defaults are the 0.9 row written
    // out in full -- a default member initializer cannot call the lookup, and
    // the two must agree TO THE BIT or a state that went through withSpeed(0.9)
    // would branch differently from one that did not. The old bracket-midpoint
    // 1.9165 sat 0.0001 above it, which no velocity on GD's 0.001 grid can tell
    // apart.
    double accelSwitchVy = 1.9163980484008789;
    // [2026-08-21] THE FLIPPED-SIDE THRESHOLD IS NOT NECESSARILY -accelSwitchVy.
    // [2026-09-01] It is, though. The listing's flipped arm compares against the
    // same per-speed g summed in double rather than in float -- a 1e-8
    // difference -- and it ignores the dual flag that negates the upright one.
    // The separately measured 0.7 row is gone (see accelSwitchVyFlipForSpeed).
    double accelSwitchVyFlip = -1.9163980484008789;

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
        // NOT scaled by the mini factor -- the same absolute value is used, and
        // the listing agrees (playerIsFallingBugged reads the speed's gravity
        // and nothing else; no size term appears in it). The default carries the
        // same 0.9 value, so this line only repeats it.
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
    // [2026-09-01] THERE IS A FORMULA AFTER ALL, and it is not a fitted curve:
    // the threshold is 2 x GD's own per-speed gravity, read out of
    // playerIsFallingBugged (see accelSwitchVyForSpeed in models/speed.hpp).
    // The note that used to stand here -- "kept as a lookup on purpose, a curve
    // through two points would be an invention" -- was right to refuse a curve:
    // the true sequence 1.8804 / 1.9164 / 1.9144 / 1.9224 is not monotone, which
    // is exactly why no curve fitted it. It is a table in GD too; the table is
    // just one level down, in the gravity rather than in the threshold.
    // Every bracket below still contains its value. The measurements were right
    // and are kept as the record; what changes is that four of them no longer
    // need a midpoint.
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
        // 1.8803980 / 1.9163980 / 1.9143980 / 1.9223980, against the brackets
        // recorded above and below: 0.7 "T < 1.915", 0.9 (1.916, 1.917),
        // 1.1 (1.912, 1.915), 1.3 (1.914, 1.931] -- all four contain it.
        return gdapprox::accelSwitchVyForSpeed(playerSpeed);
    }

        // [2026-08-30] 1.3 AND 1.6 WERE MEASURED HERE, and the value they were
        // given (1.9165, inherited from 0.9) is the ONE ROW THE FORMULA MOVES.
        // calib_speedcal_ship_s3: hold the input, release, and the release
        // acceleration switches from -0.103 to -0.069 somewhere between two
        // samples 0.103 apart -- too coarse on its own. Varying the HOLD LENGTH
        // moves the sampled grid by 0.086-0.103 = -0.017 a time, so eight holds
        // (40..47) give eight brackets that intersect:
        //     hold 40 (1.879, 1.982]   hold 44 (1.914, 2.017]
        //     hold 41 (1.862, 1.965]   hold 45 (1.897, 2.000]
        //     hold 42 (1.845, 1.948]   hold 46 (1.880, 1.983]
        //     hold 43 (1.828, 1.931]   hold 47 (1.863, 1.966]
        //                    all eight -> T in (1.914, 1.931]
        // All eight are mutually consistent, and 0.017 is the beat of the two
        // accelerations, i.e. the floor of this method. The bracket EXCLUDES
        // 1.885 (the 0.7 row) and 1.9135 (the 1.1 row) -- and it contains BOTH
        // 1.9165 and 1.9223980, which is why taking the value it shared with 0.9
        // looked settled. A finer instrument separates them: injecting vy on a
        // mini ship at lv15 t=12,701 (sp1.3, released) reads weak at 1.917 and
        // 1.922 and strong at 1.923, i.e. T in (1.922, 1.923], which excludes
        // 1.9165 and contains 2g(1.3) = 1.9223980. Six grid velocities
        // (1.917..1.922) were taking the wrong branch at every 1.3 and 1.6 tick.
        // 1.6 shares it: calib_speedcal_ship_s4 reproduces the 3x run's y, vy
        // and dvy tick for tick -- only dx differs (2.400 against 1.950). The
        // ship's vertical physics does not read the speed band at all here, and
        // GD's own gravity table gives 1.3 and 1.6 one shared row.

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
    // [2026-09-01] The 0.7 row's -1.8795 is gone: the mirror is exact in the
    // listing (the flipped arm sums the same per-speed g, in double instead of
    // float), and -2g(0.7) = -1.8803980 sits inside the overlap (-1.881, -1.878)
    // that midpoint was taken from. One less separately measured number.
    static double accelSwitchVyFlipForSpeed(double playerSpeed) {
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
