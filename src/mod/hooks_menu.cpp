// MenuLayer hook: auto-enter and the developer panel.
#include "mod/playlayer_helpers.hpp"

using namespace p1;

// ---- Auto-enter ----
class $modify(P1MenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        // Announce once that the mod is here and what it suppresses. This is the minimal
        // implementation of "the mod's enabled state must be detectable from outside".
        // Say what the gate actually does: it is botDriving(), an automated session being
        // open -- NOT the mod being installed. The line used to claim the block was on
        // whenever the mod was loaded and told the reader to disable it to record progress,
        // which is not true and is the opposite of the promise the safety gate makes.
        static bool s_notified = false;
        if (!s_notified) {
            s_notified = true;
            log::info("gdsolver: loaded. While the bot drives -- solving or replaying -- "
                      "achievements, statistics, coins and the level's own record are all "
                      "blocked. Your own attempts, with the bot idle, record as usual.");
        }
        // The panel is created once and follows scene transitions via a resident ticker
        // (attached only on the screens a level is started from -- see isLevelLaunchScreen)
        static bool s_panelMade = false;
        if (!s_panelMade) {
            s_panelMade = true;
            if (auto* p = SolverPanelLayer::create()) {
                p->retain(); // keep it alive so re-parenting between scenes does not free it
                g_panel = p;
                auto* keeper = new PanelKeeper();
                // A 0.1 s interval is enough (every frame is wasteful). It catches up ~100ms
                // after a scene switch
                CCDirector::sharedDirector()->getScheduler()->scheduleSelector(
                    schedule_selector(PanelKeeper::tick), keeper, 0.1f, false);
            }
            // ...and the one that carries a `levels=` suite from one level to the next. Same
            // scheduler and same reason: between two levels of a suite there is no game layer
            // and no session, so this is the only thing of ours still ticking.
            auto* suiteKeeper = new SuiteKeeper();
            CCDirector::sharedDirector()->getScheduler()->scheduleSelector(
                schedule_selector(SuiteKeeper::tick), suiteKeeper, 0.1f, false);
        }
        static bool s_ranOnce = false;
        if (!s_ranOnce) {
            s_ranOnce = true;
            resolveDataDir(); // must be settled BEFORE the first file access
            postmortem::install(); // leave at least one line of crash cause (after DATA_DIR)
            loadConfig();
            if (g_cfg.enabled) {
                openFiles();
                stallwatch::start();
                updateWindowTitle();
                writeResult("session_start", true);
                // Load the plan onto the very first attempt of serve mode too. Relying on
                // rerun, a session that finishes without dying (nodeath) would never receive
                // the plan. Start-Serve always writes plan_in.txt before launch (if it is
                // empty this branch does nothing)
                if (g_serveMode) {
                    std::vector<InputCmd> plan;
                    loadPlanExtras(std::string(DATA_DIR) + "/plan_in.txt");
                    if (loadInputsFile(std::string(DATA_DIR) + "/plan_in.txt", plan)) {
                        std::sort(plan.begin(), plan.end(),
                                  [](const InputCmd& a, const InputCmd& b) { return a.step < b.step; });
                        g_cfg.inputs = std::move(plan);
                        writeResult("serve: initial plan "
                            + std::to_string(g_cfg.inputs.size()) + " inputs");
                    }
                }
                if (g_cfg.fps > 0) {
                    auto* gm = GameManager::sharedState();
                    gm->m_vsyncEnabled = false;
                    gm->m_customFPSTarget = (float)g_cfg.fps;
                    gm->updateCustomFPS();
                    // vsync is really the GL swap interval. Disable it directly at runtime
                    using SwapIntervalFn = BOOL(WINAPI*)(int);
                    auto ogl = GetModuleHandleA("opengl32.dll");
                    auto getProc = (PROC(WINAPI*)(LPCSTR))GetProcAddress(ogl, "wglGetProcAddress");
                    if (getProc) {
                        auto swapInterval = (SwapIntervalFn)getProc("wglSwapIntervalEXT");
                        if (swapInterval) {
                            swapInterval(0);
                            log::info("phase1: vsync disabled via wglSwapIntervalEXT");
                        }
                    }
                    log::info("phase1: forced custom FPS to {}", g_cfg.fps);
                }
                this->runAction(CCSequence::create(
                    CCDelayTime::create(g_cfg.delaySec),
                    CCCallFunc::create(this, callfunc_selector(P1MenuLayer::onAutoEnter)),
                    nullptr
                ));
            }
        }
        return true;
    }

    // The body of this lives in ui_panel.hpp as enterConfiguredLevel(), so that the level
    // suite enters its second and later levels through the same code as its first.
    void onAutoEnter() {
        if (suite::active())
            writeResult("suite: level=" + std::to_string(suite::current())
                        + " (1/" + std::to_string(suite::g_levels.size()) + ")");
        enterConfiguredLevel();
    }
};
