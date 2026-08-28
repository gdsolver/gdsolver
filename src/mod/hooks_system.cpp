// GameManager / FMODAudioEngine / AppDelegate / GameStatsManager hooks: achievement and stat blocking, music, focus.
#include "mod/playlayer_helpers.hpp"

using namespace p1;

// ---- Achievement / statistics blocking ----
// Blocked while the mod is DRIVING the game -- solving or replaying -- and only then
// (spec §9: a design that cannot be used to fake records). A replay is not human play, so
// nothing it does may be recorded; a human playing with the mod merely loaded is playing, so
// everything they do is recorded as usual. [2026-08-23, user decision -- it was unconditional
// before that.]
//
// The gate is `botDriving()`, which asks "is an automated session open", not "which mode is
// this". See the note on it in config.hpp: gating on a list of modes is what leaked last time.
// The blocked counts are printed at the end of every session, so whether it is working can
// always be confirmed
inline bool statBlockActive() { return botDriving(); }

// ---- Level-progress blocking ----
// Achievements and statistics are only half of what a run records. GD writes the LEVEL's own
// record -- the percentage, the attempt count, the rewards -- from PlayLayer::destroyPlayer and
// PlayLayer::levelComplete, through the functions hooked below. Measured on the 2.2081 binary
// with py/gddisasm.py:
//   PlayLayer::destroyPlayer  calls  GJGameLevel::savePercentage,
//                                    GameManager::reportPercentageForLevel
//   PlayLayer::levelComplete  calls  savePercentage, saveNewScore x2,
//                                    reportPercentageForLevel, completedLevel,
//                                    awardCurrencyForLevel, awardDiamondsForLevel,
//                                    awardSecretKey, checkCoinAchievement, commitJumps
// commitJumps goes through GameStatsManager::incrementStat, which is already blocked below.
//
// Both call sites are guarded in GD itself by `m_isTestMode` (GJBaseGameLayer +0x3230): when it
// is set, GD skips the whole recording block. The mod raises that flag around the two originals
// (NoRecordGuard in playlayer_helpers.hpp), which covers every path inside them -- including any
// this list has missed. The hooks here are the second layer and the audit: if one of them ever
// counts, a recording path escaped the flag, and the session-end line says so.
//
// `progressBlockActive()` (config.hpp) is the same gate as statBlockActive plus a switch, and
// the switch exists only so that the session-end audit can be shown to be non-vacuous: with cfg
// `progblock=0` the same run reports the fields GD wrote.

class $modify(GJGameLevel) {
    void savePercentage(int percent, bool isPracticeMode, int clicks, int attempts,
                        bool isChkValid) {
        if (progressBlockActive()) { ++g_blockedProgress; return; }
        GJGameLevel::savePercentage(percent, isPracticeMode, clicks, attempts, isChkValid);
    }
    void saveNewScore(int value, int type, int ticks, int clicks, int coins,
                      gd::string inputs, bool save) {
        if (progressBlockActive()) { ++g_blockedProgress; return; }
        GJGameLevel::saveNewScore(value, type, ticks, clicks, coins, inputs, save);
    }
};

class $modify(GameManager) {
    // The percentage GD records for the level (and reports for the leaderboard).
    void reportPercentageForLevel(int levelID, int percentage, bool isPlatformer) {
        if (progressBlockActive()) { ++g_blockedProgress; return; }
        GameManager::reportPercentageForLevel(levelID, percentage, isPlatformer);
    }
    void reportAchievementWithID(char const* key, int percent, bool dontNotify) {
        if (statBlockActive()) {
            ++g_blockedAch;
            // Log only the first time: GD re-reports every still-unachieved achievement on
            // each statistics update, so logging every time floods the log and can freeze
            static std::set<std::string> s_seen;
            if (key && s_seen.insert(key).second)
                log::info("achievement blocked during solve session: {}", key);
            return;
        }
        GameManager::reportAchievementWithID(key, percent, dontNotify);
    }
};

