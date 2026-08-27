#pragma once
// On-screen HUD that stays visible while rendering is skipped.
#include "mod/ui_panel.hpp"

using namespace p1;

// ---- On-screen HUD: keeps progress visible even in fast mode (render skip) ----
// Attached to the game layer's parent (the scene), so even when PlayLayer::visit skips rendering
// it is drawn independently by the scene's visit. Progress text sits on top of the black screen.
// Note: the label is not cached as a raw pointer; it is looked up by tag every time. Touching a
// raw pointer after a scene rebuild has freed the old label is UB (access violation inside
// update -> freeze)
constexpr int HUD_TAG = 0x51D50;
constexpr int KEYS_TAG = 0x51D52;
constexpr int BADGE_TAG = 0x51D53;

// Every overlay is one column down the LEFT edge, laid out top to bottom in a single pass.
//
// It is not spread over the corners because the right and bottom of getWinSize() are not
// reliably on screen: measured 2026-08-23 in a 1706x960 worker window, getWinSize() reports
// 569x320 with the scene at scale 1 and position (0,0), yet a label at design y=10 does not
// appear at all and one anchored to design x=559 is clipped at the right edge (its glyphs run
// off screen). Whatever the cause -- the framebuffer is 2276x1280, four times the design size
// rather than three -- the top-left corner is the one region that is always visible, so
// everything hangs off it and the column grows downwards by each label's own height.
struct OverlayCursor { float y; };

// Fetch or create one overlay label. `y` is advanced past it, so the next one lands below.
inline cocos2d::CCLabelBMFont* overlayLabel(cocos2d::CCNode* parent, int tag, const char* font,
                                            float scale, bool visible, OverlayCursor& cur) {
    using namespace cocos2d;
    auto* lbl = static_cast<CCLabelBMFont*>(parent->getChildByTag(tag));
    if (!visible) {
        if (lbl) lbl->setVisible(false);
        return nullptr;
    }
    if (!lbl) {
        lbl = CCLabelBMFont::create("", font);
        if (!lbl) return nullptr;
        lbl->setTag(tag);
        lbl->setAnchorPoint({0.f, 1.f});
        lbl->setScale(scale);
        lbl->setZOrder(1 << 20);
        parent->addChild(lbl);
    }
    lbl->setVisible(true);
    lbl->setPositionX(10.f);
    lbl->setPositionY(cur.y);
    cur.y -= lbl->getContentSize().height * scale + 3.f;
    return lbl;
}

// How the game is being advanced right now, in the words the keys use.
// "FAST" means the fast loop owns the frames (the spectating notch does nothing there); anything
// else is the notch the arrow keys set. Slow motion is a separate divider on dt, so it is
// reported separately rather than folded into the multiplier.
inline void speedText(char* out, size_t n) {
    const bool fastLoop = g_started && !g_sessionOver && g_cfg.fastdt > 0 && !g_realtimeOverride;
    char slow[24] = "";
    if (g_slowmo > 1.f) snprintf(slow, sizeof(slow), "  slowmo 1/%.0f", (double)g_slowmo);
    if (fastLoop)
        snprintf(out, n, "FAST (%d loops/frame)%s", g_cfg.fastloops, slow);
    else
        snprintf(out, n, "%gx%s", (double)g_watchSpeed, slow);
}

// Keys, current speed and whether time is stopped. Hidden with F1 -- unlike the badge, this is
// the user's to turn off. In a replay this is the ONLY line besides the badge, so the speed
// lives here and nowhere else (it used to be printed twice).
inline void fillKeysHud(cocos2d::CCLabelBMFont* lbl) {
    if (!lbl) return;
    static auto s_last = std::chrono::steady_clock::time_point{};
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - s_last).count() < 200
        && *lbl->getString())
        return;
    s_last = now;
    char spd[64];
    speedText(spd, sizeof(spd));
    // The screen switch only earns its place in a solve: there it hands the frames back to the
    // fast loop, which is most of the speed. In a replay there is nothing to hand them to and
    // nothing to gain by not looking at the run, so neither the key nor the state is shown.
    //
    // The whole SESSION, matching the key's own condition. Listing it only while the search
    // thread happens to be running meant that the one moment you needed it -- the screen dark,
    // the solve stopped -- was the moment it was not on the list.
    const bool showRender = solveSession();
    char renderPart[32] = "";
    char renderLine[40] = "";
    if (showRender) {
        snprintf(renderPart, sizeof(renderPart), "        RENDER %s",
                 renderingOn() ? "on" : "off");
        snprintf(renderLine, sizeof(renderLine), "F5   render on / off\n");
    }
    // Listed in key order. Anything else reads as a jumble -- there is no other order a reader
    // can predict, and this list is scanned, not read.
    char buf[640];
    snprintf(buf, sizeof(buf),
        "SPEED  %s%s%s\n"
        "F1   hide this overlay%s\n"
        "F2   pause / resume\n"
        "F3 / F4   step 1 / 10 substeps (hold to repeat)\n"
        "%s"
        "F6   hitboxes: off / on / only\n"
        "F7   replay from the start\n"
        "F9   quit to the level screen\n"
        "left / right   speed down / up",
        spd, (g_paused || probe::g_pause) ? "  [PAUSED]" : "", renderPart,
        showingSolve() ? "  (disabled while solving)" : "",
        renderLine);
    lbl->setString(buf);
}

