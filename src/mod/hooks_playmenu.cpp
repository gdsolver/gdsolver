// The play menu: the popup a level's play button opens, where the session's mode is chosen.
#include "mod/playlayer_helpers.hpp"
#include "mod/level_warnings.hpp"
#include "mod/window_guard.hpp"
#include "mod/gd_compat.hpp"
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/LevelPage.hpp>
#include <Geode/modify/EditLevelLayer.hpp>
#include <Geode/modify/CCMenuItemSpriteExtra.hpp>
#include <Geode/ui/Popup.hpp>
#include "mod/ui_popup.hpp"   // the layout helpers, and on 2.209 the popup base and the scroll window: see that file
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string_view>

using namespace p1;
namespace ui = gdsolver::ui;

#if GEODE_COMP_GD_VERSION >= 22090
using PopupBase = ui::Popup;
#else
using PopupBase = geode::Popup;
#endif

// ---- The play menu ---------------------------------------------------------------------------
// Pressing a level's play button opens this instead of starting the level: the three modes side
// by side -- Normal (play it yourself), Replay (a stored solution) and Solve (solve it here) --
// the Coins switch under them, and Start. Start hands the press back to the game's own onPlay,
// so every check the game makes before a level starts (a song that is not downloaded, the
// object-count warning, ...) still happens, in the game's own words; and it arms the chosen mode
// for this level (g_uiArmedLevel), which PlayLayer::init reads and disarms.
//
// Three screens start a level, each with its own play button: the main-level wheel (LevelPage),
// the page of an online or downloaded level (LevelInfoLayer) and a locally saved one
// (EditLevelLayer). Each hook lets the press straight through when the game would not start a
// level from it anyway -- the early-outs its own onPlay takes, read in the 2.2081 binary -- so
// the menu never opens in front of a press that does nothing. A platformer level is let through
// as well: the solver does not play platformer mode, so there is nothing to choose.
namespace playmenu {

// Set while Start hands the press back, so the hook it goes through lets it pass.
inline bool g_passing = false;

// A session in progress is not interfered with: an autorun or a suite drives itself.
inline bool sessionBusy() { return g_started && !g_sessionOver; }

// A click on the play button opens the menu (the default). The setting "play-menu" is that; off, a click plays the level, as
// without the mod, and the menu opens on a hold instead (holdOpens, below): a way to the menu is always left, so the setting
// can be turned either way from the menu itself.
inline bool wanted() {
    if (g_passing || sessionBusy()) return false;
    return Mod::get()->getSettingValue<bool>("play-menu");
}
inline bool holdOpens() {
    if (g_passing || sessionBusy()) return false;
    return !Mod::get()->getSettingValue<bool>("play-menu");
}

// What the menu has to know about a level before it opens: whether the game will play it in
// platformer mode, and how many coins it has. Both come from the level string -- the header's
// kA22 is the mode setting the game plays by, and a coin is an object with ID 142 (secret) or
// 1329 (user), the two the solver collects (solver.hpp) -- so a local level is judged by what it
// is now rather than by fields its last save or the server filled in.
struct LevelFacts {
    bool platformer = false;
    int coins = 0;
    bool random = false;   // uses the game's random numbers (isRandomObject)
    // What the model cannot express (red) and whether it holds objects from 2.0 on (yellow,
    // custom levels only) -- see level_warnings.hpp.
    uint32_t unmodelled = 0;
    bool newer = false;
    int newest = 0;          // the newest version among them (22 = 2.2): Findings::newest
    bool official = false;   // a main level (GJLevelType::Main), where the yellow note is not shown
    std::string newerIds = "none";   // which objects from 2.0 on, for the log (newerSummary)
    // Reads the attempt count or keeps items across attempts, and the attempt pin is on
    // (g_attemptOne): solved as its first attempt, a yellow note rather than a red one.
    bool attempts = false;
    // Two-player mode (header kA10): the second player is planned too, so it is a yellow note (the support is
    // experimental and a level whose press switches the other player's world is not solved), not a red one.
    bool twoPlayer = false;
    // Reads the attempt count or keeps items across attempts, but only what is shown depends on it
    // (levelwarn::attemptReachesPlay): no note, a line in the log.
    bool attemptShown = false;
};

// The key's value in a "k,v,k,v" run, or empty.
inline std::string_view valueOf(std::string_view kv, std::string_view key) {
    size_t p = 0;
    while (p < kv.size()) {
        const size_t c1 = kv.find(',', p);
        if (c1 == std::string_view::npos) break;
        const size_t c2 = kv.find(',', c1 + 1);
        const size_t e = (c2 == std::string_view::npos) ? kv.size() : c2;
        if (kv.substr(p, c1 - p) == key) return kv.substr(c1 + 1, e - c1 - 1);
        if (c2 == std::string_view::npos) break;
        p = c2 + 1;
    }
    return {};
}

// Does this object make the level depend on GD's random numbers? A Random or Advanced Random
// trigger (1912, 2068) always does. An area or enter effect trigger (3006-3015, 3017-3021) does
// when any of its variances is set: the keys are the ones EnterEffectObject::getSaveString
// (0x496630) writes for the fields EnterEffectInstance::loadValuesFromObject (0x1387a0) copies
// into the variances areaenv reads -- 223 length, 221 offset, 253 offset y, 219 distance,
// 232 angle, 238 x move, 240 y move, 234 x scale, 236 y scale, 285 rotation. -99 is an edit
// trigger's "leave as it is". Checked on lv22: its Area Move writes 222,1500 and 223,500 (length
// 1500 +- 500) and its Edit Area Move 218,700 and 219,150 (distance 700 +- 150).
inline bool isRandomObject(std::string_view obj, std::string_view id) {
    if (id == "1912" || id == "2068") return true;
    int n = 0;
    for (char ch : id) {
        if (ch < '0' || ch > '9') return false;
        n = n * 10 + (ch - '0');
    }
    if (n < 3006 || n > 3021 || n == 3016) return false;
    for (std::string_view key : {"223", "221", "253", "219", "232", "238", "240", "234", "236",
                                 "285"}) {
        const auto v = valueOf(obj, key);
        if (!v.empty() && v != "0" && v != "-99") return true;
    }
    return false;
}

inline LevelFacts readLevel(GJGameLevel* level, bool mainLevel) {
    LevelFacts f;
    f.official = level->m_levelType == GJLevelType::Main;
    // The main-level wheel builds its levels without their string (LevelSelectLayer::init asks
    // getMainLevel for none); LevelPage::playStep3 fetches it from LocalLevelManager just before
    // the level starts, and so does this.
    std::string s = mainLevel
        ? std::string(LocalLevelManager::sharedState()->getMainLevelString(
              level->m_levelID.value()))
        : std::string(level->m_levelString);
    // The main levels' files (Resources/levels/N.txt) begin with whitespace before the data --
    // Polargeist's with four spaces -- and a test for "H4sI" at position 0 left them compressed,
    // so every official level read as having no coins. Trim before looking.
    const size_t b = s.find_first_not_of(" \t\r\n");
    const size_t e = s.find_last_not_of(" \t\r\n");
    s = (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
    s = gdcompat::rawLevelString(std::move(s));    // the stored, compressed form: gzip ("H4sI") or zlib ("eJ", a level made before 2.0)
    if (s.empty()) {
        // No string to read (not expected on a level that can start): fall back to the fields.
        f.platformer = level->isPlatformer();
        f.coins = level->m_coins;
        return f;
    }
    const std::string_view all(s);
    const size_t head = all.find(';');
    f.platformer = valueOf(all.substr(0, head), "kA22") == "1";
    for (size_t pos = head; pos != std::string_view::npos;) {
        const size_t end = all.find(';', pos + 1);
        const auto obj = all.substr(pos + 1, end == std::string_view::npos
                                                 ? std::string_view::npos : end - pos - 1);
        const auto id = valueOf(obj, "1");
        if (id == "142" || id == "1329") ++f.coins;
        if (!f.random && isRandomObject(obj, id)) f.random = true;
        pos = end;
    }
    const auto w = levelwarn::scan(all, f.platformer);
    f.unmodelled = w.unmodelled;
    f.newer = w.newer;
    f.newest = w.newest;
    f.newerIds = levelwarn::newerSummary(w);
    f.attemptShown = w.attemptShown;
    if (f.unmodelled & levelwarn::kTwoPlayer) {
        f.unmodelled &= ~levelwarn::kTwoPlayer;
        f.twoPlayer = true;
    }
#ifdef GEODE_IS_WINDOWS
    // With the attempt pin (cfg attemptone, hooks_attempt.cpp) the model is no longer blind to such
    // a level: every attempt is its first. What it still cannot do -- a level that opens only after
    // dying -- is what the yellow note says.
    if (g_attemptOne && (f.unmodelled & levelwarn::kAttemptDependent)) {
        f.unmodelled &= ~levelwarn::kAttemptDependent;
        f.attempts = true;
    }
#endif
    return f;
}

inline const char* describe(int mode, bool coins) {
    switch (mode) {
        case UI_MODE_REPLAY:
            return coins ? "Play back the stored coin solution for this level."
                         : "Play back the stored solution for this level.";
        case UI_MODE_SOLVE:
            return coins ? "Solve for the end and every coin here and now,\n"
                           "then play the solution through."
                         : "Solve the level here and now,\nthen play the solution through.";
        default:
            return "Play it yourself. The mod stays idle,\nand your progress records as usual.";
    }
}

inline const cocos2d::ccColor3B kWhite{255, 255, 255};
inline const cocos2d::ccColor3B kDim{110, 110, 110};
inline const cocos2d::ccColor3B kRefused{255, 140, 140};
// The notes under the Coins row: red for what the model cannot express, yellow for the newer
// objects it is still being measured against, and a plain blue for the random-number line, which
// states a fact rather than warning.
inline const cocos2d::ccColor3B kWarnRed{255, 100, 100};
inline const cocos2d::ccColor3B kWarnYellow{255, 215, 70};
inline const cocos2d::ccColor3B kInfoBlue{160, 205, 255};

// What each search choice is: the detail behind Options' (i) button, and the Solve menu's one line.
inline const char* describeSearch(int s) {
    switch (s) {
        case UI_SEARCH_BEAM:
            return "Experimental: keeps the best states each tick. More likely not to be solved. Follows coin routes.";
        case UI_SEARCH_MONTECARLO:
            return "Experimental: random playouts. More likely not to be solved. With Coins, or on a level with rotation, "
                   "control windows or item gates, it cannot follow the call and the DP answers it (the log says so).";
        default:
            return "The reachability DP, which the solver is built around.";
    }
}

// The saved Avoid glitches choice. It used to be a checkbox (the saved bool "play-glitch"): a saved on
// reads as "Glitches only", an off as "Off"; the new key ("play-glitch-mode") wins once there is one.
inline int savedGlitchMode() {
    const int m = Mod::get()->getSavedValue<int>("play-glitch-mode", -1);
    if (m >= 0) return std::clamp(m, 0, UI_GLITCH_COUNT - 1);
    return Mod::get()->getSavedValue<bool>("play-glitch", false) ? UI_GLITCH_ONLY : UI_GLITCH_OFF;
}

// What each Avoid glitches choice is: the detail behind Options' (i) button, and the Solve menu's one line.
inline const char* describeGlitch(int g) {
    switch (g) {
        case UI_GLITCH_ONLY:
            return "Drops the routes that need frame-level precision, the gaps no hand can thread. "
                   "The level may not be solved.";
        case UI_GLITCH_WAVE:
            return "Also keeps the wave's button changes no faster than a hand can, keeps the wave off the "
                   "terrain, and prefers fewer button changes and a line nearer the middle of its corridor. "
                   "Experimental: where no route keeps that, it is dropped for that stretch (and the screen "
                   "says so).";
        default:
            return "Search as usual.";
    }
}

inline const char* describeSecSolve(bool on) {
    return on ? "A wall only the game can search past is handed to the section search, which asks the game."
              : "Solve never hands a wall to the section search: a wall only the game can search past will not "
                "be solved.";
}

// The whole of an Options choice, every value with its name, one paragraph each, for its (i) button.
inline std::vector<std::string> glitchDetails() {
    std::vector<std::string> v;
    for (int g = 0; g < UI_GLITCH_COUNT; ++g)
        v.push_back(std::string(uiGlitchName(g)) + (g == UI_GLITCH_OFF ? " (default)" : "") + ": " + describeGlitch(g));
    return v;
}

inline std::vector<std::string> searchDetails() {
    std::vector<std::string> v;
    for (int i = 0; i < UI_SEARCH_COUNT; ++i)
        v.push_back(std::string(uiSearchName(i)) + (i == UI_SEARCH_DP ? " (default)" : "") + ": " + describeSearch(i));
    return v;
}

inline std::vector<std::string> holdPlayDetails() {
    return {"Unchecked (default): a click on a level's Play button opens this menu first.",
            "Checked: a click on Play starts the level at once, as without the mod, and the menu opens when the button is held "
            "(a ring fills around it). For when you mostly play the levels yourself."};
}

inline std::vector<std::string> replayStepDetails() {
    return {"On: the replay is advanced in fixed steps of 1/60 s. It no longer depends on the length of the frames. On a monitor faster than 60 Hz the motion looks like 60 fps.",
            "Off (default): the replay runs on the game's own frame time, as it does without the mod."};
}

inline std::vector<std::string> secSolveDetails() {
    return {std::string("On (default): ") + describeSecSolve(true), std::string("Off: ") + describeSecSolve(false)};
}

// What an (i) button opens: the detail in a small popup whose text scrolls when it is longer than the
// window (the game's own alert grows with its text and runs off the screen on the longer ones).
// The geometry, all in the popup's own units (the numbers a port to other UI parts needs):
//   kW x kH            the frame;
//   kAreaW x kAreaH    the scrolling window, its left edge (kW - kAreaW) / 2 from the frame's, its bottom kAreaY above the frame's;
//   kBarW + kBarGap    the strip at the window's right edge that the text leaves free for the scrollbar: text wraps at
//                      kAreaW - kBarW - kBarGap, and the bar sits inside the window, its centre kBarW / 2 + 2 from the edge;
//   the bar is as tall as the window, and hidden when the text fits in it.
// Each paragraph is a label of its own, one line of kParaGap between them, so the gap is the same whatever the font does with a
// blank line. A label's wrap width is taken as given (it is not divided by the label's scale), so a scaled label is made
// with the width it should have on screen.
class InfoPopup : public PopupBase {
public:
    static InfoPopup* create(const char* title, const std::vector<std::string>& paragraphs) {
        auto* r = new InfoPopup();
        if (r->init(title, paragraphs)) { r->autorelease(); return r; }
        delete r;
        return nullptr;
    }

protected:
    static constexpr float kW = 340.f, kH = 236.f, kAreaW = 300.f, kAreaH = 150.f, kAreaY = 54.f;
    static constexpr float kBarW = 8.f, kBarGap = 8.f, kParaGap = 7.f, kScale = .7f;

    bool init(const char* title, const std::vector<std::string>& paragraphs) {
        using namespace cocos2d;
        if (!PopupBase::init(kW, kH)) return false;
        this->setID("play-menu-info"_spr);
        this->setTitle(title);
        const float wrapW = kAreaW - kBarW - kBarGap;
        std::vector<CCLabelBMFont*> labels;
        float textH = 0.f;
        for (const std::string& para : paragraphs) {
            auto* label = CCLabelBMFont::create(para.c_str(), "chatFont.fnt", wrapW, kCCTextAlignmentLeft);
            label->setScale(kScale);
            label->setAnchorPoint({0.f, 1.f});
            textH += ui::scaledContentHeight(label) + (labels.empty() ? 0.f : kParaGap);
            labels.push_back(label);
        }
        const float contentH = std::max(kAreaH, textH + 4.f);
        auto* scroll = ui::ScrollLayer::create(CCSize{kAreaW, kAreaH});
        m_scroll = scroll;
        scroll->m_contentLayer->setContentSize({kAreaW, contentH});
        float y = contentH;
        for (auto* label : labels) {
            label->setPosition({0.f, y});
            scroll->m_contentLayer->addChild(label);
            y -= ui::scaledContentHeight(label) + kParaGap;
        }
        scroll->scrollToTop();
        scroll->setPosition({(kW - kAreaW) / 2.f, kAreaY});
        scroll->setID("info-scroll"_spr);
        m_mainLayer->addChild(scroll);
        if (contentH > kAreaH + .5f) {
            // The bar is centred on its position (the track is drawn about the layer's centre), and as tall as the window.
            auto* bar = ui::Scrollbar::create(scroll);
            bar->setPosition({(kW + kAreaW) / 2.f - kBarW / 2.f - 2.f, kAreaY + kAreaH / 2.f});
            bar->setID("info-scrollbar"_spr);
            m_mainLayer->addChild(bar);
        }

        auto* okSpr = ButtonSprite::create("OK", "goldFont.fnt", "GJ_button_01.png", .8f);
        auto* ok = CCMenuItemSpriteExtra::create(okSpr, this, menu_selector(InfoPopup::onOk));
        ok->setID("ok-button"_spr);
        ui::addChildAtPosition(m_buttonMenu, ok, Anchor::Bottom, ccp(0, 26.f));
        return true;
    }

    ui::ScrollLayer* m_scroll = nullptr;

    void onOk(CCObject*) { this->onClose(nullptr); }

    void keyDown(cocos2d::enumKeyCodes key, double p1) override {
        if (key == cocos2d::enumKeyCodes::KEY_Enter) { onOk(nullptr); return; }
#if GEODE_COMP_GD_VERSION >= 22090
        // The text scrolls from the keyboard too (a touch drag is the other way).
        if (m_scroll) {
            using K = cocos2d::enumKeyCodes;
            if (key == K::KEY_Up) { m_scroll->scrollBy(-24.f); return; }
            if (key == K::KEY_Down) { m_scroll->scrollBy(24.f); return; }
            if (key == K::KEY_PageUp) { m_scroll->scrollBy(-kAreaH); return; }
            if (key == K::KEY_PageDown) { m_scroll->scrollBy(kAreaH); return; }
        }
#endif
        PopupBase::keyDown(key, p1);
    }
};

inline void showInfo(const char* title, const std::vector<std::string>& paragraphs) {
    if (auto* p = InfoPopup::create(title, paragraphs)) p->show();
}

// ---- Options: the settings the mod has that a player might want ----------------------------------
// A second popup over the menu, opened by its Options button (Solve only). Two kinds of rows. The choices about how Solve searches --
// whether it may hand a wall to the section search, Avoid glitches (off, the glitch margins, or those plus the wave's behaviour) and
// which search answers the calls -- go back to the menu at once (onChange), which shows its warnings; nothing of them is kept until
// the menu's Start. And one of the mod's own settings (mod.json), "play-menu", shown here as its inverse (Hold Play to open this menu)
// and written to the setting as it is changed: Geode's settings page is not there on every game version the mod runs on. What each
// row means is behind an (i) button next to it. No key list and no way to rebind keys is shown here: the keys are the settings
// page's. Solve faster is not a setting at all any more (cfg dpplainbeside=3 still starts it).
class Options : public PopupBase {
public:
    using Change = std::function<void(int, int, bool)>;   // glitch mode, search, section search
    using ReplayChange = std::function<void(bool)>;        // the Replay form: fixed replay step on or off
    // The form a Replay has: the fixed replay step and the Hold Play row; the choices about how Solve searches are not in it.
    static Options* createReplay(bool replayStep, ReplayChange onChange) {
        auto* r = new Options();
        r->m_replayForm = true;
        if (r->initReplay(replayStep, std::move(onChange))) { r->autorelease(); return r; }
        delete r;
        return nullptr;
    }
    static Options* create(int glitch, int search, bool secSolve, Change onChange) {
        auto* r = new Options();
        if (r->init(glitch, search, secSolve, std::move(onChange))) { r->autorelease(); return r; }
        delete r;
        return nullptr;
    }

protected:
    int m_glitch = UI_GLITCH_OFF;
    int m_search = UI_SEARCH_DP;
    bool m_secSolve = true;
    bool m_replayForm = false;
    bool m_replayStep = false;
    ReplayChange m_onReplay;
    CCMenuItemToggler* m_stepToggle = nullptr;
    Change m_onChange;
    CCMenuItemToggler* m_secToggle = nullptr;
    CCMenuItemToggler* m_menuToggle = nullptr;
    cocos2d::CCLabelBMFont* m_menuNote = nullptr;
    cocos2d::CCLabelBMFont* m_glitchValue = nullptr;
    cocos2d::CCLabelBMFont* m_searchValue = nullptr;

    // Top to bottom: the title; the Section search row; Avoid glitches (title, value between arrows); Search (the same); the Hold Play row and,
    // when it is checked, its note; OK. 284 high, inside GD's design height (320; the popup's close button rises above its frame).
    static constexpr float kW = 300.f, kH = 284.f;

    // A checkbox with its label and its (i) button, the three centred as a row (as the menu's rows are).
    CCMenuItemToggler* addToggleRow(const char* id, const char* text, float y, bool on, cocos2d::SEL_MenuHandler handler, int infoWhat) {
        using namespace cocos2d;
        auto* toggle = CCMenuItemToggler::createWithStandardSprites(this, handler, .6f);
        toggle->toggle(on);
        toggle->setID((std::string(Mod::get()->getID()) + "/" + id + "-toggle").c_str());
        auto* label = CCLabelBMFont::create(text, "bigFont.fnt");
        label->setScale(.45f);
        label->setAnchorPoint({0.f, .5f});
        label->setID((std::string(Mod::get()->getID()) + "/" + id + "-label").c_str());
        const float boxW = 18.f, gap = 6.f, infoGap = 16.f;
        const float labelW = ui::scaledContentWidth(label);
        // the checkbox, the label and the (i) button as one row, centred
        const float left = -(boxW + gap + labelW + infoGap + 8.f) / 2.f;
        ui::addChildAtPosition(m_buttonMenu, toggle, Anchor::Top, ccp(left + boxW / 2.f, y));
        ui::addChildAtPosition(m_mainLayer, label, Anchor::Top, ccp(left + boxW + gap, y));
        infoButton(id, left + boxW + gap + labelW + infoGap, y, infoWhat);
        return toggle;
    }

    // An (i) button at (x, y) from the top edge: the game's own information icon, which opens `what`.
    void infoButton(const char* id, float x, float y, int what) {
        using namespace cocos2d;
        auto* spr = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png");
        spr->setScale(.6f);
        auto* btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(Options::onInfo));
        btn->setTag(what);
        btn->setID((std::string(Mod::get()->getID()) + "/" + id + "-info").c_str());
        ui::addChildAtPosition(m_buttonMenu, btn, Anchor::Top, ccp(x, y));
    }

    // A choice between names: its title (with its (i) button), and its value between two arrows (as the
    // game's own resolution choice is). The arrows' tag is the direction.
    cocos2d::CCLabelBMFont* choiceRow(const char* title, const char* id, float y, cocos2d::SEL_MenuHandler onArrow,
                                      cocos2d::CCLabelBMFont*& value, int infoWhat) {
        using namespace cocos2d;
        // node ids carry the mod's id, as `"..."_spr` does for a literal
        const std::string base = std::string(Mod::get()->getID()) + "/" + id;
        auto* t = CCLabelBMFont::create(title, "goldFont.fnt");
        t->setScale(.7f);
        t->setID((base + "-title").c_str());
        ui::addChildAtPosition(m_mainLayer, t, Anchor::Top, ccp(0, y));
        infoButton(id, ui::scaledContentWidth(t) / 2.f + 16.f, y, infoWhat);
        value = CCLabelBMFont::create("", "bigFont.fnt");
        value->setID((base + "-value").c_str());
        ui::addChildAtPosition(m_mainLayer, value, Anchor::Top, ccp(0, y - 24.f));
        for (int dir : {-1, 1}) {
            auto* spr = CCSprite::createWithSpriteFrameName(dir < 0 ? "edit_leftBtn_001.png"
                                                                    : "edit_rightBtn_001.png");
            spr->setScale(.9f);
            auto* btn = CCMenuItemSpriteExtra::create(spr, this, onArrow);
            btn->setTag(dir);
            btn->setID((base + (dir < 0 ? "-left" : "-right")).c_str());
            ui::addChildAtPosition(m_buttonMenu, btn, Anchor::Top, ccp(95.f * dir, y - 24.f));
        }
        return t;
    }

    // The Replay form: 190 high; the fixed replay step row (with its (i)), the Hold Play row and its note, OK.
    bool initReplay(bool replayStep, ReplayChange onChange) {
        using namespace cocos2d;
        if (!PopupBase::init(kW, 190.f)) return false;
        this->setID("play-menu-options"_spr);
        m_replayStep = replayStep;
        m_onReplay = std::move(onChange);
        this->setTitle("Options");
        m_stepToggle = addToggleRow("replay-step", "Fixed replay step", -50.f, m_replayStep, menu_selector(Options::onReplayStep), 4);
        m_menuToggle = addToggleRow("menu", "Hold Play to open this menu", -86.f, !Mod::get()->getSettingValue<bool>("play-menu"),
                                    menu_selector(Options::onMenuSetting), 3);
        m_menuNote = CCLabelBMFont::create("", "chatFont.fnt");
        m_menuNote->setScale(.55f);
        m_menuNote->setColor(kWarnYellow);
        m_menuNote->setID("menu-note"_spr);
        ui::addChildAtPosition(m_mainLayer, m_menuNote, Anchor::Top, ccp(0, -104.f));
        auto* okSpr = ButtonSprite::create("OK", "goldFont.fnt", "GJ_button_01.png", .8f);
        auto* ok = CCMenuItemSpriteExtra::create(okSpr, this, menu_selector(Options::onOk));
        ok->setID("ok-button"_spr);
        ui::addChildAtPosition(m_buttonMenu, ok, Anchor::Bottom, ccp(0.f, 26.f));
        showMenuNote(!Mod::get()->getSettingValue<bool>("play-menu"));
        return true;
    }

    // CCMenuItemToggler calls its target before it flips its own state (see the menu's onCoins).
    void onReplayStep(CCObject*) {
        m_replayStep = !m_stepToggle->isToggled();
        if (m_onReplay) m_onReplay(m_replayStep);
    }

    bool init(int glitch, int search, bool secSolve, Change onChange) {
        using namespace cocos2d;
        if (!PopupBase::init(kW, kH)) return false;
        this->setID("play-menu-options"_spr);
        m_glitch = std::clamp(glitch, 0, UI_GLITCH_COUNT - 1);
        m_search = std::clamp(search, 0, UI_SEARCH_COUNT - 1);
        m_secSolve = secSolve;
        m_onChange = std::move(onChange);
        this->setTitle("Options");

        m_secToggle = addToggleRow("section-search", "Section search", -50.f, m_secSolve, menu_selector(Options::onSecSolve), 0);
        choiceRow("Avoid glitches", "glitch", -86.f, menu_selector(Options::onGlitch), m_glitchValue, 1);
        choiceRow("Search", "search", -146.f, menu_selector(Options::onSearch), m_searchValue, 2);
        // The setting is stored as "play-menu" (on: a click on Play opens the menu); this row is its inverse, "hold Play to open the
        // menu", checked when the setting is off.
        m_menuToggle = addToggleRow("menu", "Hold Play to open this menu", -204.f, !Mod::get()->getSettingValue<bool>("play-menu"),
                                    menu_selector(Options::onMenuSetting), 3);
        m_menuNote = CCLabelBMFont::create("", "chatFont.fnt");
        m_menuNote->setScale(.55f);
        m_menuNote->setColor(kWarnYellow);
        m_menuNote->setID("menu-note"_spr);
        ui::addChildAtPosition(m_mainLayer, m_menuNote, Anchor::Top, ccp(0, -222.f));

        auto* okSpr = ButtonSprite::create("OK", "goldFont.fnt", "GJ_button_01.png", .8f);
        auto* ok = CCMenuItemSpriteExtra::create(okSpr, this, menu_selector(Options::onOk));
        ok->setID("ok-button"_spr);
        ui::addChildAtPosition(m_buttonMenu, ok, Anchor::Bottom, ccp(0.f, 26.f));

        refresh();
        showMenuNote(!Mod::get()->getSettingValue<bool>("play-menu"));
        return true;
    }

    void showMenuNote(bool hold) {
        m_menuNote->setString(hold ? "Play starts the level at once. Hold it to open this menu." : "");
    }

    // The mod setting this page keeps, written as it is changed. CCMenuItemToggler calls its target before it flips
    // its own state (see the menu's onCoins), so the state the press leads to is the opposite of isToggled().
    void onMenuSetting(CCObject*) {
        const bool hold = !m_menuToggle->isToggled();
        Mod::get()->setSettingValue<bool>("play-menu", !hold);
        showMenuNote(hold);
    }

    void refresh() {
        m_glitchValue->setString(uiGlitchName(m_glitch));
        ui::limitNodeWidth(m_glitchValue, 150.f, .6f, .3f);
        m_glitchValue->setColor(m_glitch == UI_GLITCH_OFF ? kWhite : kWarnYellow);
        m_searchValue->setString(m_search == UI_SEARCH_DP ? "DP (default)" : uiSearchName(m_search));
        ui::limitNodeWidth(m_searchValue, 150.f, .6f, .3f);
        m_searchValue->setColor(m_search == UI_SEARCH_DP ? kWhite : kWarnRed);
        if (m_onChange) m_onChange(m_glitch, m_search, m_secSolve);
    }

    // CCMenuItemToggler calls its target before it flips its own state (see the menu's onCoins).
    void onSecSolve(CCObject*) {
        m_secSolve = !m_secToggle->isToggled();
        refresh();
    }

    void onGlitch(CCObject* sender) {
        const int dir = static_cast<cocos2d::CCNode*>(sender)->getTag();
        m_glitch = (m_glitch + dir + UI_GLITCH_COUNT) % UI_GLITCH_COUNT;
        refresh();
    }

    void onSearch(CCObject* sender) {
        const int dir = static_cast<cocos2d::CCNode*>(sender)->getTag();
        m_search = (m_search + dir + UI_SEARCH_COUNT) % UI_SEARCH_COUNT;
        refresh();
    }

    void onOk(CCObject*) { this->onClose(nullptr); }

    // The (i) buttons: 0 Section search, 1 Avoid glitches, 2 Search -- every value of the choice, named -- and 3 Hold Play.
    void onInfo(CCObject* sender) {
        switch (static_cast<cocos2d::CCNode*>(sender)->getTag()) {
            case 0: showInfo("Section search", secSolveDetails()); break;
            case 1: showInfo("Avoid glitches", glitchDetails()); break;
            case 3: showInfo("Hold Play to open this menu", holdPlayDetails()); break;
            case 4: showInfo("Fixed replay step", replayStepDetails()); break;
            default: showInfo("Search", searchDetails()); break;
        }
    }

    // The arrow keys move the search, as they did when it was the only choice here; the glitch choice
    // takes the up and down keys.
    void keyDown(cocos2d::enumKeyCodes key, double p1) override {
        if (m_replayForm) {
            if (key == cocos2d::enumKeyCodes::KEY_Enter) { onOk(nullptr); return; }
            PopupBase::keyDown(key, p1);
            return;
        }
        if (key == cocos2d::enumKeyCodes::KEY_Left || key == cocos2d::enumKeyCodes::KEY_Right) {
            m_search = (m_search + (key == cocos2d::enumKeyCodes::KEY_Left ? -1 : 1)
                        + UI_SEARCH_COUNT) % UI_SEARCH_COUNT;
            refresh();
            return;
        }
        if (key == cocos2d::enumKeyCodes::KEY_Up || key == cocos2d::enumKeyCodes::KEY_Down) {
            m_glitch = (m_glitch + (key == cocos2d::enumKeyCodes::KEY_Up ? -1 : 1)
                        + UI_GLITCH_COUNT) % UI_GLITCH_COUNT;
            refresh();
            return;
        }
        if (key == cocos2d::enumKeyCodes::KEY_Enter) { onOk(nullptr); return; }
        PopupBase::keyDown(key, p1);
    }
};

class Popup : public PopupBase {
public:
    static Popup* create(GJGameLevel* level, LevelFacts facts, std::function<void()> play) {
        auto* r = new Popup();
        if (r->init(level, facts, std::move(play))) { r->autorelease(); return r; }
        delete r;
        return nullptr;
    }

protected:
    Ref<GJGameLevel> m_level;
    LevelFacts m_facts;
    std::function<void()> m_play;
    int m_mode = UI_MODE_NORMAL;
    bool m_coins = false;
    bool m_started = false;
    ButtonSprite* m_modeSpr[UI_MODE_COUNT] = {};
    cocos2d::CCLabelBMFont* m_desc = nullptr;
    CCMenuItemToggler* m_coinToggle = nullptr;
    cocos2d::CCLabelBMFont* m_coinLabel = nullptr;
    // The Options popup's three choices (Solve only), and its button.
    int m_glitch = UI_GLITCH_OFF;
    bool m_secSolve = true;
    int m_search = UI_SEARCH_DP;
    bool m_replayStep = false;   // the Replay form of Options: fixed replay step (saved "play-replaystep")
    CCMenuItemSpriteExtra* m_optBtn = nullptr;
    cocos2d::CCLabelBMFont* m_note = nullptr;
    cocos2d::CCLabelBMFont* m_rngNote = nullptr;
    cocos2d::CCLabelBMFont* m_redNote = nullptr;
    cocos2d::CCLabelBMFont* m_yellowNote = nullptr;
    cocos2d::CCLabelBMFont* m_attemptNote = nullptr;
    cocos2d::CCLabelBMFont* m_twoNote = nullptr;
    cocos2d::CCLabelBMFont* m_versionNote = nullptr;
    // ONE line for everything Options changed (Avoid glitches, no section search, another search), red if
    // the search is an experiment and yellow otherwise, with an (i) button for what each costs.
    cocos2d::CCLabelBMFont* m_optNote = nullptr;
    CCMenuItemSpriteExtra* m_optInfo = nullptr;
    int m_levelId = 0;
    ButtonSprite* m_startSpr = nullptr;
    CCMenuItemSpriteExtra* m_startBtn = nullptr;

