#pragma once

#include "core/state.hpp"
#include "geometry/corridor.hpp"
#include "models/ufo_params.hpp"

namespace gdapprox {

// UFO control is an event stream, not a level. `ControlSegment::held == true`
// means "a flap is issued on the FIRST tick of this segment"; the remaining
// ticks of the segment are coast. Holding does not repeat the flap, which is
// what GD does (one press = one flap), so a segment is exactly "flap, then
// wait `ticks` ticks".
class UfoModel final : public IApproxModel {
public:
    UfoModel() = default;

    ApproxResult simulate(const ApproxState& start,
                          const std::vector<ControlSegment>& controls,
                          const ICorridor* corridor,
                          const SimulateOptions& options) const override;
    using IApproxModel::simulate;

    // `boostLatch` is GD's velocity-limit exemption, the byte [player+0x952]
    // (dp/state.hpp State::boost, dp/slopes.hpp boostLatchMode). Unlike the
    // ship's, the UFO's acceleration block (updateJump 0x38c701-0x38c8df)
    // never reads it -- a byte scan of the whole flying branch finds the four
    // references at 0x38c59e (the clear), 0x38c5be / 0x38c5d8 (the ship) and
    // 0x38ca9f (the clamp) and no other -- so for the UFO the latch does one
    // thing only: it skips the terminal clamp.
    static double stepVy(double vy, bool flap, const UfoParams& p,
                         bool gravityFlipped = false, bool boostLatch = false) {
        // A flap RAISES vy to the target and then the same call's gravity step
        // runs, which is where the old constant 6.871 came from. It is not an
        // overwrite: GD (PlayerObject::updateJump) only calls setYVelocity when
        // vy is below s*literal, so a UFO already climbing faster than the
        // target keeps its speed and merely spends the press.
        if (flap && vy < p.flapTargetVy) vy = p.flapTargetVy;
        const double s = gravityFlipped ? -p.accelSwitchVy : p.accelSwitchVy;
        const double a = (vy <= s) ? p.gravityWeak : p.gravityStrong;
        double next = vy + a;
        // Both ends of GD's band, as the ship has had all along. Without the
        // rise side a UFO thrown upwards by an orb or a pad kept climbing at a
        // speed the game does not allow (see vyMaxPlayerFrame for the
        // measurement and for the exemption this does not model).
        if (boostLatch) return next;   // 0x38ca9f: the clamp block is jumped
        if (next > p.vyMaxPlayerFrame) next = p.vyMaxPlayerFrame;
        return next < p.vyMinPlayerFrame ? p.vyMinPlayerFrame : next;
    }

    const UfoParams& params(bool mini) const { return mini ? mini_ : normal_; }
    void setParams(bool mini, const UfoParams& p) { (mini ? mini_ : normal_) = p; }

private:
    UfoParams normal_ = UfoParams::normal();
    UfoParams mini_ = UfoParams::mini();
};

}  // namespace gdapprox