// The bot badge. Shown whenever the mod is driving, and NOT hidden by F1 or by cfg `hud=0`:
// spec §9 asks that a replay be visibly a replay, so this one is not the user's to turn off.
// It is the only overlay with that property, and it is the only place the product name appears
// (the session HUD under it used to repeat it).
// The level is deliberately NOT in it: at this font size the badge was wide enough to sit on
// top of the progress bar underneath, and the level is already in the window title and in the
// session block. What the badge is for is saying that this run is a bot's.
inline void fillBotBadge(cocos2d::CCLabelBMFont* lbl) {
    if (!lbl) return;
    static bool s_wasSolving = false;
    // The whole solve session, not only the moments the search thread is running. The loop
    // alternates searching with replaying its own candidates, and a badge driven by the narrow
    // predicate flipped to REPLAY and back on every iteration -- which reads as the mod changing
    // mode a dozen times rather than getting on with one job.
    const bool solving = showingSolve();
    if (!*lbl->getString() || solving != s_wasSolving) {
        s_wasSolving = solving;
        lbl->setString(solving ? "GDSOLVER BOT - SOLVING" : "GDSOLVER BOT - REPLAY");
    }
}

// What the solver is doing. Solve sessions only: a replay has nothing to report that the badge
// and the key line do not already say.
// One "label [####............]  41%   detail" line, newline-terminated. Returns how much was
// written so the caller can keep appending into the same buffer.
//
// The fraction is clamped to [0,1]: the spans here are estimates (a search may stop early, a
// replay may run past the last object), and a bar drawn wider than its own box reads as a bug in
// the thing being measured.
inline int barLine(char* out, size_t cap, const char* label,
                   double done, double total, const char* detail) {
    if (cap == 0) return 0;
    double f = (total > 0.0) ? done / total : 0.0;
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;
    char bar[24];
    const int fill = (int)(f * 20.0 + 0.5);
    for (int i = 0; i < 20; ++i) bar[i] = (i < fill) ? '#' : '.';
    bar[20] = 0;
    const int n = snprintf(out, cap, "%s [%s] %5.1f%%   %s\n", label, bar, f * 100.0, detail);
    if (n < 0) return 0;
    return (n < (int)cap) ? n : (int)cap - 1;
}

