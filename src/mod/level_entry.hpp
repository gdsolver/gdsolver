#pragma once
// Entering a level the mod names itself: auto-enter, the level suite and the mid-session
// swap. A level the player picks goes through the game's own play button and the play menu
// (hooks_playmenu.cpp).
#include "mod/gd_compat.hpp"
#include "mod/helpers_game.hpp"
#include "mod/level_warnings.hpp"
#include "mod/window_guard.hpp"

using namespace p1;

// The level's own two-player setting, header key kA10 (LevelSettingsObject), read the way the play
// menu's red note reads it (levelwarn::scan). GJGameLevel::m_twoPlayerMode is the online listing's
// flag, which a level built from a file never carries: it printed 0 for a kA10=1 level. -1 when the
// string is not at hand.
inline int levelHeaderTwoPlayer(std::string s) {
    s = gdcompat::rawLevelString(std::move(s));
    if (s.empty()) return -1;
    const std::string_view all(s);
    return levelwarn::field(all.substr(0, all.find(';')), "kA10") == "1" ? 1 : 0;
}

// The level's kA42 (LevelSettingsObject::m_reverseSync), from the header the same way. PlayerObject::
// reversePlayer books the mirror debt only with it set (GJBaseGameLayer::resetLevelVariables copies it to
// the player). The member itself is not read: in 2.209 the bindings' offsets for it and the player's copy
// both read 0 on a level whose header says 1 and whose reverse ring GD does carry (dx x1.02 after it).
// -1 when the string is not at hand.
inline int levelHeaderReverseSync(std::string s) {
    s = gdcompat::rawLevelString(std::move(s));
    if (s.empty()) return -1;
    const std::string_view all(s);
    return levelwarn::field(all.substr(0, all.find(';')), "kA42") == "1" ? 1 : 0;
}

// Whether the level's attempt-dependent triggers reach play (levelwarn::kAttemptDependent), read from
// the stored or raw string the way the play menu's note reads it.
inline bool levelAttemptDependent(std::string s) {
    s = gdcompat::rawLevelString(std::move(s));
    if (s.empty()) return false;
    const std::string_view all(s);
    const bool platformer = levelwarn::field(all.substr(0, all.find(';')), "kA22") == "1";
    return (levelwarn::scan(all, platformer).unmodelled & levelwarn::kAttemptDependent) != 0;
}

// cfg attemptauto for a level the play menu started: the same decision enterConfiguredLevel makes for a
// level the mod names itself, from the level object the game is entering. Called after the session is
// configured (its result file is open).
inline void attemptAutoForLevel(GJGameLevel* level) {
    g_attemptAutoCount = -1;
    if (!level || !g_attemptAuto || g_attemptOne || g_attemptCount >= 0) return;
    const std::string str = level->m_levelType == GJLevelType::Main
        ? std::string(LocalLevelManager::sharedState()->getMainLevelString(level->m_levelID.value()))
        : std::string(level->m_levelString);
    if (!levelAttemptDependent(str)) return;
    g_attemptAutoCount = 1;
    writeResult("attemptauto: the level reads the attempt count or keeps items across attempts "
                "into play; driven as attempt 1 would be a later one (attemptcount=1)");
}

// ---- where a suite level comes from (cfg `leveldir`, see suite:: in config.hpp) ----
//
// Sets g_cfg.levelFile for the suite's current level: `<dir>/<id>.lvl` for an ID that is not
// a main level when a directory is set, nothing otherwise. The first call checks the whole
// list -- strictly ascending, and a file for every ID outside 1-22 -- so a missing level stops
// the run before its first level instead of in the middle. Returns false (and says why) when
// the suite must not run.
inline bool suiteLevelSource() {
    g_cfg.levelFile.clear();
    const std::string& dir = suite::g_levelDir;
    if (dir.empty()) return true;
    auto fileOf = [&](int id) { return dir + "/" + std::to_string(id) + ".lvl"; };
    if (!suite::g_levelDirChecked) {
        suite::g_levelDirChecked = true;
        for (size_t i = 1; i < suite::g_levels.size(); ++i)
            if (suite::g_levels[i] <= suite::g_levels[i - 1]) {
                writeResult("suite: refused - with leveldir the levels must be strictly ascending (a "
                            "fixed order, no duplicates); " + std::to_string(suite::g_levels[i])
                            + " follows " + std::to_string(suite::g_levels[i - 1]));
                return false;
            }
        std::string missing;
        for (int id : suite::g_levels) {
            if (id >= 1 && id <= 22) continue;
            std::error_code ec;
            if (!std::filesystem::is_regular_file(fileOf(id), ec))
                missing += (missing.empty() ? "" : ",") + std::to_string(id);
        }
        if (!missing.empty()) {
            writeResult("suite: refused - no level file in leveldir for " + missing
                        + " (nothing else is read for those IDs)");
            return false;
        }
    }
    const int id = suite::current();
    if (id < 1 || id > 22) g_cfg.levelFile = fileOf(id);
    return true;
}

