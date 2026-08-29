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

// Resident ticker that follows scene transitions and re-attaches the panel to the current scene.
// (This Geode version has no keepAcrossScenes, so we follow by ourselves)
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
        if (g_panel->getParent() != scene) {
            if (g_panel->getParent())
                g_panel->removeFromParentAndCleanup(false);
            scene->addChild(g_panel, 9999);
        }
        g_panel->setVisible(true);
    }
};