    // The layout, top to bottom: the title and the level's name, the mode row, the description,
    // the Coins row (offsets from the top edge), then from the bottom edge up Start, the bot note
    // and the level's notes, one row each. Each row keeps clear of the next at its tallest -- the
    // description is two lines, the checkbox is taller than its label -- which the first cut (230
    // high) did not: the checkbox sat on the bot note. With one note the popup is 260 high; each
    // further note adds a row to it, so the notes never climb into the checkbox rows. The "Avoid
    // glitches" row that sat under Coins moved to Options (2026-10-11, the user's call: the menu
    // keeps Coins, the other Solve settings go to a page of their own), which a button in the
    // top-right corner opens, as the game's Advanced does on its video options; the popup is 260
    // high again, with a row kept for each of the two warnings Options can raise.
    // The "Solve faster (not reproducible)" row is gone from the menu (the user's call,
    // 2026-10-01), and the setting itself from mod.json (2026-10-16): cfg dpplainbeside=3 is its only way in.
    // Even so the popup can be taller than the screen -- GD's design height is 320 -- so
    // shrinkToFit scales it down to fit.
    static constexpr float kW = 340.f, kH = 260.f;
    static constexpr float kModeY = -88.f, kDescY = -128.f, kCoinY = -164.f;   // from the top edge
    static constexpr float kNoteY = 64.f, kStartY = 32.f;   // from the bottom edge
    static constexpr float kNoteRow = 14.f;   // the level's notes stack up from the bot note

