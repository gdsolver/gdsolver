#pragma once
// The solve session's two progress bars, drawn as GD's own level bar (every platform).
//
// Split out of touch_controls.hpp, where the Android port first drew them: the bars replace the
// text bars of the session HUD on every platform, while the on-screen buttons stay Android's.

namespace progressbars {

constexpr int BAR_TAG = 0x51D71;

// ---- The progress bar ----
// Two drawn bars: how far into the level the run has got (the deepest verified point, which only
// grows), and how much of what the solve is doing right now is done -- the dp search in ticks,
// a section search in layers, or the flight of the installed plan in ticks (updateBars). Solve
// sessions only, like the text.
//
// The level bar has a second fill behind the first, in the model's colour: how far the model's deepest plan gets, which shows past the
// verified point as the part that is not confirmed yet (replaypanel::unconfirmedX).
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

    // textOnly: the second row is a caption with no groove or fill (a phase with no measurable end).
    void refresh(bool visible, double levelFrac, const char* levelText, bool searching,
                 double searchFrac, const char* searchText, bool textOnly = false, double planFrac = -1.0) {
        setVisible(visible);
        if (!visible) return;
        setRow(0, true, levelFrac, levelText, planFrac);
        setRow(1, searching, searchFrac, searchText);
        if (searching && textOnly) {
            m_track[1]->setVisible(false);
            if (m_fillRect[1]) m_fillRect[1]->setVisible(false);
            if (m_fillSprite[1]) m_fillSprite[1]->setTextureRect({0.f, 0.f, 0.f, m_fillH[1]});
        }
    }

private:
    cocos2d::CCNode* m_track[2] = {};          // the groove sprite, or the plain track
    cocos2d::CCSprite* m_fillSprite[2] = {};   // GD's fill, cut by texture rect...
    cocos2d::CCLayerColor* m_fillRect[2] = {}; // ...or the plain fill
    float m_fillW[2] = {}, m_fillH[2] = {};    // the fill at 100%, in the groove's own units
    cocos2d::CCLabelBMFont* m_text[2] = {};
    cocos2d::CCSprite* m_planSprite = nullptr;       // the level row's second fill (the model's plan), behind the first
    cocos2d::CCLayerColor* m_planRect = nullptr;

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
            if (r == 0) {                                // the plan's fill: the same texture, the model's colour, one layer down
                if (auto* pf = CCSprite::create("sliderBar2.png")) {
                    pf->setColor({90, 180, 255});
                    pf->setAnchorPoint({0.f, 0.f});
                    pf->setPosition({2.f, (gs.height - m_fillH[r]) / 2.f});
                    pf->setTextureRect({0.f, 0.f, 0.f, m_fillH[r]});
                    groove->addChild(pf, -2);
                    m_planSprite = pf;
                }
            }
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
            if (r == 0) {
                auto* prect = CCLayerColor::create({90, 180, 255, 255}, 0.f, kBarH);
                prect->setPosition({x, y});
                prect->setID(fmt::format("{}-plan-fill", name));
                this->addChild(prect);
                m_planRect = prect;
            }
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

