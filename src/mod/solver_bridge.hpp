#pragma once
// Includes the in-process solver modules (solver/*.hpp) and the manual pause/step keys.
#include "mod/commands.hpp"

// OUTSIDE namespace p1, unlike the solver/ headers below: dp_bridge.cpp defines these symbols
// at global scope (it cannot see p1 -- it is the one TU with no mod headers in it)
#include "mod/dp_bridge.hpp"

namespace p1 {

// Shared foundation for state observation and level-data dumps (buildPois / g_log / colprobe /
// gatetrace)
#include "solver/solver.hpp"

#include "solver/grouptrace.hpp"
#include "solver/clearance.hpp"
#include "solver/psnap.hpp"
#include "solver/secsolve.hpp"

// ---- Stage B self-test (cfg `dpselftest=1`) ----
// Build the objrects table in memory and hand it to the solver core that is now linked into
// the mod. Two questions, both answered in result.txt:
//   1. are those the same bytes the CLI reads from objrects.txt?
//   2. does the core make the same level out of them?
// Nothing else is touched: this is an instrument, not a feature.
//
// On the byte comparison: the dump is written through a text-mode ofstream, so on Windows its
// line endings are CRLF while the in-memory buffer's are LF. The file is therefore read back
// in text mode, which undoes exactly that translation -- what is compared is the rows, which
// is what both the CLI's parser and the core's parser actually see.
inline void dpSelfTest(GJBaseGameLayer* l) {
    if (!l) return;
    std::ostringstream oss;
    solver::writeObjRects(oss, l);
    const std::string csv = oss.str();

    std::ifstream f(std::string(DATA_DIR) + "/objrects.txt");
    const std::string disk((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
    const char* verdict = disk.empty() ? "NO DUMP ON DISK"
                        : (disk == csv ? "same as objrects.txt" : "DIFFERS from objrects.txt");
    writeResult("dpselftest: in-memory csv " + std::to_string(csv.size()) + " bytes, disk "
                + std::to_string(disk.size()) + " bytes - " + verdict);

    const dpbridge::LevelStats s = dpbridge::statsFromCsv(csv);
    // Same words, same order and same formatting as leveldp's own `level:` line, so the two
    // can be put side by side without translating either
    char b[256];
    snprintf(b, sizeof(b),
             "dpselftest: level: %zu colliders, %zu portals, %zu pads, %zu orbs, "
             "%zu moving, maxX=%.0f",
             s.objs, s.portals, s.pads, s.orbs, s.moving, s.maxX);
    writeResult(b);
    writeResult("dpselftest: " + dpbridge::coreVersion());
}

// ---- Hitboxes (F6) ----
// GD draws its own hitboxes into m_debugDrawNode, but only ever shows them in practice mode
// (PlayLayer::toggleDebugDraw decides visibility from the practice flag). Here the flag and
// the node are driven directly, so the boxes can be seen in a bot run -- which is where they
// are worth seeing, because the box is what the solver reasons about.
//
// Three states, because "hitboxes only" is what you actually want when checking a near miss:
// with the artwork hidden there is nothing left on screen but the geometry the physics uses.
namespace hitbox {

inline int g_mode = 0;   // 0 = off, 1 = boxes over the level, 2 = boxes only
inline std::vector<cocos2d::CCNode*> g_hidden;   // what mode 2 hid, so it can be put back

// g_hidden nodes are retained while they sit in the vector: the object layer is what actually
// owns them, and a level torn down while mode 2 is active (scene swap, retry, results screen)
// would otherwise leave dangling CCNode* behind for the restore loop below to call through.
// Measured (2026-08-24, F6 spammed across scene transitions): the restore loop dereferenced a
// freed node's vtable and crashed at showArt(true)'s `n->setVisible(true)`, attributed by the
// crash report to cycle()'s call site further up the inlined chain.
inline void clearHidden() {
    for (cocos2d::CCNode* n : g_hidden) if (n) n->release();
    g_hidden.clear();
}

inline void showArt(bool visible) {
    if (visible) {
        for (cocos2d::CCNode* n : g_hidden) if (n) n->setVisible(true);
        clearHidden();
        return;
    }
    auto* pl = PlayLayer::get();
    if (!pl || !pl->m_objectLayer) return;
    // Everything the object layer draws except the debug node itself. The batch nodes are the
    // artwork; the debug node is the geometry
    auto* kids = pl->m_objectLayer->getChildren();
    if (!kids) return;
    for (int i = 0; i < (int)kids->count(); ++i) {
        auto* n = static_cast<cocos2d::CCNode*>(kids->objectAtIndex(i));
        if (!n || n == pl->m_debugDrawNode || !n->isVisible()) continue;
        n->setVisible(false);
        n->retain();
        g_hidden.push_back(n);
    }
}

// Returns whether there was a PlayLayer to apply against. cycle() only commits to the new mode
// on success, so a press that lands mid scene-swap (PlayLayer::get() == nullptr) leaves g_mode
// exactly where it was instead of drifting out of step with what is actually hidden.
inline bool apply() {
    auto* pl = PlayLayer::get();
    if (!pl) return false;
    pl->m_isDebugDrawEnabled = (g_mode > 0);
    if (pl->m_debugDrawNode) pl->m_debugDrawNode->setVisible(g_mode > 0);
    showArt(g_mode < 2);
    return true;
}

inline void cycle() {
    const int prev = g_mode;
    g_mode = (prev + 1) % 3;
    if (!apply()) { g_mode = prev; return; }
    log::info("hotkey: F6 -> hitboxes {}",
              g_mode == 0 ? "off" : (g_mode == 1 ? "on" : "only"));
}

// The level was rebuilt (reset / new attempt): the nodes we hid are gone, and GD turns its own
// flag back off. Re-apply against the new scene.
inline void onLevelReset() {
    clearHidden();
    if (g_mode > 0) apply();
}

// Once per frame while the boxes are on. GD fills m_debugDrawNode in updateDebugDraw and only
// calls it when its own conditions hold (practice mode), so the call is made from here --
// setting the flag alone leaves an empty node, which looks exactly like a feature that does
// nothing (measured: lv1, flag set, node visible, no boxes).
inline void perFrame(GJBaseGameLayer* l) {
    if (g_mode <= 0 || !l) return;
    l->m_isDebugDrawEnabled = true;
    l->updateDebugDraw();
    if (l->m_debugDrawNode) l->m_debugDrawNode->setVisible(true);
}

}  // namespace hitbox

// The in-process solve (cfg `dpsolve=1`) used to live here as a one-shot: solve, replay once,
// end. It is now the repair loop and lives in mod/repair.hpp, further down the chain where the
// plan reader and the session are already defined.

// ============================================================
// Manual observation controls (F2 = pause/resume, F3/F4 = step 1 / 10 substeps).
// The cheat keys that used to live here -- warp, noclip, force mode, infinite jump -- were
// removed on 2026-08-23; see the note at the top of solver/probe.hpp.
#include "solver/probe.hpp"

inline void handleProbeRequest(GJBaseGameLayer* layer) {
    if (!g_probeRequest || !layer || !layer->m_player1) return;
    auto* p = layer->m_player1;
    int req = g_probeRequest;
    g_probeRequest = 0;
    switch (req) {
        case 2: // pause/resume
            probe::g_pause = !probe::g_pause;
            // The freeze is consolidated on the probe::g_pause side. `pauseatx` raises
            // g_paused instead, so it must be cleared here or we cannot resume after stopping
            // short of the wall
            g_paused = false;
            probe::trace(probe::g_pause ? "pause" : "resume", p, g_tick);
            log::info("probe: {}", probe::g_pause ? "PAUSED" : "RESUMED");
            break;
        case 3: // 1 substep
            probe::g_step = 1;
            probe::trace("step1", p, g_tick);
            break;
        case 4: // 10 substeps
            probe::g_step = 10;
            probe::trace("step10", p, g_tick);
            break;
        case 6: // hitboxes: off -> on -> only
            hitbox::cycle();
            break;
        default: break;
    }
}

}  // namespace p1
