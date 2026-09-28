// The play menu: the popup a level's play button opens, where the session's mode is chosen.
#include "mod/playlayer_helpers.hpp"
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/LevelPage.hpp>
#include <Geode/modify/EditLevelLayer.hpp>
#include <Geode/ui/Popup.hpp>
#include <cstddef>
#include <string_view>

using namespace p1;

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

inline bool wanted() {
    if (g_passing) return false;
    // A session in progress is not interfered with: an autorun or a suite drives itself.
    if (g_started && !g_sessionOver) return false;
    // Setting off: the play button plays the level, as without the mod.
    return Mod::get()->getSettingValue<bool>("play-menu");
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
    if (s.rfind("H4sI", 0) == 0)    // base64 of a gzip header: the stored, compressed form
        s = std::string(cocos2d::ZipUtils::decompressString(s, false, 0));
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

class Popup : public geode::Popup {
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
    cocos2d::CCLabelBMFont* m_note = nullptr;
    cocos2d::CCLabelBMFont* m_rngNote = nullptr;
    ButtonSprite* m_startSpr = nullptr;
    CCMenuItemSpriteExtra* m_startBtn = nullptr;

    // The layout, top to bottom, as offsets from the popup's centre (260 high): the title and
    // the level's name, the mode row, the description, the Coins row, the bot note, Start. Each
    // row keeps clear of the next at its tallest -- the description is two lines, the checkbox
    // is taller than its label -- which the first cut (230 high) did not: the checkbox sat on
    // the bot note.
    static constexpr float kW = 340.f, kH = 260.f;
    static constexpr float kModeY = 42.f, kDescY = 2.f, kCoinY = -34.f;
    static constexpr float kNoteY = 64.f, kStartY = 32.f;   // from the bottom edge
    static constexpr float kRngNoteY = 78.f;   // just above the bot note, below the Coins row

    bool init(GJGameLevel* level, LevelFacts facts, std::function<void()> play) {
        using namespace cocos2d;
        if (!geode::Popup::init(kW, kH)) return false;
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
            return loadInputsFile(uiSolutionPath(id, coins), plan);
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
        geode::cocos::limitNodeWidth(name, 280.f, .45f, .1f);
        name->setColor({200, 200, 255});
        name->setID("level-name"_spr);
        m_mainLayer->addChildAtPosition(name, Anchor::Top, ccp(0, -46));

        const char* ids[UI_MODE_COUNT] = {"normal-button"_spr, "replay-button"_spr,
                                          "solve-button"_spr};
        for (int m = 0; m < UI_MODE_COUNT; ++m) {
            m_modeSpr[m] = ButtonSprite::create(uiModeName(m), 76, true, "bigFont.fnt",
                                                "GJ_button_04.png", 30.f, .6f);
            auto* btn = CCMenuItemSpriteExtra::create(m_modeSpr[m], this,
                                                      menu_selector(Popup::onMode));
            btn->setTag(m);
            btn->setID(ids[m]);
            m_buttonMenu->addChildAtPosition(btn, Anchor::Center,
                                             ccp(-100.f + 100.f * m, kModeY));
        }

        m_desc = CCLabelBMFont::create("", "chatFont.fnt", 300.f, kCCTextAlignmentCenter);
        m_desc->setScale(.75f);
        m_desc->setID("description"_spr);
        m_mainLayer->addChildAtPosition(m_desc, Anchor::Center, ccp(0, kDescY));

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
        const float left = -(boxW + gap + m_coinLabel->getScaledContentWidth()) / 2.f;
        m_buttonMenu->addChildAtPosition(m_coinToggle, Anchor::Center,
                                         ccp(left + boxW / 2.f, kCoinY));
        m_mainLayer->addChildAtPosition(m_coinLabel, Anchor::Center,
                                        ccp(left + boxW + gap, kCoinY));
        if (!haveCoins) {
            // Refused, and shown to be: the box greys out and the label says why.
            m_coinToggle->setEnabled(false);
            for (auto* b : {m_coinToggle->m_offButton, m_coinToggle->m_onButton})
                if (auto* spr = geode::cast::typeinfo_cast<CCSprite*>(b->getNormalImage()))
                    spr->setColor(kDim);
            m_coinLabel->setColor(kDim);
        }

        // The promise the safety gate makes, where the choice is made (see botDriving).
        m_note = CCLabelBMFont::create("The bot drives: nothing is recorded.", "chatFont.fnt");
        m_note->setScale(.6f);
        m_note->setOpacity(150);
        m_note->setID("bot-note"_spr);
        m_mainLayer->addChildAtPosition(m_note, Anchor::Bottom, ccp(0, kNoteY));
        m_rngNote = CCLabelBMFont::create("This level uses random numbers: fixed while the bot drives.",
                                          "chatFont.fnt");
        m_rngNote->setScale(.55f);
        m_rngNote->setColor({255, 230, 150});
        m_rngNote->setID("random-note"_spr);
        m_mainLayer->addChildAtPosition(m_rngNote, Anchor::Bottom, ccp(0, kRngNoteY));

        m_startSpr = ButtonSprite::create("Start", "goldFont.fnt", "GJ_button_01.png", .9f);
        m_startBtn = CCMenuItemSpriteExtra::create(m_startSpr, this,
                                                   menu_selector(Popup::onStart));
        m_startBtn->setID("start-button"_spr);
        m_buttonMenu->addChildAtPosition(m_startBtn, Anchor::Bottom, ccp(0, kStartY));

        refresh();
        return true;
    }

    // Replay needs a file it can play: one that is there and holds at least one input, which is
    // the test the session itself applies (loadInputsFile).
    bool hasSolution() const {
        std::vector<InputCmd> plan;
        return loadInputsFile(uiSolutionPath(m_level->m_levelID.value(), m_coins), plan);
    }

    void refresh() {
        for (int m = 0; m < UI_MODE_COUNT; ++m)
            m_modeSpr[m]->updateBGImage(m == m_mode ? "GJ_button_01.png" : "GJ_button_04.png");
        const bool bot = m_mode != UI_MODE_NORMAL;
        m_coinToggle->setVisible(bot);
        m_coinLabel->setVisible(bot);
        m_note->setVisible(bot);
        // Saying so here, rather than letting the session find out after the level has loaded,
        // is the point of asking before the level starts.
        const bool ok = m_mode != UI_MODE_REPLAY || hasSolution();
        // A level that draws on the game's random numbers is solved and replayed with them fixed
        // (rngfix): said here, so a replay outside the mod that goes differently is no surprise.
        m_rngNote->setVisible(bot && m_facts.random);
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

    void onStart(CCObject*) {
        if (!m_startBtn->isEnabled()) return;
        g_uiMode = m_mode;
        g_uiCoins = m_coins;
        g_uiArmedLevel = m_level.data();
        Mod::get()->setSavedValue<int>("play-mode", m_mode);
        if (m_facts.coins > 0) Mod::get()->setSavedValue<bool>("play-coins", m_coins);
        log::info("play menu: {} lv{}{} ({} coins in the level)", uiModeName(m_mode),
                  m_level->m_levelID.value(),
                  (m_mode != UI_MODE_NORMAL && m_coins) ? " with coins" : "", m_facts.coins);
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
        geode::Popup::onClose(sender);
    }

    void keyDown(cocos2d::enumKeyCodes key, double p1) override {
        if (key == cocos2d::enumKeyCodes::KEY_Enter) { onStart(nullptr); return; }
        if (key >= cocos2d::enumKeyCodes::KEY_One && key <= cocos2d::enumKeyCodes::KEY_Three) {
            m_mode = (int)key - (int)cocos2d::enumKeyCodes::KEY_One;
            refresh();
            return;
        }
        geode::Popup::keyDown(key, p1);
    }
};

// The menu for this press, or false to let the press through to the game untouched.
template <class Layer>
bool open(Layer* layer, GJGameLevel* level, CCObject* sender, bool mainLevel) {
    const LevelFacts facts = readLevel(level, mainLevel);
    if (facts.platformer) return false;
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
    g_uiMode = std::clamp(Mod::get()->getSavedValue<int>("play-mode", UI_MODE_NORMAL),
                          0, UI_MODE_COUNT - 1);
    g_uiCoins = Mod::get()->getSavedValue<bool>("play-coins", false);
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
    void onPlay(CCObject* sender) {
        if (!playmenu::wanted() || m_exiting || !m_level
            || !playmenu::open(this, m_level, sender, false))
            EditLevelLayer::onPlay(sender);
    }
};
