#pragma once
// The mod's own UI helpers, for builds on GD 2.209 where the ones Geode.dll provides cannot be used.
//
// Geode.dll is built against 2.2081 and its code is compiled with that game's addresses and vtable
// numbers. Two things in it break when the game is 2.209 (measured by disassembling Geode.dll, not
// by name):
//   * a call through an address baked at 2.2081: geode::Popup's constructor goes through Geode's own
//     FLAlertLayer::FLAlertLayer, which calls the game's constructor at the 2.2081 address -- in the
//     middle of another function on 2.209 -- so a mod that creates a popup brings the game down;
//   * a virtual call on a CCNode at a slot of 30 or more: 2.209 puts two virtuals (offsetPosition,
//     applyScale) into CCNode's table, so every CCNode-derived slot above them moved by 16 bytes.
//     Geode.dll's getContentSize lands on getAnchorPointInPoints, its setContentSize on
//     getAnchorPoint, and so on. CCNode::addChildAtPosition, updateAnchoredPosition,
//     getScaledContentWidth, geode::cocos::limitNodeWidth, CCNode::setLayout and NineSlice::create
//     are all of that kind.
// A function compiled into this mod goes through this mod's own bindings and the SDK's CCNode.h,
// whose vtable carries the two extra slots, so it lands where it should. What follows is those
// functions written out here for the members the mod uses. Where a Geode function is the source it
// is read from the Geode.dll disassembly and the Geode SDK (https://github.com/geode-sdk/geode,
// copyright the Geode contributors, Boost Software License, Version 1.0,
// https://www.boost.org/LICENSE_1_0.txt): limitNodeWidth is
// setScale(width/contentWidth > default ? default : max(min, width/contentWidth)), and the anchored
// position is offset + anchor-fraction * the parent's content size.
// Changes against Geode: no AnchorLayoutOptions are attached (nothing here has a layout), no
// force-priority touch registration (CCTouchDispatcher::registerForcePrio is gone from 2.209's
// libcocos2d), no close event, a plain title argument, a Scale9 background instead of NineSlice.
//
// On 2.2081 these are forwards to Geode's own members, so that build behaves exactly as before.
#include <Geode/Geode.hpp>
#include <Geode/ui/Layout.hpp>
#if GEODE_COMP_GD_VERSION < 22090 || defined(GDSOLVER_GEODE6)
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/ui/Scrollbar.hpp>
#endif
#include <algorithm>
#include <unordered_map>

