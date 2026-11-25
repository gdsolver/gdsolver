#pragma once
// On-screen controls for platforms without a keyboard (Android): buttons for the session keys.
//
// Every session key is read as a held state, g_keyDown[Key] (session_hotkeys.hpp), which the
// polls turn into presses, repeats and holds. A button here writes the same state while a finger
// is on it, so it does exactly what its key does -- step and seek repeat when held, the speed
// steps once per press -- and there is no second copy of any of that behaviour to drift. A button
// whose key would be refused right now is dimmed and takes no press.
//
// The pad is a block of buttons in the bottom-left corner, under the text column and clear of the replay panel at the top right and of the death
// breakdown at the bottom right, and only while the mod is driving (the same condition as the key legend it replaces). A touch that lands
// on a button is taken; any other touch goes on to the game. The top button folds the pad away.
// Icons are GD's own sprites; a frame the game has not loaded falls back to the text label.
#include <Geode/platform/cplatform.h>

#ifndef GEODE_IS_WINDOWS

namespace touchpad {

constexpr int PAD_TAG = 0x51D70;

struct ButtonSpec {
    int key;             // (int)Key, or -1 for the fold button
    const char* label;   // shown when there is no icon, or the icon's frame is missing
    const char* icon;    // a sprite frame of GD's, or nullptr
    bool flipIcon;       // mirror the icon (one arrow sprite serves both directions)
};
// In reading order. Pairs sit side by side: the pair is the same kind of thing (the layout fills whole pairs, an even number of buttons to a row).
inline constexpr ButtonSpec kButtons[] = {
    {-1, "MENU", nullptr, false},   // the fold handle: a chevron and the word (makeHandle)
    {(int)Key::Overlay, "TEXT", "GJ_infoIcon_001.png", false},
    {(int)Key::Pause, "PAUSE", "GJ_pauseEditorBtn_001.png", false},
    {(int)Key::Quit, "QUIT", "GJ_closeBtn_001.png", false},
    {(int)Key::Step1, "+1", nullptr, false},
    {(int)Key::Step10, "+10", nullptr, false},
    {(int)Key::SeekBack, "<", "edit_leftBtn_001.png", false},
    {(int)Key::SeekForward, ">", "edit_rightBtn_001.png", false},
    {(int)Key::Slower, "<<", nullptr, false},
    {(int)Key::Faster, ">>", nullptr, false},
    {(int)Key::Replay, "REPLAY", "GJ_replayBtn_001.png", false},
    {(int)Key::Itermap, "MAP", nullptr, false},
    {(int)Key::Render, "SCREEN", nullptr, false},
    {(int)Key::Hitboxes, "HITBOX", nullptr, false},
};
constexpr int kCount = (int)(sizeof(kButtons) / sizeof(kButtons[0]));
constexpr float kW = 46.f, kH = 24.f, kGap = 4.f;
// The block's place, from the window and not from a device: its left edge is the text column's (hud.hpp overlayLabel puts every line at x = 10, and that x is inside
// the view whatever the notch or the corner radius, which is why it is reused rather than a margin of its own); it stands a fraction of the height above the bottom;
// and it may reach as far right as a fraction of the width (the death breakdown at the bottom right starts past the middle of the screen at every aspect seen).
constexpr float kLeftEdge = 10.f;
constexpr float kBottomFrac = 0.04f;    // of the window's height
constexpr float kReachFrac = 0.5f;      // of the window's width, the block's right limit

// Buttons to a row for a window this wide: as many whole pairs as fit under the reach, at least one pair, never more than there are buttons; the rows then split the
// buttons evenly (14 buttons: 6 + 6 + 2 on a wide screen, 4 + 4 + 4 + 2 on a narrow one).
inline int columnsFor(float winWidth) {
    const float room = winWidth * kReachFrac - kLeftEdge;
    int cols = (int)((room + kGap) / (kW + kGap));
    cols -= cols % 2;
    return std::max(2, std::min(cols, kCount + kCount % 2));
}

// Whether a key does anything right now. The polls apply their own conditions too; these are
// the ones a person watching can tell apart, and a button outside them neither lights nor acts.
inline bool keyLive(Key k) {
    switch (k) {
        case Key::Overlay: return !solvingNow();
        // The screen switch belongs to the solve. Once the solution is being shown it is a
        // replay, and a replay that cannot be seen is not a replay.
        case Key::Render: return showingSolve();
        case Key::SeekBack:
        case Key::SeekForward: return !showingSolve();
        // A step advances a stopped game; while time runs there is nothing for it to do.
        case Key::Step1:
        case Key::Step10: return manualPaused();
        default: return true;
    }
}

// The buttons that are switches, and whether each is on. Without this a press that turned the map
// off (its first press in a solve, where it starts on) looked the same as one that did nothing.
inline bool switchedOn(Key k) {
    switch (k) {
        case Key::Itermap: return itermap::mapWanted();
        case Key::Overlay: return g_overlayHidden;
        case Key::Render: return !renderingOn();
        default: return false;
    }
}

class Pad : public cocos2d::CCLayer {
public:
    static Pad* create() {
        auto* p = new Pad();
        if (p && p->init()) {
            p->autorelease();
            return p;
        }
        delete p;
        return nullptr;
    }

