#pragma once
// The layers under the function-level record gate.
//
// The function-level gate (NoRecordGuard, the hooks in hooks_system.cpp, restoreProgress) stops the
// recording paths the game is KNOWN to have. This file is what holds when a path is not known:
//
//   network   every request GD hands to its HTTP client is refused while the mod drives, and named
//             (hooks_record_gate.cpp). `nettest` shows that layer catches a real call.
//   save      every write to a save file is refused while the mod drives (same file).
//   memory    the game's own save strings -- what it would write -- are taken when a session opens
//             and compared when it ends and again when the level has been left. A record that
//             reached memory shows up there whichever function put it, including a record kept
//             under a key the gate has never heard of (a new game version adds them).
//
// The memory audit is the one that does not depend on knowing the game: it reads the same
// serialisation the save file is made from, and diffs it key by key.
#include "mod/config.hpp"
#include "mod/gd_compat.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <unordered_map>
#include <string>
#include <vector>

#ifdef GEODE_IS_WINDOWS
#include <cocos-ext.h>
#if GEODE_COMP_GD_VERSION >= 22090
#include <Geode/binding/GDFileWriter.hpp>
#endif
#endif

// Comes after config.hpp (g_cfg, the counters, writeResult) in every include chain.
namespace p1 {
namespace gate {

// ---- the request's own fields, read through the DLL's exported getters ------------------------
//
// The accessors are inline in the SDK's header, so the mod would read the request at the offsets
// the SDK was written for. The DLL exports the same getters on both 2.2081 and 2.209 (the mangled
// names are identical; the layouts were compared: type +0x38, url +0x40, data +0x60), and going
// through them means a different layout cannot make this file read the wrong bytes.
#ifdef GEODE_IS_WINDOWS
inline void* extensionsProc(const char* name) {
    HMODULE m = GetModuleHandleA("libExtensions.dll");
    return m ? reinterpret_cast<void*>(GetProcAddress(m, name)) : nullptr;
}
inline const char* requestUrl(void* req) {
    using Fn = const char* (*)(void*);
    static Fn fn = reinterpret_cast<Fn>(extensionsProc("?getUrl@CCHttpRequest@extension@cocos2d@@QEAAPEBDXZ"));
    return fn && req ? fn(req) : nullptr;
}
inline int requestType(void* req) {
    using Fn = int (*)(void*);
    static Fn fn = reinterpret_cast<Fn>(
        extensionsProc("?getRequestType@CCHttpRequest@extension@cocos2d@@QEAA?AW4HttpRequestType@123@XZ"));
    return fn && req ? fn(req) : -1;
}
inline int requestDataSize(void* req) {
    using Fn = int (*)(void*);
    static Fn fn = reinterpret_cast<Fn>(extensionsProc("?getRequestDataSize@CCHttpRequest@extension@cocos2d@@QEAAHXZ"));
    return fn && req ? fn(req) : -1;
}
#endif

// What a request is worth saying about: the method, the host and the path -- not the query, which
// can carry the thing being sent, and not the body, of which only the size is kept.
inline std::string describeRequest(const char* url, int type, int dataSize) {
    std::string u = url ? url : "(unreadable)";
    const size_t q = u.find('?');
    if (q != std::string::npos) u = u.substr(0, q) + "?...";
    const char* m = type == 0 ? "GET" : type == 1 ? "POST" : type == 2 ? "PUT" : type == 3 ? "DELETE" : "?";
    return std::string(m) + " " + u + (dataSize > 0 ? " (body " + std::to_string(dataSize) + " B)" : "");
}

// The last path component, for a save file: the directory has the user's name in it.
inline std::string fileNameOf(const char* path) {
    std::string p = path ? path : "(null)";
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

// Say what was refused, for the first few per session and as a count after that.
inline void noteBlocked(const char* what, long long n, const std::string& detail) {
    if (n <= 8) writeResult(std::string(what) + ": refused: " + detail, false);
}

// ---- the net test --------------------------------------------------------------------------
//
// cfg `nettest=1`: the session sends ONE request through CCHttpClient::send, to port 9 of this
// machine (the discard port: nothing listens, the connection is refused, and nothing leaves the
// machine). With the layer on, the request must be refused -- counted, never sent, no answer. With
// `netblock=0` it goes out and the refusal comes back as an answer, which is what shows the call
// path works and that it was the gate, and not a broken request, that stopped it.
#ifdef GEODE_IS_WINDOWS
namespace nettest {
inline long long g_answers = 0;
struct Sink : cocos2d::CCObject {
    void onResponse(cocos2d::extension::CCHttpClient*, cocos2d::extension::CCHttpResponse* r) {
        ++g_answers;
        writeResult(std::string("nettest: an answer came back (succeeded=") + (r && r->isSucceed() ? "1" : "0")
                    + " code=" + std::to_string(r ? r->getResponseCode() : -1) + "): the request was NOT refused", false);
    }
};
inline void run() {
    static Sink* sink = nullptr;
    if (!sink) sink = new Sink();   // retained for the life of the process: the answer can come at any time
    auto* req = new cocos2d::extension::CCHttpRequest();
    req->setUrl("http://127.0.0.1:9/gdsolver-nettest");
    req->setRequestType(cocos2d::extension::CCHttpRequest::kHttpGet);
    req->setTag("gdsolver-nettest");
    req->setResponseCallback(sink, httpresponse_selector(Sink::onResponse));
    const long long before = g_blockedSend;
    cocos2d::extension::CCHttpClient::getInstance()->send(req);
    req->release();
    writeResult(std::string("nettest: one request to 127.0.0.1:9 through CCHttpClient::send; refused by the gate: ")
                    + (g_blockedSend > before ? "yes" : "NO (it was sent)") + " (netblock="
                    + (g_cfg.netBlock ? "1" : "0") + ")", false);
}
}  // namespace nettest
#endif

// ---- the net trace (TEST ONLY) -------------------------------------------------------------
//
// cfg `nettrace_testonly=1`: every request that is NOT refused is named when it is sent, and what came back is named when it arrives: the method, host
// and path, the body size; then whether the transfer succeeded, the HTTP code, the answer's length, curl's error text when it failed, and the
// answer's value when it is a bare number (a server's -1 / -12 style refusal). Never the query, the request body or the answer's text: those
// can hold credentials. It exists to tell "the server said no" from "the game's code went wrong" when a request does not do what is expected
// (a login that fails), and to do it without a debugger on the player's machine. Off unless that file says so.
//
// The request's response callback is swapped for ours, which records and then calls the original, with the same arguments the game would have
// given it. The accessors are the DLL's exports (same on 2.2081 and 2.209).
#ifdef GEODE_IS_WINDOWS
namespace nettrace {
template <class Fn> inline Fn proc(const char* name) { return reinterpret_cast<Fn>(extensionsProc(name)); }
inline const char* kGetTarget = "?getTarget@CCHttpRequest@extension@cocos2d@@QEAAPEAVCCObject@3@XZ";
inline const char* kGetSelector = "?getSelector@CCHttpRequest@extension@cocos2d@@QEAA?AV_prxy@123@XZ";
inline const char* kSetCallback =
    "?setResponseCallback@CCHttpRequest@extension@cocos2d@@QEAAXPEAVCCObject@3@P843@EAAXPEAVCCHttpClient@23@PEAVCCHttpResponse@23@@Z@Z";
inline const char* kGetRequest = "?getHttpRequest@CCHttpResponse@extension@cocos2d@@QEAAPEAVCCHttpRequest@23@XZ";
inline const char* kIsSucceed = "?isSucceed@CCHttpResponse@extension@cocos2d@@QEAA_NXZ";
inline const char* kGetCode = "?getResponseCode@CCHttpResponse@extension@cocos2d@@QEAAJXZ";
inline const char* kGetData = "?getResponseData@CCHttpResponse@extension@cocos2d@@QEAAPEAV?$vector@DV?$allocator@D@std@@@std@@XZ";
inline const char* kGetError = "?getErrorBuffer@CCHttpResponse@extension@cocos2d@@QEAAPEBDXZ";

struct Orig {
    cocos2d::CCObject* target;
    unsigned long long sel;   // the callback's member pointer: one pointer on MSVC x64 for a CCObject-based class
};
inline std::unordered_map<void*, Orig> g_orig;
inline long long g_sent = 0, g_answered = 0;

struct Sink : cocos2d::CCObject {
    void onResponse(cocos2d::extension::CCHttpClient* client, cocos2d::extension::CCHttpResponse* r) {
        ++g_answered;
        auto getReq = proc<void* (*)(void*)>(kGetRequest);
        auto isOk = proc<bool (*)(void*)>(kIsSucceed);
        auto getCode = proc<long (*)(void*)>(kGetCode);
        auto getData = proc<std::vector<char>* (*)(void*)>(kGetData);
        auto getErr = proc<const char* (*)(void*)>(kGetError);
        void* req = r && getReq ? getReq(r) : nullptr;
        std::string line = "nettrace: answer to ";
        line += describeRequest(requestUrl(req), requestType(req), -1);
        if (r && isOk && getCode && getData) {
            const bool ok = isOk(r);
            const std::vector<char>* d = getData(r);
            const size_t n = d ? d->size() : 0;
            line += std::string(": succeeded=") + (ok ? "1" : "0") + " http=" + std::to_string(getCode(r)) + " bytes=" + std::to_string(n);
            // a bare number only (digits and a minus sign, at most 12 characters): a refusal code, never text
            if (d && n > 0 && n <= 12) {
                bool numeric = true;
                for (char c : *d) numeric = numeric && ((c >= '0' && c <= '9') || c == '-');
                if (numeric) line += " value=" + std::string(d->data(), n);
            }
            if (!ok && getErr) {
                const char* e = getErr(r);
                if (e && *e) line += std::string(" error=\"") + std::string(e).substr(0, 120) + "\"";
            }
        } else {
            line += ": (the response accessors are not available)";
        }
        writeResult(line, false);
        auto it = g_orig.find(req);
        if (it != g_orig.end()) {
            const Orig o = it->second;
            g_orig.erase(it);
            if (o.target) {
                cocos2d::extension::SEL_HttpResponse sel;
                std::memcpy(&sel, &o.sel, sizeof(sel));
                (o.target->*sel)(client, r);
            }
        }
    }
};

// Called from the send hook for a request that is going out.
inline void wrap(cocos2d::extension::CCHttpRequest* request) {
    static Sink* sink = nullptr;
    if (!sink) sink = new Sink();   // lives for the process: answers come at any time
    auto getTarget = proc<cocos2d::CCObject* (*)(void*)>(kGetTarget);
    auto getSelector = proc<void* (*)(void*, unsigned long long*)>(kGetSelector);
    auto setCallback = proc<void (*)(void*, cocos2d::CCObject*, cocos2d::extension::SEL_HttpResponse)>(kSetCallback);
    ++g_sent;
    writeResult("nettrace: send " + describeRequest(requestUrl(request), requestType(request), requestDataSize(request)), false);
    if (!getTarget || !getSelector || !setCallback) { writeResult("nettrace: (the request accessors are not available; answers are not traced)", false); return; }
    cocos2d::CCObject* target = getTarget(request);
    if (!target) return;   // nobody asked to be told: nothing to trace
    unsigned long long raw = 0;
    getSelector(request, &raw);
    g_orig[request] = Orig{target, raw};
    setCallback(request, sink, httpresponse_selector(Sink::onResponse));
}
}  // namespace nettrace
#endif

// ---- the death-data test (TEST ONLY) --------------------------------------------------------
//
// cfg `deathtest_testonly=1`. 2.209 builds a death-data upload (uploadGJDeathData) only for a saved/online level the server has flagged for it
// (a flag at level+0x7b8) and at a per-mille rate (level+0x7bc), so playing an arbitrary level, however often it is lost, never builds one -- the
// request the network layer exists to refuse cannot be produced by playing. This sets both on the level being entered (a saved/online level only),
// so that the game builds the upload when the level page is next entered; with `netcatch_testonly=1` it is dropped and named (nothing leaves the
// machine). Both fields are in the saved level record: use a throwaway profile and put it back.
namespace deathtest {
inline void arm(GJGameLevel* level) {
#if GEODE_COMP_GD_VERSION >= 22090 && defined(GEODE_IS_WINDOWS)
    if (!level || (int)level->m_levelType != 3) return;   // GJLevelType::Saved: the game's own test (it skips types 1 and 2)
    auto* p = reinterpret_cast<unsigned char*>(level);
    p[0x7b8] = 1;
    *reinterpret_cast<int*>(p + 0x7bc) = 1000;
    writeResult("deathtest: level " + std::to_string(level->m_levelID.value()) + " flagged for the death-data upload (rate 1000/1000)", false);
#else
    (void)level;
#endif
}
}  // namespace deathtest

// ---- the save test -------------------------------------------------------------------------
//
// cfg `savetest=1`: the session asks the game's own atomic file writer -- the function CCPlatformerSave.dat is written through -- to write one small
// file, gdsolver-savetest.dat, in the save folder. With the layer on, the call is refused (counted, the file does not appear); with `saveblock=0` it
// is the positive control (the file appears, and the inventory in savediff names it). A platformer save is only written for a level that holds a
// record, which a driven session never earns, so this is how the writer's hook is shown to catch a real call.
namespace savetest {
// `atQuit` is the same call made from inside PlayLayer::onQuit, where the session is already torn down and only the leave-of-a-driven-level
// predicate (g_quitDriving) holds: the writer a real platformer save goes through, asked at the moment the real one would be.
inline void run(bool atQuit = false) {
#if GEODE_COMP_GD_VERSION >= 22090 && defined(GEODE_IS_WINDOWS)
    const char* name = atQuit ? "gdsolver-savetest-quit.dat" : "gdsolver-savetest.dat";
    const std::string path = std::string(cocos2d::CCFileUtils::sharedFileUtils()->getWritablePath()) + name;
    const long long before = g_blockedSave;
    GDFileWriter::writeAtomic(path, std::string("gdsolver savetest"));
    FILE* f = std::fopen(path.c_str(), "rb");
    const bool exists = f != nullptr;
    if (f) std::fclose(f);
    writeResult(std::string("savetest") + (atQuit ? " (at the leave of the level)" : "") + ": one write through the game's atomic writer; refused by the gate: "
                + (g_blockedSave > before ? "yes" : "NO (it was written)") + "; file in the save folder: " + (exists ? "yes" : "no")
                + " (saveblock=" + (g_cfg.saveBlock ? "1" : "0") + ", leave-gate=" + (g_quitDriving ? "1" : "0") + ")", false);
    (void)atQuit;
#else
    writeResult("savetest: not available on this build (the atomic writer is 2.209 Windows)", false);
#endif
}
}  // namespace savetest

// ---- the memory audit ----------------------------------------------------------------------
namespace memaudit {

using Flat = std::map<std::string, std::string>;

struct Snap {
    bool valid = false;
    std::string why;   // when not valid: what went wrong
    Flat gm;           // GameManager: CCGameManager.dat -- settings, GS_* statistics, achievements, GLM_* level records
    Flat ll;           // LocalLevelManager: CCLocalLevels.dat
};

inline std::string fnv(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char b[20];
    std::snprintf(b, sizeof(b), "%016llx", (unsigned long long)h);
    return b;
}

inline std::string scalar(const std::string& t, const std::string& x) {
    if (x.size() > 160) return t + ":<" + std::to_string(x.size()) + " chars fnv " + fnv(x) + ">";   // level strings, inputs
    return t + ":" + x;
}

// GD's plist dialect: <d>/<dict> a dictionary, <a> a list, <k> a key, <i>/<r>/<s> typed scalars,
// <t/> true, <f/> false. The paths are the ones py savediff prints (/GLM_01/1/k18), so a line from
// here and a line from the file diff name the same thing the same way.
inline void flatten(const std::string& xml, Flat& out) {
    struct Frame { bool isList; int idx; int n; std::string path; };
    std::vector<Frame> st;
    std::string key;
    auto childPath = [&]() {
        Frame& f = st.back();
        ++f.n;
        return f.isList ? f.path + "[" + std::to_string(f.idx++) + "]" : f.path + "/" + key;
    };
    const size_t n = xml.size();
    size_t i = 0;
    while (i < n) {
        const size_t lt = xml.find('<', i);
        if (lt == std::string::npos) break;
        size_t p = lt + 1;
        bool close = false;
        if (p < n && xml[p] == '/') { close = true; ++p; }
        const size_t ns = p;
        while (p < n && (std::isalnum((unsigned char)xml[p]) || xml[p] == '_')) ++p;
        const std::string name = xml.substr(ns, p - ns);
        if (p < n && xml[p] == '/') ++p;
        if (name.empty() || p >= n || xml[p] != '>') { i = lt + 1; continue; }
        ++p;
        size_t te = xml.find('<', p);
        if (te == std::string::npos) te = n;
        const std::string text = xml.substr(p, te - p);
        i = te;
        if (name == "plist" || name == "xml") continue;
        if (close) {
            if ((name == "d" || name == "dict" || name == "a") && !st.empty()) {
                const Frame f = st.back();
                st.pop_back();
                if (f.n == 0) out[f.path.empty() ? "/" : f.path] = f.isList ? "a:[]" : "d:{}";
            }
            continue;
        }
        if (name == "d" || name == "dict" || name == "a") {
            const std::string path = st.empty() ? std::string() : childPath();
            st.push_back({name == "a", 0, 0, path});
        } else if (name == "k") {
            key = text;
        } else if (!st.empty()) {
            const std::string path = childPath();
            out[path] = name == "t" ? "t:1" : name == "f" ? "f:0" : scalar(name, text);
        }
    }
}

inline bool decode(const std::string& s, std::string& xml, std::string& how) {
    if (s.empty()) { how = "empty save string"; return false; }
    // getCompressedSaveString = compressString(plist, encrypt=false, -1, 11): base64url + gzip, no XOR layer.
    const std::string a = cocos2d::ZipUtils::decompressString(s, false, 11);
    if (a.find("<dict>") != std::string::npos || a.find("<d>") != std::string::npos) { xml = a; return true; }
    const std::string b = cocos2d::ZipUtils::decompressString(s, true, 11);
    if (b.find("<dict>") != std::string::npos || b.find("<d>") != std::string::npos) { xml = b; return true; }
    how = "the save string did not decode to a plist (" + std::to_string(s.size()) + " B in)";
    return false;
}

inline std::string categoryOf(const std::string& path) {
    std::string top = path.size() > 1 && path[0] == '/' ? path.substr(1) : path;
    top = top.substr(0, top.find_first_of("/["));
    if (top == "reportedAchievements") return "achievements";
    if (top == "unlockValueKeeper") return "unlocks";
    if (top.rfind("GS_", 0) == 0) return "stats";
    if (top == "LLM_02") return "other";   // the save's binaryVersion, not a record
    if (top.rfind("GLM_", 0) == 0 || top.rfind("LLM_", 0) == 0) return "level record";
    return "other";
}

inline bool hasCategory(const Flat& f, const char* cat) {
    for (const auto& kv : f)
        if (categoryOf(kv.first) == cat) return true;
    return false;
}

// What the game would write, as of now. A fresh snapshot costs the same as a save without the
// disk write (the whole serialisation), so it is taken at the edges of a session only.
inline Snap take() {
    Snap s;
    try {
        auto* gm = GameManager::sharedState();
        auto* ll = LocalLevelManager::sharedState();
        if (!gm) { s.why = "no GameManager"; return s; }
        std::string xml, how;
        if (!decode(gm->getCompressedSaveString(), xml, how)) { s.why = "GameManager: " + how; return s; }
        flatten(xml, s.gm);
        if (ll) {
            xml.clear();
            if (decode(ll->getCompressedSaveString(), xml, how)) flatten(xml, s.ll);
            // An empty local-levels save is a normal state (no levels made): not an error.
        }
    } catch (const std::exception& e) {
        s.why = std::string("exception: ") + e.what();
        return s;
    }
    // The categories must exist in the baseline, or "none" would be a statement about a save this
    // parser cannot read (the same guard savediff has).
    for (const char* c : {"stats", "level record"})
        if (!hasCategory(s.gm, c)) { s.why = std::string("no ") + c + " keys in the game's save (" + std::to_string(s.gm.size()) + " keys)"; return s; }
    s.valid = true;
    return s;
}

inline Snap g_start;          // when the session opened, before the level had run a tick
inline bool g_haveStart = false;

inline void baseline() {
    if (!g_cfg.memAudit) { g_haveStart = false; return; }
    g_start = take();
    g_haveStart = true;
}

struct Change { std::string which, path, a, b; };

inline void diffFlat(const char* which, const Flat& a, const Flat& b, std::vector<Change>& out) {
    for (const auto& kv : a) {
        auto it = b.find(kv.first);
        if (it == b.end()) out.push_back({which, kv.first, kv.second, "(absent)"});
        else if (it->second != kv.second) out.push_back({which, kv.first, kv.second, it->second});
    }
    for (const auto& kv : b)
        if (!a.count(kv.first)) out.push_back({which, kv.first, "(absent)", kv.second});
}

inline long long g_leaks = 0;   // changes found in a leak category, summed over the reports of the process

// Compare what the game would write now with the baseline and say so in result.txt. `when` names
// the moment ("session end", "after leaving the level"). A report that could not be made says so
// too: the line is never silent about its own absence.
inline void report(const char* when) {
    if (!g_haveStart) return;   // memaudit=0 (or never baselined): the baseline's own line said so
    if (!g_start.valid) {
        writeResult(std::string("memaudit [") + when + "]: UNAVAILABLE -- no baseline (" + g_start.why + ")", false);
        return;
    }
    const Snap now = take();
    if (!now.valid) {
        writeResult(std::string("memaudit [") + when + "]: UNAVAILABLE -- " + now.why, false);
        return;
    }
    std::vector<Change> ch;
    diffFlat("gm", g_start.gm, now.gm, ch);
    diffFlat("ll", g_start.ll, now.ll, ch);
    std::map<std::string, int> by;
    for (const auto& c : ch) ++by[categoryOf(c.path)];
    long long leak = 0;
    for (const char* c : {"achievements", "unlocks", "stats", "level record"}) leak += by[c];
    g_leaks += leak;
    std::string line = std::string("memaudit [") + when + "]: " + (ch.empty() ? "none" : (leak ? "LEAK" : "settings only"))
        + " (compared gm=" + std::to_string(now.gm.size()) + " ll=" + std::to_string(now.ll.size()) + " keys; changed:"
        + " achievements=" + std::to_string(by["achievements"]) + " unlocks=" + std::to_string(by["unlocks"])
        + " stats=" + std::to_string(by["stats"]) + " levelrecord=" + std::to_string(by["level record"])
        + " other=" + std::to_string(by["other"]) + ")";
    writeResult(line, false);
    // The record categories first, then the rest (settings: shown, never a failure), up to a cap.
    int shown = 0;
    for (const bool leakPass : {true, false})
        for (const auto& c : ch) {
            const std::string cat = categoryOf(c.path);
            if ((cat != "other") != leakPass) continue;
            if (++shown > 24) { writeResult("memaudit: ... " + std::to_string(ch.size()) + " changes in all", false); return; }
            writeResult("memaudit:   [" + cat + "] " + c.which + " " + c.path + ": " + c.a + " -> " + c.b, false);
        }
}

// The level-record keys the game would save differently now than when the session opened, for the session's `level record changed:` line: "" when
// there are none, or when the audit could not be made (its own line says so). The line's fixed list of counters (LevelProgress) cannot name a field
// nobody has heard of -- 2.209's practice-attempt counter grew by one per restore in a takeover while the line read `none` -- and this is what
// stops that line from saying `none` about a key it does not look at.
inline std::string levelRecordChanges() {
    if (!g_haveStart || !g_start.valid) return std::string();
    const Snap now = take();
    if (!now.valid) return std::string();
    std::vector<Change> ch;
    diffFlat("gm", g_start.gm, now.gm, ch);
    diffFlat("ll", g_start.ll, now.ll, ch);
    std::string names;
    int n = 0;
    for (const auto& c : ch) {
        if (categoryOf(c.path) != "level record") continue;
        if (++n <= 3) names += (names.empty() ? "" : ", ") + c.path + " " + c.a + "->" + c.b;
    }
    if (!n) return std::string();
    return "memaudit=" + std::to_string(n) + " key(s) (" + names + (n > 3 ? ", ..." : "") + ")";
}

}  // namespace memaudit
}  // namespace gate
}  // namespace p1