namespace gdsolver::ui {

#if GEODE_COMP_GD_VERSION >= 22090 && !defined(GDSOLVER_GEODE6)

inline cocos2d::CCPoint anchorFraction(geode::Anchor a) {
    using geode::Anchor;
    switch (a) {
        case Anchor::TopLeft: return {0.f, 1.f};
        case Anchor::Top: return {.5f, 1.f};
        case Anchor::TopRight: return {1.f, 1.f};
        case Anchor::Right: return {1.f, .5f};
        case Anchor::BottomRight: return {1.f, 0.f};
        case Anchor::Bottom: return {.5f, 0.f};
        case Anchor::BottomLeft: return {0.f, 0.f};
        case Anchor::Left: return {0.f, .5f};
        default: return {.5f, .5f};
    }
}

inline void updateAnchoredPosition(cocos2d::CCNode* node, geode::Anchor a, cocos2d::CCPoint const& offset) {
    auto* parent = node->getParent();
    if (!parent) return;
    const auto f = anchorFraction(a);
    const auto size = parent->getContentSize();
    node->setPosition({offset.x + f.x * size.width, offset.y + f.y * size.height});
}

inline void addChildAtPosition(cocos2d::CCNode* parent, cocos2d::CCNode* child, geode::Anchor a, cocos2d::CCPoint const& offset) {
    parent->addChild(child);
    updateAnchoredPosition(child, a, offset);
}

// Where the touch handlers of the mod's popups sit. GD 2.209's CCTouchDispatcher clamps a handler registered while one of the game's own popups is up
// (the level-complete screen is one: it holds a "force priority" of 2, which moves the base from -500 to -502) to that base, and the clamp stays when
// the popup goes: leaving a level through the end screen's Menu button leaves the level page's handlers at -502 / -503 for good. A popup of the
// mod registered at a fixed -500 then sits behind the game's own menus, which take the touches meant for it. The game's popups avoid this by
// registering a force priority of their own, which lowers the base under everything registered before; the mod's popups take the other way out:
// a priority far below anything the game's clamp can produce, and below every popup of the mod that is already up (a popup opened from another
// one is the top one). A popup's own scroll window and bar sit just below it, so that a drag that starts in them is theirs.
namespace popupprio {
inline constexpr int kTop = -700;
inline std::unordered_map<const cocos2d::CCNode*, int>& table() {
    static std::unordered_map<const cocos2d::CCNode*, int> t;
    return t;
}
inline int takeNext(const cocos2d::CCNode* popup) {
    if (auto it = table().find(popup); it != table().end()) return it->second;   // registered again: where it was
    int lowest = kTop + 4;
    for (auto const& [n, p] : table()) lowest = std::min(lowest, p);
    const int mine = lowest - 4;
    table()[popup] = mine;
    return mine;
}
inline void release(const cocos2d::CCNode* popup) { table().erase(popup); }
// the priority of the popup this node is in (kTop when it is in none)
inline int of(cocos2d::CCNode* node) {
    for (auto* n = node; n; n = n->getParent()) {
        auto it = table().find(n);
        if (it != table().end()) return it->second;
    }
    return kTop;
}
}  // namespace popupprio

inline float scaledContentWidth(cocos2d::CCNode* node) { return node->getContentSize().width * node->getScaleX(); }
inline float scaledContentHeight(cocos2d::CCNode* node) { return node->getContentSize().height * node->getScaleY(); }

inline void limitNodeWidth(cocos2d::CCNode* node, float width, float defaultScale, float minScale) {
    const float r = width / node->getContentSize().width;
    node->setScale(defaultScale < r ? defaultScale : std::max(minScale, r));
}

// A window onto content taller than itself, clipped to its own rectangle and dragged with a finger: what geode::ScrollLayer is on 2.2081
// (it is built on a game class Geode.dll reaches through baked addresses, so on 2.209 its constructor lands in the wrong function). The
// content goes in m_contentLayer, which the caller gives its size; its origin is the bottom left corner and its children's y runs up from
// there, as in Geode's. scrollToTop() puts the top of the content at the top of the window. Clipping is a GL scissor over the window's
// rectangle in world space, the way Geode's does it, but through CCEGLView::sharedOpenGLView(): CCEGLView::get() is Geode.dll's own, which reads
// the director's view field at 2.2081's offset (the first version of this class crashed there). Touches are taken at a priority above the
// popup's (-500), so a drag that starts in the window is the window's; the mouse wheel is not wired up here.
class ScrollLayer : public cocos2d::CCLayer {
public:
    cocos2d::CCNode* m_contentLayer = nullptr;

    static ScrollLayer* create(cocos2d::CCSize const& size) {
        auto* r = new ScrollLayer();
        if (r->init(size)) { r->autorelease(); return r; }
        delete r;
        return nullptr;
    }

    float maxScroll() const { return std::max(0.f, m_contentLayer->getContentSize().height - this->getContentSize().height); }
    float scrollOffset() const { return m_scroll; }   // how far the content has moved up from its top-aligned place
    void setScroll(float s) { m_scroll = std::clamp(s, 0.f, maxScroll()); place(); }
    void scrollBy(float dy) { setScroll(m_scroll + dy); }
    void scrollToTop() { setScroll(0.f); }

    void visit() override {
        using namespace cocos2d;
        if (!this->isVisible()) return;
        const CCPoint bl = this->convertToWorldSpace(CCPoint{0.f, 0.f});
        const CCPoint tr = this->convertToWorldSpace(CCPoint{this->getContentSize().width, this->getContentSize().height});
        glEnable(GL_SCISSOR_TEST);
        CCEGLView::sharedOpenGLView()->setScissorInPoints(bl.x, bl.y, tr.x - bl.x, tr.y - bl.y);
        CCLayer::visit();
        glDisable(GL_SCISSOR_TEST);
    }

protected:
    float m_scroll = 0.f;
    cocos2d::CCPoint m_last;

    bool init(cocos2d::CCSize const& size) {
        using namespace cocos2d;
        if (!CCLayer::init()) return false;
        this->setContentSize(size);
        m_contentLayer = CCNode::create();
        m_contentLayer->setAnchorPoint({0.f, 0.f});
        m_contentLayer->setContentSize(size);
        this->addChild(m_contentLayer);
        this->setTouchEnabled(true);
        place();
        return true;
    }

