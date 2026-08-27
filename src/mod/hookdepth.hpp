#pragma once
// Hook recursion-depth counters (names the culprit of stack-overflow crashes).
#include "mod/notify.hpp"

namespace p1 {

// ============================================================
// Hook recursion depth (hookdepth). Makes the culprit of stack-overflow CTDs name itself from
// the inside: put a depth counter on the main hooks and emit the maxima in the FATAL line and
// at session end
// ============================================================
namespace hookdepth {
struct Slot { const char* name; int cur; int max; };
inline Slot g_slots[] = {
    { "visit", 0, 0 }, { "bglUpdate", 0, 0 }, { "plUpdate", 0, 0 },
    { "resetLevel", 0, 0 }, { "destroyPlayer", 0, 0 }, { "updateVis", 0, 0 },
    { "delayedReset", 0, 0 }, { "levelComplete", 0, 0 }, { "poUpdate", 0, 0 },
};
enum Which { VISIT, BGL_UPDATE, PL_UPDATE, RESET, DESTROY, UPDATEVIS,
             DELAYED, COMPLETE, PO_UPDATE };
struct Guard {
    int i;
    explicit Guard(int idx) : i(idx) {
        if (++g_slots[i].cur > g_slots[i].max) g_slots[i].max = g_slots[i].cur;
    }
    ~Guard() { --g_slots[i].cur; }
};
// Format into one line without allocating (called in the middle of bad_alloc / stack exhaustion)
inline void format(char* out, size_t n) {
    size_t p = 0;
    for (auto& s : g_slots) {
        if (s.max <= 1 && s.cur == 0) continue;   // stay silent about ones that only went 1 deep
        int w = snprintf(out + p, n - p, " %s=%d/%d", s.name, s.cur, s.max);
        if (w <= 0 || (size_t)w >= n - p) break;
        p += (size_t)w;
    }
    out[p < n ? p : n - 1] = '\0';
}
} // namespace hookdepth

}  // namespace p1
