// Conservative static geometry.
//
// Instruction section 8: do not reproduce GD collision. Treat terrain as a
// corridor -- for each x, the band of y the player may occupy -- and sort
// candidates into three buckets, not two:
//
//   Safe       clearly inside. Screen it in.
//   Uncertain  near the boundary. **Do not reject.** Send it to GD.
//   Collide    clearly outside. Screen it out.
//
// The asymmetry is deliberate. A false positive costs one GD verification. A
// false negative costs a real solution, and the search may never generate that
// candidate again. So the Uncertain band is priced as cheap and used widely.
#pragma once

#include <limits>
#include <vector>

namespace gdapprox {

enum class Verdict {
    Safe,
    Uncertain,
    Collide,
};

const char* toString(Verdict v);

// How much clearance is "clearly" one way or the other. Both are in px.
// See docs/benchmark.md for the measurement that sets the defaults.
struct MarginPolicy {
    // Clearance at or above this counts as clearly safe.
    double safeAbove = 8.0;
    // Clearance at or below this counts as clearly colliding. Negative: the
    // trajectory has to be this far OUTSIDE the corridor before we believe it.
    double collideBelow = -8.0;

    Verdict classify(double clearance) const {
        if (!(clearance == clearance)) return Verdict::Uncertain;  // NaN
        if (clearance >= safeAbove) return Verdict::Safe;
        if (clearance <= collideBelow) return Verdict::Collide;
        return Verdict::Uncertain;
    }
};

class ICorridor {
public:
    virtual ~ICorridor() = default;
    // Signed clearance at (x, y): positive inside the corridor, negative
    // outside. Magnitude is the distance to the nearest boundary in px.
    // Infinity means "no opinion" -- outside the modelled span.
    virtual double clearance(double x, double y) const = 0;
};

// Piecewise-constant corridor sampled on a uniform x grid.
class BandCorridor final : public ICorridor {
public:
    struct Band {
        double loY;
        double hiY;
    };

    BandCorridor() = default;
    BandCorridor(double x0, double dx, std::vector<Band> bands)
        : x0_(x0), dx_(dx), bands_(std::move(bands)) {}

    double clearance(double x, double y) const override {
        if (bands_.empty()) return std::numeric_limits<double>::infinity();
        const auto i = static_cast<long long>((x - x0_) / dx_);
        if (i < 0 || i >= static_cast<long long>(bands_.size())) {
            return std::numeric_limits<double>::infinity();
        }
        const Band& b = bands_[static_cast<std::size_t>(i)];
        const double below = y - b.loY;
        const double above = b.hiY - y;
        return below < above ? below : above;
    }

    bool empty() const { return bands_.empty(); }
    double x0() const { return x0_; }
    double dx() const { return dx_; }
    const std::vector<Band>& bands() const { return bands_; }

private:
    double x0_ = 0.0;
    double dx_ = 1.0;
    std::vector<Band> bands_;
};

}  // namespace gdapprox