    static std::string modVersion() { return Mod::get()->getVersion().toVString(); }

    // The version that filed the stored solution for these coins when it is not this one ("an
    // earlier version" for a solution filed before versions were written), or "" -- no solution,
    // or this version's (see solvedWithPath).
    std::string olderSolve(bool coins) const {
        const std::string path = uiSolutionPath(m_levelId, coins);
        std::vector<InputCmd> plan;
        if (!loadInputsFile(path, plan, true)) return {};
        const std::string v = readSolvedWith(path);
        if (v == modVersion()) return {};
        return v.empty() ? std::string("an earlier version") : v;
    }

    // The most notes the menu can show at once: the random-number line, and either red and
    // yellow (no stored solution for the Coins choice) or the version line (one stored by another
    // version).
    int noteCount() const {
        const int unsolved = (m_facts.unmodelled ? 1 : 0)
                             + (m_facts.newer && !m_facts.official ? 1 : 0)
                             + (m_facts.attempts ? 1 : 0)
                             + (m_facts.twoPlayer ? 1 : 0);
        const int solved = (!olderSolve(false).empty()
                            || (m_facts.coins > 0 && !olderSolve(true).empty())) ? 1 : 0;
        // ...and one row kept for the line about what Options changed, which comes and goes with the
        // choices (it is one line however many of them there are, with an (i) button for the detail).
        return std::max(unsolved, solved) + (m_facts.random ? 1 : 0) + 1;
    }

