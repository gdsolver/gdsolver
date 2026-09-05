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
    // `boostLatch` is GD's velocity-limit exemption, the byte [player+0x952]
    // (dp/state.hpp State::boost, dp/slopes.hpp boostLatchMode). The SHIP is
    // the one mode whose acceleration reads it as well as the clamp -- see the
    // selector below.
    static double stepVy(double vy, bool held, const ShipParams& p,
                         bool gravityFlipped = false, bool boostLatch = false) {
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
        // playerIsFallingBugged (0x39a430): `vp` against 2 x the speed's own
        // gravity, which is what accelSwitchVy already is.
        const bool fallingBugged = (vy <= s);
        // GD's selector, updateJump's ship branch 0x38c5b2-0x38c665, written
        // out in its own order. `xmm3` is the factor and `xmm4` the second
        // one; the acceleration is -G0 * xmm3 * xmm4 / chi in the gravity
        // frame, and the four products are exactly the four measured rungs:
        //
        //   xmm3 = 0.8                                          (default)
        //   if (held && !latch)          xmm3 = -1.0; goto accel   0x38c5be
        //   if (latch && vp < 0)         xmm3 = -1.0               0x38c5d8..0x38c604
        //   if (!held && !fallingBugged) xmm3 = 1.2                0x38c623
        //   xmm4 = (held && fallingBugged) ? 0.5 : 0.4             0x38c645/0x38c65d
        //
        //   -1.0 x 0.5 = holdStrong (+0.108)   -1.0 x 0.4 = holdWeak  (+0.086)
        //    1.2 x 0.4 = releaseStrong(-0.103)  0.8 x 0.4 = releaseWeak(-0.069)
        //    0.8 x 0.5 = -|holdWeak|  (-0.086)  <- reachable ONLY while latched
        //
        // With `boostLatch` false the chain collapses to the pair this
        // function has always computed, bit for bit.
        //
        // Measured in the corpus (gdref, gravity frame, both from a ramp
        // launch at 6.906 that leaves the body outside the band):
        //   lv16 8,913-8,919, button released : +0.086/tick  (-1.0 x 0.4)
        //   lv16 14,366-14,370, button held   : +0.086 then +0.108 x3
        //                                       (-1.0 x 0.4, then x 0.5)
        // The fifth rung (0.8 x 0.5, a held body still rising while latched)
        // has no witness on disk; it is the disassembly's, like the -1.0 x 0.4
        // held-and-not-fallingBugged case.
        double a;
        if (held && !boostLatch)
            a = fallingBugged ? p.holdStrong : p.holdWeak;
        else if (boostLatch && vy < 0.0)
            a = (held && fallingBugged) ? p.holdStrong : p.holdWeak;
        else if (!held && !fallingBugged)
            a = p.releaseStrong;
        else
            a = (held && fallingBugged) ? -p.holdWeak : p.releaseWeak;
        double next = vy + a;
        // THE CLAMP IS SKIPPED OUTRIGHT WHILE THE LATCH IS UP (0x38ca9f: `cmp
        // byte [rdi+0x952], 0 / jne` past the whole block). It is not a
        // softened pull -- maxsd/minsd write the limit in one tick when it
        // does run, and nothing at all when it does not.
        if (boostLatch) return next;
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