    void setRow(int r, bool on, double frac, const char* text, double planFrac = -1.0) {
        m_track[r]->setVisible(on);
        if (m_fillRect[r]) m_fillRect[r]->setVisible(on);
        m_text[r]->setVisible(on);
        if (r == 0) {
            if (m_planRect) m_planRect->setVisible(on);
            if (m_planSprite) m_planSprite->setVisible(on);
        }
        if (!on) return;
        const double f = frac < 0.0 ? 0.0 : (frac > 1.0 ? 1.0 : frac);
        if (m_fillSprite[r])
            m_fillSprite[r]->setTextureRect({0.f, 0.f, (float)(f * m_fillW[r]), m_fillH[r]});
        if (m_fillRect[r]) m_fillRect[r]->setContentSize({(float)(f * kBarW), kBarH});
        if (r == 0) {                                    // the plan's fill, from the left too: the verified one covers its first part
            const double pf = planFrac < 0.0 ? 0.0 : (planFrac > 1.0 ? 1.0 : planFrac);
            const double pw = pf > f ? pf : 0.0;
            if (m_planSprite) m_planSprite->setTextureRect({0.f, 0.f, (float)(pw * m_fillW[r]), m_fillH[r]});
            if (m_planRect) m_planRect->setContentSize({(float)(pw * kBarW), kBarH});
        }
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
    // the model's deepest plan, past the verified point: the part of the level that is planned and not yet confirmed
    const double ux = replaypanel::unconfirmedX();
    const double pfrac = (lvl > 0.0 && ux > (double)g_hudVerifiedX) ? ux / lvl : -1.0;
    char lt[128];
    if (pfrac >= 0.0)
        snprintf(lt, sizeof(lt), "level %.1f%%  (plan %.1f%%)   iter %d", lf * 100.0, pfrac * 100.0, g_hudIter);
    else
        snprintf(lt, sizeof(lt), "level %.1f%%   iter %d", lf * 100.0, g_hudIter);
    // The second row is whatever the solve is doing now that has a measurable end: the dp
    // search (its ticks), else a recording pass of the moving geometry (x over the level), else
    // a section search in the game (its layers, 1 .. g_horizon: the loop in
    // hooks_gamelayer.cpp), else the game flying a plan (the player's x over the level). While
    // the level is held still -- a plan being installed, a reset -- the row names the phase
    // (phase.hpp) as text, with no fill, and is hidden only if no phase is set.
    const dpbridge::SolveProgress pr = dpbridge::progress();
    double sf = 0.0;
    char st[96] = "";
    bool second = false, textOnly = false;
    if (pr.running && pr.horizon > pr.from) {
        sf = (double)(pr.tick - pr.from) / (double)(pr.horizon - pr.from);
        snprintf(st, sizeof(st), "search %.1f%%", sf * 100.0);
        second = true;
    } else if (dpsolve::g_recordAttempt && len > 1.f) {
        // A recording pass (repair.hpp RecordKind): the moving geometry recorded by a run with no
        // inputs (the first, before any solving) or along the deepest plan. It runs to the end of
        // the level or to its death, so the player's x over the level is how far it has got.
        float px = 0.f;
        if (auto* pl = PlayLayer::get()) if (pl->m_player1) px = pl->m_player1->getPositionX();
        sf = (double)px / (double)len;
        snprintf(st, sizeof(st), "%s %.1f%%",
                 dpsolve::g_recordKind == dpsolve::RecDeep ? "recording the moving geometry (deepest plan)"
                                                           : "recording the moving geometry",
                 sf * 100.0);
        second = true;
    } else if (secsolve::g_active && secsolve::g_horizon > 0) {
        sf = (double)secsolve::g_depth / (double)secsolve::g_horizon;
        snprintf(st, sizeof(st), "section search %.1f%%   layer %d / %d", sf * 100.0,
                 secsolve::g_depth, secsolve::g_horizon);
        second = true;
    } else if (std::string pname; !pr.running && phase::current(pname, sf)) {
        // None of the four measures applies, but the loop has said what it is doing (phase.hpp):
        // the name and how long it has taken, as text -- there is no end to fill towards.
        snprintf(st, sizeof(st), "%s  %.0f s", pname.c_str(), sf);
        sf = 0.0;
        second = true;
        textOnly = true;
    } else if (!pr.running && !g_paused && len > 1.f) {
        // A flight, the first one included (no plan installed yet: it runs on whatever the loop
        // gave it). The player's x over the level: a plan's last input is not where its flight
        // ends -- the player runs on past it -- so ticks against that went over 100%.
        float px = 0.f;
        if (auto* pl = PlayLayer::get()) if (pl->m_player1) px = pl->m_player1->getPositionX();
        sf = (double)px / (double)len;
        snprintf(st, sizeof(st), "flying the plan %.1f%%   x %.0f / %.0f", sf * 100.0,
                 (double)px, (double)len);
        second = true;
    }
    bars->refresh(true, lf, lt, second, sf, st, textOnly, pfrac);
}

}  // namespace progressbars