    cocos2d::CCLabelBMFont* makeNote(const std::string& text, const cocos2d::ccColor3B& color,
                                     const char* id) {
        using namespace cocos2d;
        auto* l = CCLabelBMFont::create(text.c_str(), "chatFont.fnt");
        l->setScale(.55f);
        ui::limitNodeWidth(l, kW - 30.f, .55f, .3f);
        l->setColor(color);
        l->setID(id);
        ui::addChildAtPosition(m_mainLayer, l, Anchor::Bottom, ccp(0, kNoteY + kNoteRow));
        return l;
    }

    bool init(GJGameLevel* level, LevelFacts facts, std::function<void()> play) {
        using namespace cocos2d;
        m_facts = facts;
        m_levelId = level->m_levelID.value();
        if (!PopupBase::init(kW, kH + kNoteRow * (float)std::max(0, noteCount() - 1)))
            return false;
        // ...and the stored solutions the version line is decided from.
        auto solution = [this](bool coins) -> std::string {
            const std::string path = uiSolutionPath(m_levelId, coins);
            std::vector<InputCmd> plan;
            if (!loadInputsFile(path, plan, true)) return "none";
            const std::string v = readSolvedWith(path);
            return v.empty() ? std::string("stored, version not recorded") : "stored by " + v;
        };
        log::info("play menu: lv{} solutions: plain {}, coins {} (this is {})", m_levelId,
                  solution(false), solution(true), modVersion());
        this->setID("play-menu"_spr);
        m_level = level;
        m_facts = facts;
        m_play = std::move(play);
        // The menu opens on what can be done with the level now: Replay where a solution is
        // stored -- the coin one, with Coins ticked, when the level has coins and it is stored --
        // and Solve only where none is. On Solve, Coins keeps the last Start's choice. A level
        // without coins has nothing for the switch to route for: it stays off there, and the
        // choice made on a level that has them is not overwritten by this one (onStart).
        const int id = level->m_levelID.value();
        auto stored = [id](bool coins) {
            std::vector<InputCmd> plan;
            return loadInputsFile(uiSolutionPath(id, coins), plan, true);
        };
        if (m_facts.coins > 0 && stored(true)) {
            m_mode = UI_MODE_REPLAY;
            m_coins = true;
        } else if (stored(false)) {
            m_mode = UI_MODE_REPLAY;
            m_coins = false;
        } else {
            m_mode = UI_MODE_SOLVE;
            m_coins = g_uiCoins && m_facts.coins > 0;
        }

        this->setTitle("GD Solver");
        auto* name = CCLabelBMFont::create(level->m_levelName.c_str(), "bigFont.fnt");
        ui::limitNodeWidth(name, 280.f, .45f, .1f);
        name->setColor({200, 200, 255});
        name->setID("level-name"_spr);
        ui::addChildAtPosition(m_mainLayer, name, Anchor::Top, ccp(0, -46));

        const char* ids[UI_MODE_COUNT] = {"normal-button"_spr, "replay-button"_spr,
                                          "solve-button"_spr};
        for (int m = 0; m < UI_MODE_COUNT; ++m) {
            m_modeSpr[m] = ButtonSprite::create(uiModeName(m), 76, true, "bigFont.fnt",
                                                "GJ_button_04.png", 30.f, .6f);
            auto* btn = CCMenuItemSpriteExtra::create(m_modeSpr[m], this,
                                                      menu_selector(Popup::onMode));
            btn->setTag(m);
            btn->setID(ids[m]);
            ui::addChildAtPosition(m_buttonMenu, btn, Anchor::Top,
                                   ccp(-100.f + 100.f * m, kModeY));
        }

        m_desc = CCLabelBMFont::create("", "chatFont.fnt", 300.f, kCCTextAlignmentCenter);
        m_desc->setScale(.75f);
        m_desc->setID("description"_spr);
        ui::addChildAtPosition(m_mainLayer, m_desc, Anchor::Top, ccp(0, kDescY));

        // The checkbox and its label, centred as a pair: the label is one of two lengths.
        const bool haveCoins = m_facts.coins > 0;
        m_coinToggle = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(Popup::onCoins), .6f);
        m_coinToggle->toggle(m_coins);
        m_coinToggle->setID("coins-toggle"_spr);
        m_coinLabel = CCLabelBMFont::create(haveCoins ? "Coins" : "Coins: none in this level",
                                            "bigFont.fnt");
        m_coinLabel->setScale(haveCoins ? .45f : .35f);
        m_coinLabel->setAnchorPoint({0.f, .5f});
        m_coinLabel->setID("coins-label"_spr);
        // 18 units is the box as drawn at scale .6, measured on screen (63 px of a 1180 px wide
        // popup, 2026-09-26) -- the toggler's own content size is not the scaled sprite's.
        const float boxW = 18.f, gap = 6.f;
        const float left = -(boxW + gap + ui::scaledContentWidth(m_coinLabel)) / 2.f;
        ui::addChildAtPosition(m_buttonMenu, m_coinToggle, Anchor::Top,
                               ccp(left + boxW / 2.f, kCoinY));
        ui::addChildAtPosition(m_mainLayer, m_coinLabel, Anchor::Top,
                               ccp(left + boxW + gap, kCoinY));
        if (!haveCoins) {
            // Refused, and shown to be: the box greys out and the label says why.
            m_coinToggle->setEnabled(false);
            for (auto* b : {m_coinToggle->m_offButton, m_coinToggle->m_onButton})
                if (auto* spr = geode::cast::typeinfo_cast<CCSprite*>(b->getNormalImage()))
                    spr->setColor(kDim);
            m_coinLabel->setColor(kDim);
        }
        // Options: Avoid glitches and the search, choices about how Solve searches, so the button
        // is only shown there. They keep the last Start's choices (saved values), like Coins.
        m_glitch = savedGlitchMode();
        m_secSolve = Mod::get()->getSavedValue<bool>("play-secsolve", true);
        m_replayStep = Mod::get()->getSavedValue<bool>("play-replaystep", false);
        m_search = std::clamp(Mod::get()->getSavedValue<int>("play-search", UI_SEARCH_DP), 0,
                              UI_SEARCH_COUNT - 1);
        {
            auto* spr = ButtonSprite::create("Options", "goldFont.fnt", "GJ_button_04.png", .55f);
            m_optBtn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(Popup::onOptions));
            m_optBtn->setID("options-button"_spr);
            ui::addChildAtPosition(m_buttonMenu, m_optBtn, Anchor::TopRight, ccp(-46.f, -22.f));
        }

