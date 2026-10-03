#pragma once
#include <algorithm>

namespace p1::solver {

// Reset-confirmed failures carry real progress but no measured p1 collision.
enum class AttemptEndKind { Collision, Forced, ConfirmedP2Reset };

struct AttemptEndPolicy {
    bool learnCollision;
    bool creditProgress;
};

// Keep collision learning independent of the evidence that the flight really advanced.
inline constexpr AttemptEndPolicy attemptEndPolicy(AttemptEndKind kind) {
    return {kind == AttemptEndKind::Collision, kind != AttemptEndKind::Forced};
}

// Plans keep the driver's input counter. The pre-physics command boundary is S(input+1).
inline long long commandStateTick(long long inputTick) { return inputTick + 1; }

inline long long g_commandStateTick = -1;

// Pin the pre-physics state through command callbacks, including after the counter advances.
struct CommandStateScope {
    long long saved;
    explicit CommandStateScope(long long inputTick) : saved(g_commandStateTick) {
        g_commandStateTick = commandStateTick(inputTick);
    }
    ~CommandStateScope() { g_commandStateTick = saved; }
    CommandStateScope(const CommandStateScope&) = delete;
    CommandStateScope& operator=(const CommandStateScope&) = delete;
};

// A physical collision follows processCommands' counter increment; command-time guards do not.
inline long long attemptStateTick(long long counterTick) {
    return g_commandStateTick >= 0 ? g_commandStateTick : counterTick + 1;
}

// Capture before GD handles destroyPlayer: it may end dual mode or kill the other body.
struct RunDeathState {
    bool p1 = false;
    bool p2 = false;
    bool dual = false;
};

// Ignore pseudo-calls, but accept either body's actual alive-to-dead transition.
inline bool deathEndsAttempt(const RunDeathState& before, const RunDeathState& after, int caller) {
    if (caller != 1 && !(caller == 2 && before.dual)) return false;
    return (!before.p1 && after.p1) || (before.dual && !before.p2 && after.p2);
}

// An inactive p2 can die without ending play; only a subsequent reset confirms a failed flight.
struct UnbookedP2Death {
    long long tick = -1;
    float x = 0.f, y = 0.f;

    // Keep the first actual transition, not pseudo-calls or the later death-to-reset ticks.
    void observe(const RunDeathState& before, const RunDeathState& after, int caller,
                 long long at, float px, float py) {
        if (tick >= 0 || caller != 2 || before.dual || before.p2 || !after.p2) return;
        tick = at; x = px; y = py;
    }

    // Consume before the reset clears recordings, and never duplicate an already booked death.
    UnbookedP2Death takeForReset(bool verifying, bool booked) {
        const auto saved = *this;
        *this = {};
        return verifying && !booked ? saved : UnbookedP2Death{};
    }
};

// Preserve the old minimum while allowing the solver's whole-level range plus a replay margin.
inline long long attemptTickLimit(long long horizon) {
    return std::max(40000LL, horizon + 3000);
}

}  // namespace p1::solver