// ---- Audio while the mod is driving ----
// A solving session makes no sound at all: it runs hundreds of attempts a minute with the
// physics tens of times faster than the song, so every note and every death effect is in the
// wrong place. The stop side is never blocked -- letting a stop through is always safe, and
// blocking one is how a track gets left playing over a silent session.
// A replay is left audible and is kept in step with the game in audio::sync (helpers_game.hpp).
class $modify(FMODAudioEngine) {
    // Count what got through while the session was still solving, and name the first few. A
    // report of "it still makes a sound while solving" is otherwise unactionable: silent() lets
    // the song back in on purpose when the screen goes on (the note there), so the question is
    // always WHICH call, from which phase -- and a leak that only happens in one phase is
    // invisible to anything but a tally that outlives it.
    static void notedSound(const char* what, const gd::string& path) {
        if (!solveSession() || g_dpShowSolution) return;
        if (++g_soundWhileSolving <= 5)
            writeResult(std::string("audio: ") + what + " played while solving: "
                        + std::string(path.c_str())
                        + " (screen=" + (renderingOn() ? "on" : "off")
                        + " fast=" + (fastModeActive() ? "1" : "0") + ")");
    }
    // ...and the other half of the tally: what was ASKED FOR and refused.
    //
    // Reported 2026-08-28 -- a solve made a sound with the screen off, and the passed-tally above
    // was empty, which says the sound did not come through any of these three. That is only
    // actionable if the refused side is counted too: if GD is calling them and being refused, the
    // sound has another way out of the engine; if it is not calling them at all, these hooks are
    // not the ones that matter for it.
    static void notedBlocked(const char* what, const gd::string& path) {
        if (!solveSession() || g_dpShowSolution) return;
        if (++g_soundBlockedWhileSolving <= 5)
            writeResult(std::string("audio: ") + what + " REFUSED while solving: "
                        + std::string(path.c_str()));
    }
    void playMusic(gd::string path, bool shouldLoop, float fadeInTime, int channel) {
        if (audio::silent()) { notedBlocked("music", path); return; }
        // A seek restarts the level, and the restart starts the song from the top. Hold the call
        // instead of making it; audio::sync plays it when the seek lands, seeked to where the
        // game arrived. Muting or pausing after the fact is a frame too late -- that frame is the
        // blip of the track's opening you hear on every rewind.
        if (audio::effectsMuted()) {
            audio::holdMusic(path, shouldLoop, fadeInTime, channel);
            return;
        }
        notedSound("music", path);
        FMODAudioEngine::playMusic(path, shouldLoop, fadeInTime, channel);
    }
    // THE OTHER DOORS. Measured 2026-08-28: a whole lv21 solve with the screen off made a sound,
    // and the three hooks below it counted two calls in total (both refused). So GD 2.2 does not
    // reach the engine this way for most of what it plays -- it queues. These are the queued and
    // async siblings of the same calls, gated identically. Refusing a call that starts a sound is
    // the same contract the originals have, and outside a driven session both gates are false, so
    // ordinary play is untouched.
    void loadAndPlayMusic(gd::string path, unsigned int time, int musicID) {
        if (audio::silent() || audio::effectsMuted()) { notedBlocked("loadAndPlayMusic", path); return; }
        notedSound("loadAndPlayMusic", path);
        FMODAudioEngine::loadAndPlayMusic(path, time, musicID);
    }
    void queueStartMusic(gd::string path, float pitch, float unk, float volume, bool loop,
                         int start, int end, int fadeIn, int fadeOut, int musicID, bool p10,
                         int channelID, bool noPrepare, bool dontReset) {
        if (audio::silent() || audio::effectsMuted()) { notedBlocked("queueStartMusic", path); return; }
        notedSound("queueStartMusic", path);
        FMODAudioEngine::queueStartMusic(path, pitch, unk, volume, loop, start, end, fadeIn,
                                         fadeOut, musicID, p10, channelID, noPrepare, dontReset);
    }
    int queuePlayEffect(gd::string path, float speed, float unk, float volume, float pitch,
                        bool fft, bool reverb, int start, int end, int fadeIn, int fadeOut,
                        bool loop, int effectID, bool override, int uniqueID, float minInterval,
                        int group) {
        if (audio::silent() || audio::effectsMuted()) { notedBlocked("queuePlayEffect", path); return 0; }
        notedSound("queuePlayEffect", path);
        return FMODAudioEngine::queuePlayEffect(path, speed, unk, volume, pitch, fft, reverb,
                                                start, end, fadeIn, fadeOut, loop, effectID,
                                                override, uniqueID, minInterval, group);
    }
    // (playEffectAsync is inline on Windows and cannot be hooked. It forwards into the queued
    // path above, which is hooked, so nothing is lost by not having it.)
    int playEffect(gd::string path, float speed, float unknown, float volume) {
        if (audio::silent() || audio::effectsMuted()) { notedBlocked("effect", path); return 0; }
        notedSound("effect", path);
        return FMODAudioEngine::playEffect(path, speed, unknown, volume);
    }
    int playEffectAdvanced(gd::string path, float speed, float unknown, float volume,
                           float pitch, bool fft, bool reverb, int startMillis, int endMillis,
                           int fadeIn, int fadeOut, bool loopEnabled, int effectID,
                           bool override, bool noPreload, int channelID, int uniqueID,
                           float minInterval, int sfxGroup) {
        if (audio::silent() || audio::effectsMuted()) {
            notedBlocked("effectAdv", path);
            return 0;
        }
        notedSound("effectAdv", path);
        return FMODAudioEngine::playEffectAdvanced(path, speed, unknown, volume, pitch, fft,
            reverb, startMillis, endMillis, fadeIn, fadeOut, loopEnabled, effectID,
            override, noPreload, channelID, uniqueID, minInterval, sfxGroup);
    }
};