        // The promise the safety gate makes, where the choice is made (see botDriving).
        m_note = CCLabelBMFont::create("The bot drives: nothing is recorded.", "chatFont.fnt");
        m_note->setScale(.6f);
        m_note->setOpacity(150);
        m_note->setID("bot-note"_spr);
        ui::addChildAtPosition(m_mainLayer, m_note, Anchor::Bottom, ccp(0, kNoteY));
        m_rngNote = makeNote("This level uses random numbers: fixed while the bot drives.",
                             kInfoBlue, "random-note"_spr);
        // What the model cannot express, named (red); and, on a custom level, objects from 2.0 on
        // (yellow), named by the newest version among them. Neither refuses the run: see
        // level_warnings.hpp.
        m_redNote = makeNote("Not modelled by the solver: " + levelwarn::describe(m_facts.unmodelled)
                                 + ". Unlikely to be solved.",
                             kWarnRed, "unmodelled-note"_spr);
        m_yellowNote = makeNote("Its newest objects are from " + levelwarn::versionName(m_facts.newest)
                                    + ", where the solver is still being matched to the game: "
                                      "slower, and it may not be solved.",
                                kWarnYellow, "newer-objects-note"_spr);
        m_attemptNote = makeNote("It changes from attempt to attempt: the bot takes it as its "
                                 "first attempt, so a level that opens only after dying will not "
                                 "be solved.",
                                 kWarnYellow, "attempt-note"_spr);
        m_twoNote = makeNote("A two-player level: both players are planned (experimental), and a "
                             "level where one player's press switches the other's world will not "
                             "be solved.",
                             kWarnYellow, "two-player-note"_spr);
        // A level already solved needs neither; solving it again with another version than the one
        // that did may still fail, and that is said instead (refresh fills it in).
        m_versionNote = makeNote("", kWarnYellow, "solved-version-note"_spr);
        // What Options changed narrows what a Solve can do on purpose (Avoid glitches, no section search),
        // or swaps the search for an experiment: one line for all of it, refresh words it, and an (i)
        // button says what each costs.
        m_optNote = makeNote("", kWarnYellow, "options-note"_spr);
        {
            auto* spr = CCSprite::createWithSpriteFrameName("GJ_infoIcon_001.png");
            spr->setScale(.5f);
            m_optInfo = CCMenuItemSpriteExtra::create(spr, this, menu_selector(Popup::onOptInfo));
            m_optInfo->setID("options-note-info"_spr);
            ui::addChildAtPosition(m_buttonMenu, m_optInfo, Anchor::Bottom, ccp(0, kNoteY + kNoteRow));
        }

        m_startSpr = ButtonSprite::create("Start", "goldFont.fnt", "GJ_button_01.png", .9f);
        m_startBtn = CCMenuItemSpriteExtra::create(m_startSpr, this,
                                                   menu_selector(Popup::onStart));
        m_startBtn->setID("start-button"_spr);
        ui::addChildAtPosition(m_buttonMenu, m_startBtn, Anchor::Bottom, ccp(0, kStartY));

        refresh();
        return true;
    }

    // A popup taller than the screen loses its title and its Start button off the edges: GD's
    // design height is 320 whatever the window, and the notes add rows to the popup. The scale
    // that fits it with a margin, about its centre (m_mainLayer's anchor); 1 when it fits. The
    // close button sits on the top-left corner, 3 below and so mostly above the frame (Popup::init),
    // and counts: fitting the frame alone left a sliver of it off the top (2026-10-01).
    float fitScale() const {
        const float winH = cocos2d::CCDirector::sharedDirector()->getWinSize().height;
        const float h = m_mainLayer->getContentSize().height;
        const float btnH = m_closeBtn ? m_closeBtn->getScaledContentSize().height : 30.f;
        const float over = std::max(0.f, btnH / 2.f - 3.f);   // how far the button rises above
        const float half = h / 2.f + over;                     // centre to the highest point
        const float room = winH / 2.f - 6.f;
        return (half > room && half > 0.f) ? room / half : 1.f;
    }

public:
    // FLAlertLayer::show runs the popup's own entrance, an elastic scale from 0.1 to 1 on
    // m_mainLayer, which would undo any scale set before it. So the same entrance is run again,
    // to the scale that fits.
    void show() override {
        PopupBase::show();
        const float s = fitScale();
        if (s >= 1.f) return;
        using namespace cocos2d;
        m_mainLayer->stopAllActions();
        m_mainLayer->setScale(0.1f * s);
        m_mainLayer->runAction(CCEaseElasticOut::create(CCScaleTo::create(0.5f, s), 0.6f));
    }

    // For the diagnostic preview (cfg uishow): put the menu in a mode, and open the Options line's (i), as the presses would.
    void previewMode(int mode, bool openInfo) {
        m_mode = std::clamp(mode, 0, UI_MODE_COUNT - 1);
        refresh();
        if (openInfo) onOptInfo(nullptr);
    }

    // For the diagnostic (cfg uishow=press): choose a mode and press Start, as the player does -- the whole route short of the clicks.
    void pressStart(int mode) {
        m_mode = std::clamp(mode, 0, UI_MODE_COUNT - 1);
        refresh();
        onStart(nullptr);
    }