// FNV-1a of a level's bytes, for the `levelsource:` line: which level a run actually played.
inline std::string levelContentSig(const std::string& bytes) {
    uint64_t h = 1469598103934665603ULL;
    for (char c : bytes) {
        h ^= (uint8_t)c;
        h *= 1099511628211ULL;
    }
    char b[24];
    snprintf(b, sizeof(b), "%016llx", (unsigned long long)h);
    return b;
}

// ---- entering the level g_cfg names ----------------------------------------
//
// The body of MenuLayer's auto-enter, lifted out so that the level suite can use the SAME
// path rather than a second one that only looks like it. Everything it needs is in g_cfg, and
// that is the point: the second level of a suite is entered by the code that enters the first.
//
// Returns false when the level could not be found or built; the caller decides what that
// means (the suite steps past it, auto-enter just stops).
inline bool enterConfiguredLevel() {
    if (g_started) return false;
    g_started = true;
    // The elapsed-time origin is set only on the first entry of the session (resetting it on
    // re-entry would under-report the elapsed seconds)
    if (solver::g_totalAttempts == 0)
        solver::g_solveStart = std::chrono::steady_clock::now();
    // A suite level is the suite's current ID and nothing else (see suiteLevelSource).
    if (suite::active() && g_cfg.levelId != suite::current()) {
        writeResult("suite: refused - about to enter level " + std::to_string(g_cfg.levelId)
                    + " where the suite is at " + std::to_string(suite::current()));
        return false;
    }
    // Main levels are 1-22, and the official platformer levels 5001-5004 (the game's own resources,
    // no download); anything else is a saved / online-cached custom level
    GJGameLevel* level = nullptr;
    auto* glm = GameLevelManager::sharedState();
    std::string srcKind, srcBytes;   // for the `levelsource:` line below
    // If levelfile= is given, build the level from the RAW level string in that file
    // (the save file is never touched). Used for calibration maps.
    if (!g_cfg.levelFile.empty()) {
        std::ifstream lf(g_cfg.levelFile, std::ios::binary);
        if (!lf.is_open()) {
            log::error("phase1: cannot open levelfile {}", g_cfg.levelFile);
            writeResult("error: levelfile not found");
            return false;
        }
        std::string raw((std::istreambuf_iterator<char>(lf)),
                        std::istreambuf_iterator<char>());
        level = GJGameLevel::create();
        level->m_levelName = "gdsolver calib";
        level->m_levelID = g_cfg.levelId;
        level->m_levelType = g_cfg.levelFileMain ? GJLevelType::Main : GJLevelType::Editor;
        // Run it through GD's own compression. Doing base64+gzip by hand here silently
        // fails to load because of encoding differences, so ALWAYS hand it to ZipUtils.
        level->m_levelString = gdcompat::compressLevelString(raw);
        log::info("phase1: built the level from levelfile {} ({} chars)",
                  g_cfg.levelFile, raw.size());
        srcKind = "file";
        srcBytes = raw;
    } else if ((g_cfg.levelId >= 1 && g_cfg.levelId <= 22) || (g_cfg.levelId >= 5001 && g_cfg.levelId <= 5004)) {
        level = glm->getMainLevel(g_cfg.levelId, false);
        srcKind = "main";
    } else {
        level = glm->getSavedLevel(g_cfg.levelId);
        if (!level && glm->m_onlineLevels)
            level = static_cast<GJGameLevel*>(
                glm->m_onlineLevels->objectForKey(std::to_string(g_cfg.levelId)));
        srcKind = "saved";
    }
    if (!level) {
        log::error("phase1: level {} not found (main or saved)", g_cfg.levelId);
        writeResult("error: level not found");
        return false;
    }
    // Which level this run played, by content: the raw file for a level file, the level string
    // the game holds otherwise. A run's manifest keeps it (py/cold_manifest.py).
    if (srcKind != "file") srcBytes = std::string(level->m_levelString);
    solver::g_levelReverseSync = levelHeaderReverseSync(srcKind == "main"
        ? std::string(LocalLevelManager::sharedState()->getMainLevelString(g_cfg.levelId))
        : srcBytes);
    writeResult("levelsource: id=" + std::to_string(g_cfg.levelId) + " kind=" + srcKind
                + " sig=" + levelContentSig(srcBytes) + " bytes=" + std::to_string(srcBytes.size()));
    // cfg attemptauto: decided afresh at every entry, so a pin never outlives its level.
    g_attemptAutoCount = -1;
    if (g_attemptAuto && !g_attemptOne && g_attemptCount < 0) {
        const std::string fullStr = srcKind == "main"
            ? std::string(LocalLevelManager::sharedState()->getMainLevelString(g_cfg.levelId))
            : srcBytes;
        if (levelAttemptDependent(fullStr)) {
            g_attemptAutoCount = 1;
            writeResult("attemptauto: the level reads the attempt count or keeps items across attempts "
                        "into play; driven as attempt 1 would be a later one (attemptcount=1)");
        }
    }
    // Line for ruling out the suspicion that the calibration rig (levelfile=) and the
    // official levels have DIFFERENT physics. The rig's gravity-flipped ship climbed at
    // 0.069/tick while the same condition in lv7 gave 0.103. Compares the defaults of
    // GJGameLevel::create() against the values from getMainLevel.
    // twoPlayer is the level's header kA10 (levelHeaderTwoPlayer); listTwoPlayer the listing's flag.
    writeResult(fmt::format(
        "levelinfo: id={} type={} levelVersion={} gameVersion={} "
        "twoPlayer={} listTwoPlayer={} objCount={}",
        g_cfg.levelId, (int)level->m_levelType, level->m_levelVersion,
        level->m_gameVersion,
        levelHeaderTwoPlayer(srcKind == "main"
                                 ? std::string(LocalLevelManager::sharedState()->getMainLevelString(
                                       g_cfg.levelId))
                                 : srcBytes),
        (int)level->m_twoPlayerMode, (int)level->m_objectCount).c_str());
    log::info("phase1: entering level {}", g_cfg.levelId);
    // the window back at its launch size before the level loads (window_guard.hpp)
    if (g_cfg.windowGuard) windowguard::enforce("level entry", true, [](const std::string& s) { writeResult(s); });
    g_forceCleanStart = true;
    auto* scene = PlayLayer::scene(level, false, false);
    CCDirector::sharedDirector()->replaceScene(CCTransitionFade::create(0.5f, scene));
    return true;
}