inline void fillSessionHud(cocos2d::CCLabelBMFont* hud) {
    using namespace cocos2d;
    if (!hud) return;
    // setString rebuilds the glyph sprites, so every frame is heavy (drops the spectating FPS).
    // Refreshing the display every 0.25 seconds is enough
    static auto s_lastHud = std::chrono::steady_clock::time_point{};
    auto nowHud = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(nowHud - s_lastHud).count() < 250
        && *hud->getString())
        return;
    s_lastHud = nowHud;
    auto sec = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - solver::g_solveStart).count();
    float len = 0.f;
    if (auto* pl = PlayLayer::get()) len = pl->m_levelLength;
    char coinLine[64] = "";
    if (g_cfg.coinMode) {
        size_t got = 0;
        for (auto pu : solver::g_coinPickupTick) if (pu >= 0) ++got;
        snprintf(coinLine, sizeof(coinLine), "coins %zu/%zu\n", got, solver::g_coins.size());
    }
    float px = 0.f;
    if (auto* pl = PlayLayer::get()) if (pl->m_player1) px = pl->m_player1->getPositionX();
    // In-process solve (the panel's Solve mode): there is no external driver writing hud.txt, so
    // the loop fills the same numbers itself and this block reads them.
    //
    // TWO PROGRESSES, because they answer different questions and conflating them was actively
    // misleading. `level` is how far into the level the run has actually got -- the deepest point
    // a replay reached, so it only ever grows and it is the same yardstick as the game's own
    // percentage. `search` is how much of the current search is left. The old single bar was the
    // frontier leader's x over the level, which reached 100% without the search ending (getting
    // to the end of the level is not the same as proving a route through it) and walked backwards
    // whenever the frontier was pruned or a tail started from a re-anchor further back.
    if (showingSolve() && g_cfg.dpSolve) {
        const dpbridge::SolveProgress pr = dpbridge::progress();
        const double lvl = (len > 1.f) ? (double)len : 0.0;
        char b[480];
        int n = snprintf(b, sizeof(b), "iter %d   %s   %llds\n%s",
                         g_hudIter, g_hudPhase.empty() ? "starting" : g_hudPhase.c_str(),
                         (long long)sec, coinLine);
        char detail[192];
        snprintf(detail, sizeof(detail), "best x %.0f / %.0f",
                 (double)g_hudVerifiedX, lvl);
        n += barLine(b + n, sizeof(b) - n, "level ", (double)g_hudVerifiedX, lvl, detail);
        if (pr.running) {
            // Ticks, not x: the span is exactly the work the search will do, so the bar is
            // monotone and 100% coincides with the search ending. The frontier's x is still
            // shown, as a number, because that is where the plan being built has reached
            snprintf(detail, sizeof(detail), "tick %lld / %lld   x %.0f   states %zu",
                     pr.tick, pr.horizon, pr.x, pr.alive);
            n += barLine(b + n, sizeof(b) - n, "search",
                         (double)(pr.tick - pr.from), (double)(pr.horizon - pr.from), detail);
        } else {
            snprintf(b + n, sizeof(b) - n, "now    x %.0f  (%.1f%%)   anchor t=%lld x=%.0f",
                     (double)px, lvl > 0.0 ? px / lvl * 100.0 : 0.0,
                     (long long)g_hudAnchorT, (double)g_hudAnchorX);
        }
        hud->setString(b);
        return;
    }
    // Progress is "how far the GD-verified prefix has grown". The numbers are written by the
    // external driver to data/hud.txt; this only displays them
    const double nowPct = (len > 1.f) ? (px / len * 100.0) : 0.0;
    const double vPct = (len > 1.f && g_hudVerifiedX > 0)
                      ? (g_hudVerifiedX / len * 100.0) : 0.0;
    // Bar of the verified prefix (20 characters)
    char bar[24];
    const int fill = (int)(vPct / 5.0 + 0.5);
    for (int i = 0; i < 20; ++i) bar[i] = (i < fill) ? '#' : '.';
    bar[20] = 0;
    char buf[512];
    snprintf(buf, sizeof(buf),
        "iter %d   %s\n"
        "%s"
        "[%s] %.1f%%\n"
        "verified  x=%.0f / %.0f\n"
        "anchor    t=%lld  x=%.0f   fixups %d/%d\n"
        "now       x=%.0f  (%.1f%%)   %llds",
        g_hudIter, g_hudPhase.empty() ? "starting" : g_hudPhase.c_str(),
        coinLine,
        bar, vPct,
        (double)g_hudVerifiedX, (double)len,
        (long long)g_hudAnchorT, (double)g_hudAnchorX,
        g_hudFixups, g_hudFixupCap,
        (double)px, nowPct, (long long)sec);
    hud->setString(buf);
}

// One layout pass over every overlay: a single column down the top-left, in draw order.
// Called once per frame from the game layer's update.
inline void updateOverlays(cocos2d::CCNode* gameLayer) {
    using namespace cocos2d;
    CCNode* parent = gameLayer ? gameLayer->getParent() : nullptr;
    if (!parent) return;
    OverlayCursor cur{CCDirector::sharedDirector()->getWinSize().height - 4.f};
    // 1. the bot badge -- neither F1 nor cfg hud=0 reaches it (spec 9)
    fillBotBadge(overlayLabel(parent, BADGE_TAG, "bigFont.fnt", 0.35f, botDriving(), cur));
    const bool show = g_hudOn && !g_overlayHidden;
    // 2. what the solver is doing (solve sessions only)
    //    Visible for the whole solve, not only while the search thread runs: the loop's
    //    verification replays are part of the solve, and a block that vanished for each one made
    //    the overlay flicker between two layouts once per iteration. It goes away for the
    //    finale, which is a replay and shows only the badge, the keys and the speed.
    //
    //    It also stays up after the session ends. A solve that gave up leaves the level standing
    //    at the point it stopped, and the one thing worth reading then is how far it got.
    fillSessionHud(overlayLabel(parent, HUD_TAG, "chatFont.fnt", 0.6f,
                                show && showingSolve(), cur));
    // 3. the keys, the current speed and whether time is stopped. Only while the mod is
    //    driving: during ordinary play these keys do nothing, and a legend for them on screen
    //    reads as "the mod is running something", which is exactly the wrong impression
    fillKeysHud(overlayLabel(parent, KEYS_TAG, "chatFont.fnt", 0.5f,
                             show && botDriving(), cur));
}