protected:

    // Replay needs a file it can play: one that is there and holds at least one input, which is
    // the test the session itself applies (loadInputsFile).
    bool hasSolution() const {
        std::vector<InputCmd> plan;
        return loadInputsFile(uiSolutionPath(m_level->m_levelID.value(), m_coins), plan, true);
    }

    void refresh() {
        for (int m = 0; m < UI_MODE_COUNT; ++m)
            m_modeSpr[m]->updateBGImage(m == m_mode ? "GJ_button_01.png" : "GJ_button_04.png");
        const bool bot = m_mode != UI_MODE_NORMAL;
        m_coinToggle->setVisible(bot);
        m_coinLabel->setVisible(bot);
        m_optBtn->setVisible(m_mode == UI_MODE_SOLVE || m_mode == UI_MODE_REPLAY);
        m_optBtn->setEnabled(m_mode == UI_MODE_SOLVE || m_mode == UI_MODE_REPLAY);
        m_note->setVisible(bot);
        // The one line for what Options changed.
        const bool optChanged = m_glitch != UI_GLITCH_OFF || !m_secSolve || m_search != UI_SEARCH_DP;
        {
            std::string t;
            auto add = [&](const std::string& part) { t += (t.empty() ? "" : ", ") + part; };
            if (m_glitch != UI_GLITCH_OFF) add(std::string("Avoid glitches: ") + uiGlitchName(m_glitch));
            if (!m_secSolve) add("no section search");
            if (m_search != UI_SEARCH_DP) add(std::string("Search: ") + uiSearchName(m_search));
            m_optNote->setString((t + ". The level is more likely not to be solved.").c_str());
            ui::limitNodeWidth(m_optNote, kW - 60.f, .55f, .3f);
            m_optNote->setColor(m_search != UI_SEARCH_DP ? kWarnRed : kWarnYellow);
        }
        // Saying so here, rather than letting the session find out after the level has loaded,
        // is the point of asking before the level starts.
        const bool ok = m_mode != UI_MODE_REPLAY || hasSolution();
        // A level that draws on the game's random numbers is solved and replayed with them fixed
        // (rngfix): said here, so a replay outside the mod that goes differently is no surprise.
        // Above it, what the model cannot express (red) and, on a custom level, objects from 2.0 on
        // (yellow) -- for a level with no stored solution for the Coins choice. One that has one
        // was solvable; Solve then says only when another version solved it. The ones shown stack
        // up from the bot note, red on top.
        const bool solved = hasSolution();
        const std::string older = (m_mode == UI_MODE_SOLVE && solved) ? olderSolve(m_coins)
                                                                      : std::string();
        if (!older.empty()) {
            m_versionNote->setString(("Solved with " + older + ". Solving it again with "
                                      + modVersion() + " may not succeed.").c_str());
            ui::limitNodeWidth(m_versionNote, kW - 30.f, .55f, .3f);
        }
        float y = kNoteY;
        const bool optShown = bot && m_mode == UI_MODE_SOLVE && optChanged;
        m_optInfo->setVisible(optShown);
        m_optInfo->setEnabled(optShown);
        for (auto [label, show] :
             {std::pair{m_rngNote, m_facts.random},
              std::pair{m_optNote, m_mode == UI_MODE_SOLVE && optChanged},
              std::pair{m_versionNote, !older.empty()},
              std::pair{m_yellowNote, !solved && m_facts.newer && !m_facts.official},
              std::pair{m_attemptNote, !solved && m_facts.attempts},
              std::pair{m_twoNote, !solved && m_facts.twoPlayer},
              std::pair{m_redNote, !solved && m_facts.unmodelled != 0}}) {
            label->setVisible(bot && show);
            if (bot && show) {
                y += kNoteRow;
                ui::updateAnchoredPosition(label, Anchor::Bottom, cocos2d::CCPoint(0.f, y));
                if (label == m_optNote)   // its (i) button sits to the right of the line
                    ui::updateAnchoredPosition(m_optInfo, Anchor::Bottom,
                                               cocos2d::CCPoint(ui::scaledContentWidth(label) / 2.f + 12.f, y));
            }
        }
        m_desc->setString(ok ? describe(m_mode, m_coins)
                             : (m_coins ? "No stored coin solution for this level yet.\n"
                                          "Solve it with Coins on first."
                                        : "No stored solution for this level yet.\n"
                                          "Solve it first."));
        m_desc->setColor(ok ? kWhite : kRefused);
        m_startBtn->setEnabled(ok);
        m_startSpr->setColor(ok ? kWhite : kDim);
    }

    void onMode(CCObject* sender) {
        m_mode = static_cast<cocos2d::CCNode*>(sender)->getTag();
        refresh();
    }

    // CCMenuItemToggler calls its target BEFORE it flips its own state, so the state the press
    // leads to is the opposite of what isToggled() says here.
    void onCoins(CCObject*) {
        m_coins = !m_coinToggle->isToggled();
        refresh();
    }

    // The (i) beside the Options line: what each choice that is not the default costs.
    void onOptInfo(CCObject*) {
        std::vector<std::string> v;
        if (m_glitch != UI_GLITCH_OFF)
            v.push_back(std::string("Avoid glitches, ") + uiGlitchName(m_glitch) + ": " + describeGlitch(m_glitch));
        if (!m_secSolve) v.push_back(std::string("No section search: ") + describeSecSolve(false));
        if (m_search != UI_SEARCH_DP)
            v.push_back(std::string("Search, ") + uiSearchName(m_search) + ": " + describeSearch(m_search));
        showInfo("Solve options", v);
    }

    // Options over the menu; its choices come back here as they are made.
    void onOptions(CCObject*) {
        if (m_mode == UI_MODE_REPLAY) {
            Ref<Popup> me = this;
            if (auto* o = Options::createReplay(m_replayStep, [me](bool on) { me->m_replayStep = on; })) o->show();
            return;
        }
        if (m_mode != UI_MODE_SOLVE) return;
        Ref<Popup> self = this;
        auto* o = Options::create(m_glitch, m_search, m_secSolve, [self](int glitch, int search, bool secSolve) {
            self->m_glitch = glitch;
            self->m_search = search;
            self->m_secSolve = secSolve;
            self->refresh();
        });
        if (o) o->show();
    }

    void onStart(CCObject*) {
        if (!m_startBtn->isEnabled()) return;
        g_uiMode = m_mode;
        g_uiCoins = m_coins;
        // Only a Solve asks; the choices themselves are kept for the next one either way.
        g_uiGlitchMode = m_mode == UI_MODE_SOLVE ? m_glitch : UI_GLITCH_OFF;
        g_uiGlitch = g_uiGlitchMode != UI_GLITCH_OFF;
        g_uiSecSolve = m_mode != UI_MODE_SOLVE || m_secSolve;
        g_uiReplayStep = m_mode == UI_MODE_REPLAY && m_replayStep;
        g_uiSearch = m_mode == UI_MODE_SOLVE ? m_search : UI_SEARCH_DP;
        g_uiArmedLevel = m_level.data();
        Mod::get()->setSavedValue<int>("play-mode", m_mode);
        if (m_facts.coins > 0) Mod::get()->setSavedValue<bool>("play-coins", m_coins);
        if (m_mode == UI_MODE_REPLAY) Mod::get()->setSavedValue<bool>("play-replaystep", m_replayStep);
        if (m_mode == UI_MODE_SOLVE) {
            // The checkbox's old key keeps meaning "any glitch avoidance", for a downgrade.
            Mod::get()->setSavedValue<int>("play-glitch-mode", m_glitch);
            Mod::get()->setSavedValue<bool>("play-glitch", m_glitch != UI_GLITCH_OFF);
            Mod::get()->setSavedValue<bool>("play-secsolve", m_secSolve);
            Mod::get()->setSavedValue<int>("play-search", m_search);
        }
        log::info("play menu: {} lv{}{}{}{}{} ({} coins in the level)", uiModeName(m_mode),
                  m_level->m_levelID.value(),
                  (m_mode != UI_MODE_NORMAL && m_coins) ? " with coins" : "",
                  g_uiGlitch ? (std::string(" avoiding glitches (") + uiGlitchName(g_uiGlitchMode) + ")")
                             : std::string(),
                  g_uiSecSolve ? "" : ", no section search",
                  g_uiSearch != UI_SEARCH_DP ? std::string(", search ") + uiSearchName(g_uiSearch)
                                             : std::string(),
                  m_facts.coins);
        // Close first: the game's onPlay may open an alert of its own (no song, high object
        // count), and that alert must not end up underneath this popup.
        auto play = std::move(m_play);
        m_started = true;
        this->onClose(nullptr);
        g_passing = true;
        play();
        g_passing = false;
    }

    // Closed without Start (the X, Escape): a mode an earlier Start armed for this level -- one
    // whose press the game then turned back with an alert of its own -- is withdrawn with it.
    void onClose(CCObject* sender) override {
        if (!m_started && g_uiArmedLevel == m_level.data()) g_uiArmedLevel = nullptr;
        PopupBase::onClose(sender);
    }

    void keyDown(cocos2d::enumKeyCodes key, double p1) override {
        if (key == cocos2d::enumKeyCodes::KEY_Enter) { onStart(nullptr); return; }
        if (key >= cocos2d::enumKeyCodes::KEY_One && key <= cocos2d::enumKeyCodes::KEY_Three) {
            m_mode = (int)key - (int)cocos2d::enumKeyCodes::KEY_One;
            refresh();
            return;
        }
        PopupBase::keyDown(key, p1);
    }
};

// The menu for this press, or false to let the press through to the game untouched.
template <class Layer>
bool open(Layer* layer, GJGameLevel* level, CCObject* sender, bool mainLevel) {
    const LevelFacts facts = readLevel(level, mainLevel);
    if (facts.platformer) return false;
    // What the notes were decided from, so a note can be traced back to the objects behind it.
    log::info("play menu: lv{} checked: not modelled: {}; objects from 2.0 on: {}{}{}; random "
              "numbers: {}; changes by attempt (taken as the first): {}; changes by attempt in what "
              "is shown only: {}; two-player: {}",
              level->m_levelID.value(), levelwarn::describeAll(facts.unmodelled),
              facts.newerIds,
              facts.newer ? " (newest " + levelwarn::versionName(facts.newest) + ")" : std::string(),
              facts.official ? " (a main level: yellow not shown)" : "",
              facts.random ? "yes" : "no", facts.attempts ? "yes" : "no",
              facts.attemptShown ? "yes" : "no", facts.twoPlayer ? "yes" : "no");
    Ref<Layer> self = layer;
    Ref<CCObject> from = sender;
    auto* p = Popup::create(level, facts, [self, from] { self->onPlay(from.data()); });
    if (!p) return false;
    p->show();
    return true;
}

}  // namespace playmenu

// The last Start's choice, kept in the mod's saved values so it survives a restart. The menu opens
// on what the level has stored (Popup::init) and takes only the Coins choice from here, for Solve;
// the saved mode no longer sets the menu's first choice. g_uiMode is read by the session setup
// (uiConfigureSession) after a Start, and cfg `uimode=` (loadConfig) only presets it there.
$on_mod(Loaded) {
    if (!gdsolver::exeguard::allowed()) return;   // not this game build: nothing of the mod's runs (exe_guard.hpp)
    g_uiMode = std::clamp(Mod::get()->getSavedValue<int>("play-mode", UI_MODE_NORMAL),
                          0, UI_MODE_COUNT - 1);
    g_uiCoins = Mod::get()->getSavedValue<bool>("play-coins", false);
    g_uiGlitchMode = playmenu::savedGlitchMode();
    g_uiGlitch = g_uiGlitchMode != UI_GLITCH_OFF;
    g_uiSecSolve = Mod::get()->getSavedValue<bool>("play-secsolve", true);
    // The session reads g_uiSearch only after a Start, which sets it; the menu reads the saved
    // value itself. Left at the DP here, so nothing but a Start picks an experimental search.
    g_uiSearch = UI_SEARCH_DP;
    windowguard::noteLaunch();   // the size the save put the window at (window_guard.hpp)
}

// The early-outs below are the first tests each onPlay makes, by the offsets the 2.2081 binary
// tests them at. The asserts tie each to the member the bindings give that offset, so a
// bindings update that moves one fails here instead of opening the menu over a dead press.
static_assert(offsetof(LevelInfoLayer, m_isBusy) == gdoff::kLevelInfoBusy);
static_assert(offsetof(LevelInfoLayer, m_enterTransitionFinished) == gdoff::kLevelInfoTransitionDone);
static_assert(offsetof(LevelPage, m_isBusy) == gdoff::kLevelPageBusy);
static_assert(offsetof(EditLevelLayer, m_exiting) == gdoff::kEditLevelExiting);
static_assert(offsetof(GJGameLevel, m_requiredCoins) == gdoff::kLevelRequiredCoins);

// Online or downloaded levels. onPlay returns at once while busy or before the page has finished
// coming in, and hands a level that is not downloaded yet to the download; none of those start
// a level, so none of them get the menu. (The song and object-count alerts come after, from the
// game's own onPlay once Start has handed the press back.)
class $modify(PlayMenuLevelInfoLayer, LevelInfoLayer) {
    GDSOLVER_HOOK_GUARD()
    void onPlay(CCObject* sender) {
        if (!playmenu::wanted() || m_isBusy || !m_enterTransitionFinished || !m_level
            || this->shouldDownloadLevel()
            || !playmenu::open(this, m_level, sender, false))
            LevelInfoLayer::onPlay(sender);
    }