    bool init() override {
        using namespace cocos2d;
        if (!CCLayer::init()) return false;
        this->setTag(PAD_TAG);
        this->setID("touch-pad"_spr);
        this->setZOrder(1 << 20);
        const auto win = CCDirector::sharedDirector()->getWinSize();
        const int cols = columnsFor(win.width);
        const int rows = (kCount + cols - 1) / cols;
        const float bottom = win.height * kBottomFrac;
        for (int i = 0; i < kCount; ++i) {
            const float x = kLeftEdge + (float)(i % cols) * (kW + kGap);
            const float y = bottom + (float)(rows - 1 - i / cols) * (kH + kGap);
            auto* bg = CCLayerColor::create({0, 0, 0, 140}, kW, kH);
            bg->setPosition({x, y});
            bg->setTag(i);
            bg->setID(fmt::format("button-{}", i));
            if (kButtons[i].key < 0) {
                makeHandle(bg);
                this->addChild(bg);
                m_bg[i] = bg;
                continue;
            }
            CCNode* face = nullptr;
            if (kButtons[i].icon) {
                if (auto* spr = CCSprite::createWithSpriteFrameName(kButtons[i].icon)) {
                    m_sprite[i] = spr;
                    const auto sz = spr->getContentSize();
                    const float fit = std::min((kW - 6.f) / std::max(sz.width, 1.f),
                                               (kH - 4.f) / std::max(sz.height, 1.f));
                    spr->setScale(fit);
                    spr->setFlipX(kButtons[i].flipIcon);
                    face = spr;
                }
            }
            if (!face) {
                auto* lbl = CCLabelBMFont::create(kButtons[i].label, "bigFont.fnt");
                lbl->limitLabelWidth(kW - 8.f, 0.4f, 0.1f);
                m_label[i] = lbl;
                face = lbl;
            }
            face->setPosition({kW / 2.f, kH / 2.f});
            face->setTag(1);
            bg->addChild(face);
            this->addChild(bg);
            m_bg[i] = bg;
        }
        this->setTouchMode(kCCTouchesOneByOne);
        this->setTouchEnabled(true);
        return true;
    }

    // Ahead of the game's own handlers and its menus, so a press on a button never reaches the
    // level; ccTouchBegan claims only the touches that land on a button.
    void registerWithTouchDispatcher() override {
        cocos2d::CCDirector::sharedDirector()->getTouchDispatcher()->addTargetedDelegate(
            this, -600, true);
    }

    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent*) override {
        if (!isVisible() || blockedByMenu()) return false;
        const int i = hit(touch);
        if (i < 0) return false;
        // A dimmed button still takes the touch (it is on top of the level), it just does nothing.
        const int k = kButtons[i].key;
        if (k >= 0 && !keyLive((Key)k)) return true;
        m_touch[touch->getID() & 15] = i;
        press(i, true);
        return true;
    }
    void ccTouchMoved(cocos2d::CCTouch*, cocos2d::CCEvent*) override {}
    void ccTouchEnded(cocos2d::CCTouch* touch, cocos2d::CCEvent*) override { lift(touch); }
    void ccTouchCancelled(cocos2d::CCTouch* touch, cocos2d::CCEvent*) override { lift(touch); }

    void onExit() override {
        releaseAll();
        CCLayer::onExit();
    }

