// m_playerSpeed -> horizontal advance per tick.
//
// Measured on levels 18 and 20 with an isolated GD worker (measurement M2).
// Official levels 1-14 contain no speed portal at all, which is why every
// dump recorded before 2026-07-26 ran at 0.9 and this table could not be
// built from the existing data.
//
// The result that matters for the model: the accelerations, the velocity caps,
// the UFO flap constant and the 0.225 position scale are all identical at 0.7,
// 0.9 and 1.1.
//
// ...but the BRANCH THRESHOLD IS NOT, and the old wording here ("speed changes
// nothing else", "the 1.9165 branch threshold") was wrong about it. GD holds a
// per-speed gravity at player+0x7c8 and the accel switch is twice that value;
// the flight modes and the ball then compute their accelerations from a FIXED
// 0.958199 instead of reading it back, which is how one number can be
// speed-invariant while the threshold built from it is not. See
// accelSwitchVyForSpeed below.
#pragma once

#include <cmath>
#include <cstddef>

namespace gdapprox {

struct SpeedEntry {
    double playerSpeed;
    double dxPerTick;
    bool measured;  // false = interpolated/unknown, see below
};

// Measured over baselines of 780-891 ticks; the quoted uncertainty is the
// dump's 6-significant-digit x divided by the baseline length, ~1e-5 px/tick.
inline constexpr SpeedEntry kSpeedTable[] = {
    {0.7, 1.046386, true},   // 251.13 px/s, lv18/lv20, 891-tick baseline
    {0.9, 1.298250, true},   // 311.58 px/s, lv1,    780-tick baseline
    {1.1, 1.614270, true},   // 387.42 px/s, lv18/lv20, 2527-tick baseline
    {1.3, 1.950199, true},   // 468.05 px/s, lv20,   906-tick baseline
    // No official level 1-21 contains a 1.6 portal, so this one is the round
    // number the others are near, not a measurement. Callers are told.
    {1.6, 2.400000, false},
};

// Returns the advance per tick, and sets `measured` to false when the caller
// is relying on an assumed value. Callers that care about correctness on a
// level with a 1.3 or 1.6 portal must check it.
inline double dxPerTickForSpeed(double playerSpeed, bool* measured = nullptr) {
    const SpeedEntry* best = &kSpeedTable[0];
    double bestErr = std::fabs(playerSpeed - best->playerSpeed);
    for (const SpeedEntry& e : kSpeedTable) {
        const double err = std::fabs(playerSpeed - e.playerSpeed);
        if (err < bestErr) {
            bestErr = err;
            best = &e;
        }
    }
    if (measured != nullptr) *measured = best->measured && bestErr < 1e-6;
    return best->dxPerTick;
}

// The acceleration switch is not a stored constant: it is TWICE THE PLAYER'S
// GRAVITY for the current speed.
//
//   PlayerObject::playerIsFallingBugged @ base+0x39a430, upright arm:
//       return vy < (double)((float)g + (float)g);
//
// with g the per-speed double at player+0x7c8, written by updateTimeMod
// (base+0x3a0d50): 0.7 -> 0.940199, 0.9 -> 0.958199024, 1.1 -> 0.957199,
// 1.3 and 1.6 -> 0.961199 (the two fast speeds share one row in GD's own
// table). The sum is done in FLOAT on this arm, so the cast below is part of
// the value and not a tidy-up.
//
// The flipped arm sums in double instead and the difference lands at 1e-8,
// nowhere near vy's 0.001 grid, so the model mirrors this one value.
// A DUAL's first body negates the threshold (player+0xa99) rather than taking a
// different one, which is what the model spells "dual => flipped threshold".
//
// This replaces four separately measured constants. Every one of the brackets
// they were taken from CONTAINS the value below -- 0.7's "T < 1.915", 0.9's
// (1.916, 1.917), 1.1's (1.912, 1.915), 1.3's (1.914, 1.931], the UFO rig's
// three intervals and the mini UFO's (1.905, 1.915) -- so nothing measured is
// being overruled; the formula is simply sharper than the beat of the two
// accelerations that bracketed them. The one that was actually WRONG is 1.3/1.6
// (1.9165 for 1.9223980): an injection at lv15 t=12,701 brackets it to
// (1.922, 1.923] and rules the old value out.
// CUT, not nearest-match. GD's own multiplier for the "1.3x" portal reads
// **1.2** in the dump, which is equidistant from the 1.1 and 1.3 rows -- a
// nearest-match lookup resolves that tie by table order and silently returns the
// wrong row. dp/speed.hpp's dxForSpeedMul cuts at the same boundaries for the
// same reason; this follows it.
inline double accelSwitchVyForSpeed(double playerSpeed) {
    double g;
    if (playerSpeed < 0.8) g = 0.940199;            // 0.7
    else if (playerSpeed < 1.0) g = 0.958199024;    // 0.9
    else if (playerSpeed < 1.2) g = 0.957199;       // 1.1
    else g = 0.961199;                              // 1.3 and 1.6, one row in GD
    // float(g) + float(g), exactly as the listing does it. Doubling a float is
    // exact, so this is the same number GD compares against:
    //   0.7 -> 1.8803980  0.9 -> 1.9163980  1.1 -> 1.9143980  1.3/1.6 -> 1.9223980
    const double gf = static_cast<double>(static_cast<float>(g));
    return gf + gf;
}

}  // namespace gdapprox
