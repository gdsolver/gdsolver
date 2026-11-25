// What leaves the process and what reaches the disk while the mod drives (see record_gate.hpp).
//
// The function-level gate keeps the level's record from being written. These are the two doors
// under it, shut for the same reason and on the same predicate (botDriving()): a bot's session
// is not a player's, so nothing it does may be sent to a server or written into a save file --
// whichever of the game's functions ends up asking, including ones this mod has never heard of.
//
// Measured on the 2.209 binary:
//   - every request the game makes goes through CCHttpClient::send (7 callers in the executable;
//     the death-data upload is built in LevelInfoLayer::onEnterTransitionDidFinish and needs a
//     logged-in account);
//   - CCGameManager.dat / CCLocalLevels.dat are written by DS_Dictionary::saveRootSubDictTo-
//     CompressedFile, whose only caller is GManager::save;
//   - CCPlatformerSave.dat (new in 2.209) is written through one atomic writer (RVA 0x736b0),
//     called from PlayLayer::onQuit and the account sync. PlayLayer::onQuit tests whether the
//     level is a platformer, not the test-mode flag, so the guard the record gate raises does
//     not cover it -- this hook does.
#include "mod/playlayer_helpers.hpp"
#include "mod/record_gate.hpp"

#include <cstring>

#include <Geode/modify/CCHttpClient.hpp>
#include <Geode/modify/DS_Dictionary.hpp>
#if GEODE_COMP_GD_VERSION >= 22090 && defined(GEODE_IS_WINDOWS)
#include <Geode/modify/GDFileWriter.hpp>
#endif

using namespace p1;

// ---- network ----
// A refused request is not retained: GD releases its own reference after send, as cocos2d-x
// callers do, and that frees it. Nothing is called back -- a request that never happened has no
// answer -- so the game's own "in flight" bookkeeping for it is left open until the level is left;
// the requests a session can make are few (a network test at the menu, a song download) and a
// player's next launch starts clean.
class $modify(RecordGateHttp, cocos2d::extension::CCHttpClient) {
    GDSOLVER_HOOK_GUARD()
    void send(cocos2d::extension::CCHttpRequest* request) {
        // TEST ONLY: netcatch_testonly=deathdata refuses just the death-data upload
        bool deathUpload = false;
#ifdef GEODE_IS_WINDOWS
        if (g_netCatchTestOnly == 2) {
            const char* u = gate::requestUrl(request);
            deathUpload = u && std::strstr(u, "uploadGJDeathData") != nullptr;
        }
#endif
        if (netBlockActive() || deathUpload) {
            ++g_blockedSend;
#ifdef GEODE_IS_WINDOWS
            gate::noteBlocked(g_netCatchTestOnly && !botDriving() ? "net (netcatch_testonly)" : "net", g_blockedSend,
                              gate::describeRequest(gate::requestUrl(request), gate::requestType(request),
                                                    gate::requestDataSize(request)));
#endif
            return;
        }
#ifdef GEODE_IS_WINDOWS
        // TEST ONLY: nettrace_testonly=1 names the request and, when it comes, the answer (see nettrace in record_gate.hpp)
        if (g_netTraceTestOnly) gate::nettrace::wrap(request);
#endif
        CCHttpClient::send(request);
    }
};

// ---- save files ----
// A refused save reports success: the caller (GManager::save and the like) treats a failed write
// as an error to log and retry, and there is nothing wrong with it. The game keeps its state in
// memory; whatever it holds is written the next time it saves outside a driven session.
class $modify(RecordGateSave, DS_Dictionary) {
    GDSOLVER_HOOK_GUARD()
#if GEODE_COMP_GD_VERSION >= 22090
    bool saveRootSubDictToCompressedFile(char const* path, bool flag) {
        if (saveBlockActive()) {
            ++g_blockedSave;
            gate::noteBlocked("save", g_blockedSave, "DS_Dictionary compressed file " + gate::fileNameOf(path));
            return true;
        }
        return DS_Dictionary::saveRootSubDictToCompressedFile(path, flag);
    }
#else
    bool saveRootSubDictToCompressedFile(char const* path) {
        if (saveBlockActive()) {
            ++g_blockedSave;
            gate::noteBlocked("save", g_blockedSave, "DS_Dictionary compressed file " + gate::fileNameOf(path));
            return true;
        }
        return DS_Dictionary::saveRootSubDictToCompressedFile(path);
    }
#endif
    bool saveRootSubDictToFile(char const* path) {
        if (saveBlockActive()) {
            ++g_blockedSave;
            gate::noteBlocked("save", g_blockedSave, "DS_Dictionary file " + gate::fileNameOf(path));
            return true;
        }
        return DS_Dictionary::saveRootSubDictToFile(path);
    }
};

#if GEODE_COMP_GD_VERSION >= 22090 && defined(GEODE_IS_WINDOWS)
// The atomic writer under CCPlatformerSave.dat (and whatever else 2.209 writes through it).
class $modify(RecordGateWriter, GDFileWriter) {
    GDSOLVER_HOOK_GUARD()
    static bool writeAtomic(std::string const& path, std::string const& data) {
        if (saveBlockActive()) {
            ++g_blockedSave;
            gate::noteBlocked("save", g_blockedSave,
                              "atomic writer " + gate::fileNameOf(path.c_str()) + " (" + std::to_string(data.size()) + " B)");
            return true;
        }
        return GDFileWriter::writeAtomic(path, data);
    }
};
#endif