    void place() {
        m_contentLayer->setPosition({0.f, this->getContentSize().height - m_contentLayer->getContentSize().height + m_scroll});
    }

    bool shown() {
        for (cocos2d::CCNode* n = this; n; n = n->getParent())
            if (!n->isVisible()) return false;
        return true;
    }

    void registerWithTouchDispatcher() override {
        cocos2d::CCDirector::sharedDirector()->getTouchDispatcher()->addTargetedDelegate(this, popupprio::of(this) - 1, true);
    }

    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent*) override {
        if (!shown()) return false;
        const auto p = this->convertToNodeSpace(touch->getLocation());
        const auto s = this->getContentSize();
        if (p.x < 0.f || p.y < 0.f || p.x > s.width || p.y > s.height) return false;
        m_last = touch->getLocation();
        return true;
    }

    void ccTouchMoved(cocos2d::CCTouch* touch, cocos2d::CCEvent*) override {
        const auto cur = touch->getLocation();
        // a finger moving up moves the content up: the offset from the top grows
        scrollBy(this->convertToNodeSpace(cur).y - this->convertToNodeSpace(m_last).y);
        m_last = cur;
    }
};

// The bar beside a ScrollLayer: a track as tall as the window and a thumb as tall as the part of the content the window shows, which follows
// the scroll every frame and can be dragged. It is centred on its position (as Geode's is), eight wide.
class Scrollbar : public cocos2d::CCLayer {
public:
    static Scrollbar* create(ScrollLayer* target) {
        auto* r = new Scrollbar();
        if (r->init(target)) { r->autorelease(); return r; }
        delete r;
        return nullptr;
    }

    void update(float) override {
        const float contentH = m_target->m_contentLayer->getContentSize().height;
        const float viewH = m_target->getContentSize().height;
        const float h = this->getContentSize().height;
        const float thumbH = (contentH <= viewH) ? h : std::clamp(h * viewH / contentH, 24.f, h);
        const float maxS = m_target->maxScroll();
        const float frac = maxS > 0.f ? m_target->scrollOffset() / maxS : 0.f;
        m_thumb->setContentSize({this->getContentSize().width, thumbH});
        m_thumb->setPositionY((h - thumbH) * (1.f - frac));
    }

protected:
    ScrollLayer* m_target = nullptr;   // the window this belongs to; its parent keeps it alive
    cocos2d::CCLayerColor* m_track = nullptr;
    cocos2d::CCLayerColor* m_thumb = nullptr;

    bool init(ScrollLayer* target) {
        using namespace cocos2d;
        if (!CCLayer::init()) return false;
        m_target = target;
        const float w = 8.f, h = target->getContentSize().height;
        this->setContentSize({w, h});
        this->ignoreAnchorPointForPosition(false);
        this->setAnchorPoint({.5f, .5f});
        m_track = CCLayerColor::create({0, 0, 0, 90}, w, h);
        this->addChild(m_track);
        m_thumb = CCLayerColor::create({255, 255, 255, 150}, w, h);
        this->addChild(m_thumb);
        this->setTouchEnabled(true);
        this->scheduleUpdate();
        update(0.f);
        return true;
    }

    void registerWithTouchDispatcher() override {
        cocos2d::CCDirector::sharedDirector()->getTouchDispatcher()->addTargetedDelegate(this, popupprio::of(this) - 2, true);
    }

    void dragTo(cocos2d::CCPoint const& world) {
        const auto p = this->convertToNodeSpace(world);
        const float h = this->getContentSize().height, thumbH = m_thumb->getContentSize().height;
        if (h <= thumbH) return;
        const float frac = std::clamp(1.f - (p.y - thumbH / 2.f) / (h - thumbH), 0.f, 1.f);
        m_target->setScroll(frac * m_target->maxScroll());
    }

    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent*) override {
        for (cocos2d::CCNode* n = this; n; n = n->getParent())
            if (!n->isVisible()) return false;
        const auto p = this->convertToNodeSpace(touch->getLocation());
        const auto s = this->getContentSize();
        if (p.x < -6.f || p.y < 0.f || p.x > s.width + 6.f || p.y > s.height) return false;
        dragTo(touch->getLocation());
        return true;
    }

    void ccTouchMoved(cocos2d::CCTouch* touch, cocos2d::CCEvent*) override { dragTo(touch->getLocation()); }
};
class Popup : public FLAlertLayer {
protected:
    cocos2d::CCSize m_size;
    cocos2d::extension::CCScale9Sprite* m_bgSprite = nullptr;
    cocos2d::CCLabelBMFont* m_title = nullptr;
    CCMenuItemSpriteExtra* m_closeBtn = nullptr;

