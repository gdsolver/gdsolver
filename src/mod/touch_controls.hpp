#pragma once
// On-screen controls for platforms without a keyboard (Android): buttons for the session keys and
// a drawn progress bar.
//
// Every session key is read as a held state, g_keyDown[Key] (session_hotkeys.hpp), which the
// polls turn into presses, repeats and holds. A button here writes the same state while a finger
// is on it, so it does exactly what its key does -- step and seek repeat when held, the speed
// steps once per press -- and there is no second copy of any of that behaviour to drift. A button
// whose key would be refused right now is dimmed and takes no press.
//
// The pad is a column of buttons down the right edge, under GD's own pause button, and only
// while the mod is driving (the same condition as the key legend it replaces). A touch that lands
// on a button is taken; any other touch goes on to the game. The top button folds the pad away.
// Icons are GD's own sprites; a frame the game has not loaded falls back to the text label.
#include <Geode/platform/cplatform.h>

#ifndef GEODE_IS_WINDOWS

namespace touchpad {

constexpr int PAD_TAG = 0x51D70;
constexpr int BAR_TAG = 0x51D71;

struct ButtonSpec {
    int key;             // (int)Key, or -1 for the fold button
    const char* label;   // shown when there is no icon, or the icon's frame is missing
    const char* icon;    // a sprite frame of GD's, or nullptr
    bool flipIcon;       // mirror the icon (one arrow sprite serves both directions)
};
// Two columns, top to bottom. Pairs sit side by side: the pair is the same kind of thing.
inline constexpr ButtonSpec kButtons[] = {
    {-1, "<", "GJ_arrow_01_001.png", false},
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
// Below GD's pause button in the top-right corner.
constexpr float kTopMargin = 44.f, kRightMargin = 6.f;

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
        const float xRight = win.width - kRightMargin - kW;
        const float xLeft = xRight - kGap - kW;
        for (int i = 0; i < kCount; ++i) {
            const float x = (i % 2) ? xRight : xLeft;
            const float y = win.height - kTopMargin - kH - (float)(i / 2) * (kH + kGap);
            auto* bg = CCLayerColor::create({0, 0, 0, 140}, kW, kH);
            bg->setPosition({x, y});
            bg->setTag(i);
            bg->setID(fmt::format("button-{}", i));
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
            bg->setColor(k >= 0 && switchedOn((Key)k) ? cocos2d::ccColor3B{40, 90, 170}
                                                      : cocos2d::ccColor3B{0, 0, 0});
            if (m_sprite[i]) m_sprite[i]->setOpacity(live ? 255 : 70);
            if (m_label[i]) m_label[i]->setOpacity(live ? 255 : 70);
        }
        // The fold button: the arrow points the way the pad will go.
        if (m_sprite[0]) m_sprite[0]->setFlipX(!m_folded);
        if (m_label[0]) m_label[0]->setString(m_folded ? "<" : ">");
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

// ---- The progress bar ----
// Two drawn bars at the top centre, the same two numbers the session text gives: how far into the
// level the run has got (the deepest verified point, which only grows), and -- while a search is
// running -- how much of it is done, in ticks, so the bar reaches the end when the search does.
// Solve sessions only, like the text.
//
// They are GD's own level bar, twice: the groove "slidergroove2.png" with "sliderBar2.png" inside
// it, the fill's texture repeating and its rect cut to the fraction -- which is how
// PlayLayer::setupHasCompleted builds the one at the top of the screen. Plain rectangles stand in
// if the images are missing.
//
// They are a row of the overlay column (hud.hpp), placed by its cursor under the session text, so
// no line of that text can run into them. Both rows keep their place while the search row is
// hidden, or the column would jump every time a search started.
constexpr float kBarW = 200.f, kBarH = 7.f, kRowGap = 20.f;
constexpr float kBarsH = kRowGap + kBarH + 12.f;   // two rows, the upper one's caption included

class Bars : public cocos2d::CCNode {
public:
    static Bars* create() {
        auto* b = new Bars();
        if (b && b->init()) {
            b->autorelease();
            return b;
        }
        delete b;
        return nullptr;
    }

    bool init() override {
        using namespace cocos2d;
        if (!CCNode::init()) return false;
        this->setTag(BAR_TAG);
        this->setID("progress-bars"_spr);
        this->setZOrder(1 << 20);
        makeRow(0, 0.f, kRowGap, {80, 220, 100}, "level");
        makeRow(1, 0.f, 0.f, {90, 170, 255}, "search");
        return true;
    }

    void refresh(bool visible, double levelFrac, const char* levelText, bool searching,
                 double searchFrac, const char* searchText) {
        setVisible(visible);
        if (!visible) return;
        setRow(0, true, levelFrac, levelText);
        setRow(1, searching, searchFrac, searchText);
    }

private:
    cocos2d::CCNode* m_track[2] = {};          // the groove sprite, or the plain track
    cocos2d::CCSprite* m_fillSprite[2] = {};   // GD's fill, cut by texture rect...
    cocos2d::CCLayerColor* m_fillRect[2] = {}; // ...or the plain fill
    float m_fillW[2] = {}, m_fillH[2] = {};    // the fill at 100%, in the groove's own units
    cocos2d::CCLabelBMFont* m_text[2] = {};

    void makeRow(int r, float x, float y, cocos2d::ccColor3B color, const char* name) {
        using namespace cocos2d;
        auto* groove = CCSprite::create("slidergroove2.png");
        auto* fill = groove ? CCSprite::create("sliderBar2.png") : nullptr;
        if (groove && fill) {
            const auto gs = groove->getTextureRect().size;
            groove->setScale(kBarW / std::max(gs.width, 1.f));
            groove->setAnchorPoint({0.f, 0.5f});
            groove->setPosition({x, y + kBarH / 2.f});
            groove->setID(fmt::format("{}-groove", name));
            ccTexParams params{GL_LINEAR, GL_LINEAR, GL_REPEAT, GL_REPEAT};
            fill->getTexture()->setTexParameters(&params);
            fill->setColor(color);
            fill->setAnchorPoint({0.f, 0.f});
            m_fillW[r] = gs.width - 4.f;
            m_fillH[r] = 8.f;
            fill->setPosition({2.f, (gs.height - m_fillH[r]) / 2.f});
            fill->setTextureRect({0.f, 0.f, 0.f, m_fillH[r]});
            groove->addChild(fill, -1);
            this->addChild(groove);
            m_track[r] = groove;
            m_fillSprite[r] = fill;
        } else {
            auto* track = CCLayerColor::create({0, 0, 0, 150}, kBarW, kBarH);
            track->setPosition({x, y});
            track->setID(fmt::format("{}-track", name));
            auto* rect = CCLayerColor::create({color.r, color.g, color.b, 255}, 0.f, kBarH);
            rect->setPosition({x, y});
            rect->setID(fmt::format("{}-fill", name));
            this->addChild(track);
            this->addChild(rect);
            m_track[r] = track;
            m_fillRect[r] = rect;
        }
        m_text[r] = CCLabelBMFont::create("", "chatFont.fnt");
        m_text[r]->setScale(0.45f);
        m_text[r]->setAnchorPoint({0.f, 0.f});
        m_text[r]->setPosition({x, y + kBarH + 2.f});
        m_text[r]->setID(fmt::format("{}-text", name));
        this->addChild(m_text[r]);
    }

    void setRow(int r, bool on, double frac, const char* text) {
        m_track[r]->setVisible(on);
        if (m_fillRect[r]) m_fillRect[r]->setVisible(on);
        m_text[r]->setVisible(on);
        if (!on) return;
        const double f = frac < 0.0 ? 0.0 : (frac > 1.0 ? 1.0 : frac);
        if (m_fillSprite[r])
            m_fillSprite[r]->setTextureRect({0.f, 0.f, (float)(f * m_fillW[r]), m_fillH[r]});
        if (m_fillRect[r]) m_fillRect[r]->setContentSize({(float)(f * kBarW), kBarH});
        m_text[r]->setString(text);
    }
};

// One row of the overlay column: drawn with its top at `y`, which then moves below it.
inline void updateBars(cocos2d::CCNode* parent, bool show, float& y) {
    if (!parent) return;
    auto* bars = static_cast<Bars*>(parent->getChildByTag(BAR_TAG));
    const bool want = show && showingSolve() && g_cfg.dpSolve && PlayLayer::get() != nullptr;
    if (!bars) {
        if (!want) return;
        bars = Bars::create();
        if (!bars) return;
        parent->addChild(bars);
    }
    if (!want) {
        bars->refresh(false, 0, "", false, 0, "");
        return;
    }
    bars->setPosition({10.f, y - kBarsH});
    y -= kBarsH + 3.f;
    // setString rebuilds glyph sprites; a few times a second is plenty for a progress bar.
    static auto s_last = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - s_last).count() < 250
        && bars->isVisible())
        return;
    s_last = now;
    float len = 0.f;
    if (auto* pl = PlayLayer::get()) len = pl->m_levelLength;
    const double lvl = (len > 1.f) ? (double)len : 0.0;
    const double lf = lvl > 0.0 ? (double)g_hudVerifiedX / lvl : 0.0;
    char lt[96];
    snprintf(lt, sizeof(lt), "level %.1f%%   iter %d", lf * 100.0, g_hudIter);
    const dpbridge::SolveProgress pr = dpbridge::progress();
    double sf = 0.0;
    char st[96] = "";
    if (pr.running && pr.horizon > pr.from) {
        sf = (double)(pr.tick - pr.from) / (double)(pr.horizon - pr.from);
        snprintf(st, sizeof(st), "search %.1f%%", sf * 100.0);
    }
    bars->refresh(true, lf, lt, pr.running, sf, st);
}

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
