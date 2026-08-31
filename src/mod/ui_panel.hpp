#pragma once
// Developer control panel shown on menu scenes.
#include "mod/helpers_game.hpp"

using namespace p1;

// ---- On-screen control panel (shown on the screens a level is started from) ----
// Mode switching only. Level selection uses the game's own UI as-is (custom levels supported).
// On level entry (PlayLayer::init) the session is configured automatically with the current
// mode. Kept across scenes with SceneManager, and hidden everywhere else.
// 0=Normal (normal play) 1=Replay (a stored solution) 2=Solve (the mod solves it here and now).
// The modes and their names live in fxcensus.hpp, next to g_uiMode -- the session setup needs
// them and comes earlier in the include chain.

class SolverPanelLayer : public cocos2d::CCLayer {
public:
    cocos2d::CCLabelBMFont* m_modeLabel = nullptr;

    static SolverPanelLayer* create() {
        auto* r = new SolverPanelLayer();
        if (r->init()) { r->autorelease(); return r; }
        delete r;
        return nullptr;
    }

    bool init() override {
        using namespace cocos2d;
        if (!CCLayer::init()) return false;
        this->setID("panel"_spr);
        auto* tag = CCLabelBMFont::create("GDSOLVER", "bigFont.fnt");
        tag->setScale(0.3f);
        tag->setAnchorPoint({0.f, 0.5f});
        tag->setPosition({10.f, 52.f});
        tag->setOpacity(140);
        tag->setID("panel-tag"_spr);
        this->addChild(tag);
        m_modeLabel = CCLabelBMFont::create(uiModeName(g_uiMode), "bigFont.fnt");
        m_modeLabel->setScale(0.55f);
        auto* item = CCMenuItemLabel::create(m_modeLabel, this,
            menu_selector(SolverPanelLayer::onMode));
        item->setAnchorPoint({0.f, 0.5f});
        auto* menu = CCMenu::create(item, nullptr);
        menu->setID("panel-menu"_spr);
        item->setID("mode-button"_spr);
        menu->setPosition({0, 0});
        item->setPosition({10.f, 32.f});
        this->addChild(menu);
        this->scheduleUpdate();
        return true;
    }

    void onMode(CCObject*) {
        g_uiMode = (g_uiMode + 1) % UI_MODE_COUNT;
        if (m_modeLabel) m_modeLabel->setString(uiModeName(g_uiMode));
    }
};

inline SolverPanelLayer* g_panel = nullptr;

// ---- entering the level g_cfg names ----------------------------------------
//
// The body of MenuLayer's auto-enter, lifted out so that the level suite can use the SAME
// path rather than a second one that only looks like it. Everything it needs is in g_cfg, and
// that is the point: the second level of a suite is entered by the code that enters the first.
//
// Returns false when the level could not be found or built; the caller decides what that
// means (the suite steps past it, auto-enter just stops).
inline bool enterConfiguredLevel() {
    if (g_started) return false;
    g_started = true;
    // The elapsed-time origin is set only on the first entry of the session (resetting it on
    // re-entry would under-report the elapsed seconds)
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
            return false;
        }
        std::string raw((std::istreambuf_iterator<char>(lf)),
                        std::istreambuf_iterator<char>());
        level = GJGameLevel::create();
        level->m_levelName = "gdsolver calib";
        level->m_levelID = g_cfg.levelId;
        level->m_levelType = GJLevelType::Editor;
        // Run it through GD's own compression. Doing base64+gzip by hand here silently
        // fails to load because of encoding differences, so ALWAYS hand it to ZipUtils.
        level->m_levelString = cocos2d::ZipUtils::compressString(raw, false, 0);
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
        return false;
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
    return true;
}

// Which screens the panel belongs on. It used to ride every scene that was not PlayLayer,
// which parked it on top of whatever the current menu happened to keep in its bottom-left
// corner. The mode switch only means anything on a screen a level is started from, so those
// are the only ones it appears on: the main-level wheel (LevelSelectLayer) and the page of a
// downloaded or online level (LevelInfoLayer) or a locally saved one (EditLevelLayer) --
// which is what makes custom levels reachable.
// Depth 2 rather than 1 because during a scene transition the running scene is the
// CCTransitionScene and the real layer is its grandchild; without it the panel blinks out
// for the length of every fade.
inline bool isLevelLaunchScreen(cocos2d::CCNode* n, int depth = 0) {
    if (!n || depth > 2) return false;
    if (geode::cast::typeinfo_cast<LevelSelectLayer*>(n)
        || geode::cast::typeinfo_cast<LevelInfoLayer*>(n)
        || geode::cast::typeinfo_cast<EditLevelLayer*>(n)) return true;
    auto* kids = n->getChildren();
    if (!kids) return false;
    for (unsigned i = 0; i < kids->count(); ++i)
        if (isLevelLaunchScreen(static_cast<cocos2d::CCNode*>(kids->objectAtIndex(i)),
                                depth + 1))
            return true;
    return false;
}