    void registerWithTouchDispatcher() override {
        cocos2d::CCDirector::sharedDirector()->getTouchDispatcher()->addTargetedDelegate(this, popupprio::takeNext(this), true);
    }

public:
    ~Popup() override { popupprio::release(this); }

protected:

    bool init(float width, float height, char const* bg = "GJ_square01.png", cocos2d::CCRect bgRect = {0.f, 0.f, 80.f, 80.f}) {
        using namespace cocos2d;
        m_size = CCSize{width, height};
        if (!this->initWithColor({0, 0, 0, 105})) return false;
        const auto winSize = CCDirector::sharedDirector()->getWinSize();

        m_mainLayer = CCLayer::create();
        m_mainLayer->ignoreAnchorPointForPosition(false);
        m_mainLayer->setContentSize(m_size);
        m_mainLayer->setPosition({winSize.width / 2, winSize.height / 2});
        this->addChild(m_mainLayer);

        m_bgSprite = extension::CCScale9Sprite::create(bg, bgRect);
        m_bgSprite->setContentSize(m_size);
        m_bgSprite->setPosition({m_size.width / 2, m_size.height / 2});
        m_mainLayer->addChild(m_bgSprite);

        m_buttonMenu = CCMenu::create();
        m_buttonMenu->setZOrder(100);
        m_buttonMenu->setContentSize(m_size);
        m_buttonMenu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(m_buttonMenu);

        auto* spr = CCSprite::createWithSpriteFrameName("GJ_closeBtn_001.png");
        spr->setScale(0.8f);
        m_closeBtn = CCMenuItemSpriteExtra::create(spr, nullptr, this, menu_selector(Popup::onCloseButton));
        gdsolver::ui::addChildAtPosition(m_buttonMenu, m_closeBtn, geode::Anchor::TopLeft, {3.f, -3.f});

        this->setKeypadEnabled(true);
        this->setTouchEnabled(true);
        return true;
    }

    void onCloseButton(cocos2d::CCObject* sender) { this->onClose(sender); }

    void keyBackClicked() override { this->onClose(nullptr); }

    void keyDown(cocos2d::enumKeyCodes key, double p1) override {
        if (key == cocos2d::enumKeyCodes::KEY_Space) return;
        FLAlertLayer::keyDown(key, p1);
    }

    virtual void onClose(cocos2d::CCObject*) {
        this->setKeypadEnabled(false);
        this->setTouchEnabled(false);
        this->removeFromParent();
    }

    void setTitle(char const* title, char const* font = "goldFont.fnt", float scale = .7f, float offset = 20.f) {
        using namespace cocos2d;
        if (!m_title) {
            m_title = CCLabelBMFont::create("", font);
            m_title->setZOrder(2);
            gdsolver::ui::addChildAtPosition(m_mainLayer, m_title, geode::Anchor::Top, {0.f, -offset});
        }
        m_title->setString(title);
        m_title->limitLabelWidth(m_size.width - 20.f, scale, .1f);
    }
};

#else  // 2.2081: Geode's own

inline void updateAnchoredPosition(cocos2d::CCNode* node, geode::Anchor a, cocos2d::CCPoint const& offset) {
    node->updateAnchoredPosition(a, offset);
}
inline void addChildAtPosition(cocos2d::CCNode* parent, cocos2d::CCNode* child, geode::Anchor a, cocos2d::CCPoint const& offset) {
    parent->addChildAtPosition(child, a, offset);
}
inline float scaledContentWidth(cocos2d::CCNode* node) { return node->getScaledContentWidth(); }
inline float scaledContentHeight(cocos2d::CCNode* node) { return node->getScaledContentHeight(); }
using ScrollLayer = geode::ScrollLayer;
using Scrollbar = geode::Scrollbar;
inline void limitNodeWidth(cocos2d::CCNode* node, float width, float defaultScale, float minScale) {
    geode::cocos::limitNodeWidth(node, width, defaultScale, minScale);
}

#endif

}  // namespace gdsolver::ui
