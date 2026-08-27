// Core value types shared by every approximate model.
//
// Deliberately narrow: this project approximates SHORT segments of Ship/UFO
// flight, not Geometry Dash. Anything that is not needed to roll a player
// forward through a static corridor does not belong here.
#pragma once

#include <cstdint>
#include <vector>

#include "geometry/corridor.hpp"

namespace gdapprox {

enum class Mode : std::uint8_t {
    Ship,
    Ufo,
};

// Physical size of the player. GD's dumps do not currently expose this
// (docs/measurement-requests.md M1); it is inferred during calibration.
enum class Size : std::uint8_t {
    Normal,
    Mini,
};

struct ApproxState {
    double x = 0.0;
    double y = 0.0;
    double vy = 0.0;
    // GD's PlayerObject::m_playerSpeed. 0.7/0.9/1.1/1.3 are measured (M2);
    // speed changes dxPerTick and nothing else -- acceleration, caps, the
    // branch threshold and yScale are all speed-invariant. See models/speed.hpp.
    double playerSpeed = 0.9;

    bool gravityFlipped = false;   // PlayerObject::m_isUpsideDown
    bool mini = false;
    bool held = false;

    Mode mode = Mode::Ship;

    Size size() const { return mini ? Size::Mini : Size::Normal; }
};

// A run of identical input. Ship control is naturally piecewise-constant, so
// candidates are enumerated over segments rather than per-tick bit strings.
struct ControlSegment {
    bool held = false;
    int ticks = 0;
};

// A point the run should pass through: a secret coin, or any other positional
// objective. Tracked incrementally during the rollout, so asking for it costs
// nothing beyond a distance per tick -- recording the whole trajectory just to
// measure closest approach was 5x slower.
struct PointTarget {
    double x = 0.0;
    double y = 0.0;
    double radius = 20.0;   // GD collects a coin within 20 px of the centre
};

struct ApproxResult {
    ApproxState terminal;
    // True only if some tick was CLEARLY outside the corridor. A trajectory
    // that merely grazes the boundary sets approximateUncertain instead, and
    // must still be sent to GD -- see geometry/corridor.hpp.
    bool approximateCollision = false;
    bool approximateUncertain = false;
    // Smallest signed distance to the corridor boundary over the rollout.
    // Positive means clear; negative means the corridor was violated.
    // Infinity when no corridor was supplied.
    double minimumClearance = 0.0;
    // Empty unless the caller asks for it: recording every tick dominates the
    // cost of a rollout and defeats the point of the approximator.
    std::vector<ApproxState> trajectory;
    // Closest approach to each SimulateOptions::targets entry, same order.
    // Measured against the swept segment, not just the tick positions: at
    // 1.3 px/tick the player can cross a 20 px circle between two ticks.
    std::vector<double> targetClosest;
    // Number of ticks actually simulated (may be < requested if it collided).
    int ticksSimulated = 0;

    bool reachedAllTargets(const std::vector<PointTarget>& targets) const {
        if (targetClosest.size() != targets.size()) return false;
        for (std::size_t i = 0; i < targets.size(); ++i) {
            if (targetClosest[i] > targets[i].radius) return false;
        }
        return true;
    }
};

struct SimulateOptions {
    bool recordTrajectory = false;
    // Stop the rollout as soon as the corridor is CLEARLY violated. Grazing
    // the boundary never stops a rollout.
    bool stopOnCollision = true;
    // How much clearance counts as clearly safe / clearly lethal. The defaults
    // are the values chosen by src/tools/margin.cpp; see docs/benchmark.md.
    MarginPolicy margin;
    // Positional objectives. ApproxResult::targetClosest gets one entry each,
    // in the same order.
    std::vector<PointTarget> targets;
};

class IApproxModel {
public:
    virtual ~IApproxModel() = default;

    virtual ApproxResult simulate(const ApproxState& start,
                                  const std::vector<ControlSegment>& controls,
                                  const ICorridor* corridor,
                                  const SimulateOptions& options) const = 0;

    ApproxResult simulate(const ApproxState& start,
                          const std::vector<ControlSegment>& controls) const {
        return simulate(start, controls, nullptr, SimulateOptions{});
    }
};

int totalTicks(const std::vector<ControlSegment>& controls);

}  // namespace gdapprox
