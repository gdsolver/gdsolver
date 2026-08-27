// Measured UFO constants.
//
// Recovered the same way as the Ship constants (src/tools/calibrate.cpp).
// UFO free flight turns out to share Ship's structure exactly: two gravity
// values selected by the SAME velocity threshold, and the same 1/240 s tick.
//
//     s  = gravityFlipped ? -accelSwitchVy : +accelSwitchVy
//     a  = (vy <= s) ? gravityWeak : gravityStrong
//     vy = max(vy + a, vyMinPlayerFrame)
//     y += yScale * vy
//
// A flap OVERWRITES the velocity; it does not add to it:
//
//     vy = flapVy                                  (independent of vy_in)
//
// Holding the button does not repeat the flap: one press = one flap.
#pragma once

#include "models/speed.hpp"

namespace gdapprox {

struct UfoParams {
    // Player-frame gravity, per tick. Negative: UFO has no thrust, only flaps.
    double gravityWeak = -0.086;    // vy <= switch
    double gravityStrong = -0.129;  // vy >  switch

    // Same value as ShipParams::accelSwitchVy. Measured brackets:
    //   normal (1.840, 1.969]   mini (1.784, 1.936]
    // Both contain 1.9165, so a single global constant is used.
    double accelSwitchVy = 1.9165;

    // Velocity immediately after a flap. Measured as an exact constant over a
    // 13.1-wide range of incoming velocity: the map is vy_out = 0*vy_in + b.
    double flapVy = 6.871;

    double vyMinPlayerFrame = -6.4;
    double yScale = 0.225;
    double dxPerTick = 1.29825;

    static UfoParams normal() { return UfoParams{}; }

    static UfoParams mini() {
        UfoParams p;
        p.gravityWeak = -0.101;   // = -0.086 / 0.85, rounded to 3 dp
        p.gravityStrong = -0.152; // = -0.129 / 0.85, rounded to 3 dp
        p.accelSwitchVy = 1.9165; // NOT scaled, same as Ship
        // Measured 6.648. Note this is SMALLER than the normal-size flap,
        // the opposite direction from the Ship mini scaling. Not a 1/0.85
        // relationship in either direction; treated as an independent value.
        p.flapVy = 6.648;
        p.vyMinPlayerFrame = -7.529;  // measured 3-decimal constant, not -6.4/0.85
        return p;
    }

    static UfoParams forSize(bool mini_) { return mini_ ? mini() : normal(); }

    // As with the ship, only the x advance depends on m_playerSpeed.
    UfoParams withSpeed(double playerSpeed) const {
        UfoParams p = *this;
        p.dxPerTick = dxPerTickForSpeed(playerSpeed);
        return p;
    }
};

}  // namespace gdapprox
