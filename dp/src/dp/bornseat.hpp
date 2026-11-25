#pragma once
// What is left of the dual-birth seat (--gdbornseat, removed with the old tick; the shadow's own pass seats the first body before the dual portal acts): the speed
// multiplier of a per-tick x advance, which the shadow's seed still reads.
#include "dp/object.hpp"
#include "dp/shadow.hpp"

namespace dp {

// speed multiplier from the per-tick x advance (0.7 / 0.9 / 1.1 / 1.3 / 1.6 -> 1.0465 / 1.29825 / 1.6143 / 1.9096 / 2.3438)
inline float shadowSpeedOf(float dx) {
    const float tab[5][2] = {{1.0465f, 0.7f}, {1.29825f, 0.9f}, {1.6143f, 1.1f}, {1.9096f, 1.3f}, {2.3438f, 1.6f}};
    int best = 1;
    float bd = 1e9f;
    for (int i = 0; i < 5; ++i) { const float d = std::fabs(dx - tab[i][0]); if (d < bd) { bd = d; best = i; } }
    return tab[best][1];
}

;


}  // namespace dp
