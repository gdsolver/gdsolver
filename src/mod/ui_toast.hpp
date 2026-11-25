#pragma once
// The mod's own notification toast, for builds on GD 2.209 (see ui_popup.hpp for the general reason).
//
// geode::Notification cannot be used there: Notification::create builds a NineSlice and adds
// children through CCNode virtual calls compiled at 2.2081's slot numbers. Run on 2.209 the first
// notification throws a C++ exception from inside libcocos2d (CCSpriteBatchNode::addChild, reached
// through NineSlice::setColor from Notification::init), on every frame the mod tries to raise one.
// Geode.dll's own queue is not reproduced: the mod keeps one notification of its own at a time
// (notify.hpp), so this one shows at once and takes itself off the scene when its time is up.
//
// It offers the calls notify.hpp makes of geode::Notification -- create, show, hide, cancel,
// isShowing, setTime -- and follows the same life: fade in 0.3 s, wait, fade out 0.5 s.
#include <Geode/Geode.hpp>
#include "mod/ui_popup.hpp"

namespace gdsolver::ui {

#if GEODE_COMP_GD_VERSION >= 22090 && !defined(GDSOLVER_GEODE6)

class Toast : public cocos2d::CCNodeRGBA {
public:
    static Toast* create(std::string const& text, geode::NotificationIcon icon, float seconds) {
        auto* t = new Toast();
        if (t && t->init(text, icon, seconds)) {
            t->autorelease();
            return t;
        }
        delete t;
        return nullptr;
    }

    void show() {
        using namespace cocos2d;
        if (m_showing) return;
        auto* scene = CCDirector::sharedDirector()->getRunningScene();
        if (!scene) return;
        m_showing = true;
        const auto win = CCDirector::sharedDirector()->getWinSize();
        this->setPosition({win.width / 2, win.height - this->getContentSize().height / 2 - 6.f});
        this->setOpacity(0);
        scene->addChild(this, 100000);
        this->runAction(CCSequence::create(CCFadeIn::create(.3f), CCDelayTime::create(m_time), CCFadeOut::create(.5f),
                                           CCCallFunc::create(this, callfunc_selector(Toast::finish)), nullptr));
    }

    // A scene change takes a notification with it: the scene it was shown on is released and its children are cleaned up,
    // which also stops the fade chain. Called once a frame (notify::watchdog): put it on the scene that is running now and,
    // if its actions were stopped, start the wait-then-hide again.
    void keepOnScene() {
        if (!m_showing) return;
        auto* scene = cocos2d::CCDirector::sharedDirector()->getRunningScene();
        if (!scene || this->getParent() == scene) return;
        this->retain();
        this->removeFromParentAndCleanup(false);
        scene->addChild(this, 100000);
        this->release();
        if (this->numberOfRunningActions() == 0) this->setTime(std::max(.4f, m_time));
    }

    // Fade out now, then leave the scene.
    void hide() {
        using namespace cocos2d;
        if (!m_showing) return;
        this->stopAllActions();
        this->runAction(CCSequence::create(CCFadeOut::create(.5f), CCCallFunc::create(this, callfunc_selector(Toast::finish)), nullptr));
    }

    // Leave the scene at once.
    void cancel() {
        this->stopAllActions();
        this->finish();
    }

    bool isShowing() const { return m_showing; }

    // The wait-then-hide chain again, with `seconds` left to wait (the notification is shown in full meanwhile).
    void setTime(float seconds) {
        using namespace cocos2d;
        m_time = seconds;
        if (!m_showing) return;
        this->stopAllActions();
        this->setOpacity(255);
        this->runAction(CCSequence::create(CCDelayTime::create(m_time), CCFadeOut::create(.5f),
                                           CCCallFunc::create(this, callfunc_selector(Toast::finish)), nullptr));
    }

private:
    bool m_showing = false;
    float m_time = 2.f;

    static char const* frameFor(geode::NotificationIcon icon) {
        switch (icon) {
            case geode::NotificationIcon::Success: return "GJ_completesIcon_001.png";
            case geode::NotificationIcon::Error: return "exMark_001.png";
            case geode::NotificationIcon::Warning: return "exMark_001.png";
            case geode::NotificationIcon::Info: return "GJ_infoIcon_001.png";
            default: return nullptr;
        }
    }

    bool init(std::string const& text, geode::NotificationIcon icon, float seconds) {
        using namespace cocos2d;
        if (!CCNodeRGBA::init()) return false;
        m_time = seconds;
        this->setCascadeOpacityEnabled(true);
        this->setCascadeColorEnabled(false);

        auto* label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
        label->limitLabelWidth(320.f, .5f, .2f);
        const float labelW = label->getContentSize().width * label->getScaleX();

        CCSprite* spr = nullptr;
        if (auto const* frame = frameFor(icon)) spr = CCSprite::createWithSpriteFrameName(frame);
        const float iconH = 22.f;
        float iconW = 0.f;
        if (spr) {
            spr->setScale(iconH / spr->getContentSize().height);
            iconW = spr->getContentSize().width * spr->getScaleX() + 8.f;
        }

        const float pad = 12.f;
        const float w = labelW + iconW + pad * 2;
        const float h = 32.f;
        this->setContentSize({w, h});
        this->setAnchorPoint({.5f, .5f});

        auto* bg = extension::CCScale9Sprite::create("square02b_001.png", CCRect{0.f, 0.f, 80.f, 80.f});
        bg->setContentSize({w, h});
        bg->setPosition({w / 2, h / 2});
        bg->setColor({0, 0, 0});
        bg->setOpacity(160);
        this->addChild(bg, 0);

        float x = pad;
        if (spr) {
            spr->setPosition({x + (iconW - 8.f) / 2, h / 2});
            this->addChild(spr, 1);
            x += iconW;
        }
        label->setAnchorPoint({0.f, .5f});
        label->setPosition({x, h / 2});
        this->addChild(label, 1);
        return true;
    }

    void finish() {
        m_showing = false;
        this->removeFromParent();
    }
};

#endif

}  // namespace gdsolver::ui