    // Once a frame: visibility, the folded state and which buttons are live.
    void refresh(bool visible) {
        using namespace cocos2d;
        if (!visible) {
            if (isVisible()) releaseAll();
            setVisible(false);
            return;
        }
        setVisible(true);
        for (int i = 0; i < kCount; ++i) {
            auto* bg = m_bg[i];
            if (!bg) continue;
            const int k = kButtons[i].key;
            bg->setVisible(k < 0 || !m_folded);
            const bool live = k < 0 || keyLive((Key)k);
            // A live key that stopped being live while held (the solve ended under a finger) is
            // let go here, so nothing stays pressed on a dimmed button.
            if (!live) releaseKey(k);
            const bool held = k >= 0 && g_keyDown[(size_t)k];
            bg->setOpacity(held ? 220 : 140);
            // A switch shows which way it is set: the map drawn, the text hidden, the screen off.
            bg->setColor(k < 0 ? kHandleColor
                         : switchedOn((Key)k) ? cocos2d::ccColor3B{40, 90, 170}
                                              : cocos2d::ccColor3B{0, 0, 0});
            if (m_sprite[i]) m_sprite[i]->setOpacity(live ? 255 : 70);
            if (m_label[i]) m_label[i]->setOpacity(live ? 255 : 70);
        }
        // The fold button: the arrow points the way the pad will go.
        // The handle's chevron points the way a press moves the pad: in (left) while it is out,
        // out (right) while it is folded.
        if (m_chevron) {
            const char* want = m_folded ? "edit_rightBtn_001.png" : "edit_leftBtn_001.png";
            if (want != m_chevronFrame) {
                if (auto* frame = cocos2d::CCSpriteFrameCache::sharedSpriteFrameCache()
                                      ->spriteFrameByName(want)) {
                    m_chevron->setDisplayFrame(frame);
                    m_chevronFrame = want;
                }
            }
        }
        // PAUSE shows what a press will do: resume while stopped.
        if (auto* spr = m_sprite[2]) {
            const bool stopped = probe::g_pause || g_paused;
            const char* want = stopped ? "GJ_playEditorBtn_001.png" : "GJ_pauseEditorBtn_001.png";
            if (want != m_pauseFrame) {
                if (auto* frame = CCSpriteFrameCache::sharedSpriteFrameCache()
                                      ->spriteFrameByName(want)) {
                    spr->setDisplayFrame(frame);
                    m_pauseFrame = want;
                }
            }
        }
    }

private:
    cocos2d::CCLayerColor* m_bg[kCount] = {};
    cocos2d::CCSprite* m_sprite[kCount] = {};       // the button's icon, when it has one
    cocos2d::CCLabelBMFont* m_label[kCount] = {};   // ...or its text
    int m_touch[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    bool m_folded = false;
    const char* m_pauseFrame = "GJ_pauseEditorBtn_001.png";
    cocos2d::CCSprite* m_chevron = nullptr;
    const char* m_chevronFrame = "edit_leftBtn_001.png";
    static constexpr cocos2d::ccColor3B kHandleColor{70, 70, 90};

    // The fold handle reads as one: a chevron on the left and the word beside it, on a background
    // of its own colour. Only the word if the chevron's frame is not loaded.
    void makeHandle(cocos2d::CCLayerColor* bg) {
        using namespace cocos2d;
        float textLeft = 4.f;
        if (auto* chev = CCSprite::createWithSpriteFrameName("edit_leftBtn_001.png")) {
            const auto sz = chev->getContentSize();
            chev->setScale((kH - 8.f) / std::max(sz.height, 1.f));
            chev->setPosition({4.f + chev->getScaledContentSize().width / 2.f, kH / 2.f});
            bg->addChild(chev);
            m_chevron = chev;
            textLeft = 6.f + chev->getScaledContentSize().width;
        }
        auto* lbl = CCLabelBMFont::create("MENU", "bigFont.fnt");
        lbl->limitLabelWidth(kW - textLeft - 3.f, 0.35f, 0.1f);
        lbl->setAnchorPoint({0.f, 0.5f});
        lbl->setPosition({textLeft, kH / 2.f});
        bg->addChild(lbl);
    }

    // GD's pause menu sits over the level; the pad must not take its taps. The menu is a child of
    // the play layer.
    bool blockedByMenu() {
        auto* pl = PlayLayer::get();
        return pl && pl->getChildByType<PauseLayer>(0) != nullptr;
    }

    int hit(cocos2d::CCTouch* touch) {
        const auto p = this->convertTouchToNodeSpace(touch);
        for (int i = 0; i < kCount; ++i) {
            auto* bg = m_bg[i];
            if (!bg || !bg->isVisible()) continue;
            if (bg->boundingBox().containsPoint(p)) return i;
        }
        return -1;
    }

    void press(int i, bool down) {
        const int k = kButtons[i].key;
        if (k < 0) {
            if (down) {
                m_folded = !m_folded;
                if (m_folded) releaseAll();
            }
            return;
        }
        g_keyDown[(size_t)k] = down;
    }

    void lift(cocos2d::CCTouch* touch) {
        int& slot = m_touch[touch->getID() & 15];
        if (slot >= 0) press(slot, false);
        slot = -1;
    }

    void releaseKey(int k) {
        if (k < 0) return;
        for (int& slot : m_touch) {
            if (slot >= 0 && kButtons[slot].key == k) {
                g_keyDown[(size_t)k] = false;
                slot = -1;
            }
        }
    }

    // A key must never stay down after the finger that held it is gone from the pad.
    void releaseAll() {
        for (int& slot : m_touch) {
            if (slot >= 0 && kButtons[slot].key >= 0) g_keyDown[(size_t)kButtons[slot].key] = false;
            slot = -1;
        }
    }
};

// Called once a frame from updateOverlays, with the scene the overlays hang from.
inline void update(cocos2d::CCNode* parent) {
    if (!parent) return;
    auto* pad = static_cast<Pad*>(parent->getChildByTag(PAD_TAG));
    const bool want = botDriving() && PlayLayer::get() != nullptr;
    if (!pad && want) {
        pad = Pad::create();
        if (pad) parent->addChild(pad);
    }
    if (pad) pad->refresh(want);
}

}  // namespace touchpad

#endif  // !GEODE_IS_WINDOWS