    // Two of those alerts, "No Song" (tag 9) and "High Objects" (tag 10), are questions:
    // confirming one sets the level's flag for it and calls onPlay again (FLAlert_Clicked, read
    // in the 2.2081 binary). That second call is the press the menu has already answered, so it
    // goes straight through -- without this it opened the menu a second time, and the level
    // started only on the second Start. Cancelling withdraws the mode the menu armed.
    void FLAlert_Clicked(FLAlertLayer* alert, bool btn2) {
        const int tag = alert ? alert->getTag() : 0;
        if (tag != 9 && tag != 10) return LevelInfoLayer::FLAlert_Clicked(alert, btn2);
        if (!btn2) {
            if (g_uiArmedLevel == m_level) g_uiArmedLevel = nullptr;
            return LevelInfoLayer::FLAlert_Clicked(alert, btn2);
        }
        playmenu::g_passing = true;
        LevelInfoLayer::FLAlert_Clicked(alert, btn2);
        playmenu::g_passing = false;
    }
};

// The main-level wheel. The page past the last level (id -1) and the tower's (-2) are not
// levels; a level still locked behind secret coins gets the game's own "Locked" alert.
class $modify(PlayMenuLevelPage, LevelPage) {
    GDSOLVER_HOOK_GUARD()
    void onPlay(CCObject* sender) {
        const int id = m_level ? m_level->m_levelID.value() : -1;
        const bool locked = m_level && m_level->m_requiredCoins
                                           > GameStatsManager::sharedState()->getStat("8");
        if (!playmenu::wanted() || m_isBusy || id <= 0 || locked
            || !playmenu::open(this, m_level, sender, true))
            LevelPage::onPlay(sender);
    }
};

// A locally saved level. Its onPlay sets m_exiting and leaves; a second press finds it set.
class $modify(PlayMenuEditLevelLayer, EditLevelLayer) {
    GDSOLVER_HOOK_GUARD()
    void onPlay(CCObject* sender) {
        if (!playmenu::wanted() || m_exiting || !m_level
            || !playmenu::open(this, m_level, sender, false))
            EditLevelLayer::onPlay(sender);
    }
};

// ---- Hold Play to open the menu ----------------------------------------------------------------
// With the setting "play-menu" off a click on a level's play button plays the level, as without the mod, and the menu opens when
// the button is HELD instead: so a way to the menu is always left, whichever way the setting stands, and the setting can be turned
// back from the menu itself (Options). While the button is held a ring fills around it (a cocos CCDrawNode, drawn from the
// button's own content size: a circle for a round button -- the online / saved level's page -- and the outline of the whole area
// for a wide one -- the main-level wheel's page, which is itself the button); when it is full the ring flashes and the menu opens,
// and the release that follows does not also play. Let go early and the ring is gone and the release is a plain click.
//
// The three buttons are found as the game builds them, not by their handler: the play menu of a LevelInfoLayer, the level menu of a
// LevelPage, and the biggest button of an EditLevelLayer's button menu.
namespace playhold {

constexpr float kSeconds = 0.6f;        // how long a hold takes to count
constexpr float kFlashSeconds = 0.28f;  // the ring's flash when it is full
constexpr int kTag = 0x504c48;          // "PLH": the effect's tag on the button

inline CCMenuItemSpriteExtra* g_swallow = nullptr;   // a hold opened the menu: the release that follows must not also play

enum class Kind { None, Info, Page, Edit };

// Which of the three screens this button is the play button of (and the layer), or None.
inline Kind kindOf(CCMenuItemSpriteExtra* it, cocos2d::CCNode** layerOut) {
    using namespace cocos2d;
    CCNode* menu = it->getParent();
    CCNode* layer = menu ? menu->getParent() : nullptr;
    *layerOut = layer;
    if (!layer) return Kind::None;
    if (auto* li = geode::cast::typeinfo_cast<LevelInfoLayer*>(layer))
        return menu == li->m_playBtnMenu ? Kind::Info : Kind::None;
    if (auto* lp = geode::cast::typeinfo_cast<LevelPage*>(layer))
        return menu == lp->m_levelMenu ? Kind::Page : Kind::None;
    if (auto* el = geode::cast::typeinfo_cast<EditLevelLayer*>(layer)) {
        if (menu != el->m_buttonMenu) return Kind::None;
        // The play button is the biggest of the page's buttons.
        float best = 0.f;
        CCNode* biggest = nullptr;
        for (auto* c : CCArrayExt<CCNode*>(menu->getChildren())) {
            auto* b = geode::cast::typeinfo_cast<CCMenuItemSpriteExtra*>(c);
            if (!b) continue;
            const auto s = b->getContentSize();
            if (s.width * s.height > best) { best = s.width * s.height; biggest = b; }
        }
        return biggest == it ? Kind::Edit : Kind::None;
    }
    return Kind::None;
}

// Whether the game would start a level from this press at all (the early-outs of each onPlay, as the hooks above test them), and
// the menu opening if it would.
inline bool canOpen(Kind k, cocos2d::CCNode* layer) {
    switch (k) {
        case Kind::Info: {
            auto* li = static_cast<LevelInfoLayer*>(layer);
            return !li->m_isBusy && li->m_enterTransitionFinished && li->m_level && !li->shouldDownloadLevel();
        }
        case Kind::Page: {
            auto* lp = static_cast<LevelPage*>(layer);
            const int id = lp->m_level ? lp->m_level->m_levelID.value() : -1;
            const bool locked = lp->m_level && lp->m_level->m_requiredCoins > GameStatsManager::sharedState()->getStat("8");
            return !lp->m_isBusy && id > 0 && !locked;
        }
        case Kind::Edit: {
            auto* el = static_cast<EditLevelLayer*>(layer);
            return !el->m_exiting && el->m_level;
        }
        default: return false;
    }
}
inline bool open(Kind k, cocos2d::CCNode* layer, CCMenuItemSpriteExtra* it) {
    if (!canOpen(k, layer)) return false;
    switch (k) {
        case Kind::Info: { auto* l = static_cast<LevelInfoLayer*>(layer); return playmenu::open(l, l->m_level, it, false); }
        case Kind::Page: { auto* l = static_cast<LevelPage*>(layer); return playmenu::open(l, l->m_level, it, true); }
        case Kind::Edit: { auto* l = static_cast<EditLevelLayer*>(layer); return playmenu::open(l, l->m_level, it, false); }
        default: return false;
    }
}

// The effect: a node on the button, centred on it, that draws the ring and keeps the time.
class Driver : public cocos2d::CCNode {
public:
    static Driver* create(CCMenuItemSpriteExtra* item, Kind kind, cocos2d::CCNode* layer) {
        auto* r = new Driver();
        if (r->init(item, kind, layer)) { r->autorelease(); return r; }
        delete r;
        return nullptr;
    }
    bool fired() const { return m_fired; }
    void cancel() { this->removeFromParent(); }

protected:
    CCMenuItemSpriteExtra* m_item = nullptr;   // the node this is a child of: not retained
    Kind m_kind = Kind::None;
    cocos2d::CCNode* m_layer = nullptr;        // ditto
    cocos2d::CCDrawNode* m_draw = nullptr;
    cocos2d::CCSize m_size;
    bool m_round = false;
    float m_t = 0.f, m_flash = 0.f;
    bool m_fired = false;

    bool init(CCMenuItemSpriteExtra* item, Kind kind, cocos2d::CCNode* layer) {
        using namespace cocos2d;
        if (!CCNode::init()) return false;
        m_item = item;
        m_kind = kind;
        m_layer = layer;
        m_size = item->getContentSize();
        const float big = std::max(m_size.width, m_size.height);
        m_round = big > 0.f && std::abs(m_size.width - m_size.height) <= 0.12f * big;   // a round button's box is a square
        this->setPosition(ccp(m_size.width / 2.f, m_size.height / 2.f));
        this->setTag(kTag);
        m_draw = CCDrawNode::create();
        this->addChild(m_draw);
        this->scheduleUpdate();
        redraw(0.f, 1.f);
        return true;
    }

    // The ring: a faint track all the way round and the bright part up to `fraction`, clockwise from the top. All of it inside the
    // button's own box (a margin in from the edge), in coordinates about its centre.
    void redraw(float fraction, float alpha) {
        using namespace cocos2d;
        m_draw->clear();
        const float margin = 2.5f, thick = 1.7f;
        const ccColor4F track{1.f, 1.f, 1.f, 0.22f * alpha};
        const ccColor4F bar{1.f, 0.93f, 0.3f, alpha};
        if (m_round) {
            const float r = std::min(m_size.width, m_size.height) / 2.f - margin;
            const int total = 72;
            auto at = [&](int i) {
                const float a = 3.14159265f / 2.f - 2.f * 3.14159265f * (float(i) / total);
                return ccp(std::cos(a) * r, std::sin(a) * r);
            };
            for (int i = 0; i < total; ++i) m_draw->drawSegment(at(i), at(i + 1), thick, track);
            const int n = int(fraction * total + 0.5f);
            for (int i = 0; i < n; ++i) m_draw->drawSegment(at(i), at(i + 1), thick, bar);
        } else {
            const float hw = m_size.width / 2.f - margin, hh = m_size.height / 2.f - margin;
            const CCPoint c[5] = {ccp(-hw, hh), ccp(hw, hh), ccp(hw, -hh), ccp(-hw, -hh), ccp(-hw, hh)};   // clockwise from the top-left
            const float side[4] = {2 * hw, 2 * hh, 2 * hw, 2 * hh};
            const float L = side[0] + side[1] + side[2] + side[3];
            for (int e = 0; e < 4; ++e) m_draw->drawSegment(c[e], c[e + 1], thick, track);
            float left = fraction * L;
            for (int e = 0; e < 4 && left > 0.f; ++e) {
                const float f = std::min(1.f, left / side[e]);
                m_draw->drawSegment(c[e], ccp(c[e].x + (c[e + 1].x - c[e].x) * f, c[e].y + (c[e + 1].y - c[e].y) * f), thick, bar);
                left -= side[e];
            }
        }
    }

