#pragma once
// Detection of missed speed portals (cfg speedgate).
#include "mod/stallwatch.hpp"

namespace p1 {

// ---- Detection of missed speed portals (cfg `speedgate`) ----
// A "solution that clears but differs from the chart" (e.g. passing under a slow-down portal)
// still clears, so looking only at the reached x cannot notice it. Detection is always on
// (report only, no side effects). With `speedgate=1` it becomes a gate that kills attempts that
// missed one
namespace speedgate {

inline bool g_gate = false;   // cfg `speedgate=1`: kill attempts that missed a portal
inline bool g_built = false;
inline int  g_missed = 0;     // session total (for reporting)

// The test is made only after passing the portal's x by this distance (the speed changes
// 30-47px before portal.x — the player's hitbox). With two portals close together this reads
// the later value, but it is enough to judge the nature
constexpr float EVAL_MARGIN = 60.f;

struct Portal { float x, y, speed; bool resolved, taken; };
inline std::vector<Portal> g_list;

// Speed-portal object ID -> m_playerSpeed (200/201/202/203 confirmed by measurement, only 1334
// (fastest) unverified)
inline float speedForId(int id) {
    switch (id) {
        case 200:  return 0.7f;
        case 201:  return 0.9f;
        case 202:  return 1.1f;
        case 203:  return 1.3f;
        case 1334: return 1.6f;
        default:   return -1.f;
    }
}

inline void reset() { g_list.clear(); g_built = false; g_missed = 0; }
inline void newAttempt() { for (auto& q : g_list) { q.resolved = false; q.taken = false; } }

inline void build(GJBaseGameLayer* l) {
    if (g_built || !l || !l->m_objects) return;
    g_built = true;
    for (auto* obj : CCArrayExt<GameObject*>(l->m_objects)) {
        if (!obj) continue;
        float s = speedForId(obj->m_objectID);
        if (s < 0) continue;
        auto p = obj->getPosition();
        g_list.push_back({(float)p.x, (float)p.y, s, false, false});
    }
    std::sort(g_list.begin(), g_list.end(),
              [](const Portal& a, const Portal& b) { return a.x < b.x; });
    if (!g_list.empty()) {
        std::string s = "speedportals:";
        for (auto& q : g_list)
            s += " " + std::to_string((int)q.x) + "/" + std::to_string((int)q.y)
               + "=" + std::to_string(q.speed).substr(0, 3);
        writeResult(s);
    }
}

// Return value true = this attempt missed a portal (killed when the gate is enabled)
inline bool check(PlayerObject* p, long long tick) {
    if (!g_built || g_list.empty() || !p || p->m_isDead) return false;
    float px = p->getPositionX();
    float spd = (float)p->m_playerSpeed;
    bool missed = false;
    for (auto& q : g_list) {
        if (q.resolved || px < q.x + EVAL_MARGIN) continue;
        q.resolved = true;
        q.taken = std::abs(spd - q.speed) < 0.01f;
        if (q.taken) continue;
        ++g_missed;
        static int s_logs = 0;
        if (++s_logs <= 20)
            writeResult("speedmiss: portal x=" + std::to_string((int)q.x)
                + " y=" + std::to_string((int)q.y)
                + " wants=" + std::to_string(q.speed).substr(0, 3)
                + " but speed=" + std::to_string(spd).substr(0, 3)
                + " at tick=" + std::to_string(tick)
                + " x=" + std::to_string((int)px)
                + " y=" + std::to_string((int)p->getPositionY()));
        missed = true;
    }
    return missed;
}

} // namespace speedgate

// Session management for the on-screen control panel. A path that fully resets the session
// state and starts manually, so the UI can start any number of times

}  // namespace p1
