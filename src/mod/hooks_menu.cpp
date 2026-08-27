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

    void onAutoEnter() {
        if (g_started) return;
        g_started = true;
        // The elapsed-time origin is set only on the first entry of the session (resetting
        // it on re-entry would under-report the elapsed seconds)
        if (solver::g_totalAttempts == 0)
            solver::g_solveStart = std::chrono::steady_clock::now();
        // Main levels are 1-22; anything else is a saved / online-cached custom level
        GJGameLevel* level = nullptr;
        auto* glm = GameLevelManager::sharedState();
        // If levelfile= is given, build the level from the RAW level string in that file
        // (the save file is never touched). Used for calibration maps.
        if (!g_cfg.levelFile.empty()) {
            std::ifstream lf(g_cfg.levelFile, std::ios::binary);
            if (!lf.is_open()) {
                log::error("phase1: cannot open levelfile {}", g_cfg.levelFile);
                writeResult("error: levelfile not found");
                return;
            }
            std::string raw((std::istreambuf_iterator<char>(lf)),
                            std::istreambuf_iterator<char>());
            level = GJGameLevel::create();
            level->m_levelName = "gdsolver calib";
            level->m_levelID = g_cfg.levelId;
            level->m_levelType = GJLevelType::Editor;
            // Run it through GD's own compression. Doing base64+gzip by hand here silently
            // fails to load because of encoding differences, so ALWAYS hand it to ZipUtils.
            level->m_levelString =
                cocos2d::ZipUtils::compressString(raw, false, 0);
            log::info("phase1: built the level from levelfile {} ({} chars)",
                      g_cfg.levelFile, raw.size());
        } else if (g_cfg.levelId >= 1 && g_cfg.levelId <= 22) {
            level = glm->getMainLevel(g_cfg.levelId, false);
        } else {
            level = glm->getSavedLevel(g_cfg.levelId);
            if (!level && glm->m_onlineLevels)
                level = static_cast<GJGameLevel*>(
                    glm->m_onlineLevels->objectForKey(std::to_string(g_cfg.levelId)));
        }
        if (!level) {
            log::error("phase1: level {} not found (main or saved)", g_cfg.levelId);
            writeResult("error: level not found");
            return;
        }
        // Line for ruling out the suspicion that the calibration rig (levelfile=) and the
        // official levels have DIFFERENT physics. The rig's gravity-flipped ship climbed at
        // 0.069/tick while the same condition in lv7 gave 0.103. Compares the defaults of
        // GJGameLevel::create() against the values from getMainLevel.
        writeResult(fmt::format(
            "levelinfo: id={} type={} levelVersion={} gameVersion={} "
            "twoPlayer={} objCount={}",
            g_cfg.levelId, (int)level->m_levelType, level->m_levelVersion,
            level->m_gameVersion, (int)level->m_twoPlayerMode,
            (int)level->m_objectCount).c_str());
        log::info("phase1: entering level {}", g_cfg.levelId);
        g_forceCleanStart = true;
        auto* scene = PlayLayer::scene(level, false, false);
        CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(0.5f, scene));
    }
};
