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

    static double stepVy(double vy, bool flap, const UfoParams& p,
                         bool gravityFlipped = false) {
        if (flap) return p.flapVy;  // overwrite: independent of vy
        const double s = gravityFlipped ? -p.accelSwitchVy : p.accelSwitchVy;
        const double a = (vy <= s) ? p.gravityWeak : p.gravityStrong;
        const double next = vy + a;
        return next < p.vyMinPlayerFrame ? p.vyMinPlayerFrame : next;
    }

    const UfoParams& params(bool mini) const { return mini ? mini_ : normal_; }
    void setParams(bool mini, const UfoParams& p) { (mini ? mini_ : normal_) = p; }

private:
    UfoParams normal_ = UfoParams::normal();
    UfoParams mini_ = UfoParams::mini();
};

}  // namespace gdapprox
