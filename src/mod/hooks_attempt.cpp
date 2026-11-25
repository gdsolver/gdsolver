// The attempt pin (cfg `attemptone`, see g_attemptOne in config.hpp): while the mod drives, the
// attempt count reads what it reads on a session's first attempt, and the items Item Persistence
// keeps are not carried into the next attempt. cfg `attemptcount=k` (g_attemptCount) pins the count
// alone to k on every attempt. cfg `attemptauto` (on) does the count part for a level the scan finds
// attempt-dependent and, where the kept items reach play too (g_attemptAutoKeeps, T58), the items part as well.
// Windows only: the addresses and the two callers of getItemValue were read from this build.
#include "mod/playlayer_helpers.hpp"
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/GJEffectManager.hpp>

using namespace p1;

#ifdef GEODE_IS_WINDOWS

// What the attempt count reads on a session's first attempt. The layer counts the attempts BEFORE
// the current one: measured on a custom level whose Item Compare at x=0 reads it, attempts 1 to 6
// of one session read 0 to 5. Answering 1 instead turned the first attempt itself into a later
// one (its compare "> 0" came out true and the player was teleported on tick 3).
constexpr double kFirstAttemptCount = 0.0;

class $modify(AttemptPinLayer, GJBaseGameLayer) {
    GDSOLVER_HOOK_GUARD()
    // Type 5 is the attempt count (+0x3084 on the layer); every other type is the game's.
    double getItemValue(int type, int id) {
        const double v = GJBaseGameLayer::getItemValue(type, id);
        if (type != 5 || !botDriving()) return v;
        // The first reads of a session say what the game itself had (and on which attempt), so
        // what the pin changed can be read back.
        if (g_attemptReadsSeen++ < 20)
            writeResult("attemptone: attempt count read on attempt " + std::to_string(g_attempt)
                        + ": the game has " + std::to_string((long long)v)
                        + (g_attemptOne ? ", answered as on the first attempt"
                           : g_attemptCount >= 0 ? ", answered as " + std::to_string(g_attemptCount)
                                                     + " (cfg attemptcount)"
                           : g_attemptAutoCount >= 0 ? ", answered as " + std::to_string(g_attemptAutoCount)
                                                     + " (attemptauto)"
                                                 : std::string(", left as it is")));
        if (g_attemptOne) {
            ++g_attemptReadsPinned;
            return kFirstAttemptCount;
        }
        // cfg attemptcount (config.hpp): the same count on every attempt; cfg attemptauto picks one
        // for a level the scan finds attempt-dependent (level_entry.hpp)
        const long long pin = g_attemptCount >= 0 ? g_attemptCount : g_attemptAutoCount;
        if (pin >= 0) {
            ++g_attemptReadsPinned;
            return (double)pin;
        }
        return v;
    }
};

class $modify(AttemptPinEffects, GJEffectManager) {
    GDSOLVER_HOOK_GUARD()
    // Called by resetEffects (every attempt) and loadFromState (every checkpoint restore) to copy
    // the kept items back. Not copying them leaves those items where a first attempt has them.
    // cfg itemlog (observation only): what the transfer meets and leaves, sorted item=value, from the first 200 calls that see something
    template <class M> static std::string itemsText(const M& m) {
        std::map<int, int> s;
        for (const auto& kv : m) s[(int)kv.first] = (int)kv.second;
        std::string t; int n = 0;
        for (const auto& kv : s) { if (n++ >= 12) { t += " ..."; break; } t += " " + std::to_string(kv.first) + "=" + std::to_string(kv.second); }
        return t.empty() ? " -" : t;
    }
    void transferPersistentItems() {
        if (g_itemLog && botDriving()) {
            const bool pin = attemptPinActive();
            const std::string kept = itemsText(m_persistentItemCountMap), before = itemsText(m_itemCountMap);
            if (g_itemLogLines < 200 && (kept != " -" || before != " -")) {
                ++g_itemLogLines;
                // the live map is read after the (skipped or real) transfer below
                if (pin) ++g_persistTransfersSkipped;
                else GJEffectManager::transferPersistentItems();
                writeResult(std::string("itemlog: transfer pin ") + (pin ? "on" : "off") + " | kept:" + kept + " | live before:" + before + " | live after:" + itemsText(m_itemCountMap));
                return;
            }
        }
        if (attemptPinActive()) {
            // the first few skips say what was NOT carried (the kept table holds the values the previous attempt left: item=value)
            if (g_persistTransfersSkipped < 5) {
                std::string kept;
                int n = 0;
                for (const auto& kv : m_persistentItemCountMap) {
                    if (n++ >= 8) { kept += " ..."; break; }
                    kept += " " + std::to_string(kv.first) + "=" + std::to_string(kv.second);
                }
                writeResult("attemptpin: kept items not carried over (" + std::to_string(m_persistentItemCountMap.size()) + " kept:" + kept + ")");
            }
            ++g_persistTransfersSkipped;
            return;
        }
        GJEffectManager::transferPersistentItems();
    }
};

#endif