// Resident ticker that decides whether the panel is attached at all. It hangs off Geode's
// OverlayManager, a node that outlives the running scene, so nothing here has to follow scene
// transitions.
//
// [2026-08-30] This used to re-parent the panel onto each new running scene, under a comment
// saying "This Geode version has no keepAcrossScenes, so we follow by ourselves". That was wrong
// about the SDK rather than about GD: geode::OverlayManager is in 5.8.2 and is exactly this.
// The attach/detach below is NOT part of what it replaces -- see the note on touches.
// Resident poll that carries a suite from one level to the next. It hangs off the director's
// scheduler for the same reason the panel keeper does: between two levels there is no game
// layer and no session, so nothing else in the mod is running.
//
// The wait is the whole of it. endSession asks PlayLayer to quit, GD fades to the level-select
// screen, and a level entered before that fade finishes replaces a scene that is still being
// replaced. So: no PlayLayer, not inside a CCTransitionScene, and then a settling margin
// longer than the 0.5 s fade before the next level goes in.
class SuiteKeeper : public cocos2d::CCObject {
public:
    // Ticks of the 0.1 s poll to wait after the scene looks clear. 1 s, i.e. twice the fade --
    // this runs 21 times in a full sweep, so it is 21 seconds spent buying the margin.
    static constexpr int kSettleTicks = 10;
    // ...and the bound on the wait itself. If the level never lets go, the suite must fail
    // where it can be seen: a keeper that waits forever is a silent hang, and the reader
    // outside sees only a game that stopped writing -- the least informative failure there is.
    static constexpr int kGiveUpTicks = 600;   // 60 s

    void tick(float) {
        using namespace cocos2d;
        if (!suite::g_advance) return;
        auto* scene = CCDirector::sharedDirector()->getRunningScene();
        const bool clear = PlayLayer::get() == nullptr && scene
                           && !geode::cast::typeinfo_cast<CCTransitionScene*>(scene);
        // The two counters are separate on purpose: one bounds the whole wait, the other is the
        // margin AFTER the scene became enterable. Sharing one made a slow exit eat the margin.
        ++suite::g_waited;
        if (!clear) {
            suite::g_settle = 0;
            if (suite::g_waited < kGiveUpTicks) return;
            suite::g_advance = false;
            writeResult("suite: gave up waiting to leave the level after 60s (playLayer="
                        + std::string(PlayLayer::get() ? "still up" : "gone")
                        + ") - the remaining levels are not run");
            writeResult("suite: done " + std::to_string(suite::g_levels.size()) + " levels");
            if (g_cfg.quitWhenDone)
                Loader::get()->queueInMainThread([] { utils::game::exit(false); });
            return;
        }
        if (++suite::g_settle < kSettleTicks) return;
        suite::g_advance = false;
        suite::g_settle = 0;
        suite::g_waited = 0;
        // Rebuild the configuration from the file rather than patching the outgoing one: the
        // second level of a suite is then configured by the same code, from the same bytes, as
        // the first. (PlayLayer::onQuit has already reset g_cfg to its defaults, so this is a
        // fresh parse and `dparg`'s append cannot double up.)
        g_cfg = Config{};
        loadConfig();
        g_cfg.levelId = suite::current();
        g_cfg.levelFile.clear();          // a suite is main levels by id, never a rig file
        openFiles();                      // endSession closed them
        writeResult("suite: level=" + std::to_string(g_cfg.levelId)
                    + " (" + std::to_string(suite::g_at + 1) + "/"
                    + std::to_string(suite::g_levels.size()) + ")");
        updateWindowTitle();
        if (enterConfiguredLevel()) return;
        // It could not be entered. Do not stall the sweep on it -- report it and take the next,
        // or end the run if this was the last.
        writeResult("suite: level=" + std::to_string(g_cfg.levelId)
                    + " could not be entered - skipped");
        g_started = false;
        if (suite::hasNext()) {
            ++suite::g_at;
            suite::g_advance = true;
            return;
        }
        writeResult("suite: done " + std::to_string(suite::g_levels.size()) + " levels");
        if (g_cfg.quitWhenDone)
            Loader::get()->queueInMainThread([] { utils::game::exit(false); });
    }
};

class PanelKeeper : public cocos2d::CCObject {
public:
    void tick(float) {
        using namespace cocos2d;
        if (!g_panel) return;
        auto* scene = CCDirector::sharedDirector()->getRunningScene();
        if (!scene) return;
        // While playing the HUD has the screen; everywhere outside a level-launch screen the
        // panel is detached rather than merely hidden, so it cannot eat a touch meant for the
        // menu underneath it.
        if (PlayLayer::get() != nullptr || !isLevelLaunchScreen(scene)) {
            if (g_panel->getParent())
                g_panel->removeFromParentAndCleanup(false);
            return;
        }
        if (!g_panel->getParent())
            OverlayManager::get()->addChild(g_panel, 9999);
        g_panel->setVisible(true);
    }
};