// ---- swapping the level mid-session (cmd `swaplevel <path>`) ----------------
//
// The same construction as the levelfile= branch above, minus the fade: the
// level a section solve wants is not the level the loop around it is playing.
// A clone cut down to what the window can reach restores two to three times
// faster, and the restore is 86-89% of a section search.
//
// WHAT THE SWAP DOES NOT CARRY: the new PlayLayer starts the level from the
// top. Every group, toggle, counter and moved object is back at its load state,
// and so is the attempt counter -- a caller that wants the world as it stood at
// tick T has to replay to T again, which is what the section handoff already
// does with practiceat / checkpointat.
//
// Called on the frame AFTER the request (queueInMainThread): the poll that
// reads the command runs inside GJBaseGameLayer::update, and replacing the
// scene from there tears down the layer that call is standing in.
inline bool swapLevelNow(const std::string& path) {
    std::ifstream lf(path, std::ios::binary);
    if (!lf.is_open()) {
        writeResult("swaplevel: cannot open " + path);
        return false;
    }
    std::string raw((std::istreambuf_iterator<char>(lf)), std::istreambuf_iterator<char>());
    auto* level = GJGameLevel::create();
    level->m_levelName = "gdsolver swap";
    level->m_levelID = g_cfg.levelId;
    level->m_levelType = g_cfg.levelFileMain ? GJLevelType::Main : GJLevelType::Editor;
    level->m_levelString = gdcompat::compressLevelString(raw);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - g_swapT0).count();
    g_forceCleanStart = true;
    auto* scene = PlayLayer::scene(level, false, false);
    CCDirector::sharedDirector()->replaceScene(scene);
    writeResult("swaplevel: read+compressed " + std::to_string(raw.size()) + " chars in "
                + std::to_string(ms) + " ms; scene replaced");
    return true;
}

