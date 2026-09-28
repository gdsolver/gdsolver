#pragma once
// On-screen buttons for the session keys, for platforms without a keyboard (Android).
//
// Every session key is read as a held state, g_keyDown[Key] (session_hotkeys.hpp), which the
// polls turn into presses, repeats and holds. A button here writes the same state while a finger
// is on it, so it does exactly what its key does -- step and seek repeat when held, the speed
// steps once per press -- and there is no second copy of any of that behaviour to drift.
//
// The pad is a column of buttons down the right edge, under GD's own pause button, and only
// while the mod is driving (the same condition as the key legend it replaces). A touch that lands
// on a button is taken; any other touch goes on to the game. The top button folds the pad away.
#include <Geode/platform/cplatform.h>

#ifndef GEODE_IS_WINDOWS

namespace touchpad {

constexpr int PAD_TAG = 0x51D70;

struct ButtonSpec {
    int key;             // (int)Key, or -1 for the fold button
    const char* label;
};
// Two columns, top to bottom. Pairs sit side by side: the pair is the same kind of thing.
inline constexpr ButtonSpec kButtons[] = {
    {-1, "MENU"},                     {(int)Key::Overlay, "TEXT"},
    {(int)Key::Pause, "PAUSE"},       {(int)Key::Quit, "QUIT"},
    {(int)Key::Step1, "+1"},          {(int)Key::Step10, "+10"},
    {(int)Key::SeekBack, "<"},        {(int)Key::SeekForward, ">"},
    {(int)Key::Slower, "<<"},         {(int)Key::Faster, ">>"},
    {(int)Key::Replay, "REPLAY"},     {(int)Key::Itermap, "MAP"},
    {(int)Key::Render, "SCREEN"},     {(int)Key::Hitboxes, "HITBOX"},
};
constexpr int kCount = (int)(sizeof(kButtons) / sizeof(kButtons[0]));
constexpr float kW = 46.f, kH = 24.f, kGap = 4.f;
// Below GD's pause button in the top-right corner.
constexpr float kTopMargin = 44.f, kRightMargin = 6.f;

// Whether a key does anything right now: a button that would be refused is drawn dimmed, the
// same conditions the polls apply.
inline bool keyLive(Key k) {
    switch (k) {
        case Key::Overlay: return !solvingNow();
        case Key::Render: return solveSession();
        case Key::SeekBack:
        case Key::SeekForward: return !showingSolve();
        default: return true;
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
        const float xRight = win.width - kRightMargin - kW;
        const float xLeft = xRight - kGap - kW;
        for (int i = 0; i < kCount; ++i) {
            const float x = (i % 2) ? xRight : xLeft;
            const float y = win.height - kTopMargin - kH - (float)(i / 2) * (kH + kGap);
            auto* bg = CCLayerColor::create({0, 0, 0, 140}, kW, kH);
            bg->setPosition({x, y});
            bg->setTag(i);
            bg->setID(fmt::format("button-{}", i));
            auto* lbl = CCLabelBMFont::create(kButtons[i].label, "bigFont.fnt");
            lbl->limitLabelWidth(kW - 8.f, 0.4f, 0.1f);
            lbl->setPosition({kW / 2.f, kH / 2.f});
            lbl->setTag(1);
            bg->addChild(lbl);
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
            const bool held = k >= 0 && g_keyDown[(size_t)k];
            bg->setOpacity(held ? 220 : 140);
            if (auto* lbl = static_cast<cocos2d::CCLabelBMFont*>(bg->getChildByTag(1)))
                lbl->setOpacity(live ? 255 : 90);
        }
        if (auto* lbl = m_bg[0] ? static_cast<cocos2d::CCLabelBMFont*>(m_bg[0]->getChildByTag(1))
                                : nullptr)
            lbl->setString(m_folded ? "KEYS" : "MENU");
    }

private:
    cocos2d::CCLayerColor* m_bg[kCount] = {};
    int m_touch[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    bool m_folded = false;

    // GD's pause menu (and anything like it) sits over the level; the pad must not take its
    // taps. The menu is a child of the play layer.
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
    if (!pad) {
        if (!want) return;
        pad = Pad::create();
        if (!pad) return;
        parent->addChild(pad);
    }
    pad->refresh(want);
}

}  // namespace touchpad

#endif  // !GEODE_IS_WINDOWS
