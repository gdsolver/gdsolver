// Measured UFO constants.
//
// Recovered the same way as the Ship constants (src/tools/calibrate.cpp).
// UFO free flight turns out to share Ship's structure exactly: two gravity
// values selected by the SAME velocity threshold, and the same 1/240 s tick.
//
//     s  = gravityFlipped ? -accelSwitchVy : +accelSwitchVy
//     a  = (vy <= s) ? gravityWeak : gravityStrong
//     vy = clamp(vy + a, vyMinPlayerFrame, vyMaxPlayerFrame)
//     y += yScale * vy
//
// A flap RAISES the velocity to a target; it neither adds to it nor overwrites
// it, and it does not slow a UFO that is already climbing faster:
//
//     if (vy < flapTargetVy) vy = flapTargetVy     (then the same tick's gravity)
//
// Holding the button does not repeat the flap: one press = one flap.
#pragma once

#include "models/speed.hpp"

namespace gdapprox {

struct UfoParams {
    // Player-frame gravity, per tick. Negative: UFO has no thrust, only flaps.
    double gravityWeak = -0.086;    // vy <= switch
    double gravityStrong = -0.129;  // vy >  switch

    // Same value as ShipParams::accelSwitchVy, and now known to be the same
    // MECHANISM: playerIsFallingBugged is one function and every mode calls it,
    // so the UFO's threshold is 2 x the speed's gravity too (models/speed.hpp).
    // The old brackets -- normal (1.840, 1.969], mini (1.784, 1.936] -- and the
    // rig's per-speed ones all contain the formula's values; what they could not
    // do is separate the speeds, which is why this stood as one global constant
    // with two hand-placed exceptions in step.hpp's ufoParamsFor.
    // This default is the 0.9 row, bit for bit what withSpeed(0.9) returns.
    double accelSwitchVy = 1.9163980484008789;

    // What a flap raises vy TO, before the same call's gravity step. The
    // literal GD holds (PlayerObject::updateJump, base+0x38b900): 7.0 at full
    // size, 8.0 at mini, times the flight-mode size factor s (1.0 / 0.85).
    //
    // The measurement this replaces -- "vy_out = 0*vy_in + b with b = 6.871
    // over a 13.1-wide range of incoming velocity" -- was taken entirely below
    // the target, where the map really is constant. Above it there is no flap
    // at all, which is the case the constant form got wrong.
    double flapTargetVy = 7.0;

    // The rise cap. The UFO shares the ship's band -- GD clamps every flight
    // mode into (-6.4/f, +8.0/f) with f the flight-mode size factor -- and the
    // model had the fall side only, so nothing stopped a UFO that an orb, a pad
    // or a boost had thrown upwards from climbing at that speed forever.
    // Measured in the game (2026-09-01, lv20 t=16,046): vy injected at 10.0
    // reads 8.0 one tick later and 7.742 = 8.0 - 2 x 0.129 two ticks after that.
    //
    // GD SKIPS THIS CLAMP WHILE THE VELOCITY-LIMIT EXEMPTION (player+0x952) IS
    // SET -- slope launch, red orb, red pad -- and clears the flag again as soon
    // as vy is back inside the band. The model's exemption state (State::boost)
    // is swing-scoped, so this clamp is unconditional exactly as the ship's is;
    // the injection above was taken with the flag clear, so it measures the
    // clamp and not its gate.
    double vyMaxPlayerFrame = 8.0;
    double vyMinPlayerFrame = -6.4;
    double yScale = 0.225;
    double dxPerTick = 1.29825;

    static UfoParams normal() { return UfoParams{}; }

    static UfoParams mini() {
        UfoParams p;
        p.gravityWeak = -0.101;   // = -0.086 / 0.85, rounded to 3 dp
        p.gravityStrong = -0.152; // = -0.129 / 0.85, rounded to 3 dp
        // accelSwitchVy is NOT scaled by the mini factor -- same as the ship,
        // and the listing has no size term in it. The default already carries
        // the value, so there is nothing to set here.
        // 0.85 x 8.0. The measured 6.648 was SMALLER than the normal-size
        // 6.871 and in neither direction a 1/0.85 relationship, so the two
        // stood here as independent values; the mechanism is a different
        // literal (8.0, not 7.0) multiplied by the same size factor that
        // divides the ship's, and the post-gravity values follow from it
        // exactly: 7.0 - 0.129 = 6.871 and 6.8 - 0.152 = 6.648, both equal to
        // the old constants to the last bit.
        p.flapTargetVy = 6.8;
        // Both caps are the printed 3-decimal constants, not the exact
        // 8.0/0.85 = 9.411765 and -6.4/0.85 = -7.529412 -- the same pair the
        // ship's mini carries, and read the same way (the dump prints 9.412 and
        // -7.529 at six significant digits).
        p.vyMaxPlayerFrame = 9.412;
        p.vyMinPlayerFrame = -7.529;
        return p;
    }

    static UfoParams forSize(bool mini_) { return mini_ ? mini() : normal(); }

    // The velocity a flap leaves behind ONCE THE SAME TICK'S GRAVITY HAS RUN --
    // 6.871 full, 6.648 mini, the values that were measured. gravityStrong is
    // the right step because the target is always above accelSwitchVy. For a
    // caller that has already integrated (the portal re-issue in step.hpp) this
    // is both the value to write and the threshold to compare against: testing
    // a post-gravity vy against a post-gravity target gives the same verdict as
    // GD's own pre-gravity test, since the two differ by the same step in every
    // case where the answer is in doubt.
    double flapPostVy() const { return flapTargetVy + gravityStrong; }

    // As with the ship: the x advance AND the accel-switch threshold depend on
    // m_playerSpeed. The threshold used to be missing here, which is why
    // step.hpp's ufoParamsFor carried hand-built statics for the two speeds
    // somebody had measured -- and nothing at all for 1.3 and 1.6.
    UfoParams withSpeed(double playerSpeed) const {
        UfoParams p = *this;
        p.dxPerTick = dxPerTickForSpeed(playerSpeed);
        p.accelSwitchVy = accelSwitchVyForSpeed(playerSpeed);
        return p;
    }
};

}  // namespace gdapprox
