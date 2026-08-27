#pragma once

#include "core/state.hpp"
#include "geometry/corridor.hpp"
#include "models/ship_params.hpp"

namespace gdapprox {

class ShipModel final : public IApproxModel {
public:
    ShipModel() = default;
    explicit ShipModel(ShipParams normalParams, ShipParams miniParams)
        : normal_(normalParams), mini_(miniParams) {}

    ApproxResult simulate(const ApproxState& start,
                          const std::vector<ControlSegment>& controls,
                          const ICorridor* corridor,
                          const SimulateOptions& options) const override;

    // The override would otherwise hide the base class's 2-argument convenience form.
    using IApproxModel::simulate;

    // Single tick, exposed for tests and for callers that drive their own loop.
    // `vy` is in the player frame; `held` is the input governing this
    // transition (i.e. the plan state kInputLatencyTicks ticks earlier).
    // `gravityFlipped` only affects which side of the switch threshold counts
    // as "strong" -- the accelerations themselves are player-frame.
    static double stepVy(double vy, bool held, const ShipParams& p,
                         bool gravityFlipped = false) {
        // [2026-08-20 RESOLVED] A long unresolved note used to sit here saying
        // "the sign of the flipped threshold disagrees between the rig and the
        // corpus", but THERE WAS NO CONTRADICTION -- the calibration rig's
        // header was at fault. `header()` in `py/mklevel.py` spelled out
        // kA22-kA45 (the 2.2 compatibility flags), and GD read them and ran a
        // different physics from the official levels. The raw header of an
        // official level (gzip-expand `Resources/levels/7.txt`) ENDS AT kA11 AND
        // HAS NOT A SINGLE kA22-or-later KEY.
        // After deleting that block and rebuilding the rig, the rig's flipped
        // ship (zero input, sp0.9) went from +0.069 to +0.103/tick and matched
        // lv7 t=5,793...
        // THE SIGN BELOW (gravityFlipped ? -s : +s) IS CORRECT. Do not touch it.
        // Details are in the 2026-08-20 entry of docs/findings.md.
        // [2026-08-21] The flipped side has its own measured table
        // (accelSwitchVyFlipForSpeed). Its default equals -accelSwitchVy, so
        // unmeasured speeds are unchanged.
        const double s = gravityFlipped ? p.accelSwitchVyFlip : p.accelSwitchVy;
        const double a = held ? (vy <= s ? p.holdStrong : p.holdWeak)
                              : (vy > s ? p.releaseStrong : p.releaseWeak);
        double next = vy + a;
        if (next > p.vyMaxPlayerFrame) next = p.vyMaxPlayerFrame;
        if (next < p.vyMinPlayerFrame) next = p.vyMinPlayerFrame;
        return next;
    }

    const ShipParams& params(bool mini) const { return mini ? mini_ : normal_; }
    void setParams(bool mini, const ShipParams& p) { (mini ? mini_ : normal_) = p; }

private:
    ShipParams normal_ = ShipParams::normal();
    ShipParams mini_ = ShipParams::mini();
};

}  // namespace gdapprox