    void update(float dt) override {
        if (!m_fired) {
            m_t += dt;
            redraw(std::min(1.f, m_t / kSeconds), 1.f);
            if (m_t >= kSeconds) {
                m_fired = true;
                // The hold counts: open the menu now, and keep the release that follows from also playing.
                if (open(m_kind, m_layer, m_item)) g_swallow = m_item;
                else this->removeFromParent();   // nothing to open (the game would not play it either)
            }
        } else {
            m_flash += dt;
            const float k = m_flash / kFlashSeconds;
            if (k >= 1.f) { this->removeFromParent(); return; }
            redraw(1.f, 1.f - k);
            this->setScale(1.f + 0.1f * k);
        }
    }
};

inline void onSelected(CCMenuItemSpriteExtra* it) {
    g_swallow = nullptr;   // a stale one from a hold whose release never came
    if (!playmenu::holdOpens()) return;
    cocos2d::CCNode* layer = nullptr;
    const Kind k = kindOf(it, &layer);
    if (k == Kind::None || !canOpen(k, layer)) return;
    if (it->getChildByTag(kTag)) return;
    if (auto* d = Driver::create(it, k, layer)) it->addChild(d, 100);
}

inline void onUnselected(CCMenuItemSpriteExtra* it) {
    // Let go: before the ring is full it is gone and the release is a plain click; after, it is already flashing out on its own.
    if (auto* d = geode::cast::typeinfo_cast<Driver*>(it->getChildByTag(kTag)))
        if (!d->fired()) d->cancel();
}

inline bool swallow(CCMenuItemSpriteExtra* it) {
    if (g_swallow != it) return false;
    g_swallow = nullptr;
    return true;
}

}  // namespace playhold

class $modify(PlayHoldItem, CCMenuItemSpriteExtra) {
    GDSOLVER_HOOK_GUARD()
    void selected() {
        CCMenuItemSpriteExtra::selected();
        playhold::onSelected(this);
    }
    void unselected() {
        CCMenuItemSpriteExtra::unselected();
        playhold::onUnselected(this);
    }
    void activate() {
        if (playhold::swallow(this)) return;   // the hold opened the menu: this release is not a press
        CCMenuItemSpriteExtra::activate();
    }
};

// Diagnostic, cfg `uishow=infoplay[:<stars>][:z]`: the real route to the Play menu of an online / downloaded level, short of the click. A level (cfg
// `levelfile`, cfg `level` for its id, <stars> stars, default 0) is opened on a LevelInfoLayer the way the game opens one, and once the page has come in
// onPlay is called on it -- the hooked one above, so "play menu: lv<N> checked" in the Geode log (or its absence) is the answer, and the result lines say
// what the hook saw: the layer's m_isBusy and m_enterTransitionFinished by the bindings, and the bytes at +0x27d and +0x285. (Written for the update
// of 2.209: the hook read m_enterTransitionFinished at the old build's offset, which in the update is a byte of m_orbsIcon, the sprite pointer's byte 5,
// zero whenever that address is below 2^40.) With `:z` the byte at +0x27d is set to 0 just before the press: the case of the update's old offset reading a zero, made on purpose (it corrupts
// m_orbsIcon, which nothing reads before the process is closed). LevelInfoLayer::scene has no Windows address in the bindings, so its place in each build is here.
#if GEODE_COMP_GD_VERSION >= 22090
namespace infoplay {
inline void run(int stars, bool zero27d) {
    if (g_cfg.levelFile.empty()) { writeResult("infoplay: no cfg levelfile"); return; }
    std::ifstream f(g_cfg.levelFile, std::ios::binary);
    const std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    auto* level = GJGameLevel::create();
    level->m_levelName = "preview";
    level->m_levelID = g_cfg.levelId;
    level->m_levelType = GJLevelType::Saved;
    level->m_levelString = gdcompat::compressLevelString(raw);
    level->m_stars = stars;
    using SceneFn = cocos2d::CCScene* (*)(GJGameLevel*, bool);
    const auto scene = reinterpret_cast<SceneFn>(geode::base::get() + (GEODE_COMP_GD_VERSION >= 22091 ? 0x41bc20 : 0x418ec0));
    CCDirector::sharedDirector()->replaceScene(scene(level, false));
    writeResult("infoplay: lv" + std::to_string(g_cfg.levelId) + " stars=" + std::to_string(stars) + " opened");
    std::thread([id = (int)g_cfg.levelId, stars, zero27d] {
        std::this_thread::sleep_for(std::chrono::seconds(4));
        Loader::get()->queueInMainThread([id, stars, zero27d] {
            auto* running = CCDirector::sharedDirector()->getRunningScene();
            LevelInfoLayer* layer = nullptr;
            if (running)
                for (auto* c : CCArrayExt<CCNode*>(running->getChildren()))
                    if (auto* l = typeinfo_cast<LevelInfoLayer*>(c)) layer = l;
            if (!layer) { writeResult("infoplay: lv" + std::to_string(id) + " no LevelInfoLayer in the running scene"); return; }
            auto* raw = reinterpret_cast<unsigned char*>(layer);
            if (zero27d) raw[0x27d] = 0;
            writeResult("infoplay: lv" + std::to_string(id) + " stars=" + std::to_string(stars) + " m_isBusy=" + std::to_string((int)layer->m_isBusy)
                        + " m_enterTransitionFinished(bindings)=" + std::to_string((int)layer->m_enterTransitionFinished)
                        + " byte[0x27d]=" + std::to_string((int)raw[0x27d]) + " byte[0x285]=" + std::to_string((int)raw[0x285])
                        + " m_level=" + std::to_string(layer->m_level != nullptr ? 1 : 0) + (zero27d ? " (byte[0x27d] forced to 0)" : ""));
            layer->onPlay(nullptr);
            writeResult("infoplay: lv" + std::to_string(id) + " onPlay returned");
        });
    }).detach();
}
}  // namespace infoplay
#endif

// Diagnostic, cfg `uishow=<kind>` (default none): one of the popups above over the main menu 3 s after it appears, so a layout can be looked at
// (and photographed) without playing to it. info0 / info1 / info2 are Options' (i) texts; options is the Options popup with every choice moved off its
// default; info3 is the Hold Play row's; menu is the Solve menu for cfg level (a main level) or levelfile (a custom one), also with Options moved off the defaults.
// press is the player's route without the clicks: the menu for that level, in the mode cfg `uimode` names (1 Replay, 2 Solve), Start
// pressed, and the level entered through the play menu's own arming (g_uiArmedLevel), with the saved choices left as they are.
class $modify(PlayMenuPreview, MenuLayer) {
    GDSOLVER_HOOK_GUARD()
    bool init() {
        if (!MenuLayer::init()) return false;
        static bool once = false;
        if (!once) {
            once = true;
            this->scheduleOnce(schedule_selector(PlayMenuPreview::preview), 3.f);
        }
        return true;
    }

    void preview(float) {
        const std::string kind = g_cfg.uiShow;
        if (kind.empty()) return;
#if GEODE_COMP_GD_VERSION >= 22090
        if (kind.rfind("infoplay", 0) == 0) {
            infoplay::run(kind.size() > 9 && kind[8] == ':' ? std::atoi(kind.c_str() + 9) : 0,
                          kind.size() >= 2 && kind.compare(kind.size() - 2, 2, ":z") == 0);
            return;
        }
#endif
        if (kind == "info0") { playmenu::showInfo("Section search", playmenu::secSolveDetails()); return; }
        if (kind == "info1") { playmenu::showInfo("Avoid glitches", playmenu::glitchDetails()); return; }
        if (kind == "info2") { playmenu::showInfo("Search", playmenu::searchDetails()); return; }
        if (kind == "info3") { playmenu::showInfo("Hold Play to open this menu", playmenu::holdPlayDetails()); return; }
        if (kind == "info4") { playmenu::showInfo("Fixed replay step", playmenu::replayStepDetails()); return; }
        if (kind == "optionsreplay") {
            if (auto* o = playmenu::Options::createReplay(true, nullptr)) o->show();
            return;
        }
        if (kind == "options") {
            if (auto* o = playmenu::Options::create(UI_GLITCH_WAVE, UI_SEARCH_BEAM, false, nullptr)) o->show();
            return;
        }
        if (kind == "infolong") {   // a text longer than the window, to see the scroll and its bar
            auto v = playmenu::glitchDetails();
            for (auto s : playmenu::searchDetails()) v.push_back(s);
            for (auto s : playmenu::secSolveDetails()) v.push_back(s);
            playmenu::showInfo("Avoid glitches", v);
            return;
        }
        if (kind != "menu" && kind != "menuinfo" && kind != "press" && kind != "pressstep") return;
        // `press` leaves the saved choices alone: it is the player's route, not a layout photograph. `pressstep` is `press` with the
        // Replay form's Fixed replay step chosen first (the saved value the menu reads), to reach the option without a click.
        const bool press = kind == "press" || kind == "pressstep";
        if (kind == "pressstep") Mod::get()->setSavedValue<bool>("play-replaystep", true);
        if (!press) {
            Mod::get()->setSavedValue<int>("play-glitch-mode", UI_GLITCH_WAVE);
            Mod::get()->setSavedValue<bool>("play-secsolve", false);
            Mod::get()->setSavedValue<int>("play-search", UI_SEARCH_BEAM);
        }
        GJGameLevel* level = nullptr;
        bool mainLevel = false;
        if (!g_cfg.levelFile.empty()) {
            std::ifstream f(g_cfg.levelFile, std::ios::binary);
            const std::string raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            level = GJGameLevel::create();
            level->m_levelName = "preview";
            level->m_levelID = g_cfg.levelId;
            level->m_levelType = GJLevelType::Editor;
            level->m_levelString = gdcompat::compressLevelString(raw);
        } else {
            level = GameLevelManager::get()->getMainLevel(g_cfg.levelId, false);
            mainLevel = true;
        }
        if (!level) return;
        if (press) {
            // The level page's own press ends in the same scene: the play menu is the only thing between this and the layer.
            auto enter = [level] {
                CCDirector::sharedDirector()->replaceScene(
                    CCTransitionFade::create(0.5f, PlayLayer::scene(level, false, false)));
            };
            if (auto* p = playmenu::Popup::create(level, playmenu::readLevel(level, mainLevel), enter)) {
                p->show();
                p->pressStart(g_uiMode);   // cfg uimode: 1 Replay, 2 Solve
            }
            return;
        }
        if (auto* p = playmenu::Popup::create(level, playmenu::readLevel(level, mainLevel), [] {})) {
            p->show();
            p->previewMode(UI_MODE_SOLVE, kind == "menuinfo");
        }
    }
};
