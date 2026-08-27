// m_playerSpeed -> horizontal advance per tick.
//
// Measured on levels 18 and 20 with an isolated GD worker (measurement M2).
// Official levels 1-14 contain no speed portal at all, which is why every
// dump recorded before 2026-07-26 ran at 0.9 and this table could not be
// built from the existing data.
//
// The result that matters for the model: **speed changes nothing else.**
// The accelerations, the velocity caps, the 1.9165 branch threshold, the UFO
// flap constant and the 0.225 position scale are all identical at 0.7, 0.9 and
// 1.1. Only the x advance changes. So a speed portal is a one-field update,
// not a different physics regime.
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

}  // namespace gdapprox