// ---- Do not stop the worker on focus loss ----
// cocos2d-x's applicationDidEnterBackground calls CCDirector::stopAnimation()
// = the whole main loop stops (blocking pauseGame alone is not enough).
// During an automated session the backgrounding itself is ignored. Outside a session it
// passes through untouched
class $modify(AppDelegate) {
    void applicationDidEnterBackground() {
        if (g_started && !g_sessionOver) {
            ++g_bgBlocked;
            if (g_bgBlocked <= 3)
                writeResult("focus: ignored applicationDidEnterBackground (n="
                    + std::to_string(g_bgBlocked) + ")");
            return;
        }
        AppDelegate::applicationDidEnterBackground();
    }
    void applicationWillResignActive() {
        if (g_started && !g_sessionOver) {
            ++g_resignBlocked;
            if (g_resignBlocked <= 3)
                writeResult("focus: ignored applicationWillResignActive (n="
                    + std::to_string(g_resignBlocked) + ")");
            return;
        }
        AppDelegate::applicationWillResignActive();
    }
    // The mod holds the engine's volume at zero while it is driving quietly (audio::hushVolumes),
    // and gives it back when the session ends. If the app goes away first -- a crash, an exit
    // mid-solve -- it has to be given back here, or the player's next launch is silent for a
    // reason nothing on screen explains.
    void trySaveGame(bool p0) {
        audio::restoreVolumes();
        AppDelegate::trySaveGame(p0);
    }
};

class $modify(GameStatsManager) {
    void incrementStat(char const* key, int amount) {
        if (statBlockActive()) {
            ++g_blockedStat;
            static std::set<std::string> s_seen;
            if (key && s_seen.insert(key).second)
                log::info("stat increment blocked during solve session: {} (+{})", key, amount);
            return;
        }
        GameStatsManager::incrementStat(key, amount);
    }
    // Coin persistence is also blocked while solving (bot runs must not earn coin
    // achievements. The solver's pickup detection is its own (coordinate collision), so it
    // does not depend on GD's records)
    void storeSecretCoin(char const* key) {
        if (statBlockActive()) { ++g_blockedCoin; return; }
        GameStatsManager::storeSecretCoin(key);
    }
    void storeUserCoin(char const* key) {
        if (statBlockActive()) { ++g_blockedCoin; return; }
        GameStatsManager::storeUserCoin(key);
    }
    // storePendingUserCoin cannot be hooked because its binding is defined inline.
    // The real work is covered by the block on the storeUserCoin side (pending → store path)

    // ---- the rewards levelComplete hands out ----
    void completedLevel(GJGameLevel* level) {
        if (progressBlockActive()) { ++g_blockedProgress; return; }
        GameStatsManager::completedLevel(level);
    }
    void awardCurrencyForLevel(GJGameLevel* level) {
        if (progressBlockActive()) { ++g_blockedProgress; return; }
        GameStatsManager::awardCurrencyForLevel(level);
    }
    void awardDiamondsForLevel(GJGameLevel* level) {
        if (progressBlockActive()) { ++g_blockedProgress; return; }
        GameStatsManager::awardDiamondsForLevel(level);
    }
    bool awardSecretKey() {
        if (progressBlockActive()) { ++g_blockedProgress; return false; }
        return GameStatsManager::awardSecretKey();
    }
    void checkCoinAchievement(GJGameLevel* level) {
        if (progressBlockActive()) { ++g_blockedProgress; return; }
        GameStatsManager::checkCoinAchievement(level);
    }
};