// Resident poll that carries a suite from one level to the next. It hangs off the director's
// scheduler because between two levels there is no game layer and no session, so nothing
// else in the mod is running.
//
// The wait is the whole of it. endSession asks PlayLayer to quit, GD fades to the level-select
// screen, and a level entered before that fade finishes replaces a scene that is still being
// replaced. So: no PlayLayer, not inside a CCTransitionScene, and then a settling margin
// longer than the 0.5 s fade before the next level goes in.
class SuiteKeeper : public cocos2d::CCObject {
public:
    // Ticks of the 0.1 s poll to wait after the scene looks clear. 1 s, i.e. twice the fade --
    // this runs 21 times in a full sweep, so it is 21 seconds spent buying the margin.
    static constexpr int kSettleTicks = 10;
    // ...and the bound on the wait itself. If the level never lets go, the suite must fail
    // where it can be seen: a keeper that waits forever is a silent hang, and the reader
    // outside sees only a game that stopped writing -- the least informative failure there is.
    static constexpr int kGiveUpTicks = 600;   // 60 s

    void tick(float) {
        using namespace cocos2d;
        if (!suite::g_advance) return;
        auto* scene = CCDirector::sharedDirector()->getRunningScene();
        const bool clear = PlayLayer::get() == nullptr && scene
                           && !geode::cast::typeinfo_cast<CCTransitionScene*>(scene);
        // The two counters are separate on purpose: one bounds the whole wait, the other is the
        // margin AFTER the scene became enterable. Sharing one made a slow exit eat the margin.
        ++suite::g_waited;
        if (!clear) {
            suite::g_settle = 0;
            if (suite::g_waited < kGiveUpTicks) return;
            suite::g_advance = false;
            writeResult("suite: gave up waiting to leave the level after 60s (playLayer="
                        + std::string(PlayLayer::get() ? "still up" : "gone")
                        + ") - the remaining levels are not run");
            writeResult("suite: done " + std::to_string(suite::g_levels.size()) + " levels");
            if (g_cfg.quitWhenDone)
                Loader::get()->queueInMainThread([] { gdcompat::exitGame(); });
            return;
        }
        if (++suite::g_settle < kSettleTicks) return;
        suite::g_advance = false;
        suite::g_settle = 0;
        suite::g_waited = 0;
        // Rebuild the configuration from the file rather than patching the outgoing one: the
        // second level of a suite is then configured by the same code, from the same bytes, as
        // the first. (PlayLayer::onQuit has already reset g_cfg to its defaults, so this is a
        // fresh parse and `dparg`'s append cannot double up.)
        g_cfg = Config{};
        loadConfig();
        g_cfg.levelId = suite::current();
        openFiles();                      // endSession closed them
        // Main levels by ID; with cfg leveldir, the others from that directory (never a rig file
        // left in the cfg by `levelfile=`).
        if (!suiteLevelSource()) {
            writeResult("suite: done " + std::to_string(suite::g_levels.size()) + " levels");
            if (g_cfg.quitWhenDone)
                Loader::get()->queueInMainThread([] { gdcompat::exitGame(); });
            return;
        }
        writeResult("suite: level=" + std::to_string(g_cfg.levelId)
                    + " (" + std::to_string(suite::g_at + 1) + "/"
                    + std::to_string(suite::g_levels.size()) + ")");
        // cfg memcensus (print only): a suite's later levels start their session here, not where the
        // first one wrote `session_start`
        dpsolve::memCensus("session start");
        updateWindowTitle();
        if (enterConfiguredLevel()) return;
        // It could not be entered. Do not stall the sweep on it -- report it and take the next,
        // or end the run if this was the last.
        writeResult("suite: level=" + std::to_string(g_cfg.levelId)
                    + " could not be entered - skipped");
        g_started = false;
        if (suite::hasNext()) {
            ++suite::g_at;
            suite::g_advance = true;
            return;
        }
        writeResult("suite: done " + std::to_string(suite::g_levels.size()) + " levels");
        if (g_cfg.quitWhenDone)
            Loader::get()->queueInMainThread([] { gdcompat::exitGame(); });
    }
};
